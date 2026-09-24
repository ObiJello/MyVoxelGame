// File: src/common/text/TextComponent.hpp
//
// Mirrors net.minecraft.network.chat.Component / MutableComponent / Style —
// the rich text a written book's pages, a book title's hover, a loot table's
// `set_written_book_pages` and (later) chat and signs are made of.
//
// A component is a CONTENTS (literal text, a translation key with
// arguments, a keybind, a score, an entity selector, an NBT path), a STYLE
// (colour, bold/italic/underlined/strikethrough/obfuscated, click and hover
// events, insertion, font) and a list of SIBLINGS ("extra") that inherit the
// style. Rendering walks the tree exactly as MC's Component.visit does
// (Visit below): each node's style is applied on top of its parent's
// (Style.applyTo), then its contents, then its siblings.
//
// Formats handled, all MC 26.3's ComponentSerialization.CODEC shapes:
//   • a bare string                       — literal text
//   • a list [c0, c1, …]                  — c0 with the rest appended as extra
//   • an object {text|translate|keybind|score|selector|nbt, …style, extra}
// in JSON (loot tables, data packs) through FromJson / ToJson, and in NBT
// (item components on disk) through the same JSON shape — the server's
// ItemStackNbt converts NBT tags to and from it. The wire format is this
// engine's own compact binary (Write / Read).
//
// What is NOT modelled: shadow colours are carried but not drawn (the font
// renderer draws MC's default shadow only); hover events other than
// show_text are dropped on read; score / selector / nbt contents are carried
// verbatim but only resolve through Resolve (a selector naming the reader).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Network { class PacketBuffer; class PacketReader; }

namespace Game::Text {

    // ── ChatFormatting ──────────────────────────────────────────────────────
    // MC ChatFormatting. Indices 0..15 are the colours in legacy-code order
    // (§0..§f), then the five styles and RESET.
    enum class Formatting : uint8_t {
        Black = 0, DarkBlue, DarkGreen, DarkAqua, DarkRed, DarkPurple, Gold, Gray,
        DarkGray, Blue, Green, Aqua, Red, LightPurple, Yellow, White,
        Obfuscated, Bold, Strikethrough, Underline, Italic, Reset,
    };
    constexpr int kFormattingColorCount = 16;

    // 0xRRGGBB of a colour formatting (index 0..15).
    uint32_t FormattingRgb(int colorIndex);
    // MC's serialized name ("dark_aqua", "light_purple"); nullptr past 15.
    const char* FormattingName(int colorIndex);
    // The colour index for a name, or -1.
    int FormattingColorByName(std::string_view name);
    // MC ChatFormatting.getByCode — the formatting a legacy '§' code picks,
    // or nullopt for a character that is not a code.
    std::optional<Formatting> FormattingByCode(char code);

    // MC TextColor: an RGB value, remembering the name it was given so a
    // named colour writes back as "red" rather than "#FF5555".
    struct TextColor {
        uint32_t rgb = 0xFFFFFF;
        int8_t   named = -1;                       // colour index, or -1 for a custom colour

        static TextColor FromFormatting(int colorIndex);
        static TextColor FromRgb(uint32_t rgb) { return TextColor{rgb & 0xFFFFFFu, -1}; }
        // MC TextColor.parseColor: "#RRGGBB" or a formatting name.
        static std::optional<TextColor> Parse(std::string_view text);
        // MC TextColor.serialize.
        std::string Serialize() const;

        bool operator==(const TextColor& o) const { return rgb == o.rgb && named == o.named; }
        bool operator!=(const TextColor& o) const { return !(*this == o); }
    };

    // MC ClickEvent (the eight 26.3 actions). `value` carries the url, file
    // path, command, clipboard text, dialog id or custom id; `page` the
    // change_page target (1-based, POSITIVE_INT).
    struct ClickEvent {
        enum class Action : uint8_t {
            OpenUrl = 0, OpenFile, RunCommand, SuggestCommand, ShowDialog, ChangePage,
            CopyToClipboard, Custom,
        };
        Action      action = Action::RunCommand;
        std::string value;
        int         page = 1;

        // MC ClickEvent.Action serialized names ("run_command", …).
        static const char* ActionName(Action action);
        static std::optional<Action> ActionByName(std::string_view name);

        bool operator==(const ClickEvent& o) const {
            return action == o.action && value == o.value && page == o.page;
        }
    };

    struct Component;

    // MC Style. Every field is optional: an absent field inherits from the
    // parent when styles are merged (ApplyTo).
    struct Style {
        std::optional<TextColor>   color;
        std::optional<int32_t>     shadowColor;    // ARGB
        std::optional<bool>        bold;
        std::optional<bool>        italic;
        std::optional<bool>        underlined;
        std::optional<bool>        strikethrough;
        std::optional<bool>        obfuscated;
        std::optional<ClickEvent>  clickEvent;
        // HoverEvent.ShowText's value. Shared and immutable: a Component
        // cannot hold a Component by value inside its own Style.
        std::shared_ptr<const Component> hoverText;
        std::optional<std::string> insertion;
        std::optional<std::string> font;

        bool IsEmpty() const;
        // MC Style.applyTo(parent): this style's fields, falling back to the
        // parent's where this one has none.
        Style ApplyTo(const Style& parent) const;
        // MC Style.applyLegacyFormat — what a '§' code in literal text does.
        Style ApplyLegacyFormat(Formatting format) const;

        bool IsBold() const          { return bold.value_or(false); }
        bool IsItalic() const        { return italic.value_or(false); }
        bool IsUnderlined() const    { return underlined.value_or(false); }
        bool IsStrikethrough() const { return strikethrough.value_or(false); }
        bool IsObfuscated() const    { return obfuscated.value_or(false); }

        bool operator==(const Style& o) const;
        bool operator!=(const Style& o) const { return !(*this == o); }
    };

    struct Component {
        // MC's ComponentContents types (ComponentSerialization.bootstrap),
        // less `object` (sprites/player heads inline — no renderer for it).
        enum class Kind : uint8_t { Text = 0, Translatable, Keybind, Score, Selector, Nbt };

        Kind        kind = Kind::Text;
        // Text: the literal. Translatable: the key. Keybind: the key mapping
        // name. Score: the holder name / selector. Selector: the pattern.
        // Nbt: the NBT path.
        std::string text;
        std::optional<std::string> fallback;       // Translatable: "fallback"
        std::vector<Component>     args;           // Translatable: "with"
        std::string objective;                     // Score: "objective"
        // Nbt: the data source — sourceKind "block" / "entity" / "storage"
        // and its value (coordinates, selector, storage id).
        std::string sourceKind;
        std::string source;
        bool        interpret = false;             // Nbt
        bool        plain     = false;             // Nbt
        std::vector<Component> separator;          // Selector / Nbt: 0 or 1 element

        Style                  style;
        std::vector<Component> extra;              // siblings

        // MC Component.literal / translatable / empty.
        static Component Literal(std::string text);
        static Component Translatable(std::string key, std::vector<Component> with = {});
        static Component Empty() { return Literal({}); }

        // MC MutableComponent.append.
        Component& Append(Component sibling) { extra.push_back(std::move(sibling)); return *this; }

        // MC Component.tryCollapseToString: a plain literal with no style and
        // no siblings serializes as a bare string.
        bool TryCollapseToString(std::string& out) const;

        bool operator==(const Component& o) const;
        bool operator!=(const Component& o) const { return !(*this == o); }
    };

    // ── Serialization ───────────────────────────────────────────────────────

    // MC ComponentSerialization.CODEC decode over JSON. Tolerant the way a
    // loader should be: something that is not a component at all yields
    // nullopt; unknown style keys are ignored. Numbers and booleans are read
    // as their literal text (the NBT path hands bytes over as numbers, and a
    // translatable's primitive arguments are numbers too).
    std::optional<Component> FromJson(const nlohmann::json& json);
    // Same, over a JSON document in a string.
    std::optional<Component> ParseJson(std::string_view text);
    // MC ComponentSerialization.CODEC encode: a bare string when the
    // component collapses, else the object form.
    nlohmann::json ToJson(const Component& component);
    std::string    ToJsonString(const Component& component);

    // Engine wire codec — every field, recursively; depth-limited on read
    // (MC's NBT depth limit, 512) so a hostile packet cannot overflow the
    // stack. Throws std::runtime_error on malformed input.
    void      Write(Network::PacketBuffer& buffer, const Component& component);
    Component Read(Network::PacketReader& reader);

    // ── Content resolution / visiting ───────────────────────────────────────

    // MC FormattedText.visit(StyledContentConsumer, Style): every run of
    // text in order, with its fully merged style. Translatable contents are
    // decomposed against the Language table ("%s", "%1$s", "%%") with their
    // arguments visited inline; keybinds show their translated key name;
    // unresolved score / selector / nbt contents show nothing, as in MC.
    // The sink returns false to stop the walk.
    using StyledSink = std::function<bool(const Style& style, std::string_view text)>;
    void Visit(const Component& component, const Style& parentStyle, const StyledSink& sink);

    // MC Component.getString — the plain text of the whole tree.
    std::string GetString(const Component& component);

    // MC ComponentUtils.resolve, narrowed to what the engine can answer:
    // `selectorName` returns the name a selector resolves to ("@s"/"@p" →
    // the reader), or nullopt to leave it unresolved. Score and NBT contents
    // have no scoreboard or data sources behind them and resolve to empty
    // text. The result's style and siblings are kept.
    Component Resolve(const Component& component,
                      const std::function<std::optional<std::string>(const std::string&)>& selectorName);

    // MC ComponentSerialization.flatRestrictedCodec's size check: the JSON
    // encoding longer than `maxLength` characters (the written-book page
    // limit is 32767).
    bool EncodesLongerThan(const Component& component, size_t maxLength);

} // namespace Game::Text
