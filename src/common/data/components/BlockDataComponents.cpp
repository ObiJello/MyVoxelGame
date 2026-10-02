// File: src/common/data/components/BlockDataComponents.cpp
#include "BlockDataComponents.hpp"
#include "common/world/level/World.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/world/level/ILevelWrite.hpp"

#include "ComponentTooltips.hpp"
#include "common/text/Language.hpp"
#include "common/world/banner/BannerPatterns.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "server/world/storage/anvil/PlayerUuid.hpp"

#include <algorithm>
#include <stdexcept>

namespace Game {

    namespace {

        constexpr const char* kDyeNames[16] = {
            "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
            "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };

        std::string_view Bare(std::string_view id) {
            return id.rfind("minecraft:", 0) == 0 ? id.substr(10) : id;
        }

        // Sorted insert/replace into a name-keyed vector (the component maps).
        void PutSorted(std::vector<std::pair<std::string, std::string>>& v, std::string_view key,
                       std::string_view value) {
            auto it = std::lower_bound(v.begin(), v.end(), key,
                                       [](const auto& e, std::string_view k) { return e.first < k; });
            if (it != v.end() && it->first == key) { it->second = std::string(value); return; }
            v.insert(it, {std::string(key), std::string(value)});
        }

        std::optional<std::string_view> FindSorted(const std::vector<std::pair<std::string, std::string>>& v,
                                                   std::string_view key) {
            auto it = std::lower_bound(v.begin(), v.end(), key,
                                       [](const auto& e, std::string_view k) { return e.first < k; });
            if (it != v.end() && it->first == key) return std::string_view(it->second);
            return std::nullopt;
        }

    } // namespace

    // ── BlockItemStateProperties ──────────────────────────────────────────

    std::optional<std::string_view> BlockItemStateProperties::Get(std::string_view name) const {
        return FindSorted(properties, name);
    }

    BlockItemStateProperties BlockItemStateProperties::With(std::string_view name, std::string_view value) const {
        BlockItemStateProperties out = *this;
        PutSorted(out.properties, name, value);
        return out;
    }

    BlockState BlockItemStateProperties::Apply(BlockState state) const {
        if (properties.empty()) return state;
        const BlockID block = state.Block();
        const uint16_t count = BlockStates::PropertyCount(block);
        for (uint16_t slot = 0; slot < count; ++slot) {
            const PropertyId prop = BlockStates::PropertyAt(block, slot);
            if (auto value = Get(BlockStates::PropertyName(prop))) {
                // updateState: property.getValue(value).map(setValue).orElse(state)
                // — SetName leaves the state alone for a value the property
                // does not have.
                state = state.SetName(prop, *value);
            }
        }
        return state;
    }

    BlockState ApplyBlockItemStateProperties(const ItemStack& stack, BlockState state) {
        if (stack.IsEmpty()) return state;
        const auto props = stack.get(DataComponents::BLOCK_STATE);
        return props ? props->Apply(state) : state;
    }

    // ── DebugStickState ──────────────────────────────────────────────────

    std::optional<std::string_view> DebugStickState::Get(std::string_view block) const {
        return FindSorted(properties, block);
    }

    DebugStickState DebugStickState::With(std::string_view block, std::string_view property) const {
        DebugStickState out = *this;
        PutSorted(out.properties, block, property);
        return out;
    }

    // ── SignTextComponent ────────────────────────────────────────────────

    bool SignTextComponent::IsEmpty() const {
        if (filteredMessages || color != 15 || glowing) return false;
        for (const Text::Component& m : messages) {
            if (!Text::GetString(m).empty()) return false;
        }
        return true;
    }

    // ── ResolvableProfile ────────────────────────────────────────────────

    std::array<int32_t, 4> ResolvableProfile::EffectiveId() const {
        if (id) return *id;
        if (name) {
            const Anvil::PlayerUuid bytes = Anvil::OfflinePlayerUuid(*name);
            std::array<int32_t, 4> out{};
            for (size_t i = 0; i < 4; ++i) {
                out[i] = static_cast<int32_t>((static_cast<uint32_t>(bytes[i * 4]) << 24) |
                                              (static_cast<uint32_t>(bytes[i * 4 + 1]) << 16) |
                                              (static_cast<uint32_t>(bytes[i * 4 + 2]) << 8) |
                                              static_cast<uint32_t>(bytes[i * 4 + 3]));
            }
            return out;
        }
        return {0, 0, 0, 0};   // Util.NIL_UUID
    }

    namespace BlockData {

        bool OnlyOpCanSetNbt(std::string_view blockEntityType) {
            // MC BlockEntityType: every type built with OP_ONLY_CUSTOM_DATA —
            // SPAWNER, COMMAND_BLOCK, STRUCTURE_BLOCK, JIGSAW, LECTERN (its
            // book), TRIAL_SPAWNER, VAULT, TEST_BLOCK, TEST_INSTANCE_BLOCK.
            static constexpr std::string_view kOpOnly[] = {
                "spawner", "command_block", "structure_block", "jigsaw", "lectern",
                "trial_spawner", "vault", "test_block", "test_instance_block" };
            const std::string_view bare = Bare(blockEntityType);
            for (std::string_view t : kOpOnly) if (t == bare) return true;
            return false;
        }

        bool DebugStickInteract(ILevelWrite& level, IUsePlayer& player, ItemStack& stick,
                                const glm::ivec3& pos, bool update) {
            if (level.IsClientSide() || !player.isCreative()) return false;
            const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
            const BlockID block = state.Block();
            const std::string blockId = "minecraft:" + std::string(BlockRegistry::Get(block).registrySlug);
            const uint16_t count = BlockStates::PropertyCount(block);
            const auto message = [&player](const char* key, std::vector<std::string> args) {
                std::vector<Text::Component> with;
                for (std::string& a : args) with.push_back(Text::Component::Literal(std::move(a)));
                player.DisplayClientMessage(Text::GetString(Text::Component::Translatable(key, std::move(with))),
                                            /*actionBar=*/true);
            };
            if (count == 0) {
                message("item.minecraft.debug_stick.empty", {blockId});
                return false;
            }
            const auto stickState = stick.get(DataComponents::DEBUG_STICK_STATE);
            if (!stickState) return false;
            // The selected property: the stick's entry for this block, when
            // the block has a property of that name.
            int selected = -1;
            if (auto name = stickState->Get(blockId)) {
                for (uint16_t i = 0; i < count; ++i) {
                    if (BlockStates::PropertyName(BlockStates::PropertyAt(block, i)) == *name) selected = i;
                }
            }
            const bool backwards = player.IsSneaking();   // isSecondaryUseActive
            const auto valueName = [](BlockState s, PropertyId prop) {
                return std::string(s.GetName(prop));
            };
            if (update) {
                // cycleState: the next (previous) value of the property.
                const PropertyId prop = BlockStates::PropertyAt(block, static_cast<uint16_t>(selected < 0 ? 0 : selected));
                const int values = BlockStates::PropertyValueCount(prop);
                const int current = state.GetIndex(prop);
                const int next = ((backwards ? current - 1 : current + 1) % values + values) % values;
                const BlockState newState = state.SetIndex(prop, next);
                // Block.UPDATE_CLIENTS | UPDATE_KNOWN_SHAPE (18).
                level.SetBlock(pos.x, pos.y, pos.z, newState, World::UpdateFlags::UpdateClients | World::UpdateFlags::KnownShape);
                message("item.minecraft.debug_stick.update",
                        {std::string(BlockStates::PropertyName(prop)), valueName(newState, prop)});
                return true;
            }
            // getRelative: the next (previous) property, wrapping.
            const int next = selected < 0 ? 0
                : ((backwards ? selected - 1 : selected + 1) % count + count) % count;
            const PropertyId prop = BlockStates::PropertyAt(block, static_cast<uint16_t>(next));
            stick.components.set(DataComponents::DEBUG_STICK_STATE,
                                 stickState->With(blockId, BlockStates::PropertyName(prop)));
            message("item.minecraft.debug_stick.select",
                    {std::string(BlockStates::PropertyName(prop)), valueName(state, prop)});
            return true;
        }

        UseResult UseOnDebugStick(const UseOnContext& ctx, ItemStack& stack) {
            // DebugStickItem.useOn: the update half, on the server; the
            // client consumes the click.
            if (!ctx.world || !ctx.player) return UseResult::Pass;
            if (ctx.world->IsClientSide()) return UseResult::Success;
            if (!DebugStickInteract(*ctx.world, *ctx.player, stack, ctx.hitResult.blockPos, /*update=*/true)) {
                return UseResult::Fail;
            }
            ctx.player->markSlotDirty(ctx.player->handSlotIndex(ctx.hand));
            return UseResult::Success;
        }

        std::array<std::string, 4> SignLines(const SignTextComponent& text) {
            std::array<std::string, 4> out;
            for (size_t i = 0; i < 4; ++i) out[i] = Text::GetString(text.messages[i]);
            return out;
        }

        SignTextComponent SignComponentFromLines(const std::array<std::string, 4>& lines, int32_t color,
                                                 bool glowing) {
            SignTextComponent out;
            for (size_t i = 0; i < 4; ++i) out.messages[i] = Text::Component::Literal(lines[i]);
            out.color = color;
            out.glowing = glowing;
            return out;
        }

    } // namespace BlockData

} // namespace Game

namespace Game::DataComponents {

    namespace {

        // ── Wire codecs ───────────────────────────────────────────────────

        // TypedEntityData.streamCodec: the type (registry), then the tag.
        void SerTypedBlockEntity(Network::PacketBuffer& b, const TypedBlockEntityData& v) {
            b.WriteString(v.type);
            SerNbtCompoundValue(b, v.tag);
        }
        TypedBlockEntityData DeTypedBlockEntity(Network::PacketReader& r) {
            TypedBlockEntityData v;
            v.type = r.ReadString(256);
            v.tag = DeNbtCompoundValue(r);
            return v;
        }

        void SerStringPairs(Network::PacketBuffer& b, const std::vector<std::pair<std::string, std::string>>& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.size()));
            for (const auto& [k, value] : v) {
                b.WriteString(k);
                b.WriteString(value);
            }
        }
        std::vector<std::pair<std::string, std::string>> DeStringPairs(Network::PacketReader& r) {
            std::vector<std::pair<std::string, std::string>> v;
            const uint32_t n = r.ReadVarInt();
            if (n > 1024) throw std::runtime_error("item component map too large");
            for (uint32_t i = 0; i < n; ++i) {
                std::string k = r.ReadString(32767);
                std::string value = r.ReadString(32767);
                PutSorted(v, k, value);
            }
            return v;
        }

        // BlockItemStateProperties.STREAM_CODEC: a string → string map.
        void SerBlockState(Network::PacketBuffer& b, const BlockItemStateProperties& v) { SerStringPairs(b, v.properties); }
        BlockItemStateProperties DeBlockState(Network::PacketReader& r) { return {DeStringPairs(r)}; }

        void SerDebugStick(Network::PacketBuffer& b, const DebugStickState& v) { SerStringPairs(b, v.properties); }
        DebugStickState DeDebugStick(Network::PacketReader& r) { return {DeStringPairs(r)}; }

        // Bees.STREAM_CODEC: Occupant.STREAM_CODEC list — entity data,
        // ticks_in_hive, min_ticks_in_hive (VAR_INTs).
        void SerBees(Network::PacketBuffer& b, const Bees& v) {
            b.WriteVarInt(static_cast<uint32_t>(v.bees.size()));
            for (const BeehiveOccupant& o : v.bees) {
                b.WriteString(o.entityType);
                SerNbtCompoundValue(b, o.entityData);
                b.WriteVarInt(static_cast<uint32_t>(o.ticksInHive));
                b.WriteVarInt(static_cast<uint32_t>(o.minTicksInHive));
            }
        }
        Bees DeBees(Network::PacketReader& r) {
            Bees v;
            const uint32_t n = r.ReadVarInt();
            if (n > 256) throw std::runtime_error("bees: too many occupants");
            for (uint32_t i = 0; i < n; ++i) {
                BeehiveOccupant o;
                o.entityType = r.ReadString(256);
                o.entityData = DeNbtCompoundValue(r);
                o.ticksInHive = static_cast<int32_t>(r.ReadVarInt());
                o.minTicksInHive = static_cast<int32_t>(r.ReadVarInt());
                v.bees.push_back(std::move(o));
            }
            return v;
        }

        void SerLock(Network::PacketBuffer& b, const LockCode& v) { SerNbtCompoundValue(b, v.predicate); }
        LockCode DeLock(Network::PacketReader& r) { return {DeNbtCompoundValue(r)}; }

        void SerLoot(Network::PacketBuffer& b, const SeededContainerLoot& v) {
            b.WriteString(v.lootTable);
            b.WriteLong(static_cast<uint64_t>(v.seed));
        }
        SeededContainerLoot DeLoot(Network::PacketReader& r) {
            SeededContainerLoot v;
            v.lootTable = r.ReadString(32767);
            v.seed = static_cast<int64_t>(r.ReadLong());
            return v;
        }

        // SignText.STREAM_CODEC: 4 messages, optional 4 filtered messages,
        // DyeColor, has_glowing_text.
        void SerSignText(Network::PacketBuffer& b, const SignTextComponent& v) {
            for (const Text::Component& m : v.messages) Text::Write(b, m);
            b.WriteByte(v.filteredMessages ? 1 : 0);
            if (v.filteredMessages) for (const Text::Component& m : *v.filteredMessages) Text::Write(b, m);
            b.WriteVarInt(static_cast<uint32_t>(v.color & 15));
            b.WriteByte(v.glowing ? 1 : 0);
        }
        SignTextComponent DeSignText(Network::PacketReader& r) {
            SignTextComponent v;
            for (Text::Component& m : v.messages) m = Text::Read(r);
            if (r.ReadByte() != 0) {
                std::array<Text::Component, 4> filtered;
                for (Text::Component& m : filtered) m = Text::Read(r);
                v.filteredMessages = std::move(filtered);
            }
            v.color = static_cast<int32_t>(r.ReadVarInt() & 15u);
            v.glowing = r.ReadByte() != 0;
            return v;
        }

        void SerUnit(Network::PacketBuffer&, const bool&) {}
        bool DeUnit(Network::PacketReader&) { return true; }

        void SerDye(Network::PacketBuffer& b, const int32_t& v) { b.WriteVarInt(static_cast<uint32_t>(v & 15)); }
        int32_t DeDye(Network::PacketReader& r) { return static_cast<int32_t>(r.ReadVarInt() & 15u); }

        void SerId(Network::PacketBuffer& b, const std::string& v) { b.WriteString(v); }
        std::string DeId(Network::PacketReader& r) { return r.ReadString(32767); }

        void SerOptString(Network::PacketBuffer& b, const std::optional<std::string>& v) {
            b.WriteByte(v ? 1 : 0);
            if (v) b.WriteString(*v);
        }
        std::optional<std::string> DeOptString(Network::PacketReader& r) {
            if (r.ReadByte() == 0) return std::nullopt;
            return r.ReadString(32767);
        }

        // ResolvableProfile.STREAM_CODEC: the partial profile (name?, id?,
        // properties) and the skin patch (texture?, cape?, elytra?, model?).
        void SerProfile(Network::PacketBuffer& b, const ResolvableProfile& v) {
            SerOptString(b, v.name);
            b.WriteByte(v.id ? 1 : 0);
            if (v.id) for (int32_t w : *v.id) b.WriteInt(static_cast<uint32_t>(w));
            b.WriteVarInt(static_cast<uint32_t>(v.properties.size()));
            for (const ResolvableProfile::Property& p : v.properties) {
                b.WriteString(p.name);
                b.WriteString(p.value);
                SerOptString(b, p.signature);
            }
            b.WriteString(v.texture);
            b.WriteString(v.cape);
            b.WriteString(v.elytra);
            b.WriteString(v.model);
        }
        ResolvableProfile DeProfile(Network::PacketReader& r) {
            ResolvableProfile v;
            v.name = DeOptString(r);
            if (r.ReadByte() != 0) {
                std::array<int32_t, 4> id{};
                for (int32_t& w : id) w = static_cast<int32_t>(r.ReadInt());
                v.id = id;
            }
            const uint32_t n = r.ReadVarInt();
            if (n > 16) throw std::runtime_error("profile: too many properties");
            for (uint32_t i = 0; i < n; ++i) {
                ResolvableProfile::Property p;
                p.name = r.ReadString(64);
                p.value = r.ReadString(32767);
                p.signature = DeOptString(r);
                v.properties.push_back(std::move(p));
            }
            v.texture = r.ReadString(32767);
            v.cape = r.ReadString(32767);
            v.elytra = r.ReadString(32767);
            v.model = r.ReadString(16);
            return v;
        }

    } // namespace

    const DataComponentType<TypedBlockEntityData>     BLOCK_ENTITY_DATA {"block_entity_data", 340, &SerTypedBlockEntity, &DeTypedBlockEntity};
    const DataComponentType<BlockItemStateProperties> BLOCK_STATE       {"block_state",       341, &SerBlockState,       &DeBlockState};
    const DataComponentType<Bees>                     BEES              {"bees",              342, &SerBees,             &DeBees};
    const DataComponentType<LockCode>                 LOCK              {"lock",              343, &SerLock,             &DeLock};
    const DataComponentType<SeededContainerLoot>      CONTAINER_LOOT    {"container_loot",    344, &SerLoot,             &DeLoot};
    const DataComponentType<SignTextComponent>        SIGN_TEXT_FRONT   {"sign_text_front",   345, &SerSignText,         &DeSignText};
    const DataComponentType<SignTextComponent>        SIGN_TEXT_BACK    {"sign_text_back",    346, &SerSignText,         &DeSignText};
    const DataComponentType<bool>                     WAXED             {"waxed",             347, &SerUnit,             &DeUnit};
    const DataComponentType<int32_t>                  CUSHION_COLOR     {"cushion/color",     348, &SerDye,              &DeDye};
    const DataComponentType<std::string>              NOTE_BLOCK_SOUND  {"note_block_sound",  349, &SerId,               &DeId};
    const DataComponentType<ResolvableProfile>        PROFILE           {"profile",           350, &SerProfile,          &DeProfile};
    const DataComponentType<DebugStickState>          DEBUG_STICK_STATE {"debug_stick_state", 351, &SerDebugStick,       &DeDebugStick};
    const DataComponentType<int32_t>                  BASE_COLOR        {"base_color",        352, &SerDye,              &DeDye};

} // namespace Game::DataComponents

// ── Tooltip providers (MC TooltipProvider.addToTooltip) ─────────────────────

namespace Game {

    namespace {

        using ComponentTooltips::Line;
        using ComponentTooltips::Slot;

        std::string Translate(const char* key, std::vector<std::string> args, const char* fallback = nullptr) {
            std::vector<Text::Component> with;
            with.reserve(args.size());
            for (std::string& a : args) with.push_back(Text::Component::Literal(std::move(a)));
            if (fallback && !Language::GetOrDefault(key, "").size()) {
                // A key the shipped language file predates: its English text.
                std::string text = fallback;
                for (const Text::Component& a : with) {
                    const size_t at = text.find("%s");
                    if (at == std::string::npos) break;
                    text.replace(at, 2, Text::GetString(a));
                }
                return text;
            }
            return Text::GetString(Text::Component::Translatable(key, std::move(with)));
        }

        // Bees.addToTooltip: "Bees: n / 3" in grey.
        void BeesTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            const auto bees = stack.get(DataComponents::BEES);
            if (!bees) return;
            out.push_back({Translate("container.beehive.bees", {std::to_string(bees->bees.size()), "3"}),
                           ComponentTooltips::kGray, std::nullopt});
        }

        // BlockItemStateProperties.addToTooltip: a honey_level, as "Honey:
        // n / 5" in grey.
        void BlockStateTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            const auto props = stack.get(DataComponents::BLOCK_STATE);
            if (!props) return;
            const auto honey = props->Get("honey_level");
            if (!honey) return;
            // HONEY_LEVEL is IntegerProperty 0..5: a value it cannot parse is null.
            int level = -1;
            try { level = std::stoi(std::string(*honey)); } catch (...) { level = -1; }
            if (level < 0 || level > 5) return;
            out.push_back({Translate("container.beehive.honey", {std::to_string(level), "5"}),
                           ComponentTooltips::kGray, std::nullopt});
        }

        // SeededContainerLoot.addToTooltip: UNKNOWN_CONTENTS.
        void ContainerLootTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            if (!stack.get(DataComponents::CONTAINER_LOOT)) return;
            out.push_back({Translate("item.container.loot_table.unknown", {}, "Unknown loot table"),
                           ComponentTooltips::kWhite, std::nullopt});
        }

        // SignText.createTooltip(title): nothing for SignText.EMPTY, else the
        // grey title and each message indented by two spaces.
        void SignTooltip(const SignTextComponent& text, const char* titleKey, std::vector<Line>& out) {
            if (text.IsEmpty()) return;
            out.push_back({Language::Get(titleKey), ComponentTooltips::kGray, std::nullopt});
            for (const Text::Component& message : text.messages) {
                Text::Component line = Text::Component::Literal("  ");
                line.Append(message);
                out.push_back({Text::GetString(line), ComponentTooltips::kWhite, line});
            }
        }
        void SignFrontTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            if (auto text = stack.get(DataComponents::SIGN_TEXT_FRONT)) SignTooltip(*text, "sign.front_text", out);
        }
        void SignBackTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            if (auto text = stack.get(DataComponents::SIGN_TEXT_BACK)) SignTooltip(*text, "sign.back_text", out);
        }

        // ResolvableProfile: a Dynamic profile says so ("Dynamic", grey); a
        // Static one adds nothing.
        void ProfileTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            const auto profile = stack.get(DataComponents::PROFILE);
            if (!profile || !profile->IsDynamic()) return;
            out.push_back({Language::Get("component.profile.dynamic"), ComponentTooltips::kGray, std::nullopt});
        }

        // BannerPatternLayers.addToTooltip: the first six layers' descriptions
        // ("<translation_key>.<dye>") in grey.
        void BannerPatternsTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            const auto patterns = stack.get(DataComponents::BANNER_PATTERNS);
            if (!patterns) return;
            const size_t n = std::min<size_t>(patterns->layers.size(), 6);
            for (size_t i = 0; i < n; ++i) {
                const BannerPatternLayer& layer = patterns->layers[i];
                std::string key = BannerPatterns::TranslationKeyOf(layer.pattern);
                if (key.empty()) key = "block.minecraft.banner." + std::string(Bare(layer.pattern));
                out.push_back({Language::Get(key + "." + kDyeNames[layer.color & 15]), ComponentTooltips::kGray,
                               std::nullopt});
            }
        }

        // ItemStack.addDetailsToTooltip's spawner branch (Spawner.
        // appendHoverText with "SpawnData"): the spawned entity's name in
        // grey, else the blank line and the two "Interact with Spawn Egg:" /
        // " Sets Mob Type" lines.
        void SpawnerTooltip(const ItemStack& stack, const ComponentTooltips::Context&, std::vector<Line>& out) {
            const bool isSpawner = stack.itemId == ItemRegistry::FromBlock(BlockID::Spawner) ||
                                   stack.itemId == ItemRegistry::FromBlock(BlockID::TrialSpawner);
            if (!isSpawner) return;
            // display.shows(BLOCK_ENTITY_DATA) — the builder does not test a
            // provider registered without a type.
            if (auto display = stack.get(DataComponents::TOOLTIP_DISPLAY);
                display && !display->Shows(DataComponents::BLOCK_ENTITY_DATA)) return;
            std::string entityId;
            if (auto data = stack.get(DataComponents::BLOCK_ENTITY_DATA)) {
                auto spawnData = std::dynamic_pointer_cast<::World::NBTTagCompound>(data->tag.Tag().GetTag("SpawnData"));
                if (spawnData) {
                    if (auto entity = std::dynamic_pointer_cast<::World::NBTTagCompound>(spawnData->GetTag("entity"))) {
                        entityId = entity->GetValue<std::string>("id", "");
                    }
                }
            }
            if (!entityId.empty()) {
                const std::string_view id(entityId);
                const size_t colon = id.find(':');
                const std::string ns = colon == std::string_view::npos ? "minecraft" : std::string(id.substr(0, colon));
                const std::string path(colon == std::string_view::npos ? id : id.substr(colon + 1));
                out.push_back({Language::Get("entity." + ns + "." + path), ComponentTooltips::kGray, std::nullopt});
                return;
            }
            out.push_back({"", ComponentTooltips::kWhite, std::nullopt});
            out.push_back({Language::Get("block.minecraft.spawner.desc1"), ComponentTooltips::kGray, std::nullopt});
            out.push_back({" " + Language::Get("block.minecraft.spawner.desc2"), ComponentTooltips::kBlue, std::nullopt});
        }

        const ComponentTooltips::Registrar kBeesTooltip(Slot::Bees, &DataComponents::BEES, &BeesTooltip);
        const ComponentTooltips::Registrar kContainerLootTooltip(Slot::ContainerLoot, &DataComponents::CONTAINER_LOOT,
                                                                 &ContainerLootTooltip);
        const ComponentTooltips::Registrar kBannerTooltip(Slot::BannerPatterns, &DataComponents::BANNER_PATTERNS,
                                                          &BannerPatternsTooltip);
        const ComponentTooltips::Registrar kProfileTooltip(Slot::Profile, &DataComponents::PROFILE, &ProfileTooltip);
        const ComponentTooltips::Registrar kSignFrontTooltip(Slot::SignTextFront, &DataComponents::SIGN_TEXT_FRONT,
                                                             &SignFrontTooltip);
        const ComponentTooltips::Registrar kSignBackTooltip(Slot::SignTextBack, &DataComponents::SIGN_TEXT_BACK,
                                                            &SignBackTooltip);
        const ComponentTooltips::Registrar kBlockStateTooltip(Slot::BlockState, &DataComponents::BLOCK_STATE,
                                                              &BlockStateTooltip);
        const ComponentTooltips::Registrar kSpawnerTooltip(Slot::SpawnerBlockEntityData, nullptr, &SpawnerTooltip);

    } // namespace

} // namespace Game
