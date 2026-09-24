// File: src/common/entity/ModMobNbt.hpp
//
// The Twilight Forest / Aether mobs' own saved fields — each mod class's
// addAdditionalSaveData / readAdditionalSaveData — without the mob code
// reaching into the server's NBT layer. Mob::SaveModNbt / LoadModNbt take
// these two narrow interfaces; EntityNbt.cpp adapts them over its writer and
// the parsed compound, after the vanilla per-type fields, for every mob.
// Names are the mods' own keys ("Saddle", "SheepuffColor", "BirdType", ...).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Game {

    class ModNbtOut {
    public:
        virtual ~ModNbtOut() = default;
        virtual void Byte(std::string_view name, int8_t v) = 0;
        void Bool(std::string_view name, bool v) { Byte(name, v ? 1 : 0); }
        virtual void Int(std::string_view name, int32_t v) = 0;
        virtual void Float(std::string_view name, float v) = 0;
        virtual void String(std::string_view name, std::string_view v) = 0;
    };

    class ModNbtIn {
    public:
        virtual ~ModNbtIn() = default;
        virtual bool Has(const std::string& name) const = 0;
        virtual int8_t  Byte(const std::string& name, int8_t def) const = 0;
        bool Bool(const std::string& name, bool def) const { return Byte(name, def ? 1 : 0) != 0; }
        virtual int32_t Int(const std::string& name, int32_t def) const = 0;
        virtual float   Float(const std::string& name, float def) const = 0;
        virtual std::string String(const std::string& name, const std::string& def) const = 0;
    };

} // namespace Game
