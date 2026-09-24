// File: src/client/shader/ShaderOptions.cpp
#include "client/shader/ShaderOptions.hpp"
#include "client/shader/ShaderPacks.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace Shaders {

    namespace {

        // `#define NAME`, `//#define NAME`, `#define NAME value // text [a b c]`.
        const std::regex kDefine(
            R"re(^[ \t]*(//[ \t]*)?#[ \t]*define[ \t]+([A-Za-z_][A-Za-z0-9_]*)(?:[ \t]+([^ \t/]+))?[ \t]*(?://(.*))?$)re");

        std::string Trim(std::string s) {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        // The `[a b c]` list out of a trailing comment, and the text before it.
        bool ParseList(const std::string& comment, std::vector<std::string>& values, std::string& text) {
            const size_t open = comment.find('[');
            const size_t close = comment.find(']', open == std::string::npos ? 0 : open);
            if (open == std::string::npos || close == std::string::npos || close < open) { text = Trim(comment); return false; }
            std::stringstream ss(comment.substr(open + 1, close - open - 1));
            std::string v;
            while (ss >> v) values.push_back(v);
            text = Trim(comment.substr(0, open));
            return !values.empty();
        }

        bool IsShaderFile(const fs::path& p) {
            const std::string e = p.extension().string();
            return e == ".vsh" || e == ".fsh" || e == ".gsh" || e == ".csh" || e == ".glsl" || e == ".inc" ||
                   e == ".settings" || e == ".h";
        }

        std::string OverridesPath(const std::string& packName) {
            return (fs::path(PacksDirectory()) / (packName + ".txt")).string();
        }

    } // namespace

    std::vector<Option> Discover(const std::string& shadersDir) {
        std::vector<Option> out;
        std::map<std::string, size_t> index;
        std::vector<fs::path> files;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(fs::path(shadersDir), ec)) {
            if (e.is_regular_file(ec) && IsShaderFile(e.path())) files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            std::ifstream in(f);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::smatch m;
                if (!std::regex_match(line, m, kDefine)) continue;
                const bool commented = m[1].matched;
                const std::string name = m[2].str();
                const std::string value = m[3].matched ? m[3].str() : "";
                const std::string comment = m[4].matched ? m[4].str() : "";
                Option opt;
                opt.name = name;
                if (value.empty()) {
                    // A bare define is an option only as a toggle; MC's own
                    // convention is that every bare #define may be toggled.
                    opt.isToggle = true;
                    opt.defaultOn = !commented;
                    opt.comment = Trim(comment);
                } else {
                    std::vector<std::string> values;
                    std::string text;
                    if (!ParseList(comment, values, text)) continue;   // a constant, not an option
                    if (commented) continue;
                    opt.isToggle = false;
                    opt.defaultValue = value;
                    opt.values = values;
                    if (std::find(values.begin(), values.end(), value) == values.end()) opt.values.insert(opt.values.begin(), value);
                    opt.comment = text;
                }
                auto it = index.find(name);
                if (it == index.end()) { index[name] = out.size(); out.push_back(std::move(opt)); }
            }
        }
        return out;
    }

    Overrides LoadOverrides(const std::string& packName) {
        Overrides o;
        std::ifstream in(OverridesPath(packName));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line.empty() || line[0] == '#') continue;
            o[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
        }
        return o;
    }

    void SaveOverrides(const std::string& packName, const Overrides& overrides) {
        std::error_code ec;
        if (overrides.empty()) { fs::remove(OverridesPath(packName), ec); return; }
        std::ofstream out(OverridesPath(packName));
        for (const auto& [k, v] : overrides) out << k << '=' << v << '\n';
    }

    std::string Apply(const std::string& source, const Overrides& overrides) {
        if (overrides.empty()) return source;
        std::string out;
        out.reserve(source.size() + 64);
        std::istringstream in(source);
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_match(line, m, kDefine)) {
                auto ov = overrides.find(m[2].str());
                if (ov != overrides.end()) {
                    const bool hasValue = m[3].matched;
                    const std::string rest = m[4].matched ? " //" + m[4].str() : "";
                    if (!hasValue) {
                        const bool on = ov->second == "true";
                        line = (on ? "#define " : "//#define ") + m[2].str() + rest;
                    } else {
                        line = "#define " + m[2].str() + " " + ov->second + rest;
                    }
                }
            }
            out += line;
            out += '\n';
        }
        return out;
    }

    std::string CurrentValue(const Option& option, const Overrides& overrides) {
        auto it = overrides.find(option.name);
        if (option.isToggle) {
            if (it != overrides.end()) return it->second == "true" ? "ON" : "OFF";
            return option.defaultOn ? "ON" : "OFF";
        }
        return it != overrides.end() ? it->second : option.defaultValue;
    }

} // namespace Shaders
