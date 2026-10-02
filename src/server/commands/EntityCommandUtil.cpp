// File: src/server/commands/EntityCommandUtil.cpp
#include "EntityCommandUtil.hpp"
#include "CommandCoords.hpp"
#include "CommandSourceStack.hpp"
#include "SnbtParser.hpp"
#include "../IntegratedServer.hpp"
#include "../entity/ItemEntityManager.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../world/storage/anvil/ComponentNbt.hpp"
#include "../world/storage/anvil/ItemStackNbt.hpp"

#include "common/data/DataComponentType.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/text/TextComponent.hpp"
#include "common/world/tags/DataTags.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Server::EntityCmd {

    std::string Tr(const char* key, std::initializer_list<std::string> args) {
        std::vector<Game::Text::Component> with;
        with.reserve(args.size());
        for (const std::string& a : args) with.push_back(Game::Text::Component::Literal(a));
        return Game::Text::GetString(Game::Text::Component::Translatable(key, std::move(with)));
    }

    void Success(const CommandSourceStack& source, ServerConnection& connection, bool broadcast,
                 const std::string& text) {
        source.SendSuccess(connection, text, broadcast);
    }

    void Failure(ServerConnection& connection, const std::string& text) {
        Network::ChatMessageS2CPacket packet;
        packet.senderId = 0;
        packet.position = 1;
        packet.segments.push_back(Network::ChatSegmentData{
            text, 0xFFFF5555u, Network::ChatClickAction::None, "", ""});
        connection.SendChatMessage(packet);
    }

    ServerLevel* LevelOf(Game::DimensionId dimension) {
        return g_integratedServer ? g_integratedServer->GetLevel(dimension) : nullptr;
    }

    std::string DisplayName(const SelectedEntity& entity) {
        switch (entity.kind) {
            case SelectedEntity::Kind::Player:
                return entity.player ? entity.player->getName() : entity.name;
            case SelectedEntity::Kind::Mob:
                // SelectedEntity.name is already Entity.getName (the custom
                // name, else the translated type) — EntitySelector's.
                return entity.name;
            case SelectedEntity::Kind::Item: {
                // ItemEntity.getName: the stack's hover name.
                ServerLevel* level = LevelOf(entity.dimension);
                ItemEntityManager* items = level ? level->Items() : nullptr;
                if (const Game::ItemEntity* item = items ? items->Find(entity.id) : nullptr) {
                    return Game::GetItemStackHoverName(item->stack);
                }
                return Tr("entity.minecraft.item");
            }
            case SelectedEntity::Kind::Orb:
                return Tr("entity.minecraft.experience_orb");
        }
        return entity.name;
    }

    Game::LivingEntity* LivingOf(const SelectedEntity& entity) {
        switch (entity.kind) {
            case SelectedEntity::Kind::Player:
                return entity.player ? entity.player->effectEntity() : nullptr;
            case SelectedEntity::Kind::Mob:
                return entity.mob;
            default:
                return nullptr;
        }
    }

    // ── Numbers ────────────────────────────────────────────────────────────

    namespace {
        // Brigadier StringReader.readInt/readFloat/readDouble accept only
        // [0-9.-] — no leading '+', no exponent.
        bool NumberChars(const std::string& s, bool allowDot) {
            if (s.empty()) return false;
            for (const char c : s) {
                const bool ok = (c >= '0' && c <= '9') || c == '-' || (allowDot && c == '.');
                if (!ok) return false;
            }
            return true;
        }
    } // namespace

    bool ParseInt(const std::string& token, int min, int max, int& out, std::string& error) {
        if (!NumberChars(token, false)) {
            error = token.empty() ? "Expected integer" : "Invalid integer '" + token + "'";
            return false;
        }
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(token.c_str(), &end, 10);
        if (errno != 0 || !end || *end != '\0' || v < INT32_MIN || v > INT32_MAX) {
            error = "Invalid integer '" + token + "'";
            return false;
        }
        if (v < min) {
            error = Tr("argument.integer.low", {std::to_string(min), std::to_string(v)});
            return false;
        }
        if (v > max) {
            error = Tr("argument.integer.big", {std::to_string(max), std::to_string(v)});
            return false;
        }
        out = static_cast<int>(v);
        return true;
    }

    bool ParseFloat(const std::string& token, float min, float max, float& out, std::string& error) {
        if (!NumberChars(token, true)) {
            error = token.empty() ? "Expected float" : "Invalid float '" + token + "'";
            return false;
        }
        char* end = nullptr;
        const float v = std::strtof(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(v)) {
            error = "Invalid float '" + token + "'";
            return false;
        }
        if (v < min) { error = Tr("argument.float.low", {JavaFloat(min), JavaFloat(v)}); return false; }
        if (v > max) { error = Tr("argument.float.big", {JavaFloat(max), JavaFloat(v)}); return false; }
        out = v;
        return true;
    }

    bool ParseDouble(const std::string& token, double min, double max, double& out, std::string& error) {
        if (!NumberChars(token, true)) {
            error = token.empty() ? "Expected double" : "Invalid double '" + token + "'";
            return false;
        }
        char* end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(v)) {
            error = "Invalid double '" + token + "'";
            return false;
        }
        if (v < min) { error = Tr("argument.double.low", {JavaDouble(min), JavaDouble(v)}); return false; }
        if (v > max) { error = Tr("argument.double.big", {JavaDouble(max), JavaDouble(v)}); return false; }
        out = v;
        return true;
    }

    namespace {
        // Java's Float/Double.toString: the shortest decimal that reads back
        // to the same value, at least one digit after the point, scientific
        // ("1.0E10") outside 1e-3 .. 1e7.
        template <typename T>
        std::string JavaNumber(T v, int maxDigits) {
            if (std::isnan(v)) return "NaN";
            if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
            if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
            const double mag = std::fabs(static_cast<double>(v));
            const bool sci = mag < 1e-3 || mag >= 1e7;
            char buf[64];
            for (int digits = 1; digits <= maxDigits; ++digits) {
                std::snprintf(buf, sizeof(buf), sci ? "%.*e" : "%.*g", sci ? digits - 1 : digits,
                              static_cast<double>(v));
                if (static_cast<T>(std::strtod(buf, nullptr)) == v) break;
            }
            std::string s = buf;
            if (sci) {
                // "1.5e+10" -> "1.5E10"; "1e-05" -> "1.0E-5".
                const size_t e = s.find('e');
                std::string mant = s.substr(0, e);
                std::string exp = s.substr(e + 1);
                if (mant.find('.') == std::string::npos) mant += ".0";
                const bool neg = !exp.empty() && exp[0] == '-';
                if (!exp.empty() && (exp[0] == '+' || exp[0] == '-')) exp.erase(0, 1);
                while (exp.size() > 1 && exp[0] == '0') exp.erase(0, 1);
                return mant + "E" + (neg ? "-" : "") + exp;
            }
            if (s.find('.') == std::string::npos) s += ".0";
            return s;
        }
    } // namespace

    std::string JavaFloat(float v)   { return JavaNumber<float>(v, 9); }
    std::string JavaDouble(double v) { return JavaNumber<double>(v, 17); }

    std::string FormatList(const std::vector<std::string>& items) {
        std::string out;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i) out += ", ";
            out += items[i];
        }
        return out;
    }

    bool ParseRotation(const std::string& yaw, const std::string& pitch, const CommandSourceStack& source,
                       float& outYRot, float& outXRot, bool& yRelative, bool& xRelative,
                       std::string& error) {
        if ((!yaw.empty() && yaw[0] == '^') || (!pitch.empty() && pitch[0] == '^')) {
            error = "Local coordinates are not allowed here";
            return false;
        }
        double y = 0.0, x = 0.0;
        if (!ParseCoord(yaw, source.rotation.yRot, false, y) ||
            !ParseCoord(pitch, source.rotation.xRot, false, x)) {
            error = Tr("argument.rotation.incomplete");
            return false;
        }
        yRelative = !yaw.empty() && yaw[0] == '~';
        xRelative = !pitch.empty() && pitch[0] == '~';
        outYRot = static_cast<float>(y);
        outXRot = static_cast<float>(x);
        return true;
    }

    // ── ItemPredicateArgument ──────────────────────────────────────────────

    namespace {

        std::string WithNamespace(const std::string& id) {
            return id.find(':') == std::string::npos ? "minecraft:" + id : id;
        }
        std::string StripMinecraft(const std::string& id) {
            return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
        }

        bool IsIdChar(char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                   c == '.' || c == '/' || c == ':';
        }

        // The end of a value starting at `i` inside the brackets: the next
        // top-level ',', '|' or the closing ']' (quotes and nesting skipped).
        size_t ValueEnd(const std::string& s, size_t i) {
            int depth = 0;
            char quote = 0;
            for (; i < s.size(); ++i) {
                const char c = s[i];
                if (quote) {
                    if (c == '\\') { ++i; continue; }
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '"' || c == '\'') { quote = c; continue; }
                if (c == '{' || c == '[') { ++depth; continue; }
                if (c == '}' || c == ']') {
                    if (depth == 0) return i;
                    --depth;
                    continue;
                }
                if (depth == 0 && (c == ',' || c == '|')) return i;
            }
            return i;
        }

        // An SNBT value as a tag (wrapped in a compound so any value type
        // parses with the compound parser).
        std::shared_ptr<::World::NBTTag> ParseValue(const std::string& text, std::string& error) {
            auto wrapped = Snbt::ParseCompound("{v:" + text + "}", error);
            return wrapped ? wrapped->GetTag("v") : nullptr;
        }

    } // namespace

    bool ParseItemPredicate(const std::string& text, ItemPredicate& out, std::string& error) {
        out = ItemPredicate{};
        size_t i = 0;
        std::string head;
        if (!text.empty() && text[0] == '*') {
            out.kind = ItemPredicate::Kind::Any;
            i = 1;
        } else {
            const bool isTag = !text.empty() && text[0] == '#';
            size_t start = isTag ? 1 : 0;
            i = start;
            while (i < text.size() && IsIdChar(text[i])) ++i;
            head = text.substr(start, i - start);
            if (head.empty()) { error = Tr("arguments.item.malformed", {text}); return false; }
            if (isTag) {
                out.kind = ItemPredicate::Kind::Tag;
                out.tag = WithNamespace(head);
                if (!Game::DataTags::TagExists(Game::DataTags::Registry::Item, out.tag)) {
                    error = Tr("arguments.item.tag.unknown", {out.tag});
                    return false;
                }
            } else {
                out.kind = ItemPredicate::Kind::Item;
                out.item = Game::Anvil::ItemFromName(WithNamespace(head));
                if (out.item == Game::Items::Air && StripMinecraft(head) != "air") {
                    error = Tr("argument.item.id.invalid", {WithNamespace(head)});
                    return false;
                }
            }
        }
        if (i == text.size()) return true;
        if (text[i] != '[' || text.back() != ']') {
            error = Tr("arguments.item.malformed", {text});
            return false;
        }
        ++i;
        std::vector<ItemPredicate::Term> group;
        while (i < text.size()) {
            while (i < text.size() && text[i] == ' ') ++i;
            if (i < text.size() && text[i] == ']') {
                if (!group.empty()) out.groups.push_back(std::move(group));
                return i + 1 == text.size() ? true : (error = Tr("arguments.item.malformed", {text}), false);
            }
            ItemPredicate::Term term;
            if (i < text.size() && text[i] == '!') { term.negated = true; ++i; }
            const size_t nameStart = i;
            while (i < text.size() && IsIdChar(text[i])) ++i;
            const std::string name = StripMinecraft(text.substr(nameStart, i - nameStart));
            if (name.empty()) { error = Tr("arguments.item.component.expected"); return false; }
            while (i < text.size() && text[i] == ' ') ++i;
            const char op = i < text.size() ? text[i] : ']';
            if (op == '=' || op == '~') {
                const size_t valueStart = i + 1;
                const size_t valueEnd = ValueEnd(text, valueStart);
                const std::string value = text.substr(valueStart, valueEnd - valueStart);
                std::string parseError;
                auto tag = ParseValue(value, parseError);
                if (!tag) {
                    error = Tr(op == '=' ? "arguments.item.component.malformed" : "arguments.item.predicate.malformed",
                               {"minecraft:" + name, parseError});
                    return false;
                }
                if (op == '=' && !Game::DataComponents::ByName(name)) {
                    error = Tr("arguments.item.component.unknown", {"minecraft:" + name});
                    return false;
                }
                // {components:{name:value}} or {predicates:{name:value}} —
                // the ItemPredicate NBT form ItemPredicateMatches reads.
                auto inner = std::make_shared<::World::NBTTagCompound>();
                inner->value[name] = tag;
                term.nbt = std::make_shared<::World::NBTTagCompound>();
                term.nbt->value[op == '=' ? "components" : "predicates"] = inner;
                i = valueEnd;
            } else {
                // A bare name: the component is present (DataComponentType
                // existence test).
                const Game::DataComponentTypeBase* type = Game::DataComponents::ByName(name);
                if (!type) {
                    error = Tr("arguments.item.component.unknown", {"minecraft:" + name});
                    return false;
                }
                term.presentType = type;
            }
            group.push_back(std::move(term));
            while (i < text.size() && text[i] == ' ') ++i;
            if (i >= text.size()) break;
            if (text[i] == '|') { ++i; continue; }          // another alternative, same group
            if (text[i] == ',') { ++i; out.groups.push_back(std::move(group)); group.clear(); continue; }
            if (text[i] == ']') continue;
            error = Tr("arguments.item.malformed", {text});
            return false;
        }
        error = Tr("arguments.item.malformed", {text});
        return false;
    }

    bool ItemPredicate::Test(const Game::ItemStack& stack) const {
        if (stack.IsEmpty()) return false;
        switch (kind) {
            case Kind::Any: break;
            case Kind::Item: if (stack.itemId != item) return false; break;
            case Kind::Tag: {
                const std::string slug(Game::ItemRegistry::Slug(stack.itemId));
                if (!Game::DataTags::HasTag(Game::DataTags::Registry::Item, slug, tag)) return false;
                break;
            }
        }
        for (const std::vector<Term>& alternatives : groups) {
            bool any = false;
            for (const Term& term : alternatives) {
                bool pass = false;
                if (term.presentType) {
                    const auto* type = static_cast<const Game::DataComponentTypeBase*>(term.presentType);
                    pass = stack.components.has(*type) ||
                           (!stack.components.isRemoved(*type) &&
                            Game::ItemRegistry::Get(stack.itemId).defaultComponents.has(*type));
                } else if (term.nbt) {
                    pass = Game::Anvil::ComponentNbt::ItemPredicateMatches(*term.nbt, stack);
                }
                if (pass != term.negated) { any = true; break; }
            }
            if (!any) return false;
        }
        return true;
    }

} // namespace Server::EntityCmd
