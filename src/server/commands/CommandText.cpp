// File: src/server/commands/CommandText.cpp
#include "CommandText.hpp"
#include "CommandCoords.hpp"
#include "CommandStorage.hpp"
#include "EntitySelector.hpp"
#include "NbtPath.hpp"
#include "SnbtParser.hpp"
#include "../IntegratedServer.hpp"
#include "../level/ServerLevel.hpp"
#include "../network/ServerConnection.hpp"
#include "../session/PlayerSession.hpp"
#include "../session/PlayerSessionManager.hpp"
#include "../player/ServerPlayer.hpp"
#include "../world/storage/anvil/BlockEntityNbt.hpp"
#include "../world/storage/anvil/ItemStackNbt.hpp"
#include "common/entity/Mob.hpp"
#include "common/nbt/NbtWrite.hpp"
#include "common/text/Language.hpp"
#include "common/world/block/entity/BlockEntity.hpp"
#include "common/world/level/World.hpp"
#include "common/world/math/WorldCoordinates.hpp"

#include <cctype>
#include <climits>
#include <cstdlib>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>

namespace Server::CommandText {

    namespace {

        using Game::Text::Component;
        using Game::Text::Style;

        // MC ResolutionContext's default depth limit.
        constexpr int kMaxResolveDepth = 100;

        // MC ComponentUtils.DEFAULT_SEPARATOR (", " in grey) and
        // DEFAULT_NO_STYLE_SEPARATOR (", " plain).
        Component GreySeparator() {
            Component c = Component::Literal(", ");
            c.style.color = Game::Text::TextColor::FromFormatting(static_cast<int>(Game::Text::Formatting::Gray));
            return c;
        }

        // MC ComponentUtils.formatList(values, separator).
        Component FormatList(const std::vector<Component>& values, const Component& separator) {
            if (values.empty()) return Component::Empty();
            if (values.size() == 1) return values.front();
            Component out = Component::Empty();
            for (size_t i = 0; i < values.size(); ++i) {
                if (i > 0) out.Append(separator);
                out.Append(values[i]);
            }
            return out;
        }

        // A block entity's saved compound (the /data block accessor's read).
        std::shared_ptr<::World::NBTTagCompound> BlockEntityData(Game::World& world, const glm::ivec3& pos) {
            Game::BlockEntity* be = world.GetBlockEntity(pos);
            if (!be) return nullptr;
            const auto chunk = Game::Math::WorldCoordinates::WorldToChunkPos(pos.x, pos.z);
            Game::Nbt::Writer w;
            w.BeginRootCompound();
            auto list = w.BeginList("E", Game::Nbt::TagType::Compound);
            const bool ok = Game::Anvil::WriteBlockEntity(w, list, *be, chunk);
            w.EndList(list);
            w.EndRootCompound();
            if (!ok || !w.ok()) return nullptr;
            std::shared_ptr<::World::NBTTagCompound> root;
            try {
                root = std::dynamic_pointer_cast<::World::NBTTagCompound>(::World::NBTParser::Parse(w.Bytes()));
            } catch (const std::exception&) {
                return nullptr;
            }
            auto elements = root ? std::dynamic_pointer_cast<::World::NBTTagList>(root->GetTag("E")) : nullptr;
            if (!elements || elements->value.empty()) return nullptr;
            return std::dynamic_pointer_cast<::World::NBTTagCompound>(elements->value.front());
        }

        // MC NbtContents.dataSource.getData(source): the compounds the nbt
        // contents read from — every entity the selector matches, the block
        // entity at the position, or the storage.
        std::vector<::World::NBTTagPtr> NbtSources(const Component& c, const CommandSourceStack& source) {
            std::vector<::World::NBTTagPtr> out;
            if (c.sourceKind == "entity") {
                std::vector<SelectedEntity> found;
                std::string error;
                if (!ResolveSelector(c.source, SelectorKind::OptionalEntities, source, found, error)) return out;
                for (const SelectedEntity& e : found) {
                    if (auto data = SavedEntityNbt(e)) out.push_back(data);
                }
            } else if (c.sourceKind == "block") {
                std::istringstream in(c.source);
                std::string x, y, z;
                if (!(in >> x >> y >> z)) return out;
                glm::ivec3 pos;
                std::string error;
                if (!ParseBlockPos(x, y, z, source, source.rotation, pos, error)) return out;
                ServerLevel* level = g_integratedServer ? g_integratedServer->GetLevel(source.dimension) : nullptr;
                Game::World* world = level ? level->World() : nullptr;
                if (!world || !world->IsChunkLoaded(pos.x >> 4, pos.z >> 4)) return out;
                if (auto data = BlockEntityData(*world, pos)) out.push_back(data);
            } else if (c.sourceKind == "storage") {
                std::string id;
                if (CommandStorage::NormalizeId(c.source, id)) out.push_back(CommandStorage::Get(id));
            }
            return out;
        }

        Component ResolveImpl(const Component& c, const CommandSourceStack& source, int depth);

        std::optional<Component> ResolveSeparator(const Component& c, const CommandSourceStack& source, int depth) {
            if (c.separator.empty()) return std::nullopt;
            return ResolveImpl(c.separator.front(), source, depth);
        }

        // MC ComponentContents.resolve for each kind.
        Component ResolveContents(const Component& c, const CommandSourceStack& source, int depth) {
            switch (c.kind) {
                case Component::Kind::Selector: {
                    // SelectorContents.resolve: formatList(findEntities,
                    // separator or the grey ", ", Entity::getDisplayName).
                    std::vector<SelectedEntity> found;
                    std::string error;
                    std::vector<Component> names;
                    if (ResolveSelector(c.text, SelectorKind::OptionalEntities, source, found, error)) {
                        for (const SelectedEntity& e : found) names.push_back(DisplayName(e));
                    }
                    const std::optional<Component> separator = ResolveSeparator(c, source, depth);
                    return FormatList(names, separator ? *separator : GreySeparator());
                }
                case Component::Kind::Score:
                    // ScoreContents: no scoreboard in this engine, so no
                    // objective exists and MC's answer is empty text.
                    return Component::Empty();
                case Component::Kind::Nbt: {
                    // NbtContents.resolve: every tag the path finds in every
                    // source — interpreted as a component, or printed as
                    // SNBT (plain, or with NbtUtils' colours).
                    Server::Nbt::Path path;
                    std::string error;
                    if (!Server::Nbt::Path::Parse(c.text, path, error)) return Component::Empty();
                    std::vector<::World::NBTTagPtr> tags;
                    for (const ::World::NBTTagPtr& root : NbtSources(c, source)) {
                        std::vector<::World::NBTTagPtr> found;
                        if (path.Get(root, found, error)) tags.insert(tags.end(), found.begin(), found.end());
                    }
                    const std::optional<Component> separator = ResolveSeparator(c, source, depth);
                    std::vector<Component> parts;
                    for (const ::World::NBTTagPtr& tag : tags) {
                        if (!tag) continue;
                        if (c.interpret) {
                            if (auto parsed = Game::Anvil::ReadTextComponent(*tag)) {
                                parts.push_back(ResolveImpl(*parsed, source, depth + 1));
                            }
                        } else if (c.plain) {
                            parts.push_back(Component::Literal(Server::Nbt::ToSnbt(*tag)));
                        } else {
                            std::vector<Network::ChatSegmentData> pretty;
                            Server::Nbt::AppendPretty(*tag, pretty);
                            Component rich = Component::Empty();
                            for (const Network::ChatSegmentData& s : pretty) {
                                Component run = Component::Literal(s.text);
                                if ((s.color & 0xFFFFFFu) != 0xFFFFFFu) {
                                    run.style.color = Game::Text::TextColor::FromRgb(s.color & 0xFFFFFFu);
                                }
                                rich.Append(std::move(run));
                            }
                            parts.push_back(std::move(rich));
                        }
                    }
                    return FormatList(parts, separator ? *separator : Component::Literal(", "));
                }
                case Component::Kind::Translatable: {
                    Component out = c;
                    out.extra.clear();
                    for (Component& arg : out.args) arg = ResolveImpl(arg, source, depth + 1);
                    return out;
                }
                default: {
                    Component out = c;
                    out.extra.clear();
                    return out;
                }
            }
        }

        // MC ComponentUtils.resolve(context, component, depth).
        Component ResolveImpl(const Component& c, const CommandSourceStack& source, int depth) {
            if (depth > kMaxResolveDepth) return c;
            Component out = ResolveContents(c, source, depth + 1);
            for (const Component& sibling : c.extra) out.Append(ResolveImpl(sibling, source, depth + 1));
            // withStyle(resolveStyle): the component's own style over what
            // the contents produced, its show_text hover resolved too.
            Style style = c.style;
            if (style.hoverText) {
                style.hoverText = std::make_shared<const Component>(ResolveImpl(*style.hoverText, source, depth + 1));
            }
            out.style = style.ApplyTo(out.style);
            return out;
        }

        // The font's '§' style codes for a style (colour excluded).
        std::string StyleCodes(const Style& style) {
            std::string codes;
            if (style.IsObfuscated())    codes += "\xC2\xA7k";
            if (style.IsBold())          codes += "\xC2\xA7l";
            if (style.IsStrikethrough()) codes += "\xC2\xA7m";
            if (style.IsUnderlined())    codes += "\xC2\xA7n";
            if (style.IsItalic())        codes += "\xC2\xA7o";
            return codes;
        }

        // The named colour nearest an RGB value (squared distance).
        int NearestNamedColor(uint32_t rgb) {
            int best = 15;
            long bestDistance = LONG_MAX;
            for (int i = 0; i < Game::Text::kFormattingColorCount; ++i) {
                const uint32_t c = Game::Text::FormattingRgb(i);
                const long dr = static_cast<long>((c >> 16) & 0xFF) - static_cast<long>((rgb >> 16) & 0xFF);
                const long dg = static_cast<long>((c >> 8) & 0xFF) - static_cast<long>((rgb >> 8) & 0xFF);
                const long db = static_cast<long>(c & 0xFF) - static_cast<long>(rgb & 0xFF);
                const long d = dr * dr + dg * dg + db * db;
                if (d < bestDistance) { bestDistance = d; best = i; }
            }
            return best;
        }

        bool IsSelectorStart(const std::string& text, size_t i) {
            if (text[i] != '@' || i + 1 >= text.size()) return false;
            const char kind = text[i + 1];
            if (kind != 'p' && kind != 'a' && kind != 'r' && kind != 's' && kind != 'e' && kind != 'n') return false;
            // "@a" must end there or open its options: "@ab" is a word.
            if (i + 2 < text.size()) {
                const char next = text[i + 2];
                if (next != '[' && (std::isalnum(static_cast<unsigned char>(next)) || next == '_')) return false;
            }
            return true;
        }

    } // namespace

    std::string Join(const std::vector<std::string>& args, size_t from) {
        std::string out;
        for (size_t i = from; i < args.size(); ++i) {
            if (i > from) out += ' ';
            out += args[i];
        }
        return out;
    }

    bool ParseComponent(const std::string& text, Game::Text::Component& out, std::string& error) {
        std::string parseError;
        const ::World::NBTTagPtr tag = Snbt::ParseValue(text, parseError);
        if (!tag) {
            error = "Invalid chat component: " + parseError;
            return false;
        }
        std::optional<Game::Text::Component> component = Game::Anvil::ReadTextComponent(*tag);
        if (!component) {
            error = "Invalid chat component: not a text component: " + text;
            return false;
        }
        out = std::move(*component);
        return true;
    }

    Game::Text::Component Resolve(const Game::Text::Component& component,
                                  const CommandSourceStack& source,
                                  const SelectedEntity* self) {
        if (self) return ResolveImpl(component, source.WithEntity(*self), 0);
        return ResolveImpl(component, source, 0);
    }

    std::vector<Network::ChatSegmentData> ToSegments(const Game::Text::Component& component,
                                                    uint32_t baseColor, const std::string& baseStyleCodes) {
        std::vector<Network::ChatSegmentData> out;
        Game::Text::Visit(component, Game::Text::Style{},
                          [&](const Game::Text::Style& style, std::string_view text) {
                              if (text.empty()) return true;
                              Network::ChatSegmentData seg;
                              seg.text  = baseStyleCodes + StyleCodes(style) + std::string(text);
                              seg.color = style.color ? (0xFF000000u | style.color->rgb) : baseColor;
                              if (style.clickEvent) {
                                  using Action = Game::Text::ClickEvent::Action;
                                  switch (style.clickEvent->action) {
                                      case Action::RunCommand:
                                          seg.click = Network::ChatClickAction::RunCommand;
                                          seg.clickValue = style.clickEvent->value;
                                          break;
                                      case Action::SuggestCommand:
                                          seg.click = Network::ChatClickAction::SuggestCommand;
                                          seg.clickValue = style.clickEvent->value;
                                          break;
                                      case Action::CopyToClipboard:
                                          seg.click = Network::ChatClickAction::CopyToClipboard;
                                          seg.clickValue = style.clickEvent->value;
                                          break;
                                      default:
                                          // open_url / open_file / change_page /
                                          // show_dialog / custom: no chat-side
                                          // action in this engine's chat.
                                          break;
                                  }
                              }
                              if (style.hoverText) seg.hoverText = Game::Text::GetString(*style.hoverText);
                              out.push_back(std::move(seg));
                              return true;
                          });
        if (out.empty()) {
            Network::ChatSegmentData empty;
            empty.color = baseColor;
            out.push_back(std::move(empty));
        }
        return out;
    }

    std::string ToLegacyString(const Game::Text::Component& component) {
        static const char kCodes[] = "0123456789abcdef";
        std::string out;
        Game::Text::Visit(component, Game::Text::Style{},
                          [&out](const Game::Text::Style& style, std::string_view text) {
                              if (text.empty()) return true;
                              out += "\xC2\xA7r";
                              if (style.color) {
                                  const int index = style.color->named >= 0 ? style.color->named
                                                                            : NearestNamedColor(style.color->rgb);
                                  out += "\xC2\xA7";
                                  out += kCodes[index & 15];
                              }
                              out += StyleCodes(style);
                              out += text;
                              return true;
                          });
        return out;
    }

    Game::Text::Component DisplayName(const SelectedEntity& entity) {
        if (entity.kind == SelectedEntity::Kind::Player) {
            // Player.getDisplayName → decorateDisplayNameComponent: the name
            // with a "/tell <name> " suggestion and its insertion.
            Component name = Component::Literal(entity.name);
            Game::Text::ClickEvent click;
            click.action = Game::Text::ClickEvent::Action::SuggestCommand;
            click.value = "/tell " + entity.name + " ";
            name.style.clickEvent = click;
            name.style.insertion = entity.name;
            return name;
        }
        if (entity.mob && entity.mob->HasCustomName()) return Component::Literal(*entity.mob->GetCustomName());
        return Component::Translatable("entity.minecraft." + entity.typeSlug);
    }

    std::string DisplayNameString(const SelectedEntity& entity) {
        return Game::Text::GetString(DisplayName(entity));
    }

    std::string ResolveMessage(const std::string& text, const CommandSourceStack& source, bool allowSelectors) {
        if (!allowSelectors || text.find('@') == std::string::npos) return text;
        std::string out;
        size_t i = 0;
        while (i < text.size()) {
            if (!IsSelectorStart(text, i)) {
                out += text[i++];
                continue;
            }
            // The selector's extent: "@x", plus a bracketed option list
            // (quotes and nested brackets respected).
            size_t end = i + 2;
            if (end < text.size() && text[end] == '[') {
                int depth = 0;
                char quote = 0;
                for (; end < text.size(); ++end) {
                    const char c = text[end];
                    if (quote) {
                        if (c == '\\') { ++end; continue; }
                        if (c == quote) quote = 0;
                        continue;
                    }
                    if (c == '"' || c == '\'') { quote = c; continue; }
                    if (c == '[' || c == '{') ++depth;
                    else if ((c == ']' || c == '}') && --depth == 0) { ++end; break; }
                }
            }
            const std::string selector = text.substr(i, end - i);
            std::vector<SelectedEntity> found;
            std::string error;
            if (ResolveSelector(selector, SelectorKind::OptionalEntities, source, found, error)) {
                // MessageArgument.Part.toComponent: formatList of the
                // matched entities' display names.
                std::vector<Component> names;
                for (const SelectedEntity& e : found) names.push_back(DisplayName(e));
                out += Game::Text::GetString(FormatList(names, Component::Literal(", ")));
            } else {
                out += selector;   // not a selector after all: the text stays
            }
            i = end;
        }
        return out;
    }

    bool HasGamemasterPermission(const ServerConnection& connection) {
        if (!g_integratedServer || !g_integratedServer->IsAllowCommands()) return false;
        return connection.IsSingleplayerOwner() || g_integratedServer->GetGuestCommandAccess();
    }

    void SendFailure(ServerConnection& connection, const std::string& text) {
        // MC sendFailure: ChatFormatting.RED.
        Network::ChatMessageS2CPacket packet;
        packet.senderId = 0;
        packet.position = 1;
        packet.segments.push_back(Network::ChatSegmentData{text, 0xFFFF5555u, Network::ChatClickAction::None, "", ""});
        connection.SendChatMessage(packet);
    }

    void SendSystem(ServerConnection& connection, std::vector<Network::ChatSegmentData> segments) {
        Network::ChatMessageS2CPacket packet;
        packet.senderId = 0;
        packet.position = 1;
        packet.segments = std::move(segments);
        connection.SendChatMessage(packet);
    }

    void BroadcastSystem(PlayerSessionManager& sessions, const Network::ChatMessageS2CPacket& packet) {
        for (const auto& session : sessions.GetAllSessions()) {
            if (ServerConnection* connection = session ? session->GetConnection() : nullptr) {
                connection->SendChatMessage(packet);
            }
        }
    }

} // namespace Server::CommandText
