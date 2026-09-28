// File: src/server/commands/ParticleCommand.cpp
#include "ParticleCommand.hpp"
#include "BlockStateArgument.hpp"
#include "CommandCoords.hpp"
#include "EntitySelector.hpp"
#include "../network/ServerConnection.hpp"
#include "../player/ServerPlayer.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../world/storage/anvil/ItemStackNbt.hpp"
#include "common/network/PacketRegistry.hpp"
#include "common/network/packets/game/LevelParticlePackets.hpp"
#include "common/particle/ParticleOptions.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Server {

    namespace {

        // ── A minimal SNBT reader (MC TagParser), enough for particle options
        struct Snbt {
            enum class Type : uint8_t { Compound, List, Number, String };
            Type type = Type::String;
            double number = 0.0;
            std::string text;
            std::map<std::string, Snbt> compound;
            std::vector<Snbt> list;

            const Snbt* Get(const std::string& key) const {
                auto it = compound.find(key);
                return it == compound.end() ? nullptr : &it->second;
            }
        };

        class SnbtReader {
        public:
            explicit SnbtReader(const std::string& s) : m_s(s) {}

            bool Read(Snbt& out, std::string& error) {
                if (!Value(out, error)) return false;
                SkipWs();
                if (m_i != m_s.size()) { error = "Unexpected trailing data at position " + std::to_string(m_i); return false; }
                return true;
            }

        private:
            void SkipWs() { while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i]))) ++m_i; }

            static bool IsUnquotedChar(char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '+' || c == ':';
            }

            bool QuotedString(std::string& out, std::string& error) {
                const char quote = m_s[m_i++];
                while (m_i < m_s.size()) {
                    const char c = m_s[m_i++];
                    if (c == '\\' && m_i < m_s.size()) { out += m_s[m_i++]; continue; }
                    if (c == quote) return true;
                    out += c;
                }
                error = "Unterminated string";
                return false;
            }

            bool Key(std::string& out, std::string& error) {
                SkipWs();
                if (m_i < m_s.size() && (m_s[m_i] == '"' || m_s[m_i] == '\'')) return QuotedString(out, error);
                while (m_i < m_s.size() && IsUnquotedChar(m_s[m_i]) && m_s[m_i] != ':') out += m_s[m_i++];
                if (out.empty()) { error = "Expected key at position " + std::to_string(m_i); return false; }
                return true;
            }

            bool Value(Snbt& out, std::string& error) {
                SkipWs();
                if (m_i >= m_s.size()) { error = "Expected value"; return false; }
                const char c = m_s[m_i];
                if (c == '{') {
                    out.type = Snbt::Type::Compound;
                    ++m_i;
                    SkipWs();
                    if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
                    while (true) {
                        std::string key;
                        if (!Key(key, error)) return false;
                        SkipWs();
                        if (m_i >= m_s.size() || m_s[m_i] != ':') { error = "Expected ':' after " + key; return false; }
                        ++m_i;
                        Snbt v;
                        if (!Value(v, error)) return false;
                        out.compound[key] = std::move(v);
                        SkipWs();
                        if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
                        if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
                        error = "Expected ',' or '}'";
                        return false;
                    }
                }
                if (c == '[') {
                    out.type = Snbt::Type::List;
                    ++m_i;
                    SkipWs();
                    // Typed arrays: [I;1,2,3] / [B;..] / [L;..].
                    if (m_i + 1 < m_s.size() && (m_s[m_i] == 'I' || m_s[m_i] == 'B' || m_s[m_i] == 'L') &&
                        m_s[m_i + 1] == ';') {
                        m_i += 2;
                    }
                    SkipWs();
                    if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
                    while (true) {
                        Snbt v;
                        if (!Value(v, error)) return false;
                        out.list.push_back(std::move(v));
                        SkipWs();
                        if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
                        if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
                        error = "Expected ',' or ']'";
                        return false;
                    }
                }
                if (c == '"' || c == '\'') {
                    out.type = Snbt::Type::String;
                    return QuotedString(out.text, error);
                }
                std::string token;
                while (m_i < m_s.size() && IsUnquotedChar(m_s[m_i])) token += m_s[m_i++];
                if (token.empty()) { error = "Expected value at position " + std::to_string(m_i); return false; }
                if (token == "true" || token == "false") {
                    out.type = Snbt::Type::Number;
                    out.number = token == "true" ? 1.0 : 0.0;
                    return true;
                }
                // A number, with MC's type suffixes (b s l f d).
                std::string digits = token;
                const char last = static_cast<char>(std::tolower(static_cast<unsigned char>(digits.back())));
                if (digits.size() > 1 && (last == 'b' || last == 's' || last == 'l' || last == 'f' || last == 'd')) {
                    digits.pop_back();
                }
                char* end = nullptr;
                const double v = std::strtod(digits.c_str(), &end);
                if (end && *end == '\0' && !digits.empty()) {
                    out.type = Snbt::Type::Number;
                    out.number = v;
                } else {
                    out.type = Snbt::Type::String;
                    out.text = token;
                }
                return true;
            }

            const std::string& m_s;
            size_t m_i = 0;
        };

        // ExtraCodecs.RGB_COLOR_CODEC: an int or [r, g, b] floats.
        bool ReadRgb(const Snbt* v, uint32_t& out, std::string& error, const char* field) {
            if (!v) { error = std::string("Missing '") + field + "'"; return false; }
            if (v->type == Snbt::Type::Number) {
                out = static_cast<uint32_t>(static_cast<int64_t>(v->number)) & 0xFFFFFFu;
                return true;
            }
            if (v->type == Snbt::Type::List && v->list.size() == 3) {
                uint32_t rgb = 0;
                for (const Snbt& c : v->list) {
                    if (c.type != Snbt::Type::Number) { error = std::string("Bad '") + field + "'"; return false; }
                    const int b = static_cast<int>(std::lround(std::clamp(c.number, 0.0, 1.0) * 255.0));
                    rgb = (rgb << 8) | static_cast<uint32_t>(b);
                }
                out = rgb;
                return true;
            }
            error = std::string("Bad '") + field + "'";
            return false;
        }

        // ExtraCodecs.ARGB_COLOR_CODEC: an int or [r, g, b, a] floats.
        bool ReadArgb(const Snbt* v, uint32_t& out, std::string& error) {
            if (!v) { error = "Missing 'color'"; return false; }
            if (v->type == Snbt::Type::Number) {
                out = static_cast<uint32_t>(static_cast<int64_t>(v->number));
                return true;
            }
            if (v->type == Snbt::Type::List && (v->list.size() == 4 || v->list.size() == 3)) {
                float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                for (size_t i = 0; i < v->list.size(); ++i) {
                    if (v->list[i].type != Snbt::Type::Number) { error = "Bad 'color'"; return false; }
                    c[i] = static_cast<float>(std::clamp(v->list[i].number, 0.0, 1.0));
                }
                const auto b = [](float f) { return static_cast<uint32_t>(std::lround(f * 255.0f)) & 0xFFu; };
                out = (b(c[3]) << 24) | (b(c[0]) << 16) | (b(c[1]) << 8) | b(c[2]);
                return true;
            }
            error = "Bad 'color'";
            return false;
        }

        bool ReadNumber(const Snbt* v, double& out) {
            if (!v || v->type != Snbt::Type::Number) return false;
            out = v->number;
            return true;
        }

        bool ReadVec3(const Snbt* v, glm::dvec3& out) {
            if (!v || v->type != Snbt::Type::List || v->list.size() != 3) return false;
            for (int i = 0; i < 3; ++i) {
                if (v->list[static_cast<size_t>(i)].type != Snbt::Type::Number) return false;
                out[i] = v->list[static_cast<size_t>(i)].number;
            }
            return true;
        }

        std::string Namespaced(const std::string& id) {
            return id.find(':') == std::string::npos ? "minecraft:" + id : id;
        }

        // BlockParticleOption's block_state: a state string, or a
        // BlockState compound {Name, Properties}.
        bool ReadBlockState(const Snbt* v, Game::BlockState& out, std::string& error) {
            if (!v) { error = "Missing 'block_state'"; return false; }
            std::string text;
            if (v->type == Snbt::Type::String) {
                text = v->text;
            } else if (v->type == Snbt::Type::Compound) {
                const Snbt* name = v->Get("Name");
                if (!name || name->type != Snbt::Type::String) { error = "Missing block Name"; return false; }
                text = name->text;
                if (const Snbt* props = v->Get("Properties"); props && props->type == Snbt::Type::Compound &&
                                                              !props->compound.empty()) {
                    text += "[";
                    bool firstProp = true;
                    for (const auto& [k, pv] : props->compound) {
                        if (!firstProp) text += ",";
                        firstProp = false;
                        text += k + "=" + (pv.type == Snbt::Type::String ? pv.text
                                                                          : std::to_string(static_cast<int64_t>(pv.number)));
                    }
                    text += "]";
                }
            } else {
                error = "Bad 'block_state'";
                return false;
            }
            return ParseBlockState(text, out, error);
        }

        // ParticleArgument.readParticle: the id, then the options.
        bool ParseParticle(const std::string& text, Game::ParticleOptions& out, std::string& error) {
            const size_t brace = text.find('{');
            const std::string id = text.substr(0, brace);
            const std::optional<Game::ParticleKind> kind = Game::ParticleTypes::FromName(id);
            if (!kind || !Game::ParticleTypes::Get(*kind).commandVisible) {
                error = "Unknown particle: " + Namespaced(id);
                return false;
            }
            Snbt options;
            options.type = Snbt::Type::Compound;
            if (brace != std::string::npos) {
                SnbtReader reader(text.substr(brace));
                if (!reader.Read(options, error)) return false;
                if (options.type != Snbt::Type::Compound) { error = "Expected particle options"; return false; }
            }
            using S = Game::ParticleTypes::OptionsShape;
            const Game::ParticleKind k = *kind;
            out = Game::ParticleOptions(k);
            switch (Game::ParticleTypes::Get(k).shape) {
                case S::Simple:
                    return true;
                case S::BlockState: {
                    Game::BlockState state;
                    if (!ReadBlockState(options.Get("block_state"), state, error)) return false;
                    out = Game::ParticleOptions::Block(k, state);
                    return true;
                }
                case S::Item: {
                    const Snbt* item = options.Get("item");
                    std::string itemId;
                    if (item && item->type == Snbt::Type::String) itemId = item->text;
                    else if (item && item->type == Snbt::Type::Compound && item->Get("id") &&
                             item->Get("id")->type == Snbt::Type::String) itemId = item->Get("id")->text;
                    if (itemId.empty()) { error = "Missing 'item'"; return false; }
                    const Game::ItemID idValue = Game::Anvil::ItemFromName(Namespaced(itemId));
                    if (idValue == Game::Items::Air) { error = "Unknown item '" + Namespaced(itemId) + "'"; return false; }
                    out = Game::ParticleOptions::Item(idValue);
                    return true;
                }
                case S::Dust: {
                    uint32_t rgb = 0;
                    double scale = 0.0;
                    if (!ReadRgb(options.Get("color"), rgb, error, "color")) return false;
                    if (!ReadNumber(options.Get("scale"), scale)) { error = "Missing 'scale'"; return false; }
                    out = Game::ParticleOptions::Dust(rgb, static_cast<float>(scale));
                    return true;
                }
                case S::DustTransition: {
                    uint32_t from = 0, to = 0;
                    double scale = 0.0;
                    if (!ReadRgb(options.Get("from_color"), from, error, "from_color")) return false;
                    if (!ReadRgb(options.Get("to_color"), to, error, "to_color")) return false;
                    if (!ReadNumber(options.Get("scale"), scale)) { error = "Missing 'scale'"; return false; }
                    out = Game::ParticleOptions::DustColorTransition(from, to, static_cast<float>(scale));
                    return true;
                }
                case S::Color: {
                    uint32_t argb = 0;
                    if (!ReadArgb(options.Get("color"), argb, error)) return false;
                    out = Game::ParticleOptions::Color(k, argb);
                    return true;
                }
                case S::Spell: {
                    // Both optional: colour -1 (white), power 1.
                    uint32_t rgb = 0xFFFFFF;
                    if (const Snbt* c = options.Get("color")) {
                        if (!ReadRgb(c, rgb, error, "color")) return false;
                    }
                    double power = 1.0;
                    ReadNumber(options.Get("power"), power);
                    out = Game::ParticleOptions::Spell(k, rgb, static_cast<float>(power));
                    return true;
                }
                case S::Power: {
                    double power = 1.0;
                    ReadNumber(options.Get("power"), power);
                    out = Game::ParticleOptions::Power(k, static_cast<float>(power));
                    return true;
                }
                case S::Vibration: {
                    double ticks = 0.0;
                    if (!ReadNumber(options.Get("arrival_in_ticks"), ticks)) { error = "Missing 'arrival_in_ticks'"; return false; }
                    const Snbt* dest = options.Get("destination");
                    if (!dest || dest->type != Snbt::Type::Compound) { error = "Missing 'destination'"; return false; }
                    const Snbt* type = dest->Get("type");
                    const std::string destType = type && type->type == Snbt::Type::String ? Namespaced(type->text) : "";
                    if (destType == "minecraft:block") {
                        glm::dvec3 pos;
                        if (!ReadVec3(dest->Get("pos"), pos)) { error = "Missing destination 'pos'"; return false; }
                        out = Game::ParticleOptions::VibrationToBlock(
                            glm::ivec3(static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)),
                                       static_cast<int>(std::floor(pos.z))),
                            static_cast<int>(ticks));
                        return true;
                    }
                    if (destType == "minecraft:entity") {
                        // Entity sources are addressed by the engine's entity
                        // id (this server has no entity UUIDs to name them by).
                        double entityId = 0.0;
                        if (!ReadNumber(dest->Get("source_entity"), entityId)) {
                            error = "Entity destinations take the entity's numeric id as 'source_entity'";
                            return false;
                        }
                        double yOffset = 0.0;
                        ReadNumber(dest->Get("y_offset"), yOffset);
                        out = Game::ParticleOptions::VibrationToEntity(static_cast<int32_t>(entityId),
                                                                       static_cast<float>(yOffset),
                                                                       static_cast<int>(ticks));
                        return true;
                    }
                    error = "Unknown destination type '" + destType + "'";
                    return false;
                }
                case S::Trail: {
                    glm::dvec3 target;
                    uint32_t rgb = 0;
                    double duration = 0.0;
                    if (!ReadVec3(options.Get("target"), target)) { error = "Missing 'target'"; return false; }
                    if (!ReadRgb(options.Get("color"), rgb, error, "color")) return false;
                    if (!ReadNumber(options.Get("duration"), duration) || duration < 1.0) {
                        error = "'duration' must be a positive integer";
                        return false;
                    }
                    out = Game::ParticleOptions::Trail(target, rgb, static_cast<int>(duration));
                    return true;
                }
                case S::Shriek: {
                    double delay = 0.0;
                    if (!ReadNumber(options.Get("delay"), delay)) { error = "Missing 'delay'"; return false; }
                    out = Game::ParticleOptions::Shriek(static_cast<int>(delay));
                    return true;
                }
                case S::SculkCharge: {
                    double roll = 0.0;
                    if (!ReadNumber(options.Get("roll"), roll)) { error = "Missing 'roll'"; return false; }
                    out = Game::ParticleOptions::SculkCharge(static_cast<float>(roll));
                    return true;
                }
                case S::Geyser: {
                    double water = 0.0;
                    if (!ReadNumber(options.Get("water_blocks"), water) || water < 1.0) {
                        error = "'water_blocks' must be a positive integer";
                        return false;
                    }
                    out = Game::ParticleOptions::Geyser(k, static_cast<int>(water));
                    return true;
                }
                case S::GeyserBase: {
                    double water = 0.0, burst = 0.0;
                    if (!ReadNumber(options.Get("water_blocks"), water) || water < 1.0) {
                        error = "'water_blocks' must be a positive integer";
                        return false;
                    }
                    if (!ReadNumber(options.Get("burst_impulse_base"), burst)) {
                        error = "Missing 'burst_impulse_base'";
                        return false;
                    }
                    out = Game::ParticleOptions::GeyserBase(k, static_cast<int>(water), static_cast<float>(burst));
                    return true;
                }
                case S::FireworkStarter:
                    break;
            }
            error = "Unknown particle: " + Namespaced(id);
            return false;
        }

        // Brigadier FloatArgumentType.floatArg(0) / IntegerArgumentType.integer(0).
        bool ParseNonNegative(const std::string& token, bool integer, double& out, std::string& error) {
            char* end = nullptr;
            const double v = std::strtod(token.c_str(), &end);
            if (!end || *end != '\0' || token.empty()) {
                error = std::string(integer ? "Invalid integer '" : "Invalid float '") + token + "'";
                return false;
            }
            if (integer && std::floor(v) != v) { error = "Invalid integer '" + token + "'"; return false; }
            if (v < 0.0) {
                error = std::string(integer ? "Integer" : "Float") + " must not be less than 0, found " + token;
                return false;
            }
            out = v;
            return true;
        }

        void SendUsage(ServerConnection& connection) {
            connection.SendChatMessage("Unknown or incomplete command, see below for error", 1);
            connection.SendChatMessage(
                "/particle <name> [<pos>] [<delta> <speed> <count> [force|normal] [<viewers>]]", 1);
        }

    } // namespace

    void ParticleCommand::Register(CommandDispatcher& dispatcher) {
        namespace Cmd = Game::Cmd;
        // MC ParticleCommand: <name> [<pos>] [<delta> <speed> <count>
        // [force|normal] [<viewers>]].
        const Cmd::Node viewers = Cmd::Argument("viewers", Cmd::Arg::Players).Executes();
        dispatcher.RegisterCommand("particle", ParticleCommand::Execute,
            Cmd::Root().Then(Cmd::Argument("name", Cmd::Arg::Particle).Executes()
                .Then(Cmd::Argument("pos", Cmd::Arg::Vec3).Executes()
                    .Then(Cmd::Argument("delta", Cmd::Arg::Vec3)
                        .Suggests({"0 0 0", "0.5 0.5 0.5", "1 1 1"})
                        .Then(Cmd::Argument("speed", Cmd::Arg::Float).Suggests({"0", "0.1", "1"})
                            .Then(Cmd::Argument("count", Cmd::Arg::Integer).Suggests({"1", "10", "100"}).Executes()
                                .Then(Cmd::Literals({"force", "normal"}, true, &viewers))))))));
    }

    void ParticleCommand::Execute(const CommandSourceStack& source,
                                  const std::vector<std::string>& rawArgs,
                                  ServerConnection& connection,
                                  PlayerSessionManager& sessionManager) {
        if (rawArgs.empty()) { SendUsage(connection); return; }

        // The dispatcher splits on whitespace; an SNBT option block may hold
        // spaces, so the particle argument runs until its braces balance.
        std::vector<std::string> args;
        {
            std::string particle = rawArgs[0];
            size_t i = 1;
            const auto depth = [](const std::string& s) {
                int d = 0;
                bool quoted = false;
                char quote = 0;
                for (char c : s) {
                    if (quoted) { if (c == quote) quoted = false; continue; }
                    if (c == '"' || c == '\'') { quoted = true; quote = c; continue; }
                    if (c == '{' || c == '[') ++d;
                    if (c == '}' || c == ']') --d;
                }
                return d;
            };
            while (depth(particle) > 0 && i < rawArgs.size()) particle += " " + rawArgs[i++];
            args.push_back(particle);
            for (; i < rawArgs.size(); ++i) args.push_back(rawArgs[i]);
        }

        std::string error;
        Game::ParticleOptions options;
        if (!ParseParticle(args[0], options, error)) {
            connection.SendChatMessage(error, 1);
            return;
        }

        // [<pos>] — Vec3Argument (centre-corrected), default the source's
        // position.
        glm::dvec3 pos = source.position;
        size_t next = 1;
        if (args.size() >= 4) {
            if (!ParseVec3(args[1], args[2], args[3], source, source.rotation, pos, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
            next = 4;
        } else if (args.size() > 1) {
            SendUsage(connection);
            return;
        }

        // [<delta> <speed> <count> ...] — Vec3Argument.vec3(false) for delta.
        glm::dvec3 delta(0.0);
        double speed = 0.0, count = 0.0;
        bool force = false;
        std::vector<SelectedEntity> viewers;
        bool explicitViewers = false;
        if (args.size() > next) {
            if (args.size() < next + 5) { SendUsage(connection); return; }
            CommandSourceStack deltaSource = source;
            deltaSource.position = glm::dvec3(0.0);
            // Relative deltas measure from zero; literal numbers are taken as
            // written (no centre correction for vec3(false)).
            const auto coord = [&](const std::string& a, double& out) {
                if (!a.empty() && (a[0] == '~' || a[0] == '^')) return ParseCoord(a, 0.0, false, out);
                char* end = nullptr;
                out = std::strtod(a.c_str(), &end);
                return end && *end == '\0' && !a.empty();
            };
            if (!coord(args[next], delta.x) || !coord(args[next + 1], delta.y) || !coord(args[next + 2], delta.z)) {
                connection.SendChatMessage("Invalid delta: " + args[next] + " " + args[next + 1] + " " + args[next + 2], 1);
                return;
            }
            if (!ParseNonNegative(args[next + 3], false, speed, error) ||
                !ParseNonNegative(args[next + 4], true, count, error)) {
                connection.SendChatMessage(error, 1);
                return;
            }
            next += 5;
            if (args.size() > next) {
                if (args[next] == "force") force = true;
                else if (args[next] != "normal") {
                    connection.SendChatMessage("Incorrect argument for command", 1);
                    return;
                }
                ++next;
            }
            if (args.size() > next) {
                if (!ResolveSelector(args[next], SelectorKind::Players, source, viewers, error)) {
                    connection.SendChatMessage(error, 1);
                    return;
                }
                explicitViewers = true;
                ++next;
            }
            if (args.size() > next) {
                connection.SendChatMessage("Incorrect argument for command", 1);
                return;
            }
        }

        // The packet: ClientboundLevelParticlesPacket(particle, force, false,
        // pos, delta, speed, count).
        Network::LevelParticlesS2CPacket packet;
        packet.options = options;
        packet.overrideLimiter = force;
        packet.alwaysShow = false;
        packet.pos = pos;
        packet.dist = glm::vec3(delta);
        packet.maxSpeed = glm::vec3(static_cast<float>(speed));
        packet.count = static_cast<int32_t>(count);
        const std::vector<uint8_t> payload = Network::Serialization::Serialize(packet);

        // Default viewers: source.getLevel().players().
        std::vector<std::shared_ptr<PlayerSession>> targets;
        if (explicitViewers) {
            for (const SelectedEntity& v : viewers) if (v.session) targets.push_back(v.session);
        } else {
            for (const auto& session : sessionManager.GetAllSessions()) if (session) targets.push_back(session);
        }

        const double range = force ? 512.0 : 32.0;
        int shown = 0;
        for (const auto& session : targets) {
            const ServerPlayer* player = session->GetPlayer();
            ServerConnection* conn = session->GetConnection();
            if (!player || !conn) continue;
            // ServerLevel.sendParticles(player, …): the player must be in
            // this level, and within range of their block centre.
            if (Game::DimensionFromRaw(player->getDimensionId()) != source.dimension) continue;
            const glm::dvec3 feet = player->getPosition();
            const glm::dvec3 centre(std::floor(feet.x) + 0.5, std::floor(feet.y) + 0.5, std::floor(feet.z) + 0.5);
            const glm::dvec3 d = pos - centre;
            if (d.x * d.x + d.y * d.y + d.z * d.z >= range * range) continue;
            conn->SendPacketIn(source.dimension, static_cast<uint8_t>(Network::PacketId::LevelParticlesS2C), payload);
            ++shown;
        }

        if (shown == 0) {
            connection.SendChatMessage("The particle was not visible for anybody", 1);
            return;
        }
        connection.SendChatMessage("Displaying particle " + std::string(Game::ParticleTypes::Name(options.kind)), 1);
    }

} // namespace Server
