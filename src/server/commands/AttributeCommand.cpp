// File: src/server/commands/AttributeCommand.cpp
//
// Port of MC AttributeCommand.java. Feedback is MC's en_us text for the
// same translation keys (commands.attribute.*), with the attribute's
// translated name (attribute.name.<id>) and the entity's display name.
#include "AttributeCommand.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "../level/LevelEntityStore.hpp"   // MakeMobForLoad — a type's supplied defaults
#include "common/data/components/AttributeComponents.hpp"   // AttributeOperationFromName
#include "common/entity/Attributes.hpp"
#include "common/entity/LivingEntity.hpp"
#include "common/entity/Mob.hpp"
#include "common/network/PacketTypes.hpp"   // ChatMessageS2CPacket for the red failure text
#include "common/text/Language.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Server {

    namespace {

        // ── Feedback (MC en_us) ────────────────────────────────────────────

        constexpr const char* kValueGet       = "Value of attribute %s for entity %s is %s";
        constexpr const char* kBaseGet        = "Base value of attribute %s for entity %s is %s";
        constexpr const char* kBaseSet        = "Base value for attribute %s for entity %s set to %s";
        constexpr const char* kBaseReset      = "Base value for attribute %s for entity %s reset to default %s";
        constexpr const char* kModifierAdd    = "Added modifier %s to attribute %s for entity %s";
        constexpr const char* kModifierRemove = "Removed modifier %s from attribute %s for entity %s";
        constexpr const char* kModifierGet    = "Value of modifier %s on attribute %s for entity %s is %s";
        constexpr const char* kNotLiving      = "%s is not a valid entity for this command";
        constexpr const char* kNoAttribute    = "Entity %s has no attribute %s";
        constexpr const char* kNoModifier     = "Attribute %s for entity %s has no modifier %s";
        constexpr const char* kModifierExists = "Modifier %s is already present on attribute %s for entity %s";

        // MC's %s substitution, in order.
        std::string Format(const char* pattern, std::initializer_list<std::string> args) {
            std::string out;
            auto next = args.begin();
            for (const char* p = pattern; *p; ++p) {
                if (p[0] == '%' && p[1] == 's' && next != args.end()) {
                    out += *next++;
                    ++p;
                } else {
                    out += *p;
                }
            }
            return out;
        }

        // MC CommandSourceStack.sendFailure: red, to the sender only.
        void SendFailure(ServerConnection& connection, const std::string& text) {
            Network::ChatMessageS2CPacket packet;
            packet.senderId = 0;
            packet.position = 1;
            packet.segments.push_back(Network::ChatSegmentData{
                text, 0xFFFF5555u, Network::ChatClickAction::None, "", ""});
            connection.SendChatMessage(packet);
        }


        // Java's Double.toString — how MC prints the `%s` of a double:
        // shortest round-trip digits, always a ".0" on a whole number, and
        // computerized notation ("6.0E7") outside [1e-3, 1e7).
        std::string JavaDouble(double v) {
            if (std::isnan(v)) return "NaN";
            if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
            if (v == 0.0) return std::signbit(v) ? "-0.0" : "0.0";
            const double mag = std::fabs(v);
            char buf[64];
            if (mag >= 1.0e-3 && mag < 1.0e7) {
                for (int precision = 1; precision <= 17; ++precision) {
                    std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
                    if (std::strtod(buf, nullptr) == v) break;
                }
                std::string out = buf;
                // %g may still pick an exponent for e.g. 1e+06 at low
                // precision; fixed notation is what Java writes in range.
                if (out.find('e') != std::string::npos) {
                    for (int decimals = 0; decimals <= 17; ++decimals) {
                        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
                        if (std::strtod(buf, nullptr) == v) break;
                    }
                    out = buf;
                }
                if (out.find('.') == std::string::npos) out += ".0";
                return out;
            }
            // d.dddE±n
            for (int precision = 0; precision <= 16; ++precision) {
                std::snprintf(buf, sizeof(buf), "%.*e", precision, v);
                if (std::strtod(buf, nullptr) == v) break;
            }
            std::string s = buf;
            const size_t e = s.find('e');
            std::string mantissa = s.substr(0, e);
            int exponent = std::atoi(s.c_str() + e + 1);
            if (mantissa.find('.') == std::string::npos) mantissa += ".0";
            return mantissa + "E" + std::to_string(exponent);
        }

        // Brigadier StringReader.readDouble: digits, '.', '-' — nothing
        // trailing, a finite result.
        bool ParseDouble(const std::string& text, double& out) {
            if (text.empty()) return false;
            for (const char c : text) {
                if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-')) return false;
            }
            char* end = nullptr;
            out = std::strtod(text.c_str(), &end);
            return end && *end == '\0' && end != text.c_str() && std::isfinite(out);
        }

        // Entity.getDisplayName: a player's name; a mob's custom name, else
        // its translated type name.
        std::string DisplayName(const SelectedEntity& target) {
            if (target.kind == SelectedEntity::Kind::Player) return target.name;
            if (target.mob && target.mob->HasCustomName()) return *target.mob->GetCustomName();
            std::string fallback;
            bool upper = true;
            for (const char c : target.typeSlug) {
                if (c == '_') { fallback += ' '; upper = true; continue; }
                fallback += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                upper = false;
            }
            return Game::Language::GetOrDefault("entity.minecraft." + target.typeSlug, fallback);
        }

        // AttributeCommand.getAttributeDescription: the attribute's
        // translated name.
        std::string AttributeDisplay(Game::Attribute attribute) {
            return Game::Language::Get(Game::AttributeDescriptionId(attribute));
        }

        // What /attribute acts on: a player (its own map on the ServerPlayer,
        // which outlives the per-level entity view) or a living mob.
        struct Target {
            ServerPlayer*       player = nullptr;
            Game::LivingEntity* living = nullptr;
            std::string         name;
        };

        Game::AttributeMap& OwnMap(const Target& t) {
            return t.player ? t.player->attributes() : t.living->Attributes();
        }

        // MC getAttributeInstance(…): the whole instance — a player's worn
        // items, effects and creative reach folded onto its own row.
        Game::AttributeInstance FullInstance(const Target& t, Game::Attribute attribute) {
            if (t.player) return t.player->attributeInstance(attribute);
            const Game::AttributeInstance* row = t.living->Attributes().Find(attribute);
            return row ? *row : Game::AttributeInstance(attribute, Game::GetAttributeDef(attribute).defaultValue);
        }

        // MC AttributeSupplier.getBaseValue — what resetBaseValue returns
        // to: the type's own default. A mob's comes from a freshly built
        // mob of its type (its constructor IS its createAttributes()),
        // remembered per type; a player's is Player.createAttributes'.
        double SuppliedDefault(const Target& t, Game::Attribute attribute) {
            const Game::AttributeInstance* row = OwnMap(t).Find(attribute);
            if (t.player || !t.living) return row ? row->GetDefaultValue() : Game::PlayerBaseAttributeValue(attribute);
            static std::unordered_map<uint16_t, Game::AttributeMap> s_typeDefaults;
            const auto type = t.living->GetType();
            auto it = s_typeDefaults.find(static_cast<uint16_t>(type));
            if (it == s_typeDefaults.end()) {
                Game::AttributeMap defaults;
                if (std::unique_ptr<Game::Mob> fresh = MakeMobForLoad(type, t.living->Level())) {
                    defaults = fresh->Attributes();
                }
                it = s_typeDefaults.emplace(static_cast<uint16_t>(type), std::move(defaults)).first;
            }
            if (const Game::AttributeInstance* supplied = it->second.Find(attribute)) return supplied->GetBaseValue();
            return row ? row->GetDefaultValue() : Game::GetAttributeDef(attribute).defaultValue;
        }

        // MC LivingEntity.onAttributeUpdated (and, for a player, the
        // attribute sync): health and absorption follow a lowered maximum,
        // SCALE resizes the body, and the change reaches the clients.
        void AfterChange(const Target& t, Game::Attribute attribute) {
            if (t.player) {
                if (attribute == Game::Attribute::MaxHealth || attribute == Game::Attribute::MaxAbsorption) {
                    t.player->clampToEffectMaxima();
                }
                t.player->syncAttributesToClient();
                return;
            }
            t.living->MarkAttributesCustomized();
            t.living->RefreshAttributeScale();
            if (attribute == Game::Attribute::MaxHealth) {
                const float maxHealth = t.living->GetMaxHealth();
                if (t.living->GetHealth() > maxHealth) t.living->SetHealth(maxHealth);
            } else if (attribute == Game::Attribute::MaxAbsorption) {
                const float maxAbsorption = t.living->GetMaxAbsorption();
                if (t.living->GetAbsorptionAmount() > maxAbsorption) t.living->SetAbsorptionAmount(maxAbsorption);
            }
        }

        // IdentifierArgument.id: "ns:path" or "path" (minecraft:), MC's
        // character rules.
        bool ParseModifierId(const std::string& text, std::string& out, std::string& error) {
            if (!Game::IsValidIdentifier(text)) {
                error = "Invalid ID: " + text;
                return false;
            }
            out = Game::NormalizeIdentifier(text);
            return true;
        }

        bool EqualsIgnoreCase(const std::string& a, const char* b) {
            size_t i = 0;
            for (; i < a.size() && b[i]; ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
                    return false;
                }
            }
            return i == a.size() && b[i] == '\0';
        }

        void SendUsage(ServerConnection& connection) {
            SendFailure(connection, "Usage: /attribute <target> <attribute> (get [<scale>] | base (get [<scale>] | "
                                    "set <value> | reset) | modifier (add <id> <value> <operation> | remove <id> | "
                                    "value get <id> [<scale>]))");
        }

    } // namespace

    void AttributeCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        auto scale = [] { return Cmd::Argument("scale", Cmd::Arg::Float).Suggests({"1", "10", "100"}).Executes(); };
        auto modifierId = [] { return Cmd::Argument("id", Cmd::Arg::AttributeModifierId); };

        Cmd::Node attribute = Cmd::Argument("attribute", Cmd::Arg::Attribute);
        attribute.Then(Cmd::Literal("get").Executes().Then(scale()));
        attribute.Then(Cmd::Literal("base")
            .Then(Cmd::Literal("set")
                .Then(Cmd::Argument("value", Cmd::Arg::AttributeValue).Executes()))
            .Then(Cmd::Literal("get").Executes().Then(scale()))
            .Then(Cmd::Literal("reset").Executes()));
        attribute.Then(Cmd::Literal("modifier")
            .Then(Cmd::Literal("add")
                // IdentifierArgument.id(): a new id — an example, nothing to
                // complete from.
                .Then(Cmd::Argument("id", Cmd::Arg::Word).Suggests({"custom:boost"})
                    .Then(Cmd::Argument("value", Cmd::Arg::Float).Suggests({"0.5", "1", "-0.5"})
                        .Then(Cmd::Literals({"add_value", "add_multiplied_base", "add_multiplied_total"})))))
            .Then(Cmd::Literal("remove")
                .Then(modifierId().Executes()))
            .Then(Cmd::Literal("value")
                .Then(Cmd::Literal("get")
                    .Then(modifierId().Executes().Then(scale())))));

        dispatcher.RegisterCommand("attribute", AttributeCommand::Execute,
            Cmd::Root().Then(Cmd::Argument("target", Cmd::Arg::Entity).Then(std::move(attribute))));
    }

    void AttributeCommand::Execute(const CommandSourceStack& source,
                                   const std::vector<std::string>& args,
                                   ServerConnection& connection,
                                   PlayerSessionManager& /*sessionManager*/) {
        if (args.size() < 3) { SendUsage(connection); return; }

        // <target>: EntityArgument.entity() — exactly one.
        std::vector<SelectedEntity> selected;
        std::string error;
        if (!ResolveSelector(args[0], SelectorKind::Entity, source, selected, error)) {
            SendFailure(connection, error);
            return;
        }
        const SelectedEntity& entity = selected.front();

        // <attribute>: ResourceArgument<attribute>.
        Game::Attribute attribute{};
        if (!Game::AttributeFromName(args[1], attribute)) {
            SendFailure(connection, "Unknown attribute '" + Game::NormalizeIdentifier(args[1]) + "'");
            return;
        }

        // getLivingEntity: only a LivingEntity has attributes.
        Target target;
        target.name = DisplayName(entity);
        if (entity.kind == SelectedEntity::Kind::Player) {
            target.player = entity.player;
        } else if (entity.kind == SelectedEntity::Kind::Mob && entity.mob) {
            target.living = entity.mob->AsLiving();
        }
        if (!target.player && !target.living) {
            SendFailure(connection, Format(kNotLiving, {target.name}));
            return;
        }
        const std::string attributeName = AttributeDisplay(attribute);
        Game::AttributeMap& own = OwnMap(target);
        // getEntityWithAttribute / getAttributeInstance: the type must carry
        // the attribute (a pig has no attack_damage, a player no
        // follow_range).
        if (!own.Has(attribute)) {
            SendFailure(connection, Format(kNoAttribute, {target.name, attributeName}));
            return;
        }

        const std::string& verb = args[2];
        // An optional trailing <scale> (MC's result multiplier for /execute
        // store — validated, the printed value is unscaled as in MC).
        auto checkScale = [&](size_t index) {
            if (args.size() <= index) return true;
            double scale = 0.0;
            if (args.size() > index + 1 || !ParseDouble(args[index], scale)) {
                SendFailure(connection, "Expected a number for <scale>, got '" + args[index] + "'");
                return false;
            }
            return true;
        };

        // ── get [<scale>] ──────────────────────────────────────────────────
        if (EqualsIgnoreCase(verb, "get")) {
            if (!checkScale(3)) return;
            const double value = FullInstance(target, attribute).GetValue();
            source.SendSuccess(connection, Format(kValueGet, {attributeName, target.name, JavaDouble(value)}), /*broadcast=*/false);
            return;
        }

        // ── base get|set|reset ─────────────────────────────────────────────
        if (EqualsIgnoreCase(verb, "base")) {
            if (args.size() < 4) { SendUsage(connection); return; }
            const std::string& action = args[3];
            Game::AttributeInstance* row = own.Find(attribute);
            if (EqualsIgnoreCase(action, "get")) {
                if (!checkScale(4)) return;
                source.SendSuccess(connection, Format(kBaseGet, {attributeName, target.name, JavaDouble(row->GetBaseValue())}), /*broadcast=*/false);
                return;
            }
            if (EqualsIgnoreCase(action, "set")) {
                double value = 0.0;
                if (args.size() != 5 || !ParseDouble(args[4], value)) {
                    SendFailure(connection, args.size() < 5 ? "Expected a value for <value>"
                                                            : "Expected a number for <value>, got '" + args[4] + "'");
                    return;
                }
                row->SetBaseValue(value);
                AfterChange(target, attribute);
                source.SendSuccess(connection, Format(kBaseSet, {attributeName, target.name, JavaDouble(value)}), /*broadcast=*/false);
                return;
            }
            if (EqualsIgnoreCase(action, "reset")) {
                if (args.size() != 4) { SendUsage(connection); return; }
                const double value = SuppliedDefault(target, attribute);
                row->SetBaseValue(value);
                AfterChange(target, attribute);
                source.SendSuccess(connection, Format(kBaseReset, {attributeName, target.name, JavaDouble(value)}), /*broadcast=*/false);
                return;
            }
            SendUsage(connection);
            return;
        }

        // ── modifier add|remove|value get ──────────────────────────────────
        if (EqualsIgnoreCase(verb, "modifier")) {
            if (args.size() < 5) { SendUsage(connection); return; }
            const std::string& action = args[3];

            if (EqualsIgnoreCase(action, "add")) {
                if (args.size() != 7) { SendUsage(connection); return; }
                std::string id;
                if (!ParseModifierId(args[4], id, error)) { SendFailure(connection, error); return; }
                double value = 0.0;
                if (!ParseDouble(args[5], value)) {
                    SendFailure(connection, "Expected a number for <value>, got '" + args[5] + "'");
                    return;
                }
                Game::AttributeOperation operation{};
                std::string opText = args[6];
                for (char& c : opText) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (opText != "add_value" && opText != "add_multiplied_base" && opText != "add_multiplied_total") {
                    SendFailure(connection, "Unknown operation '" + args[6] +
                                            "' (add_value, add_multiplied_base or add_multiplied_total)");
                    return;
                }
                Game::AttributeOperationFromName(opText, operation);
                const Game::ModifierId modifierId = Game::NamedModifierId(id);
                // attributeInstance.hasModifier(id): anything on the whole
                // instance — a worn item's modifier of that id included.
                if (FullInstance(target, attribute).HasModifier(modifierId)) {
                    SendFailure(connection, Format(kModifierExists, {id, attributeName, target.name}));
                    return;
                }
                // addPermanentModifier: saved with the entity.
                Game::AttributeModifier modifier;
                modifier.id = static_cast<uint32_t>(modifierId);
                modifier.amount = value;
                modifier.operation = operation;
                modifier.permanent = true;
                own.AddModifier(attribute, modifier);
                AfterChange(target, attribute);
                source.SendSuccess(connection, Format(kModifierAdd, {id, attributeName, target.name}), /*broadcast=*/false);
                return;
            }

            if (EqualsIgnoreCase(action, "remove")) {
                if (args.size() != 5) { SendUsage(connection); return; }
                std::string id;
                if (!ParseModifierId(args[4], id, error)) { SendFailure(connection, error); return; }
                const Game::ModifierId modifierId = Game::NamedModifierId(id);
                // removeModifier: what the entity's own map holds (a worn
                // item's modifier comes back with the item, so it is not
                // removable here).
                if (!own.HasModifier(attribute, modifierId)) {
                    SendFailure(connection, Format(kNoModifier, {attributeName, target.name, id}));
                    return;
                }
                own.RemoveModifier(attribute, modifierId);
                AfterChange(target, attribute);
                source.SendSuccess(connection, Format(kModifierRemove, {id, attributeName, target.name}), /*broadcast=*/false);
                return;
            }

            if (EqualsIgnoreCase(action, "value")) {
                if (args.size() < 6 || !EqualsIgnoreCase(args[4], "get")) { SendUsage(connection); return; }
                std::string id;
                if (!ParseModifierId(args[5], id, error)) { SendFailure(connection, error); return; }
                if (!checkScale(6)) return;
                const Game::AttributeInstance instance = FullInstance(target, attribute);
                const Game::AttributeModifier* modifier =
                    instance.FindModifier(static_cast<uint32_t>(Game::NamedModifierId(id)));
                if (!modifier) {
                    SendFailure(connection, Format(kNoModifier, {attributeName, target.name, id}));
                    return;
                }
                source.SendSuccess(connection, Format(kModifierGet, {id, attributeName, target.name, JavaDouble(modifier->amount)}), /*broadcast=*/false);
                return;
            }
        }

        SendUsage(connection);
    }

} // namespace Server
