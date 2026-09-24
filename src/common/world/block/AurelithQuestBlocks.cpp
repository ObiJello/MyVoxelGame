// File: src/common/world/block/AurelithQuestBlocks.cpp
//
// See AurelithQuestBlocks.hpp.
#include "common/world/block/AurelithQuestBlocks.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/network/packets/game/AurelithS2CPacket.hpp"
#include "common/sound/AurelithSoundCues.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/block/entity/AurelithBlockEntities.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/level/WorldDrops.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Game {

    namespace Aurelith {

        // ── The voice keys ────────────────────────────────────────────────

        std::optional<Voice> VoiceOfKey(ItemID id) {
            if (id == Items::SopranoVoiceKey) return Voice::Soprano;
            if (id == Items::AltoVoiceKey)    return Voice::Alto;
            if (id == Items::TenorVoiceKey)   return Voice::Tenor;
            if (id == Items::BassVoiceKey)    return Voice::Bass;
            return std::nullopt;
        }

        ItemID KeyOf(Voice v) {
            switch (v) {
                case Voice::Soprano: return Items::SopranoVoiceKey;
                case Voice::Alto:    return Items::AltoVoiceKey;
                case Voice::Tenor:   return Items::TenorVoiceKey;
                case Voice::Bass:    return Items::BassVoiceKey;
            }
            return Items::SopranoVoiceKey;
        }

        // ── The dormant city's lights ─────────────────────────────────────

        namespace {
            struct Twin { BlockID dim; BlockID lit; };
            constexpr Twin kTwins[] = {
                { BlockID::DimCyanLumenPanel,   BlockID::CyanLumenPanel },
                { BlockID::DimVioletLumenPanel, BlockID::VioletLumenPanel },
                { BlockID::DimAmberLumenPanel,  BlockID::AmberLumenPanel },
                { BlockID::DimLumenStrip,       BlockID::LumenStrip },
                { BlockID::DimStaveStone,       BlockID::StaveStone },
                { BlockID::DimChoirLamp,        BlockID::ChoirLamp },
            };
        } // namespace

        BlockID LitTwinOf(BlockID dim) {
            for (const Twin& t : kTwins) if (t.dim == dim) return t.lit;
            return BlockID::Air;
        }

        BlockID DimTwinOf(BlockID lit) {
            for (const Twin& t : kTwins) if (t.lit == lit) return t.dim;
            return BlockID::Air;
        }

        BlockState ToLit(BlockState s) {
            const BlockID twin = LitTwinOf(s.Block());
            return twin == BlockID::Air ? s : BlockStates::FromIndex(twin, s.Index());
        }

        BlockState ToDim(BlockState s) {
            const BlockID twin = DimTwinOf(s.Block());
            return twin == BlockID::Air ? s : BlockStates::FromIndex(twin, s.Index());
        }

        std::string_view NoteColourOf(BlockID plinth) {
            switch (plinth) {
                case BlockID::CyanLumenPanel:   case BlockID::DimCyanLumenPanel:   return "cyan";
                case BlockID::VioletLumenPanel: case BlockID::DimVioletLumenPanel: return "violet";
                case BlockID::AmberLumenPanel:  case BlockID::DimAmberLumenPanel:  return "amber";
                case BlockID::StaveStone:       case BlockID::DimStaveStone:       return "stave";
                default:                                                           return "";
            }
        }

    } // namespace Aurelith

    namespace {

        using namespace Aurelith;

        // Hand `stack` to the player: into the (empty) hand it was clicked
        // with, else at their feet with no pickup delay, so it is collected
        // at once (a full inventory leaves it on the floor, as MC's
        // Player.addItem → drop).
        void GiveToPlayer(ILevelWrite& world, IUsePlayer& player, uint32_t hand, const ItemStack& stack) {
            if (stack.IsEmpty()) return;
            ItemStack& held = player.getItemInHand(hand);
            if (held.IsEmpty()) {
                held = stack;
                player.markSlotDirty(player.handSlotIndex(hand));
                return;
            }
            SpawnItemEntity(world.GetDimension(), player.getPosition() + glm::dvec3(0.0, 0.3, 0.0),
                            glm::dvec3(0.0), stack, 0);
        }

        // ── chord_socket ─────────────────────────────────────────────────

        UseResult SocketUseItemOn(ItemStack& stack, ILevelWrite* world, const glm::ivec3& pos,
                                  IUsePlayer* player, uint32_t hand, const BlockHitResult& hit) {
            (void)hit;
            const std::optional<Voice> voice = stack.IsEmpty() ? std::nullopt : VoiceOfKey(stack.itemId);
            // Anything but a voice key: the empty-hand behaviour (take back).
            if (!voice) return UseResult::TryEmptyHandInteraction;
            if (!world) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;

            auto* socket = dynamic_cast<ChordSocketBlockEntity*>(world->GetBlockEntity(pos));
            if (!socket) return UseResult::Pass;
            if (socket->IsLocked()) {
                SoundCues::Play(*world, pos, Sounds::kSocketLocked, SoundSource::Blocks, 0.8f, 1.0f);
                if (player) player->DisplayClientMessage("The Chord holds the keys now.", true);
                return UseResult::Consume;
            }
            if (socket->HasKey()) {
                if (player) player->DisplayClientMessage("This socket already holds a voice.", true);
                return UseResult::Consume;
            }
            ItemStack key = stack;
            key.count = 1;
            if (!(player && player->isCreative())) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
                if (player) player->markSlotDirty(player->handSlotIndex(hand));
            }
            socket->Seat(std::move(key), world->GameTime());
            // The key's own note, where it sits in the Chord, and a ring of
            // its colour lifting off the socket.
            SoundCues::Play(*world, pos, Sounds::kSocketSeat, SoundSource::Blocks, 1.2f, VoicePitch(*voice));
            BroadcastBurst(*world, glm::dvec3(pos) + glm::dvec3(0.5, 1.0, 0.5),
                           static_cast<uint8_t>(Network::AurelithS2CPacket::BurstStyle::KeySeat),
                           VoiceColour(*voice));
            // The server decides what four seated keys mean.
            OnSocketSeated(*world, pos, player);
            return UseResult::Success;
        }

        UseResult SocketUseWithoutItem(ILevelWrite* world, const glm::ivec3& pos,
                                       IUsePlayer* player, const BlockHitResult& hit) {
            (void)hit;
            if (!world) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;
            auto* socket = dynamic_cast<ChordSocketBlockEntity*>(world->GetBlockEntity(pos));
            if (!socket) return UseResult::Pass;
            if (!socket->HasKey()) {
                if (player) player->DisplayClientMessage("An empty socket, shaped for a voice key.", true);
                return UseResult::Consume;
            }
            if (socket->IsLocked()) {
                SoundCues::Play(*world, pos, Sounds::kSocketLocked, SoundSource::Blocks, 0.8f, 1.0f);
                if (player) player->DisplayClientMessage("The Chord holds the keys now.", true);
                return UseResult::Consume;
            }
            ItemStack key = socket->TakeKey();
            SoundCues::Play(*world, pos, Sounds::kSocketTake, SoundSource::Blocks, 1.0f, 1.0f);
            if (player) {
                // The hand the click came from is the main hand here (MC's
                // useWithoutItem is only reached for it or an empty off hand).
                GiveToPlayer(*world, *player, 0, key);
            }
            return UseResult::Success;
        }

        // ── voice_pedestal ───────────────────────────────────────────────

        UseResult PedestalUseItemOn(ItemStack& stack, ILevelWrite* world, const glm::ivec3& pos,
                                    IUsePlayer* player, uint32_t hand, const BlockHitResult& hit) {
            (void)hit;
            if (stack.IsEmpty()) return UseResult::TryEmptyHandInteraction;
            if (!world) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;
            auto* pedestal = dynamic_cast<VoicePedestalBlockEntity*>(world->GetBlockEntity(pos));
            if (!pedestal) return UseResult::Pass;
            // Occupied: lift what is there instead (the hand keeps its stack).
            if (pedestal->HasItem()) return UseResult::TryEmptyHandInteraction;
            ItemStack one = stack;
            one.count = 1;
            if (!(player && player->isCreative())) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
                if (player) player->markSlotDirty(player->handSlotIndex(hand));
            }
            pedestal->SetItem(std::move(one));
            SoundCues::Play(*world, pos, Sounds::kPedestalPlace, SoundSource::Blocks, 1.0f, 1.0f);
            return UseResult::Success;
        }

        UseResult PedestalUseWithoutItem(ILevelWrite* world, const glm::ivec3& pos,
                                         IUsePlayer* player, const BlockHitResult& hit) {
            (void)hit;
            if (!world) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;
            auto* pedestal = dynamic_cast<VoicePedestalBlockEntity*>(world->GetBlockEntity(pos));
            if (!pedestal || !pedestal->HasItem()) return UseResult::Pass;
            ItemStack item = pedestal->TakeItem();
            const std::optional<Voice> voice = VoiceOfKey(item.itemId);
            BroadcastBurst(*world, glm::dvec3(pos) + glm::dvec3(0.5, 1.0, 0.5),
                           static_cast<uint8_t>(Network::AurelithS2CPacket::BurstStyle::Pedestal),
                           voice ? VoiceColour(*voice) : 0x5FF3FFu);
            SoundCues::Play(*world, pos, Sounds::kPedestalTake, SoundSource::Blocks, 1.0f,
                             voice ? VoicePitch(*voice) : 1.0f);
            if (player) {
                GiveToPlayer(*world, *player, 0, item);
                if (voice) {
                    const std::string name(VoiceName(*voice));
                    std::string title = name;
                    if (!title.empty()) title[0] = static_cast<char>(title[0] - 'a' + 'A');
                    player->DisplayClientMessage("The " + title + "'s voice key hums in your hand.", true);
                }
            }
            return UseResult::Success;
        }

        // ── choir_cabinet ────────────────────────────────────────────────

        UseResult CabinetUseWithoutItem(ILevelWrite* world, const glm::ivec3& pos,
                                        IUsePlayer* player, const BlockHitResult& hit) {
            (void)hit;
            if (!world) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;
            auto* cabinet = dynamic_cast<ChoirCabinetBlockEntity*>(world->GetBlockEntity(pos));
            if (!cabinet) return UseResult::Pass;
            if (cabinet->IsSolved()) {
                if (player) player->DisplayClientMessage("The cabinet stands open, and empty.", true);
                return UseResult::Consume;
            }
            SoundCues::Play(*world, pos, Sounds::kCabinetLocked, SoundSource::Blocks, 0.9f, 1.0f);
            if (player) {
                // The lock's own words — the only hint the cabinet gives.
                player->DisplayClientMessage(
                    "Tuned shut. The chimes before it wait for the Alto's song.", true);
            }
            return UseResult::Consume;
        }

        UseResult CabinetUseItemOn(ItemStack& stack, ILevelWrite* world, const glm::ivec3& pos,
                                   IUsePlayer* player, uint32_t hand, const BlockHitResult& hit) {
            (void)stack; (void)world; (void)pos; (void)player; (void)hand; (void)hit;
            return UseResult::TryEmptyHandInteraction;
        }

        // ── Client ambience ──────────────────────────────────────────────

        // The dormant street lamps still shed the odd glint — fewer than a
        // lit lamp's (AurelithBlocks.cpp LampAnimateTick).
        void DimLampAnimateTick(EntityLevel& level, const glm::ivec3& pos, BlockState /*state*/,
                                JavaRandom& random) {
            if (random.NextInt(8) != 0) return;
            const double angle = random.NextDouble() * 6.283185307179586;
            const double r = 0.6 + random.NextDouble() * 0.5;
            level.AddParticle(ParticleKind::HushMote,
                              pos.x + 0.5 + std::cos(angle) * r,
                              pos.y + 0.2 + random.NextDouble() * 0.6,
                              pos.z + 0.5 + std::sin(angle) * r,
                              0.0, 0.006, 0.0);
        }

        // A socket and a pedestal breathe a mote now and then, so a player
        // crossing the plaza notices them.
        void QuestBlockAnimateTick(EntityLevel& level, const glm::ivec3& pos, BlockState /*state*/,
                                   JavaRandom& random) {
            if (random.NextInt(4) != 0) return;
            level.AddParticle(ParticleKind::HushMote,
                              pos.x + 0.3 + random.NextDouble() * 0.4, pos.y + 0.9,
                              pos.z + 0.3 + random.NextDouble() * 0.4,
                              0.0, 0.015 + random.NextDouble() * 0.01, 0.0);
        }

    } // namespace

    // ── The cabinet's lock ───────────────────────────────────────────────

    void Aurelith::OnChimeStruck(ILevelWrite& level, const glm::ivec3& chime) {
        if (level.IsClientSide()) return;
        const std::string_view note = NoteColourOf(level.GetBlock(chime.x, chime.y - 1, chime.z));
        const int64_t now = level.GameTime();
        const int r = kCabinetReach;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                for (int dz = -r; dz <= r; ++dz) {
                    const glm::ivec3 p = chime + glm::ivec3(dx, dy, dz);
                    if (level.GetBlock(p.x, p.y, p.z) != BlockID::ChoirCabinet) continue;
                    auto* cabinet = dynamic_cast<ChoirCabinetBlockEntity*>(level.GetBlockEntity(p));
                    if (!cabinet || cabinet->IsSolved() || cabinet->Melody().empty()) continue;

                    // A song left hanging too long starts over.
                    if (cabinet->Progress() > 0 && now - cabinet->LastNoteTick() > kCabinetPauseTicks) {
                        cabinet->Reset();
                    }
                    const auto& melody = cabinet->Melody();
                    if (!note.empty() && melody[static_cast<size_t>(cabinet->Progress())] == note) {
                        cabinet->Advance(now);
                        if (cabinet->Progress() < static_cast<int>(melody.size())) continue;

                        // Sung right: the doors open and it gives up what it
                        // holds, out of its front face.
                        const BlockState state = level.GetBlockState(p.x, p.y, p.z);
                        level.SetBlock(p.x, p.y, p.z, WithOpen(state, true), World::UpdateFlags::UpdateClients);
                        const Direction front = FacingOf(state);
                        const glm::ivec3 out = Relative(p, front);
                        const glm::dvec3 at(out.x + 0.5, p.y + 0.6, out.z + 0.5);
                        const glm::dvec3 push(StepX(front) * 0.12, 0.25, StepZ(front) * 0.12);
                        for (const ItemStack& stack : cabinet->Open()) {
                            SpawnItemEntity(level.GetDimension(), at, push, stack, 10);
                        }
                        SoundCues::Play(level, p, Sounds::kCabinetOpen, SoundSource::Blocks, 1.2f, 1.0f);
                        BroadcastBurst(level, glm::dvec3(p) + glm::dvec3(0.5, 1.0, 0.5),
                                       static_cast<uint8_t>(Network::AurelithS2CPacket::BurstStyle::Cabinet),
                                       VoiceColour(Voice::Alto));
                        Log::Info("[Aurelith] The choir cabinet at (%d, %d, %d) is sung open", p.x, p.y, p.z);
                    } else if (cabinet->Progress() > 0 || !note.empty()) {
                        // A wrong note: the lock forgets the song. Quiet — a
                        // struck chime near the cabinet that is not part of
                        // its song simply starts it over.
                        if (cabinet->Progress() > 0) {
                            SoundCues::Play(level, p, Sounds::kCabinetWrong, SoundSource::Blocks, 0.7f, 1.0f);
                        }
                        cabinet->Reset();
                        // A wrong note may still be the song's first.
                        if (!note.empty() && melody.front() == note) cabinet->Advance(now);
                    }
                }
            }
        }
    }

    void BlockRegistry_RegisterAurelithQuestBlocks(std::array<Block, BlockRegistry::Size>& blocks) {
        // The dormant lights glow (their textures are the dimmer half).
        for (BlockID id : { BlockID::DimCyanLumenPanel, BlockID::DimVioletLumenPanel,
                            BlockID::DimAmberLumenPanel, BlockID::DimLumenStrip,
                            BlockID::DimStaveStone, BlockID::DimChoirLamp }) {
            blocks[static_cast<size_t>(id)].emissive = true;
        }
        blocks[static_cast<size_t>(BlockID::DimChoirLamp)].animateTick = &DimLampAnimateTick;

        Block& socket = blocks[static_cast<size_t>(BlockID::ChordSocket)];
        socket.useItemOn      = &SocketUseItemOn;
        socket.useWithoutItem = &SocketUseWithoutItem;
        socket.animateTick    = &QuestBlockAnimateTick;

        Block& pedestal = blocks[static_cast<size_t>(BlockID::VoicePedestal)];
        pedestal.useItemOn      = &PedestalUseItemOn;
        pedestal.useWithoutItem = &PedestalUseWithoutItem;
        pedestal.animateTick    = &QuestBlockAnimateTick;

        Block& cabinet = blocks[static_cast<size_t>(BlockID::ChoirCabinet)];
        cabinet.useItemOn      = &CabinetUseItemOn;
        cabinet.useWithoutItem = &CabinetUseWithoutItem;
    }

} // namespace Game
