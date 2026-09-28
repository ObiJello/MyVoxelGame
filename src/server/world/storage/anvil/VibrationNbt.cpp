// File: src/server/world/storage/anvil/VibrationNbt.cpp
#include "server/world/storage/anvil/VibrationNbt.hpp"

#include "common/core/Uuid.hpp"
#include "common/world/level/gameevent/GameEvent.hpp"

#include <algorithm>
#include <string>

namespace Game::Anvil {

    namespace {

        using ::World::NBTTagCompound;
        using ::World::NBTTagDouble;
        using ::World::NBTTagIntArray;
        using ::World::NBTTagList;
        using ::World::NBTTagString;

        std::shared_ptr<NBTTagCompound> Compound(const NBTTagCompound& parent, const std::string& key) {
            return std::dynamic_pointer_cast<NBTTagCompound>(parent.GetTag(key));
        }

        // UUIDUtil.CODEC: four ints.
        std::optional<Uuid> ReadUuid(const NBTTagCompound& c, const std::string& key) {
            auto arr = std::dynamic_pointer_cast<NBTTagIntArray>(c.GetTag(key));
            if (!arr || arr->value.size() != 4) return std::nullopt;
            return UuidFromIntArray(arr->value.data());
        }
        void WriteUuid(Nbt::Writer& w, std::string_view key, const Uuid& u) {
            int32_t ints[4];
            UuidToIntArray(u, ints);
            w.IntArray(key, ints, 4);
        }

        // Direction.CODEC's serialized names, in ordinal order.
        constexpr const char* kDirectionNames[6] = {"down", "up", "north", "south", "west", "east"};

        void WriteVibrationInfoBody(Nbt::Writer& w, const VibrationInfo& info) {
            w.String("game_event", "minecraft:" + std::string(GameEvents::Name(info.gameEvent)));
            w.Float("distance", info.distance);
            auto pos = w.BeginList("pos", Nbt::TagType::Double);
            w.ListDouble(pos, info.pos.x);
            w.ListDouble(pos, info.pos.y);
            w.ListDouble(pos, info.pos.z);
            w.EndList(pos);
            if (info.uuid) WriteUuid(w, "source", *info.uuid);
            if (info.projectileOwnerUuid) WriteUuid(w, "projectile_owner", *info.projectileOwnerUuid);
        }

        // VibrationInfo.CODEC. Lenient where MC is (the optional UUIDs);
        // anything malformed in a required field drops the whole info, as a
        // lenientOptionalFieldOf around it does.
        std::optional<VibrationInfo> ReadVibrationInfo(const NBTTagCompound& c) {
            auto eventTag = std::dynamic_pointer_cast<NBTTagString>(c.GetTag("game_event"));
            if (!eventTag) return std::nullopt;
            const std::optional<GameEventId> event = GameEvents::FromName(eventTag->value);
            if (!event) return std::nullopt;
            auto pos = std::dynamic_pointer_cast<NBTTagList>(c.GetTag("pos"));
            if (!pos || pos->value.size() != 3) return std::nullopt;
            double xyz[3];
            for (size_t i = 0; i < 3; ++i) {
                auto d = std::dynamic_pointer_cast<NBTTagDouble>(pos->value[i]);
                if (!d) return std::nullopt;
                xyz[i] = d->value;
            }
            VibrationInfo info;
            info.gameEvent = *event;
            info.distance = std::max(0.0f, c.GetValue<float>("distance", 0.0f));
            info.pos = glm::dvec3(xyz[0], xyz[1], xyz[2]);
            info.uuid = ReadUuid(c, "source");
            info.projectileOwnerUuid = ReadUuid(c, "projectile_owner");
            return info;
        }

    } // namespace

    void WriteVibrationData(Nbt::Writer& w, std::string_view name, const VibrationData& data) {
        w.BeginCompound(name);
        if (data.CurrentVibration()) {
            w.BeginCompound("event");
            WriteVibrationInfoBody(w, *data.CurrentVibration());
            w.EndCompound();
        }
        w.BeginCompound("selector");
        if (const std::optional<VibrationInfo> candidate = data.Selection().CandidateEvent()) {
            w.BeginCompound("event");
            WriteVibrationInfoBody(w, *candidate);
            w.EndCompound();
        }
        w.Long("tick", data.Selection().CandidateTick());
        w.EndCompound();
        w.Int("event_delay", data.GetTravelTimeInTicks());
        w.EndCompound();
    }

    VibrationData ReadVibrationData(const NBTTagCompound& parent, const std::string& name) {
        auto listener = Compound(parent, name);
        if (!listener) return VibrationData{};
        std::optional<VibrationInfo> current;
        if (auto event = Compound(*listener, "event")) current = ReadVibrationInfo(*event);
        // "selector" is a required field: without it the Data does not decode
        // and MC falls back to a fresh one.
        auto selectorTag = Compound(*listener, "selector");
        if (!selectorTag) return VibrationData{};
        std::optional<VibrationInfo> candidate;
        if (auto event = Compound(*selectorTag, "event")) candidate = ReadVibrationInfo(*event);
        VibrationSelector selector(candidate, selectorTag->GetValue<int64_t>("tick", -1));
        return VibrationData::Loaded(current, selector, listener->GetValue<int32_t>("event_delay", 0));
    }

    void WriteSculkCursors(Nbt::Writer& w, const SculkSpreader& spreader) {
        auto list = w.BeginList("cursors", Nbt::TagType::Compound);
        for (const SculkSpreader::ChargeCursor& cursor : spreader.Cursors()) {
            w.ListCompoundBegin(list);
            const int32_t pos[3] = {cursor.pos.x, cursor.pos.y, cursor.pos.z};
            w.IntArray("pos", pos, 3);
            w.Int("charge", cursor.charge);
            w.Int("decay_delay", cursor.decayDelay);
            w.Int("update_delay", cursor.updateDelay);
            if (cursor.facings) {
                auto faces = w.BeginList("facings", Nbt::TagType::String);
                for (int i = 0; i < 6; ++i) {
                    if (*cursor.facings & (1u << i)) w.ListString(faces, kDirectionNames[i]);
                }
                w.EndList(faces);
            }
            w.ListCompoundEnd(list);
        }
        w.EndList(list);
    }

    std::vector<SculkSpreader::ChargeCursor> ReadSculkCursors(const NBTTagCompound& parent) {
        std::vector<SculkSpreader::ChargeCursor> out;
        auto list = std::dynamic_pointer_cast<NBTTagList>(parent.GetTag("cursors"));
        if (!list) return out;
        for (const auto& element : list->value) {
            auto c = std::dynamic_pointer_cast<NBTTagCompound>(element);
            if (!c) continue;
            auto pos = std::dynamic_pointer_cast<NBTTagIntArray>(c->GetTag("pos"));
            if (!pos || pos->value.size() != 3) continue;
            SculkSpreader::ChargeCursor cursor;
            cursor.pos = glm::ivec3(pos->value[0], pos->value[1], pos->value[2]);
            cursor.charge = std::clamp(c->GetValue<int32_t>("charge", 0), 0, SculkSpreader::kMaxCharge);
            cursor.decayDelay = std::clamp(c->GetValue<int32_t>("decay_delay", 1), 0, 1);
            cursor.updateDelay = std::max(0, c->GetValue<int32_t>("update_delay", 0));
            if (auto faces = std::dynamic_pointer_cast<NBTTagList>(c->GetTag("facings"))) {
                uint8_t mask = 0;
                for (const auto& f : faces->value) {
                    auto s = std::dynamic_pointer_cast<NBTTagString>(f);
                    if (!s) continue;
                    for (int i = 0; i < 6; ++i) {
                        if (s->value == kDirectionNames[i]) mask = static_cast<uint8_t>(mask | (1u << i));
                    }
                }
                cursor.facings = mask;
            }
            out.push_back(cursor);
            // CODEC.sizeLimitedListOf(32): a longer list does not decode;
            // MC's load then keeps none. Past 32 here, the rest are dropped.
            if (out.size() >= static_cast<size_t>(SculkSpreader::kMaxCursors)) break;
        }
        return out;
    }

} // namespace Game::Anvil
