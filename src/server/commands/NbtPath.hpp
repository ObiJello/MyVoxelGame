// File: src/server/commands/NbtPath.hpp
//
// MC net.minecraft.commands.arguments.NbtPathArgument (+ the NbtUtils /
// Tag helpers /data leans on), over the server's NBT tree (::World::NBTTag):
//
//   a.b            compound children         "quoted key".x   quoted keys
//   a[0]  a[-1]    list / array index        a[]              every element
//   a[{k:v}]       list elements matching    a{k:v}           the child, if it matches
//   {k:v}          the root, if it matches (first node only)
//
// get / getOrCreate / set / insert / remove follow MC's node rules exactly,
// including which operations create missing parents and what counts as a
// change. Errors carry MC's en_us text.
#pragma once

#include "server/world/storage/NBTParser.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Network { struct ChatSegmentData; }

namespace Server::Nbt {

    using TagPtr = ::World::NBTTagPtr;

    // Tag.copy — a deep copy.
    TagPtr Copy(const ::World::NBTTag& tag);
    std::shared_ptr<::World::NBTTagCompound> CopyCompound(const ::World::NBTTagCompound& tag);
    // Tag.equals — same type, same value (compounds by key set, lists in order).
    bool Equals(const ::World::NBTTag* a, const ::World::NBTTag* b);
    // NbtUtils.compareNbt(expected, actual, partialListMatches = true), with
    // the engine's "minecraft:" leniency for id strings (EntitySelector's
    // nbt= uses the same rule).
    bool Compare(const ::World::NBTTag* expected, const ::World::NBTTag* actual);
    // CompoundTag.merge: compounds merge recursively, everything else is
    // replaced by a copy.
    void Merge(::World::NBTTagCompound& target, const ::World::NBTTagCompound& source);
    // NbtPath.isTooDeep (512 levels).
    bool IsTooDeep(const ::World::NBTTag& tag, int depth);
    // Numeric tags (byte … double).
    bool IsNumeric(const ::World::NBTTag& tag);
    double NumericValue(const ::World::NBTTag& tag);

    // SNBT text (Tag.toString / SnbtPrinterTagVisitor, no indentation).
    std::string ToSnbt(const ::World::NBTTag& tag);
    // NbtUtils.toPrettyComponent (TextComponentTagVisitor, RichStyling): keys
    // aqua, strings green, numbers gold, type suffixes red — as chat runs.
    void AppendPretty(const ::World::NBTTag& tag, std::vector<Network::ChatSegmentData>& out);

    class Path {
    public:
        // NbtPathArgument.parse. False with MC's message on a bad path.
        static bool Parse(const std::string& text, Path& out, std::string& error);

        const std::string& Text() const { return m_text; }

        // The matches; empty with `error` "Found no elements matching …".
        bool Get(const TagPtr& root, std::vector<TagPtr>& out, std::string& error) const;
        int  CountMatching(const TagPtr& root) const;
        // MC getOrCreate: missing parents are created (each node's preferred
        // parent type), the last node's missing value from `create`.
        bool GetOrCreate(const TagPtr& root, const std::function<TagPtr()>& create,
                         std::vector<TagPtr>& out, std::string& error) const;
        // MC set / insert / remove: how many places changed.
        bool Set(const TagPtr& root, const ::World::NBTTag& value, int& changed, std::string& error) const;
        bool Insert(int index, const TagPtr& root, const std::vector<TagPtr>& values, int& changed,
                    std::string& error) const;
        int  Remove(const TagPtr& root) const;

        struct Node;
    private:
        std::string m_text;
        std::vector<std::shared_ptr<Node>> m_nodes;
        std::vector<size_t> m_endOffsets;   // where each node ends in m_text (the not-found message)
    };

} // namespace Server::Nbt
