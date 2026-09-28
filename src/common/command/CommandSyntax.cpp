// File: src/common/command/CommandSyntax.cpp
#include "common/command/CommandSyntax.hpp"
#include "common/network/PacketRegistry.hpp"

#include <exception>
#include <utility>

namespace Game::Cmd {

    int TokenArity(Arg type) {
        switch (type) {
            case Arg::Vec3:
            case Arg::BlockPos:  return 3;
            case Arg::Vec2:
            case Arg::ColumnPos:
            case Arg::Rotation:  return 2;
            case Arg::BlockList:
            case Arg::ItemList:
            case Arg::Greedy:
            case Arg::Command:   return 0;
            default:             return 1;
        }
    }

    Node& Node::Then(std::vector<Node> alternatives) & {
        for (Node& n : alternatives) children.push_back(std::move(n));
        return *this;
    }

    Node&& Node::Then(std::vector<Node> alternatives) && {
        for (Node& n : alternatives) children.push_back(std::move(n));
        return std::move(*this);
    }

    std::vector<Node> Literals(const std::vector<std::string>& words, bool executable, const Node* then) {
        std::vector<Node> out;
        out.reserve(words.size());
        for (const std::string& w : words) {
            Node n = Literal(w);
            n.executable = executable;
            if (then) n.children.push_back(*then);
            out.push_back(std::move(n));
        }
        return out;
    }

    std::vector<Node> Literals(std::initializer_list<const char*> words, bool executable, const Node* then) {
        std::vector<std::string> list;
        list.reserve(words.size());
        for (const char* w : words) list.emplace_back(w);
        return Literals(list, executable, then);
    }

    std::string Label(const Node& node) {
        return node.IsLiteral() ? node.name : "<" + node.name + ">";
    }

    namespace {

        constexpr int kMaxUsageDepth = 24;

        std::string Continuation(const Node& node, int depth);

        std::string UsageAt(const Node& child, bool optional, int depth) {
            std::string head = Label(child);
            if (optional) head = "[" + head + "]";
            const std::string tail = Continuation(child, depth + 1);
            return tail.empty() ? head : head + " " + tail;
        }

        // The alternatives below `node` as one group: "(a|b) <rest>" when
        // they continue identically, "(a|b) ..." when they do not.
        std::string Group(const std::vector<Node>& kids, bool optional, int depth) {
            std::string labels;
            std::string sharedTail;
            bool same = true;
            for (size_t i = 0; i < kids.size(); ++i) {
                if (i) labels += "|";
                labels += Label(kids[i]);
                const std::string tail = Continuation(kids[i], depth + 1);
                if (i == 0) sharedTail = tail;
                else if (tail != sharedTail) same = false;
            }
            const std::string group = optional ? "[" + labels + "]" : "(" + labels + ")";
            if (!same) return group + " ...";
            return sharedTail.empty() ? group : group + " " + sharedTail;
        }

        std::string Continuation(const Node& node, int depth) {
            if (depth > kMaxUsageDepth) return "...";
            if (node.redirectRoot) return "...";
            if (node.children.empty()) return {};
            if (node.children.size() == 1) return UsageAt(node.children.front(), node.executable, depth);
            return Group(node.children, node.executable, depth);
        }

    } // namespace

    std::string Usage(const Node& child, bool optional, const Node& /*commandRoot*/) {
        return UsageAt(child, optional, 0);
    }

    std::vector<std::string> UsageLines(const std::string& commandName, const Node& root) {
        const std::string head = "/" + commandName;
        if (root.children.empty()) return {head};
        // Children that continue identically share a line ("/difficulty
        // [peaceful|easy|normal|hard]"); the rest get one each, in order.
        std::vector<std::string> lines;
        std::vector<bool> used(root.children.size(), false);
        for (size_t i = 0; i < root.children.size(); ++i) {
            if (used[i]) continue;
            const std::string tail = Continuation(root.children[i], 1);
            std::vector<Node> group{root.children[i]};
            for (size_t j = i + 1; j < root.children.size(); ++j) {
                if (used[j]) continue;
                if (Continuation(root.children[j], 1) == tail) {
                    group.push_back(root.children[j]);
                    used[j] = true;
                }
            }
            lines.push_back(group.size() == 1 ? head + " " + UsageAt(group.front(), root.executable, 0)
                                              : head + " " + Group(group, root.executable, 0));
        }
        return lines;
    }

    // ── Wire form ───────────────────────────────────────────────────────────
    //   byte type, string name, byte flags (1 executable, 2 redirect),
    //   VarInt n + n strings (suggestions), VarInt n + n nodes (children)

    void WriteNode(Network::PacketBuffer& out, const Node& node) {
        out.WriteByte(static_cast<uint8_t>(node.type));
        out.WriteString(node.name);
        out.WriteByte(static_cast<uint8_t>((node.executable ? 1 : 0) | (node.redirectRoot ? 2 : 0)));
        out.WriteVarInt(static_cast<uint32_t>(node.suggestions.size()));
        for (const std::string& s : node.suggestions) out.WriteString(s);
        out.WriteVarInt(static_cast<uint32_t>(node.children.size()));
        for (const Node& child : node.children) WriteNode(out, child);
    }

    namespace {

        constexpr int      kMaxReadDepth  = 64;
        constexpr uint32_t kMaxReadNodes  = 1u << 16;   // per tree
        constexpr uint32_t kMaxListLength = 4096;

        bool ReadNodeAt(Network::PacketReader& in, Node& out, int depth, uint32_t& budget) {
            if (depth > kMaxReadDepth || budget == 0) return false;
            --budget;
            const uint8_t type = in.ReadByte();
            if (type >= static_cast<uint8_t>(Arg::Count)) return false;
            out.type = static_cast<Arg>(type);
            out.name = in.ReadString(256);
            const uint8_t flags = in.ReadByte();
            out.executable   = (flags & 1) != 0;
            out.redirectRoot = (flags & 2) != 0;
            const uint32_t suggestions = in.ReadVarInt();
            if (suggestions > kMaxListLength) return false;
            out.suggestions.clear();
            out.suggestions.reserve(suggestions);
            for (uint32_t i = 0; i < suggestions; ++i) out.suggestions.push_back(in.ReadString(256));
            const uint32_t children = in.ReadVarInt();
            if (children > kMaxListLength) return false;
            out.children.clear();
            out.children.resize(children);
            for (Node& child : out.children) {
                if (!ReadNodeAt(in, child, depth + 1, budget)) return false;
            }
            return true;
        }

    } // namespace

    bool ReadNode(Network::PacketReader& in, Node& out) {
        uint32_t budget = kMaxReadNodes;
        try {
            return ReadNodeAt(in, out, 0, budget);
        } catch (const std::exception&) {
            return false;
        }
    }

} // namespace Game::Cmd
