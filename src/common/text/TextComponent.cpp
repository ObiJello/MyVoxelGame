// File: src/common/text/TextComponent.cpp
#include "common/text/TextComponent.hpp"

#include "common/network/PacketRegistry.hpp"
#include "common/text/Language.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace Game::Text {

    namespace {

        // MC ChatFormatting, colour rows: legacy code, name, 0xRRGGBB.
        struct FormattingRow { char code; const char* name; uint32_t rgb; };
        constexpr FormattingRow kColors[kFormattingColorCount] = {
            {'0', "black",        0x000000}, {'1', "dark_blue",    0x0000AA},
            {'2', "dark_green",   0x00AA00}, {'3', "dark_aqua",    0x00AAAA},
            {'4', "dark_red",     0xAA0000}, {'5', "dark_purple",  0xAA00AA},
            {'6', "gold",         0xFFAA00}, {'7', "gray",         0xAAAAAA},
            {'8', "dark_gray",    0x555555}, {'9', "blue",         0x5555FF},
            {'a', "green",        0x55FF55}, {'b', "aqua",         0x55FFFF},
            {'c', "red",          0xFF5555}, {'d', "light_purple", 0xFF55FF},
            {'e', "yellow",       0xFFFF55}, {'f', "white",        0xFFFFFF},
        };

        // MC's NBT reader depth limit (NbtAccounter.DEFAULT_MAX_DEPTH); the
        // same bound keeps every recursive walk here off the stack's edge.
        constexpr int kMaxDepth = 512;

        // Wire flags for Style's optional fields.
        enum StyleBit : uint16_t {
            kColor = 1u << 0, kShadow = 1u << 1, kBold = 1u << 2, kItalic = 1u << 3,
            kUnderlined = 1u << 4, kStrikethrough = 1u << 5, kObfuscated = 1u << 6,
            kClick = 1u << 7, kHover = 1u << 8, kInsertion = 1u << 9, kFont = 1u << 10,
        };

        constexpr size_t kMaxWireString = 262144;   // one component's longest string on the wire
        constexpr uint32_t kMaxWireList = 65536;

        // ── JSON helpers ─────────────────────────────────────────────────

        std::optional<bool> JsonBool(const nlohmann::json& j) {
            // Codec.BOOL: JSON true/false; NBT (converted to JSON by the
            // server) hands a byte over as a number.
            if (j.is_boolean()) return j.get<bool>();
            if (j.is_number()) return j.get<double>() != 0.0;
            if (j.is_string()) {
                const std::string s = j.get<std::string>();
                if (s == "true") return true;
                if (s == "false") return false;
            }
            return std::nullopt;
        }

        std::optional<std::string> JsonString(const nlohmann::json& j) {
            if (j.is_string()) return j.get<std::string>();
            return std::nullopt;
        }

        // A primitive argument or scalar component rendered as text — MC's
        // String.valueOf for numbers/booleans.
        std::string ScalarText(const nlohmann::json& j) {
            if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
            if (j.is_number_integer()) return std::to_string(j.get<long long>());
            if (j.is_number_unsigned()) return std::to_string(j.get<unsigned long long>());
            if (j.is_number_float()) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", j.get<double>());
                return buf;
            }
            return {};
        }

        std::optional<Component> FromJsonDepth(const nlohmann::json& j, int depth);

        std::vector<Component> ComponentList(const nlohmann::json& j, int depth) {
            std::vector<Component> out;
            if (j.is_array()) {
                for (const auto& e : j) {
                    if (auto c = FromJsonDepth(e, depth + 1)) out.push_back(std::move(*c));
                }
            } else if (auto c = FromJsonDepth(j, depth + 1)) {
                out.push_back(std::move(*c));
            }
            return out;
        }

        void ReadStyle(const nlohmann::json& j, Style& style, int depth) {
            auto it = j.find("color");
            if (it != j.end()) {
                if (auto s = JsonString(*it)) style.color = TextColor::Parse(*s);
            }
            it = j.find("shadow_color");
            if (it != j.end()) {
                if (it->is_number()) {
                    style.shadowColor = static_cast<int32_t>(it->get<long long>());
                } else if (it->is_array() && it->size() == 4) {
                    // ExtraCodecs.ARGB_COLOR_CODEC's float-list form [r, g, b, a].
                    auto ch = [&](size_t i) {
                        const double v = (*it)[i].is_number() ? (*it)[i].get<double>() : 0.0;
                        return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)) & 0xFFu;
                    };
                    style.shadowColor = static_cast<int32_t>((ch(3) << 24) | (ch(0) << 16) | (ch(1) << 8) | ch(2));
                }
            }
            auto flag = [&](const char* key, std::optional<bool>& out) {
                auto f = j.find(key);
                if (f != j.end()) out = JsonBool(*f);
            };
            flag("bold", style.bold);
            flag("italic", style.italic);
            flag("underlined", style.underlined);
            flag("strikethrough", style.strikethrough);
            flag("obfuscated", style.obfuscated);

            // 26.3 keys are click_event / hover_event; the pre-1.21.5
            // camelCase clickEvent / hoverEvent (value-keyed) are accepted
            // too, for data packs written against older versions.
            const nlohmann::json* click = nullptr;
            bool legacyClick = false;
            if ((it = j.find("click_event")) != j.end() && it->is_object()) click = &*it;
            else if ((it = j.find("clickEvent")) != j.end() && it->is_object()) { click = &*it; legacyClick = true; }
            if (click) {
                const auto actionIt = click->find("action");
                const auto actionName = actionIt != click->end() ? JsonString(*actionIt) : std::nullopt;
                const auto action = actionName ? ClickEvent::ActionByName(*actionName) : std::nullopt;
                if (action) {
                    ClickEvent e;
                    e.action = *action;
                    auto str = [&](const char* key) -> std::string {
                        if (legacyClick) key = "value";
                        auto f = click->find(key);
                        if (f == click->end()) return {};
                        if (f->is_string()) return f->get<std::string>();
                        return ScalarText(*f);
                    };
                    switch (e.action) {
                        case ClickEvent::Action::OpenUrl:         e.value = str("url"); break;
                        case ClickEvent::Action::OpenFile:        e.value = str("path"); break;
                        case ClickEvent::Action::RunCommand:
                        case ClickEvent::Action::SuggestCommand:  e.value = str("command"); break;
                        case ClickEvent::Action::CopyToClipboard: e.value = str("value"); break;
                        case ClickEvent::Action::ShowDialog:      e.value = str("dialog"); break;
                        case ClickEvent::Action::Custom:          e.value = str("id"); break;
                        case ClickEvent::Action::ChangePage: {
                            auto f = click->find(legacyClick ? "value" : "page");
                            if (f != click->end()) {
                                if (f->is_number()) e.page = f->get<int>();
                                else if (f->is_string()) {
                                    try { e.page = std::stoi(f->get<std::string>()); } catch (...) { e.page = 0; }
                                }
                            }
                            break;
                        }
                    }
                    // ChangePage is ExtraCodecs.POSITIVE_INT: a non-positive
                    // page fails the codec and the event is dropped.
                    if (e.action != ClickEvent::Action::ChangePage || e.page > 0) style.clickEvent = e;
                }
            }

            const nlohmann::json* hover = nullptr;
            bool legacyHover = false;
            if ((it = j.find("hover_event")) != j.end() && it->is_object()) hover = &*it;
            else if ((it = j.find("hoverEvent")) != j.end() && it->is_object()) { hover = &*it; legacyHover = true; }
            const auto hoverAction = hover ? hover->find("action") : nlohmann::json::const_iterator();
            if (hover && hoverAction != hover->end() && *hoverAction == "show_text") {
                const char* key = legacyHover && !hover->contains("value") ? "contents" : "value";
                auto f = hover->find(key);
                if (f != hover->end()) {
                    if (auto c = FromJsonDepth(*f, depth + 1)) {
                        style.hoverText = std::make_shared<const Component>(std::move(*c));
                    }
                }
            }

            if ((it = j.find("insertion")) != j.end()) style.insertion = JsonString(*it);
            if ((it = j.find("font")) != j.end()) {
                // FontDescription.CODEC: a resource id (or the 26.x object
                // forms, which are atlas sprites — kept as their id).
                if (it->is_string()) style.font = it->get<std::string>();
                else if (it->is_object() && it->contains("id") && (*it)["id"].is_string())
                    style.font = (*it)["id"].get<std::string>();
            }
        }

        std::optional<Component> FromJsonDepth(const nlohmann::json& j, int depth) {
            if (depth > kMaxDepth) return std::nullopt;
            if (j.is_string()) return Component::Literal(j.get<std::string>());
            if (j.is_number() || j.is_boolean()) return Component::Literal(ScalarText(j));
            if (j.is_array()) {
                // ComponentSerialization.createFromList: the first element,
                // the rest appended as siblings. The list must be non-empty.
                if (j.empty()) return std::nullopt;
                std::optional<Component> first = FromJsonDepth(j[0], depth + 1);
                if (!first) return std::nullopt;
                for (size_t i = 1; i < j.size(); ++i) {
                    if (auto c = FromJsonDepth(j[i], depth + 1)) first->extra.push_back(std::move(*c));
                }
                return first;
            }
            if (!j.is_object()) return std::nullopt;

            Component c;
            // StrictEither: an explicit "type" picks the contents codec;
            // otherwise the fuzzy codec tries each in registration order.
            std::string type;
            if (auto t = j.find("type"); t != j.end() && t->is_string()) type = t->get<std::string>();
            auto has = [&](const char* key) { return j.contains(key); };

            if ((type.empty() && has("text")) || type == "text") {
                c.kind = Component::Kind::Text;
                if (auto v = j.find("text"); v != j.end()) {
                    c.text = v->is_string() ? v->get<std::string>() : ScalarText(*v);
                }
            } else if ((type.empty() && has("translate")) || type == "translatable") {
                c.kind = Component::Kind::Translatable;
                if (auto s = JsonString(j.value("translate", nlohmann::json()))) c.text = *s;
                if (auto f = j.find("fallback"); f != j.end() && f->is_string()) c.fallback = f->get<std::string>();
                if (auto w = j.find("with"); w != j.end() && w->is_array()) {
                    for (const auto& arg : *w) {
                        // ARG_CODEC: a primitive or a component.
                        if (auto a = FromJsonDepth(arg, depth + 1)) c.args.push_back(std::move(*a));
                        else c.args.push_back(Component::Literal("null"));
                    }
                }
            } else if ((type.empty() && has("keybind")) || type == "keybind") {
                c.kind = Component::Kind::Keybind;
                if (auto s = JsonString(j.value("keybind", nlohmann::json()))) c.text = *s;
            } else if ((type.empty() && has("score")) || type == "score") {
                c.kind = Component::Kind::Score;
                if (auto sc = j.find("score"); sc != j.end() && sc->is_object()) {
                    if (auto n = JsonString(sc->value("name", nlohmann::json()))) c.text = *n;
                    if (auto o = JsonString(sc->value("objective", nlohmann::json()))) c.objective = *o;
                }
            } else if ((type.empty() && has("selector")) || type == "selector") {
                c.kind = Component::Kind::Selector;
                if (auto s = JsonString(j.value("selector", nlohmann::json()))) c.text = *s;
                if (auto sep = j.find("separator"); sep != j.end()) {
                    if (auto s = FromJsonDepth(*sep, depth + 1)) c.separator.push_back(std::move(*s));
                }
            } else if ((type.empty() && has("nbt")) || type == "nbt") {
                c.kind = Component::Kind::Nbt;
                if (auto s = JsonString(j.value("nbt", nlohmann::json()))) c.text = *s;
                for (const char* kind : {"block", "entity", "storage"}) {
                    if (auto v = j.find(kind); v != j.end() && v->is_string()) {
                        c.sourceKind = kind;
                        c.source = v->get<std::string>();
                        break;
                    }
                }
                if (auto v = j.find("interpret"); v != j.end()) c.interpret = JsonBool(*v).value_or(false);
                if (auto v = j.find("plain"); v != j.end()) c.plain = JsonBool(*v).value_or(false);
                if (auto sep = j.find("separator"); sep != j.end()) {
                    if (auto s = FromJsonDepth(*sep, depth + 1)) c.separator.push_back(std::move(*s));
                }
            } else {
                // No contents MC's codec recognises (incl. `object`): the
                // decode fails in MC. An empty literal keeps the rest of the
                // page rather than losing it.
                c.kind = Component::Kind::Text;
            }

            ReadStyle(j, c.style, depth);
            if (auto e = j.find("extra"); e != j.end()) c.extra = ComponentList(*e, depth);
            return c;
        }

        nlohmann::json StyleToJson(const Style& s, nlohmann::json& out);

        nlohmann::json ToJsonImpl(const Component& c) {
            std::string collapsed;
            if (c.TryCollapseToString(collapsed)) return collapsed;
            nlohmann::json out = nlohmann::json::object();
            switch (c.kind) {
                case Component::Kind::Text:
                    out["text"] = c.text;
                    break;
                case Component::Kind::Translatable:
                    out["translate"] = c.text;
                    if (c.fallback) out["fallback"] = *c.fallback;
                    if (!c.args.empty()) {
                        nlohmann::json with = nlohmann::json::array();
                        for (const auto& a : c.args) with.push_back(ToJsonImpl(a));
                        out["with"] = std::move(with);
                    }
                    break;
                case Component::Kind::Keybind:
                    out["keybind"] = c.text;
                    break;
                case Component::Kind::Score:
                    out["score"] = {{"name", c.text}, {"objective", c.objective}};
                    break;
                case Component::Kind::Selector:
                    out["selector"] = c.text;
                    if (!c.separator.empty()) out["separator"] = ToJsonImpl(c.separator.front());
                    break;
                case Component::Kind::Nbt:
                    out["nbt"] = c.text;
                    if (c.interpret) out["interpret"] = true;
                    if (c.plain) out["plain"] = true;
                    if (!c.sourceKind.empty()) out[c.sourceKind] = c.source;
                    if (!c.separator.empty()) out["separator"] = ToJsonImpl(c.separator.front());
                    break;
            }
            if (!c.extra.empty()) {
                nlohmann::json extra = nlohmann::json::array();
                for (const auto& e : c.extra) extra.push_back(ToJsonImpl(e));
                out["extra"] = std::move(extra);
            }
            StyleToJson(c.style, out);
            return out;
        }

        nlohmann::json StyleToJson(const Style& s, nlohmann::json& out) {
            if (s.color) out["color"] = s.color->Serialize();
            if (s.shadowColor) out["shadow_color"] = *s.shadowColor;
            if (s.bold) out["bold"] = *s.bold;
            if (s.italic) out["italic"] = *s.italic;
            if (s.underlined) out["underlined"] = *s.underlined;
            if (s.strikethrough) out["strikethrough"] = *s.strikethrough;
            if (s.obfuscated) out["obfuscated"] = *s.obfuscated;
            if (s.clickEvent) {
                nlohmann::json e = {{"action", ClickEvent::ActionName(s.clickEvent->action)}};
                switch (s.clickEvent->action) {
                    case ClickEvent::Action::OpenUrl:         e["url"] = s.clickEvent->value; break;
                    case ClickEvent::Action::OpenFile:        e["path"] = s.clickEvent->value; break;
                    case ClickEvent::Action::RunCommand:
                    case ClickEvent::Action::SuggestCommand:  e["command"] = s.clickEvent->value; break;
                    case ClickEvent::Action::CopyToClipboard: e["value"] = s.clickEvent->value; break;
                    case ClickEvent::Action::ShowDialog:      e["dialog"] = s.clickEvent->value; break;
                    case ClickEvent::Action::Custom:          e["id"] = s.clickEvent->value; break;
                    case ClickEvent::Action::ChangePage:      e["page"] = s.clickEvent->page; break;
                }
                out["click_event"] = std::move(e);
            }
            if (s.hoverText) {
                out["hover_event"] = {{"action", "show_text"}, {"value", ToJsonImpl(*s.hoverText)}};
            }
            if (s.insertion) out["insertion"] = *s.insertion;
            if (s.font) out["font"] = *s.font;
            return out;
        }

        // ── Wire ─────────────────────────────────────────────────────────

        void WriteImpl(Network::PacketBuffer& b, const Component& c);

        void WriteStyle(Network::PacketBuffer& b, const Style& s) {
            uint16_t mask = 0;
            if (s.color)         mask |= kColor;
            if (s.shadowColor)   mask |= kShadow;
            if (s.bold)          mask |= kBold;
            if (s.italic)        mask |= kItalic;
            if (s.underlined)    mask |= kUnderlined;
            if (s.strikethrough) mask |= kStrikethrough;
            if (s.obfuscated)    mask |= kObfuscated;
            if (s.clickEvent)    mask |= kClick;
            if (s.hoverText)     mask |= kHover;
            if (s.insertion)     mask |= kInsertion;
            if (s.font)          mask |= kFont;
            b.WriteShort(mask);
            if (s.color) {
                b.WriteInt(s.color->rgb);
                b.WriteByte(static_cast<uint8_t>(s.color->named));
            }
            if (s.shadowColor) b.WriteInt(static_cast<uint32_t>(*s.shadowColor));
            for (const auto* f : {&s.bold, &s.italic, &s.underlined, &s.strikethrough, &s.obfuscated}) {
                if (*f) b.WriteByte(**f ? 1 : 0);
            }
            if (s.clickEvent) {
                b.WriteByte(static_cast<uint8_t>(s.clickEvent->action));
                b.WriteString(s.clickEvent->value);
                b.WriteVarInt(static_cast<uint32_t>(s.clickEvent->page));
            }
            if (s.hoverText) WriteImpl(b, *s.hoverText);
            if (s.insertion) b.WriteString(*s.insertion);
            if (s.font) b.WriteString(*s.font);
        }

        void WriteList(Network::PacketBuffer& b, const std::vector<Component>& list) {
            b.WriteVarInt(static_cast<uint32_t>(list.size()));
            for (const auto& e : list) WriteImpl(b, e);
        }

        void WriteImpl(Network::PacketBuffer& b, const Component& c) {
            b.WriteByte(static_cast<uint8_t>(c.kind));
            b.WriteString(c.text);
            switch (c.kind) {
                case Component::Kind::Translatable:
                    b.WriteByte(c.fallback ? 1 : 0);
                    if (c.fallback) b.WriteString(*c.fallback);
                    WriteList(b, c.args);
                    break;
                case Component::Kind::Score:
                    b.WriteString(c.objective);
                    break;
                case Component::Kind::Selector:
                    WriteList(b, c.separator);
                    break;
                case Component::Kind::Nbt:
                    b.WriteString(c.sourceKind);
                    b.WriteString(c.source);
                    b.WriteByte(static_cast<uint8_t>((c.interpret ? 1 : 0) | (c.plain ? 2 : 0)));
                    WriteList(b, c.separator);
                    break;
                case Component::Kind::Text:
                case Component::Kind::Keybind:
                    break;
            }
            WriteStyle(b, c.style);
            WriteList(b, c.extra);
        }

        Component ReadImpl(Network::PacketReader& r, int depth);

        std::vector<Component> ReadList(Network::PacketReader& r, int depth) {
            const uint32_t n = r.ReadVarInt();
            if (n > kMaxWireList || n > r.Remaining()) {
                throw std::runtime_error("text component: list length " + std::to_string(n) + " out of range");
            }
            std::vector<Component> out;
            out.reserve(n);
            for (uint32_t i = 0; i < n; ++i) out.push_back(ReadImpl(r, depth + 1));
            return out;
        }

        Style ReadStyle(Network::PacketReader& r, int depth) {
            Style s;
            const uint16_t mask = r.ReadShort();
            if (mask & kColor) {
                TextColor color;
                color.rgb = r.ReadInt() & 0xFFFFFFu;
                color.named = static_cast<int8_t>(r.ReadByte());
                if (color.named >= kFormattingColorCount) color.named = -1;
                s.color = color;
            }
            if (mask & kShadow) s.shadowColor = static_cast<int32_t>(r.ReadInt());
            if (mask & kBold)          s.bold          = r.ReadByte() != 0;
            if (mask & kItalic)        s.italic        = r.ReadByte() != 0;
            if (mask & kUnderlined)    s.underlined    = r.ReadByte() != 0;
            if (mask & kStrikethrough) s.strikethrough = r.ReadByte() != 0;
            if (mask & kObfuscated)    s.obfuscated    = r.ReadByte() != 0;
            if (mask & kClick) {
                ClickEvent e;
                const uint8_t action = r.ReadByte();
                if (action > static_cast<uint8_t>(ClickEvent::Action::Custom)) {
                    throw std::runtime_error("text component: click action out of range");
                }
                e.action = static_cast<ClickEvent::Action>(action);
                e.value = r.ReadString(kMaxWireString);
                e.page = static_cast<int>(r.ReadVarInt());
                s.clickEvent = e;
            }
            if (mask & kHover) s.hoverText = std::make_shared<const Component>(ReadImpl(r, depth + 1));
            if (mask & kInsertion) s.insertion = r.ReadString(kMaxWireString);
            if (mask & kFont) s.font = r.ReadString(kMaxWireString);
            return s;
        }

        Component ReadImpl(Network::PacketReader& r, int depth) {
            if (depth > kMaxDepth) throw std::runtime_error("text component: nested too deeply");
            Component c;
            const uint8_t kind = r.ReadByte();
            if (kind > static_cast<uint8_t>(Component::Kind::Nbt)) {
                throw std::runtime_error("text component: kind out of range");
            }
            c.kind = static_cast<Component::Kind>(kind);
            c.text = r.ReadString(kMaxWireString);
            switch (c.kind) {
                case Component::Kind::Translatable:
                    if (r.ReadByte() != 0) c.fallback = r.ReadString(kMaxWireString);
                    c.args = ReadList(r, depth);
                    break;
                case Component::Kind::Score:
                    c.objective = r.ReadString(kMaxWireString);
                    break;
                case Component::Kind::Selector:
                    c.separator = ReadList(r, depth);
                    break;
                case Component::Kind::Nbt: {
                    c.sourceKind = r.ReadString(kMaxWireString);
                    c.source = r.ReadString(kMaxWireString);
                    const uint8_t flags = r.ReadByte();
                    c.interpret = (flags & 1) != 0;
                    c.plain = (flags & 2) != 0;
                    c.separator = ReadList(r, depth);
                    break;
                }
                case Component::Kind::Text:
                case Component::Kind::Keybind:
                    break;
            }
            c.style = ReadStyle(r, depth);
            c.extra = ReadList(r, depth);
            return c;
        }

        // ── Visiting ─────────────────────────────────────────────────────

        bool VisitImpl(const Component& c, const Style& parent, const StyledSink& sink, int depth);

        // MC TranslatableContents.decomposeTemplate + visit. Returns false
        // when the sink stopped the walk.
        bool VisitTranslatable(const Component& c, const Style& style, const StyledSink& sink, int depth) {
            const std::string format = c.fallback ? Language::GetOrDefault(c.text, *c.fallback)
                                                  : Language::Get(c.text);
            struct Part { bool isArg; std::string text; size_t arg; };
            std::vector<Part> parts;
            bool ok = true;
            size_t sequential = 0;
            size_t pos = 0;
            std::string literal;
            while (pos < format.size()) {
                const size_t pct = format.find('%', pos);
                if (pct == std::string::npos) {
                    literal.append(format, pos, std::string::npos);
                    break;
                }
                literal.append(format, pos, pct - pos);
                // %(?:(\d+)\$)?([A-Za-z%]|$)
                size_t i = pct + 1;
                size_t digitsEnd = i;
                while (digitsEnd < format.size() && format[digitsEnd] >= '0' && format[digitsEnd] <= '9') ++digitsEnd;
                std::optional<size_t> explicitIndex;
                if (digitsEnd > i && digitsEnd < format.size() && format[digitsEnd] == '$') {
                    explicitIndex = static_cast<size_t>(std::stoul(format.substr(i, digitsEnd - i)));
                    i = digitsEnd + 1;
                }
                if (i >= format.size()) { ok = false; break; }            // "%" at the end: unsupported
                const char type = format[i];
                if (type == '%' && !explicitIndex) {
                    literal += '%';
                } else if (type == 's') {
                    const size_t index = explicitIndex ? (*explicitIndex == 0 ? SIZE_MAX : *explicitIndex - 1)
                                                       : sequential++;
                    if (index >= c.args.size()) { ok = false; break; }    // "Invalid index"
                    if (!literal.empty()) { parts.push_back({false, std::move(literal), 0}); literal.clear(); }
                    parts.push_back({true, {}, index});
                } else {
                    ok = false;                                            // "Unsupported format"
                    break;
                }
                pos = i + 1;
            }
            if (!ok) {
                // TranslatableFormatException → the raw format as one part.
                return sink(style, format);
            }
            if (!literal.empty()) parts.push_back({false, std::move(literal), 0});
            for (const Part& p : parts) {
                if (p.isArg) {
                    if (!VisitImpl(c.args[p.arg], style, sink, depth + 1)) return false;
                } else if (!sink(style, p.text)) {
                    return false;
                }
            }
            return true;
        }

        bool VisitImpl(const Component& c, const Style& parent, const StyledSink& sink, int depth) {
            if (depth > kMaxDepth) return true;
            const Style style = c.style.ApplyTo(parent);
            switch (c.kind) {
                case Component::Kind::Text:
                    if (!c.text.empty() && !sink(style, c.text)) return false;
                    break;
                case Component::Kind::Translatable:
                    if (!VisitTranslatable(c, style, sink, depth)) return false;
                    break;
                case Component::Kind::Keybind: {
                    // KeybindContents: the bound key's display name. The
                    // common layer has no key bindings, so it shows the
                    // mapping's own translated name ("key.jump" → "Jump").
                    const std::string name = Language::Get(c.text);
                    if (!name.empty() && !sink(style, name)) return false;
                    break;
                }
                case Component::Kind::Score:
                case Component::Kind::Selector:
                case Component::Kind::Nbt:
                    // Unresolved: MC's contents visit nothing until
                    // ComponentUtils.resolve has replaced them.
                    break;
            }
            for (const Component& sibling : c.extra) {
                if (!VisitImpl(sibling, style, sink, depth + 1)) return false;
            }
            return true;
        }

        Component ResolveImpl(const Component& c,
                              const std::function<std::optional<std::string>(const std::string&)>& selectorName,
                              int depth) {
            if (depth > kMaxDepth) return c;
            Component out;
            switch (c.kind) {
                case Component::Kind::Selector: {
                    // SelectorContents.resolve → ComponentUtils.formatList of
                    // the matched entities' display names.
                    out = Component::Literal(selectorName ? selectorName(c.text).value_or(std::string()) : std::string());
                    break;
                }
                case Component::Kind::Score:
                case Component::Kind::Nbt:
                    // No scoreboard, no data sources: resolves to nothing.
                    out = Component::Literal({});
                    break;
                case Component::Kind::Translatable:
                    out = c;
                    for (auto& arg : out.args) arg = ResolveImpl(arg, selectorName, depth + 1);
                    out.extra.clear();
                    break;
                default:
                    out = c;
                    out.extra.clear();
                    break;
            }
            out.style = c.style;
            for (const auto& sibling : c.extra) out.extra.push_back(ResolveImpl(sibling, selectorName, depth + 1));
            return out;
        }

    } // namespace

    // ── Formatting / TextColor ──────────────────────────────────────────────

    uint32_t FormattingRgb(int colorIndex) {
        return (colorIndex >= 0 && colorIndex < kFormattingColorCount) ? kColors[colorIndex].rgb : 0xFFFFFFu;
    }

    const char* FormattingName(int colorIndex) {
        return (colorIndex >= 0 && colorIndex < kFormattingColorCount) ? kColors[colorIndex].name : nullptr;
    }

    int FormattingColorByName(std::string_view name) {
        for (int i = 0; i < kFormattingColorCount; ++i) {
            if (name == kColors[i].name) return i;
        }
        return -1;
    }

    std::optional<Formatting> FormattingByCode(char code) {
        const char lower = (code >= 'A' && code <= 'Z') ? static_cast<char>(code - 'A' + 'a') : code;
        for (int i = 0; i < kFormattingColorCount; ++i) {
            if (kColors[i].code == lower) return static_cast<Formatting>(i);
        }
        switch (lower) {
            case 'k': return Formatting::Obfuscated;
            case 'l': return Formatting::Bold;
            case 'm': return Formatting::Strikethrough;
            case 'n': return Formatting::Underline;
            case 'o': return Formatting::Italic;
            case 'r': return Formatting::Reset;
            default:  return std::nullopt;
        }
    }

    TextColor TextColor::FromFormatting(int colorIndex) {
        TextColor c;
        c.rgb = FormattingRgb(colorIndex);
        c.named = static_cast<int8_t>(colorIndex);
        return c;
    }

    std::optional<TextColor> TextColor::Parse(std::string_view text) {
        if (!text.empty() && text.front() == '#') {
            const std::string hex(text.substr(1));
            if (hex.empty() || hex.size() > 8) return std::nullopt;
            for (char ch : hex) {
                if (!std::isxdigit(static_cast<unsigned char>(ch))) return std::nullopt;
            }
            const unsigned long value = std::stoul(hex, nullptr, 16);
            if (value > 0xFFFFFFul) return std::nullopt;       // "Color value out of range"
            return FromRgb(static_cast<uint32_t>(value));
        }
        const int named = FormattingColorByName(text);
        if (named < 0) return std::nullopt;
        return FromFormatting(named);
    }

    std::string TextColor::Serialize() const {
        if (named >= 0 && named < kFormattingColorCount) return kColors[named].name;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(rgb & 0xFFFFFFu));
        return buf;
    }

    // ── ClickEvent ──────────────────────────────────────────────────────────

    const char* ClickEvent::ActionName(Action action) {
        switch (action) {
            case Action::OpenUrl:         return "open_url";
            case Action::OpenFile:        return "open_file";
            case Action::RunCommand:      return "run_command";
            case Action::SuggestCommand:  return "suggest_command";
            case Action::ShowDialog:      return "show_dialog";
            case Action::ChangePage:      return "change_page";
            case Action::CopyToClipboard: return "copy_to_clipboard";
            case Action::Custom:          return "custom";
        }
        return "run_command";
    }

    std::optional<ClickEvent::Action> ClickEvent::ActionByName(std::string_view name) {
        for (int i = 0; i <= static_cast<int>(Action::Custom); ++i) {
            if (name == ActionName(static_cast<Action>(i))) return static_cast<Action>(i);
        }
        return std::nullopt;
    }

    // ── Style ───────────────────────────────────────────────────────────────

    bool Style::IsEmpty() const {
        return !color && !shadowColor && !bold && !italic && !underlined && !strikethrough &&
               !obfuscated && !clickEvent && !hoverText && !insertion && !font;
    }

    Style Style::ApplyTo(const Style& parent) const {
        if (IsEmpty()) return parent;
        if (parent.IsEmpty()) return *this;
        Style s;
        s.color         = color         ? color         : parent.color;
        s.shadowColor   = shadowColor   ? shadowColor   : parent.shadowColor;
        s.bold          = bold          ? bold          : parent.bold;
        s.italic        = italic        ? italic        : parent.italic;
        s.underlined    = underlined    ? underlined    : parent.underlined;
        s.strikethrough = strikethrough ? strikethrough : parent.strikethrough;
        s.obfuscated    = obfuscated    ? obfuscated    : parent.obfuscated;
        s.clickEvent    = clickEvent    ? clickEvent    : parent.clickEvent;
        s.hoverText     = hoverText     ? hoverText     : parent.hoverText;
        s.insertion     = insertion     ? insertion     : parent.insertion;
        s.font          = font          ? font          : parent.font;
        return s;
    }

    Style Style::ApplyLegacyFormat(Formatting format) const {
        Style s = *this;
        switch (format) {
            case Formatting::Obfuscated:    s.obfuscated = true; break;
            case Formatting::Bold:          s.bold = true; break;
            case Formatting::Strikethrough: s.strikethrough = true; break;
            case Formatting::Underline:     s.underlined = true; break;
            case Formatting::Italic:        s.italic = true; break;
            case Formatting::Reset:         return Style{};
            default:
                // A colour code clears the decorations (Style.applyLegacyFormat).
                s.obfuscated = false;
                s.bold = false;
                s.strikethrough = false;
                s.underlined = false;
                s.italic = false;
                s.color = TextColor::FromFormatting(static_cast<int>(format));
                break;
        }
        return s;
    }

    bool Style::operator==(const Style& o) const {
        if (color != o.color || shadowColor != o.shadowColor || bold != o.bold || italic != o.italic ||
            underlined != o.underlined || strikethrough != o.strikethrough || obfuscated != o.obfuscated ||
            insertion != o.insertion || font != o.font) {
            return false;
        }
        if (clickEvent.has_value() != o.clickEvent.has_value()) return false;
        if (clickEvent && !(*clickEvent == *o.clickEvent)) return false;
        if (static_cast<bool>(hoverText) != static_cast<bool>(o.hoverText)) return false;
        if (hoverText && hoverText != o.hoverText && !(*hoverText == *o.hoverText)) return false;
        return true;
    }

    // ── Component ───────────────────────────────────────────────────────────

    Component Component::Literal(std::string text) {
        Component c;
        c.kind = Kind::Text;
        c.text = std::move(text);
        return c;
    }

    Component Component::Translatable(std::string key, std::vector<Component> with) {
        Component c;
        c.kind = Kind::Translatable;
        c.text = std::move(key);
        c.args = std::move(with);
        return c;
    }

    bool Component::TryCollapseToString(std::string& out) const {
        if (kind != Kind::Text || !style.IsEmpty() || !extra.empty()) return false;
        out = text;
        return true;
    }

    bool Component::operator==(const Component& o) const {
        return kind == o.kind && text == o.text && fallback == o.fallback && args == o.args &&
               objective == o.objective && sourceKind == o.sourceKind && source == o.source &&
               interpret == o.interpret && plain == o.plain && separator == o.separator &&
               style == o.style && extra == o.extra;
    }

    // ── Public serialization ────────────────────────────────────────────────

    std::optional<Component> FromJson(const nlohmann::json& json) {
        return FromJsonDepth(json, 0);
    }

    std::optional<Component> ParseJson(std::string_view text) {
        nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded()) return std::nullopt;
        return FromJson(j);
    }

    nlohmann::json ToJson(const Component& component) {
        return ToJsonImpl(component);
    }

    std::string ToJsonString(const Component& component) {
        return ToJsonImpl(component).dump();
    }

    void Write(Network::PacketBuffer& buffer, const Component& component) {
        WriteImpl(buffer, component);
    }

    Component Read(Network::PacketReader& reader) {
        return ReadImpl(reader, 0);
    }

    void Visit(const Component& component, const Style& parentStyle, const StyledSink& sink) {
        VisitImpl(component, parentStyle, sink, 0);
    }

    std::string GetString(const Component& component) {
        std::string out;
        Visit(component, Style{}, [&out](const Style&, std::string_view text) {
            out.append(text);
            return true;
        });
        return out;
    }

    Component Resolve(const Component& component,
                      const std::function<std::optional<std::string>(const std::string&)>& selectorName) {
        return ResolveImpl(component, selectorName, 0);
    }

    bool EncodesLongerThan(const Component& component, size_t maxLength) {
        return ToJsonString(component).size() > maxLength;
    }

} // namespace Game::Text
