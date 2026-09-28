// File: src/server/commands/SnbtParser.hpp
//
// MC net.minecraft.nbt.TagParser (SNBT, the text form of NBT that command
// arguments take — CompoundTagArgument, NbtTagArgument) into the server's
// ::World::NBTTag tree, the same tags an Anvil region file decodes to. So a
// `/summon wolf ~ ~ ~ {variant:"ashen",CollarColor:5b}` compound goes through
// exactly the loader a saved wolf does (Game::Anvil::ApplyMobNbt).
//
// Grammar (26.3's SnbtGrammar, the parts a command ever types):
//   compound   {key:value, ...}      keys unquoted [A-Za-z0-9_.+-] or quoted
//   list       [value, ...]          any element types
//   arrays     [B;1b,2b] [I;1,2] [L;1l,2l]
//   strings    "..." or '...' (\\ \" \' \n \t \r \b \f \s \uXXXX escapes),
//              or an unquoted word that is not a number or boolean
//   numbers    integers default to int; suffixes b s i l (with an optional
//              u/s signedness prefix: 255ub, -1sb), hex 0x.., binary 0b..,
//              '_' digit separators; decimals / exponents default to double,
//              suffixes f d
//   booleans   true / false → byte 1 / 0
// Out-of-range integers are errors, as in Java.
#pragma once

#include "server/world/storage/NBTParser.hpp"

#include <memory>
#include <string>

namespace Server::Snbt {

    // A whole compound ("{...}" and nothing after it but whitespace). Null on
    // error, with `error` in MC's form: "<what> at position N: ...<--[HERE]".
    std::shared_ptr<::World::NBTTagCompound> ParseCompound(const std::string& text, std::string& error);

    // Any single value (a compound, list, string, number …), same rules.
    ::World::NBTTagPtr ParseValue(const std::string& text, std::string& error);

} // namespace Server::Snbt
