// File: src/client/shader/ShaderOptions.cpp
#include "client/shader/ShaderOptions.hpp"
#include "client/shader/ShaderPacks.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
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

    // ── the pack's layout ────────────────────────────────────────────────

    namespace {
        // A properties file's logical lines: comments dropped, a trailing
        // backslash continues the line. Preprocessor lines (#if MC_VERSION
        // ...) are skipped with their contents taken as they come — the
        // layout keys are not versioned in any pack seen.
        void ForEachPropertyLine(const fs::path& file, const std::function<void(const std::string&, const std::string&)>& fn) {
            std::ifstream in(file);
            std::string line, pending;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string t = Trim(line);
                if (!pending.empty()) { t = pending + " " + t; pending.clear(); }
                if (t.empty() || t[0] == '#') continue;
                if (t.back() == '\\') { pending = t.substr(0, t.size() - 1); continue; }
                const size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                fn(Trim(t.substr(0, eq)), Trim(t.substr(eq + 1)));
            }
        }
        std::vector<std::string> Words(const std::string& text) {
            std::vector<std::string> out;
            std::stringstream ss(text);
            std::string w;
            while (ss >> w) out.push_back(w);
            return out;
        }
        std::string LangOr(const std::map<std::string, std::string>& lang, const std::string& key, const std::string& fallback) {
            auto it = lang.find(key);
            return it != lang.end() ? it->second : fallback;
        }
    } // namespace

    std::string PackLayout::OptionLabel(const std::string& option) const   { return LangOr(lang, "option." + option, option); }
    std::string PackLayout::OptionComment(const std::string& option) const { return LangOr(lang, "option." + option + ".comment", ""); }
    std::string PackLayout::ValueLabel(const std::string& option, const std::string& value) const {
        return LangOr(lang, "value." + option + "." + value, value);
    }
    std::string PackLayout::ScreenLabel(const std::string& screen) const   { return LangOr(lang, "screen." + screen, screen); }
    std::string PackLayout::ScreenComment(const std::string& screen) const { return LangOr(lang, "screen." + screen + ".comment", ""); }
    std::string PackLayout::ProfileLabel(const std::string& profile) const { return LangOr(lang, "profile." + profile, profile); }

    int PackLayout::MatchingProfile(const std::vector<Option>& options, const Overrides& overrides) const {
        for (size_t i = 0; i < profiles.size(); ++i) {
            bool all = true;
            for (const auto& [name, value] : profiles[i].values) {
                auto opt = std::find_if(options.begin(), options.end(), [&](const Option& o) { return o.name == name; });
                if (opt == options.end()) continue;
                const std::string cur = CurrentValue(*opt, overrides);
                const std::string want = opt->isToggle ? (value == "true" ? "ON" : "OFF") : value;
                if (cur != want) { all = false; break; }
            }
            if (all) return static_cast<int>(i);
        }
        return -1;
    }

    PackLayout LoadLayout(const std::string& shadersDir) {
        PackLayout layout;
        const fs::path dir(shadersDir);
        std::map<std::string, std::string> rawProfiles;   // name -> the line, inheritance resolved below
        std::vector<std::string> profileOrder;
        ForEachPropertyLine(dir / "shaders.properties", [&](const std::string& key, const std::string& value) {
            if (key == "screen") {
                layout.screens[""] = Words(value);
            } else if (key.rfind("screen.", 0) == 0) {
                const std::string rest = key.substr(7);
                const size_t dot = rest.find('.');
                if (dot == std::string::npos) layout.screens[rest] = Words(value);
                else if (rest.substr(dot + 1) == "columns") layout.columns[rest.substr(0, dot)] = std::max(1, std::atoi(value.c_str()));
            } else if (key == "sliders") {
                for (const std::string& w : Words(value)) layout.sliders.insert(w);
            } else if (key.rfind("profile.", 0) == 0) {
                const std::string name = key.substr(8);
                if (!rawProfiles.count(name)) profileOrder.push_back(name);
                rawProfiles[name] = value;
            }
        });
        // Profiles: `A=1` a value, `A` a toggle on, `!A` a toggle off,
        // `profile.OTHER` the other profile's values first (OptiFine).
        std::function<void(const std::string&, Overrides&, int)> resolve = [&](const std::string& name, Overrides& out, int depth) {
            auto it = rawProfiles.find(name);
            if (it == rawProfiles.end() || depth > 8) return;
            for (const std::string& w : Words(it->second)) {
                if (w.rfind("profile.", 0) == 0) { resolve(w.substr(8), out, depth + 1); continue; }
                const size_t eq = w.find('=');
                if (eq != std::string::npos) out[w.substr(0, eq)] = w.substr(eq + 1);
                else if (!w.empty() && w[0] == '!') out[w.substr(1)] = "false";
                else out[w] = "true";
            }
        };
        for (const std::string& name : profileOrder) {
            PackLayout::Profile p;
            p.name = name;
            resolve(name, p.values, 0);
            layout.profiles.push_back(std::move(p));
        }
        // The language file: en_us.lang in any letter case, under lang/.
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir / "lang", ec)) {
            std::string fn = e.path().filename().string();
            std::transform(fn.begin(), fn.end(), fn.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (fn != "en_us.lang") continue;
            ForEachPropertyLine(e.path(), [&](const std::string& key, const std::string& value) { layout.lang[key] = value; });
            break;
        }
        return layout;
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
