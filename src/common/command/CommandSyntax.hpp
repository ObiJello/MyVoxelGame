// File: src/common/command/CommandSyntax.hpp
//
// A command's argument grammar — MC's Brigadier command tree, as far as the
// client needs it: literal and argument nodes, which nodes may end the
// command (Brigadier's `executes`), and `redirect` back to the command root
// (/execute's chaining).
//
// THE RULE: a command registers its tree with its handler —
//
//     dispatcher.RegisterCommand("effect", Execute,
//         Cmd::Root()
//             .Then(Cmd::Literal("give")
//                 .Then(Cmd::Argument("targets", Cmd::Arg::Entities)
//                     .Then(Cmd::Argument("effect", Cmd::Arg::Effect).Executes()))));
//
// and the server ships every tree to the client in CommandsS2C (MC
// ClientboundCommandsPacket), where chat's tab-completion walks it and draws
// the usage hint from it (client/renderer/gui/CommandSuggestions). The
// client has no per-command code: a new command gets its hint and completion
// from its tree alone. The ARGUMENT TYPE picks the suggestions (block ids,
// entity types, sounds, selectors, coordinates …), exactly as MC's
// ArgumentTypeInfos do; `Suggests` adds fixed entries (a number's examples).
//
// The tree describes; the handler still parses its own args (the dispatcher
// hands it the whitespace-split tokens). Keep the two in step — the tree is
// what players are told the command accepts.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace Network { class PacketBuffer; class PacketReader; }

namespace Game::Cmd {

    // MC ArgumentTypeInfos, reduced to what this engine's commands take.
    // Appending is wire-safe (client and server ship together); never
    // reorder.
    enum class Arg : uint8_t {
        Literal = 0,     // a keyword: the node's name, matched case-insensitively
        Word,            // one free word; only the node's Suggests entries
        Integer,         // IntegerArgumentType (Suggests = examples)
        Float,           // Float/DoubleArgumentType (Suggests = examples)
        Bool,            // BoolArgumentType: true | false
        Greedy,          // StringArgumentType.greedyString: the rest of the line
        Command,         // the rest of the line is another command (/execute run)

        Player,          // EntityArgument.player(): a name or @p @r @s @n
        Players,         // EntityArgument.players(): names, @a @p @r @s @n, @a[..]
        Entity,          // EntityArgument.entity(): one entity
        Entities,        // EntityArgument.entities(): names and every selector
        PlayerName,      // a bare online player name (no selectors)

        EntityType,      // ResourceArgument<entity_type> (summonable)
        Block,           // BlockStateArgument: id[prop=value,...]
        BlockPredicate,  // BlockPredicateArgument: id[..] or #tag[..]
        BlockList,       // engine: comma-separated BlockPredicates ("dirt, #logs")
        Item,            // ItemArgument: an item id
        ItemList,        // engine: comma-separated item ids
        Effect,          // ResourceArgument<mob_effect>
        Enchantment,     // ResourceArgument<enchantment>
        Sound,           // IdentifierArgument + SuggestionProviders.AVAILABLE_SOUNDS
        Particle,        // ParticleArgument: id{options}
        GameRule,        // a game rule id
        GameRuleValue,   // the value of the rule named by the preceding "rule" argument
        Dimension,       // DimensionArgument
        Biome,           // ResourceArgument<biome> (every dimension's)
        BiomeOrTag,      // ResourceOrTagArgument<biome>
        LocateStructure, // /locate structure: this dimension's structures and tags
        LocateBiome,     // /locate biome: this dimension's biomes and tags
        LocatePoi,       // /locate poi: the portal kinds that can exist here
        LootTable,       // ResourceLocation over data/<ns>/loot_table
        GameMode,        // GameModeArgument (+ the engine's s/c/a/sp and 0-3)

        Vec3,            // Vec3Argument: x y z (three tokens; ~ and ^)
        BlockPos,        // BlockPosArgument: x y z integers
        Vec2,            // Vec2Argument: x z
        ColumnPos,       // ColumnPosArgument: x z integers
        Rotation,        // RotationArgument: yaw pitch

        Time,            // TimeArgument: <n>[d|s|t]
        EntityNbt,       // CompoundTagArgument for the entity named by the "entity" argument
        Nbt,             // CompoundTagArgument
        SoundSource,     // a SoundSource name (master, music, record …)
        Count
    };

    // How many whitespace-separated tokens a fixed-arity argument spans
    // (0 = variable: BlockList/ItemList run on after a trailing ',',
    // Greedy/Command take the rest).
    int TokenArity(Arg type);

    struct Node {
        Arg                      type = Arg::Literal;
        std::string              name;          // the keyword, or the argument's name (<name>)
        bool                     executable = false;   // the command may end here
        bool                     redirectRoot = false; // what follows is the command root's children
        std::vector<std::string> suggestions;   // fixed entries, offered before the type's own
        std::vector<Node>        children;

        // Fluent builders — usable on temporaries (`Literal("x").Executes()`)
        // and on named nodes.
        Node& Then(Node child) &  { children.push_back(std::move(child)); return *this; }
        Node&& Then(Node child) && { children.push_back(std::move(child)); return std::move(*this); }
        // Several alternatives at once (Literals(...), a loop's output).
        Node& Then(std::vector<Node> alternatives) &;
        Node&& Then(std::vector<Node> alternatives) &&;
        // Brigadier `executes`: the command is complete after this node, so
        // its children are optional ([...] in the usage line).
        Node& Executes() &  { executable = true; return *this; }
        Node&& Executes() && { executable = true; return std::move(*this); }
        // Fixed suggestions (examples for numbers and free words).
        Node& Suggests(std::vector<std::string> entries) &  { suggestions = std::move(entries); return *this; }
        Node&& Suggests(std::vector<std::string> entries) && { suggestions = std::move(entries); return std::move(*this); }
        // Brigadier `redirect(root)`: after this node the command continues
        // from its root again (/execute as <targets> <next subcommand>).
        Node& Redirect() &  { redirectRoot = true; return *this; }
        Node&& Redirect() && { redirectRoot = true; return std::move(*this); }

        bool IsLiteral() const { return type == Arg::Literal; }
    };

    // The command's own node. RegisterCommand names it; `.Executes()` when
    // the bare command (no arguments) is valid.
    inline Node Root() { return Node{}; }
    inline Node Literal(std::string word) {
        Node n;
        n.type = Arg::Literal;
        n.name = std::move(word);
        return n;
    }
    inline Node Argument(std::string name, Arg type) {
        Node n;
        n.type = type;
        n.name = std::move(name);
        return n;
    }
    // One literal per word, each with a copy of `then` below it (and each
    // executable when `executable`): enumerations such as difficulty names.
    std::vector<Node> Literals(std::initializer_list<const char*> words, bool executable = true,
                               const Node* then = nullptr);
    std::vector<Node> Literals(const std::vector<std::string>& words, bool executable = true,
                               const Node* then = nullptr);

    // A node's label in a usage line: the keyword, or "<name>".
    std::string Label(const Node& node);

    // MC CommandDispatcher.getSmartUsage-style usage of `child` (and what
    // follows it) for a line under a parent that is (`optional`) or is not
    // executable: "<targets> <effect> [<seconds>|infinite] [<amplifier>]".
    // Alternatives that continue the same way fold into one "(a|b)" group;
    // ones that differ end the line with "...".
    std::string Usage(const Node& child, bool optional, const Node& commandRoot);

    // Every usage line of a command: "/<name> " + Usage(child) per child of
    // the root, or "/<name>" alone for a command with no arguments. What a
    // handler can print for "Usage:" and what the hint shows before a space.
    std::vector<std::string> UsageLines(const std::string& commandName, const Node& root);

    // Wire form (CommandsS2C's trailing syntax block). Read returns false on
    // a malformed or absurdly deep/wide tree, leaving `out` unspecified.
    void WriteNode(Network::PacketBuffer& out, const Node& node);
    bool ReadNode(Network::PacketReader& in, Node& out);

} // namespace Game::Cmd
