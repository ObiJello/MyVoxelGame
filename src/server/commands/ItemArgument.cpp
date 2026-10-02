// File: src/server/commands/ItemArgument.cpp
#include "ItemArgument.hpp"
#include "SnbtParser.hpp"
#include "../world/storage/NBTParser.hpp"
#include "../world/storage/anvil/ItemStackNbt.hpp"
#include "../world/storage/anvil/ComponentNbt.hpp"
#include "common/data/DataComponentType.hpp"
#include "common/entity/ItemComponentHints.hpp"

#include <cctype>
#include <memory>

namespace Server {

    namespace {

        std::string Trim(const std::string& s) {
            const size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) return {};
            const size_t e = s.find_last_not_of(" \t");
            return s.substr(b, e - b + 1);
        }

        // One `key=value` of the component list ends at a ',' or the closing
        // ']' at nesting depth zero (quotes and SNBT brackets skipped).
        size_t ValueEnd(const std::string& s, size_t from) {
            int depth = 0;
            char quote = 0;
            for (size_t i = from; i < s.size(); ++i) {
                const char c = s[i];
                if (quote) {
                    if (c == '\\') { ++i; continue; }
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '{' || c == '[') { ++depth; continue; }
                if (c == '}' || c == ']') {
                    if (depth == 0) return i;   // the list's own ']'
                    --depth;
                    continue;
                }
                if (c == ',' && depth == 0) return i;
            }
            return std::string::npos;
        }

    } // namespace

    bool ParseItemArgument(const std::string& text, Game::ItemStack& out, std::string& error) {
        // ItemParser.readItem: the id runs to the '[' (or '{', which only the
        // old syntax used).
        size_t idEnd = text.find_first_of("[{");
        std::string id = text.substr(0, idEnd);
        if (id.empty()) { error = "Expected item"; return false; }
        for (char& c : id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (id.find(':') == std::string::npos) id = "minecraft:" + id;

        auto root = std::make_shared<::World::NBTTagCompound>();
        root->value["id"] = std::make_shared<::World::NBTTagString>(id);
        root->value["count"] = std::make_shared<::World::NBTTagInt>(1);

        if (idEnd != std::string::npos) {
            if (text[idEnd] == '{') {
                error = "Item NBT tags are not read since 1.20.5: use components, e.g. " +
                        text.substr(0, idEnd) + "[custom_name=\"Name\"]";
                return false;
            }
            // ItemParser.readComponents: `[` (key=value | !key) {, ...} `]`.
            auto components = std::make_shared<::World::NBTTagCompound>();
            size_t i = idEnd + 1;
            bool closed = false;
            while (i < text.size()) {
                while (i < text.size() && text[i] == ' ') ++i;
                if (i < text.size() && text[i] == ']') { closed = true; ++i; break; }
                const size_t eq = text.find_first_of("=,]", i);
                std::string key = Trim(text.substr(i, eq == std::string::npos ? std::string::npos : eq - i));
                for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                const bool removal = !key.empty() && key[0] == '!';
                if (removal) key.erase(0, 1);
                if (key.empty()) { error = "Expected item component"; return false; }
                const std::string bare = key.rfind("minecraft:", 0) == 0 ? key.substr(10) : key;
                // arguments.item.component.unknown / .repeated (a component
                // may be set or removed, once).
                if (!Game::DataComponents::ByName(bare)) {
                    error = "Unknown item component 'minecraft:" + bare + "'";
                    return false;
                }
                if (components->value.count("minecraft:" + bare) || components->value.count("!minecraft:" + bare)) {
                    error = "Item component 'minecraft:" + bare + "' was repeated, but only one value can be specified";
                    return false;
                }
                if (removal) {
                    // `!component`: the item's default is taken away
                    // (DataComponentPatch removal, ReadItemStack applies it).
                    if (eq != std::string::npos && text[eq] == '=') {
                        error = "Malformed '" + bare + "' component: a removal takes no value";
                        return false;
                    }
                    if (eq == std::string::npos) { error = "Expected ']' to close the item components"; return false; }
                    components->value["!minecraft:" + bare] = std::make_shared<::World::NBTTagCompound>();
                    i = eq;
                    if (text[i] == ',') { ++i; continue; }
                    closed = true;   // ']'
                    ++i;
                    break;
                }
                if (eq == std::string::npos || text[eq] != '=') {
                    error = "Expected '=' after component '" + key + "'";
                    return false;
                }
                // Settable: a component ReadItemStack applies — the inline
                // ones (ItemComponentHints) and every registered codec.
                if (!Game::ItemComponentHints::Known(bare) &&
                    !Game::Anvil::ComponentNbt::Find(Game::DataComponents::ByName(bare))) {
                    error = "Setting the 'minecraft:" + bare + "' component is not supported by this game";
                    return false;
                }
                const size_t end = ValueEnd(text, eq + 1);
                if (end == std::string::npos) { error = "Expected ']' to close the item components"; return false; }
                const std::string valueText = Trim(text.substr(eq + 1, end - eq - 1));
                std::string valueError;
                ::World::NBTTagPtr value = Snbt::ParseValue(valueText, valueError);
                if (!value) {
                    // arguments.item.component.malformed
                    error = "Malformed 'minecraft:" + bare + "' component: '" + valueError + "'";
                    return false;
                }
                components->value["minecraft:" + bare] = std::move(value);
                i = end;
                if (text[i] == ',') { ++i; continue; }
                closed = true;   // ']'
                ++i;
                break;
            }
            if (!closed) { error = "Expected ']' to close the item components"; return false; }
            if (i < text.size()) { error = "Unexpected text after the item: " + text.substr(i); return false; }
            root->value["components"] = std::move(components);
        }

        std::string componentError;
        Game::ItemStack stack = Game::Anvil::ReadItemStack(*root, &componentError);
        if (stack.IsEmpty()) {
            error = componentError.empty() ? "Unknown item '" + id + "'" : componentError;
            return false;
        }
        if (!componentError.empty()) {
            // arguments.item.component.malformed — a value the component's
            // codec refused (or ItemStack.validateComponents' complaint).
            error = componentError;
            return false;
        }
        stack.count = 1;
        out = std::move(stack);
        return true;
    }

} // namespace Server
