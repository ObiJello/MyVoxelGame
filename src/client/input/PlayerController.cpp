// File: src/client/input/PlayerController.cpp
#include "common/world/tags/DataTags.hpp"
#include "PlayerController.hpp"
#include "common/entity/FireworkItems.hpp"
#include "common/entity/Morph.hpp"
#include "common/world/block/BedBlock.hpp"
#include "client/entity/ClientFireworks.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "common/core/Mth.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/MiningSpeed.hpp"
#include "common/world/level/World.hpp"
#include "common/network/PacketTypes.hpp"
#include "../network/NetworkClient.hpp"
#include "Input.hpp"
#include "../network/ClientConnection.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "../world/ClientChunkManager.hpp"
#include "../world/ClientBlockAccess.hpp"
#include "../world/ClientLevel.hpp"
#include "../world/ClientLevelEvents.hpp"
#if ENABLE_IMMERSIVE_PORTALS
#include "../portal/ClientImmersivePortals.hpp"
#endif
#include <limits>
#include "../world/ClientUsePlayer.hpp"
#include "../entity/ClientMobManager.hpp"
#include "../entity/LocalItemCooldowns.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/SnowLayerBlock.hpp"
#include "common/world/block/CandleBlocks.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/fluid/FluidState.hpp"
#include "common/core/Features.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/mobs/Monsters.hpp"
#include "common/data/DataComponents.hpp"
#include "common/sound/SoundType.hpp"
#include "client/sound/ClientSounds.hpp"
#include "client/world/ClientWeather.hpp"
#include "common/world/enchantment/EnchantmentHelper.hpp"
#include "common/entity/WeaponItems.hpp"
#include "common/entity/SpearItem.hpp"
#include "common/entity/ConsumableBehavior.hpp"
#include "common/core/JavaRandom.hpp"
#include <chrono>
#if ENABLE_PORTAL_GUN
#include "../renderer/portal/PortalParticleSystem.hpp"
#include "../renderer/viewmodel/PortalGunViewmodel.hpp"
#endif
#include <glm/glm.hpp>
#include <cmath>
#include <thread>
#include <optional>

namespace Game {

    namespace {

        // The local player's own block sounds, played the moment the client
        // predicts the action. The server plays the same sounds to everyone
        // else with this player as `except` (MC's levelEvent / playSound with
        // the player), so each is heard exactly once.

        // MC MultiPlayerGameMode.continueDestroyBlock: every fourth tick of
        // mining, the hit sound — (volume + 1) / 8, pitch * 0.5, BLOCKS.
        void PlayBlockHitSound(const glm::ivec3& pos, BlockState state) {
            const SoundType& type = SoundTypeOf(state);
            if (IsEmptySound(type.hitSound)) return;
            Client::Sounds::PlayLocal(glm::dvec3(pos) + glm::dvec3(0.5), type.hitSound, SoundSource::Blocks,
                                      (type.volume + 1.0f) / 8.0f, type.pitch * 0.5f);
        }

        // MC destroyBlock → Block.playerWillDestroy → levelEvent(player, 2001):
        // LevelEventHandler's break sound, (volume + 1) / 2, pitch * 0.8.
        void PlayBlockBreakSound(const glm::ivec3& pos, BlockState state) {
            const SoundType& type = SoundTypeOf(state);
            if (IsEmptySound(type.breakSound)) return;
            Client::Sounds::PlayLocal(glm::dvec3(pos) + glm::dvec3(0.5), type.breakSound, SoundSource::Blocks,
                                      (type.volume + 1.0f) / 2.0f, type.pitch * 0.8f);
        }

        // MC BlockItem.place: level.playSound(player, pos, getPlaceSound(state),
        // BLOCKS, (volume + 1) / 2, pitch * 0.8).
        void PlayBlockPlaceSound(const glm::ivec3& pos, BlockState state) {
            const SoundType& type = SoundTypeOf(state);
            if (IsEmptySound(type.placeSound)) return;
            Client::Sounds::PlayLocal(glm::dvec3(pos) + glm::dvec3(0.5), type.placeSound, SoundSource::Blocks,
                                      (type.volume + 1.0f) / 2.0f, type.pitch * 0.8f);
        }

    } // namespace

    ClientPlayerController::ClientPlayerController()
        : player(nullptr)
        , networkClient(nullptr)
    {
        Log::Info("ClientPlayerController initialized");
    }

    int ClientPlayerController::GetDestroyStage() const {
        if (!digState.isDestroying) return -1;
        return Game::GetDestroyStage(digState.destroyProgress);
    }

    bool ClientPlayerController::ConsumeMiningSwingTrigger() {
        if (armSwingPending) { armSwingPending = false; return true; }
        return false;
    }

    uint32_t ClientPlayerController::SendDigPacket(Network::BlockActionType action,
                                                   const glm::ivec3& pos, BlockID blockId,
                                                   BlockState blockState) {
        if (!networkClient || !networkClient->IsConnected()) return 0;
        Network::BlockActionC2SPacket packet;
        packet.worldX = pos.x;
        packet.worldY = pos.y;
        packet.worldZ = pos.z;
        packet.action = action;
        packet.blockId = blockId;
        // The wire still carries the within-block index next to the id.
        packet.blockState = blockState.Index();
        // The face the dig started on; kFaceUnknown when there was none
        // (every dig path records one, so that is the defensive case).
        packet.face = (digState.destroyFace >= 0 && digState.destroyFace <= 5)
                    ? static_cast<uint8_t>(digState.destroyFace)
                    : Network::BlockActionC2SPacket::kFaceUnknown;
        packet.sequenceNumber = ++interactSeq;
        // The dimension of the block. A START (or an instant BREAK) targets
        // what the crosshair reaches now. So does a STOP for the block still
        // under the crosshair — creative instabreak sends STOP with no START
        // before it, so it cannot rely on a captured value. Only a STOP /
        // ABORT for a block the crosshair has left refers back to the dig it
        // started.
        if (player) {
            const bool startsHere = action == Network::BlockActionType::START_DESTROY ||
                                    action == Network::BlockActionType::BREAK;
            const bool underCrosshair = player->lastBlockHit.has_value() &&
                                        player->lastBlockHit->blockPos == pos;
            if (startsHere || underCrosshair) digDimension = player->lastBlockHitDimension;
        }
        packet.dimensionId = static_cast<int8_t>(Game::DimensionToRaw(digDimension));
        // Vein mine is decided at the moment the dig FINISHES: the player
        // holds the key together with Sneak while the block breaks. Both
        // are held-state reads, so a screen that is open at that instant
        // (Input::SetUiActive) reads as "not held".
        if (action == Network::BlockActionType::STOP_DESTROY ||
            action == Network::BlockActionType::BREAK) {
            packet.veinMine = Input::IsDown(*Input::Binds::Sneak) &&
                              Input::IsDown(*Input::Binds::VeinMine);
        }
        auto data = Network::Serialization::Serialize(packet);
        auto connection = networkClient->GetConnection();
        if (connection) {
            FlushMovement();   // the server measures reach from where we are NOW
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::BlockActionC2S), data);
        }
        return packet.sequenceNumber;
    }

    void ClientPlayerController::SetPlayer(ClientPlayer* playerPtr) {
        player = playerPtr;
        Log::Debug("ClientPlayerController player reference set");
    }

    void ClientPlayerController::SetBlockAccess(const IBlockAccess* access) {
        blockAccess = access;
        Log::Debug("ClientPlayerController block access set");
    }

    namespace {
        // MC Entity.isInWaterOrRain for the local player: in water, or rain
        // falling on the feet or on the top of the box (isRainingAt, the
        // client level's precipitation).
        bool LocalInWaterOrRain(const Game::ClientPlayer& p) {
            if (p.physics.isInWater) return true;
            if (!Client::g_clientBlockAccess) return false;
            const glm::ivec3 feet(static_cast<int>(std::floor(p.physics.position.x)),
                                  static_cast<int>(std::floor(p.physics.position.y)),
                                  static_cast<int>(std::floor(p.physics.position.z)));
            const glm::ivec3 top(feet.x,
                                 static_cast<int>(std::floor(p.physics.position.y +
                                                             static_cast<double>(p.physics.GetCurrentHeight()))),
                                 feet.z);
            constexpr int kRain = 1;   // BiomeRegistry precipitation RAIN
            return Client::ClientWeather::PrecipitationAt(*Client::g_clientBlockAccess, feet) == kRain ||
                   Client::ClientWeather::PrecipitationAt(*Client::g_clientBlockAccess, top) == kRain;
        }
    } // namespace

    void ClientPlayerController::LookAngles(float& yawDeg, float& pitchDeg) const {
        // player->yaw / player->pitch are STALE — mouse-look writes the camera
        // directly (see Player.hpp's lookDir comment). Derive from the live
        // look vector, matching SendUseItem's convention exactly so the
        // bucket's client-side POV clip traces the same ray the server will.
        yawDeg = 0.0f; pitchDeg = 0.0f;
        if (!player) return;
        const glm::vec3& d = player->lookDir;
        yawDeg   = Game::Mth::YRotFromVector(d);
        pitchDeg = Game::Mth::XRotFromVector(d);
    }

    void ClientPlayerController::HitLookAngles(const RaycastHit& hit, float& yawDeg,
                                               float& pitchDeg) const {
        // A ray-produced hit carries its own unit direction; a synthesised
        // one (the place-on-water clip, the sky stand-in) carries zero.
        const glm::vec3& d = hit.rayDirection;
        if (glm::dot(d, d) > 0.25f) {
            yawDeg   = Game::Mth::YRotFromVector(d);
            pitchDeg = Game::Mth::XRotFromVector(d);
            return;
        }
        LookAngles(yawDeg, pitchDeg);
    }

    BlockID ClientPlayerController::ReadBlock(const glm::ivec3& pos) const {
        try {
            if (blockAccess) return blockAccess->GetBlock(pos.x, pos.y, pos.z);
        } catch (...) {}
        return BlockID::Air;
    }

    BlockState ClientPlayerController::ReadBlockState(const glm::ivec3& pos) const {
        try {
            if (blockAccess) return blockAccess->GetBlockState(pos.x, pos.y, pos.z);
        } catch (...) {}
        return BlockState{};
    }

    namespace {
        // The client's half of the adventure-mode predicates (ItemStack.
        // canBreakBlockInAdventureMode / canPlaceOnBlockInAdventureMode):
        // a predicate that asks for block-entity NBT is left to the server,
        // which holds the saves — the client lets the action through and the
        // server's answer stands.
        bool ClientAdventureAllows(const std::optional<Game::AdventureModePredicate>& predicate, BlockState state) {
            if (!predicate) return false;
            return Game::AdventurePredicateNeedsNbt(*predicate) ||
                   Game::AdventurePredicateMatches(*predicate, state, nullptr);
        }
    }

    void ClientPlayerController::PredictBlock(const glm::ivec3& pos,
                                              BlockID newBlock,
                                              uint32_t sequence,
                                              BlockState state) {
        // The client's chunk cache is what the renderer meshes from AND what
        // ClientBlockAccess reads for raycast/physics, so a single write here
        // makes the change visible, targetable and solid on the same frame.
        if (Client::g_clientChunkManager) {
            Client::g_clientChunkManager->PredictBlockChange(pos, newBlock, sequence,
                                                             state.Index());
        }
    }
    
    void ClientPlayerController::SetNetworkClient(Client::NetworkClient* netClient) {
        networkClient = netClient;
        Log::Debug("ClientPlayerController network client reference set");
    }

    void ClientPlayerController::Tick(float deltaTime) {
        if (!player) {
            Log::Warning("ClientPlayerController::Tick called without player reference");
            return;
        }

        // Fixed-step 20 TPS state machine. Accumulate wall-clock time and
        // step UpdateBreakingTick / UpdatePlacingTick one tick at a time so
        // mining speed is framerate-independent (matches MC).
        tickAccum += deltaTime;
        // Guard against huge dt (window-drag, breakpoint) — clamp to 1s of
        // ticks so we don't run hundreds of catch-up iterations.
        if (tickAccum > 1.0f) tickAccum = 1.0f;
        while (tickAccum >= TICK_DT) {
            tickAccum -= TICK_DT;
            // MC Minecraft.tick:1803 and :1745 — both counters tick down here.
            if (missTime > 0)        --missTime;
            if (rightClickDelay > 0) --rightClickDelay;
            UpdateBreakingTick();
            UpdatePlacingTick();
            UpdateUsingTick();
        }

#if ENABLE_PORTAL_GUN
        // Tick any in-flight portal-gun projectiles; on impact each one
        // turns into a UseItemOnC2S at the hit block face.
        UpdateShooterVelocity(deltaTime);
        UpdatePendingPortalProjectiles(deltaTime);
#endif

        // Fly-state sync — MC LocalPlayer.sendIsSprintingIfNeeded-style
        // dirty check: whenever the local fly flag changes (double-tap
        // toggle, landing auto-cancel, server revoke), ship the new state
        // via PlayerAbilitiesC2S (MC ServerboundPlayerAbilitiesPacket).
        // Adopt a server-dictated state as the new baseline instead of
        // reporting it back. See ClientPlayer::abilitiesSyncedFromServer for
        // the join-time race this closes.
        if (player->abilitiesSyncedFromServer) {
            player->abilitiesSyncedFromServer = false;
            lastSentFlying = player->physics.isFlying;
            lastSentNoclip = player->physics.noclip;
        }

        if (player->physics.isFlying != lastSentFlying ||
            player->physics.noclip    != lastSentNoclip) {
            lastSentFlying = player->physics.isFlying;
            lastSentNoclip = player->physics.noclip;
            if (networkClient && networkClient->IsConnected()) {
                Network::PlayerAbilitiesC2SPacket packet;
                if (lastSentFlying) packet.flags |= Network::PlayerAbilitiesC2SPacket::FLAG_FLYING;
                // Noclip rides the same dirty check so the server can save it;
                // it grants nothing, the client already owns the behaviour.
                if (lastSentNoclip) packet.flags |= Network::PlayerAbilitiesC2SPacket::FLAG_NOCLIP;
                auto data = Network::Serialization::Serialize(packet);
                if (auto connection = networkClient->GetConnection()) {
                    connection->SendPacket(
                        static_cast<uint8_t>(Network::PacketId::PlayerAbilitiesC2S), data);
                }
            }
        }
    }

    void ClientPlayerController::SetMovementFlush(std::function<void()> flush) {
        movementFlush = std::move(flush);
    }

    void ClientPlayerController::FlushMovement() {
        if (movementFlush) movementFlush();
    }

    void ClientPlayerController::StartDig(const glm::ivec3& pos, int face) {
        // MC's MultiPlayerGameMode.startDestroyBlock:
        //   if (block is breakable && ...) {
        //       progress = 0;
        //       destroyBlockPos = pos;
        //       isDestroying = true;
        //       send START_DESTROY_BLOCK packet;
        //   }
        digState.isDestroying    = true;
        digState.destroyProgress = 0.0f;
        digState.destroyTicks    = 0;
        digState.destroyBlockPos = pos;
        digState.destroyFace     = face;
        digState.lastSwingTick   = -1000;

        // Cache block ID and state at start — the world may already be Air by
        // the time we want to finalise (integrated server shares the world),
        // and the server reads both back out of the finish packet.
        digState.destroyingBlockId    = ReadBlock(pos);
        digState.destroyingBlockState = ReadBlockState(pos);

        SendDigPacket(Network::BlockActionType::START_DESTROY, pos,
                      digState.destroyingBlockId, digState.destroyingBlockState);
        // First swing fires immediately on press.
        armSwingPending = true;
    }

    void ClientPlayerController::AbortDig() {
        if (!digState.isDestroying) return;
        SendDigPacket(Network::BlockActionType::ABORT_DESTROY,
                      digState.destroyBlockPos, digState.destroyingBlockId,
                      digState.destroyingBlockState);
        digState.isDestroying    = false;
        digState.destroyProgress = 0.0f;
        digState.destroyTicks    = 0;
    }

    void ClientPlayerController::FinishDig() {
        // STOP_DESTROY is the MC finish action. The server clears the block
        // and credits the player's inventory.
        const uint32_t sequence = SendDigPacket(Network::BlockActionType::STOP_DESTROY,
                                                digState.destroyBlockPos,
                                                digState.destroyingBlockId,
                                                digState.destroyingBlockState);

        // Local prediction — the block disappears NOW rather than one round
        // trip from now. Registered under the packet's sequence so the
        // server's BlockChangedAckS2C either confirms it silently or rolls it
        // back (MC MultiPlayerGameMode.startPrediction → destroyBlock).
        FinishBreaking(sequence);

        digState.isDestroying    = false;
        digState.destroyProgress = 0.0f;
        digState.destroyTicks    = 0;
        digState.destroyDelay    = POST_BREAK_DELAY_TICKS;
    }

    void ClientPlayerController::CreativeDestroy(const glm::ivec3& pos, int face) {
        // MC MultiPlayerGameMode's `instabuild` branch: the block is destroyed
        // outright — destroyProgress is never accumulated, so the block's
        // destroyTime (including bedrock's -1 "unbreakable" sentinel) is never
        // consulted. The only bail is "the block is already air"
        // (MultiPlayerGameMode.destroyBlock's `oldState.isAir()` check).
        const BlockID target = ReadBlock(pos);
        if (target == BlockID::Air) return;
        // MC Item.canDestroyBlock: a TOOL with can_destroy_blocks_in_creative
        // false (the swords, the mace, the trident) breaks nothing.
        if (!Game::CanDestroyBlockWith(player->inventory.GetSelectedStack(), /*instabuild=*/true)) return;
        // DebugStickItem.canDestroyBlock: false — the left click selects a
        // property on the server (START_DESTROY) and breaks nothing.
        if (player->inventory.GetSelectedItem() == Game::Items::DebugStick) {
            SendDigPacket(Network::BlockActionType::START_DESTROY, pos, target, ReadBlockState(pos));
            armSwingPending = true;
            digState.destroyDelay = CREATIVE_BREAK_DELAY_TICKS;
            return;
        }

        // Set up the minimal dig state FinishDig's packet + local-prediction
        // path expects, then finish immediately.
        digState.destroyBlockPos      = pos;
        digState.destroyFace          = face;
        digState.destroyingBlockId    = target;
        digState.destroyingBlockState = ReadBlockState(pos);
        digState.destroyProgress      = 1.0f;
        digState.destroyTicks         = 0;
        digState.isDestroying         = true;
        armSwingPending            = true;
        // MC's creative path concludes with START_DESTROY_BLOCK (its server
        // shortcuts straight to destroyAndAck on that action). Our protocol
        // treats START_DESTROY as purely informational and STOP_DESTROY as
        // "finalize the dig" (PlayerSession::HandleBlockAction), so creative
        // finishes through the same STOP_DESTROY that survival uses.
        FinishDig();
        // FinishDig applies the survival POST_BREAK_DELAY_TICKS; creative uses
        // MC's 5-tick cadence instead.
        digState.destroyDelay = CREATIVE_BREAK_DELAY_TICKS;
    }

    uint32_t ClientPlayerController::SendUseItemOn(const RaycastHit& hit, int hand, bool altInteract,
                                                   std::optional<Game::DimensionId> dimension,
                                                   bool fromUse) {
        // Build and send BlockPlaceC2S packet (Minecraft-compatible)
        Log::Debug("SendUseItemOn called for block (%d,%d,%d), hand=%d alt=%d",
                  hit.blockPos.x, hit.blockPos.y, hit.blockPos.z, hand, altInteract ? 1 : 0);
        
        if (!networkClient) {
            Log::Debug("SendUseItemOn: networkClient is null - not set on controller");
            return 0;
        }
        
        if (!networkClient->IsConnected()) {
            Log::Debug("SendUseItemOn: networkClient not connected to server");
            return 0;
        }
        
        Log::Debug("SendUseItemOn: Building BlockPlaceC2S packet...");
        
        const uint32_t direction = OurFaceToMcFace(hit.hitFace);
        
        // Build the packet
        Network::UseItemOnC2SPacket packet(
            hand,                    // Hand (0=main, 1=off)
            hit.blockPos.x,         // Block X
            hit.blockPos.y,         // Block Y
            hit.blockPos.z,         // Block Z
            direction,              // Face direction
            hit.cursorPos.x,        // Cursor X [0,1)
            hit.cursorPos.y,        // Cursor Y [0,1)
            hit.cursorPos.z,        // Cursor Z [0,1)
            hit.insideBlock,        // Inside block flag
            ++interactSeq,          // Sequence number
            altInteract             // true = left-click "use" semantics
        );
        // Immersive portals: the clicked block's level.
        packet.dimensionId = static_cast<int8_t>(Game::DimensionToRaw(
            dimension ? *dimension
                      : (player ? player->lastBlockHitDimension : Game::DimensionId::Overworld)));
        packet.fromUse = fromUse;
        // The rotation in the clicked block's space (through a portal: the
        // look mapped through it) — what the server orients a placement by.
        packet.hasLookRotation = true;
        HitLookAngles(hit, packet.lookYaw, packet.lookPitch);
        
        // Serialize and send
        auto data = Network::Serialization::Serialize(packet);
        auto connection = networkClient->GetConnection();
        if (connection) {
            FlushMovement();   // the server tests reach and the player-in-block overlap against this position
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::UseItemOnC2S), data);
            Log::Debug("Sent UseItemOnC2S: pos(%d,%d,%d) face=%d cursor=(%.2f,%.2f,%.2f) seq=%d",
                      hit.blockPos.x, hit.blockPos.y, hit.blockPos.z, direction,
                      hit.cursorPos.x, hit.cursorPos.y, hit.cursorPos.z, interactSeq);
        }
        return packet.sequence;
    }

    // Raycast face numbering -> MC's Direction ordinals. Ours is
    // 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z; MC's is
    // 0=bottom(-Y), 1=top(+Y), 2=north(-Z), 3=south(+Z), 4=west(-X), 5=east(+X).
    // Shared by the packet we send and by the local placement prediction, so the
    // two cannot disagree about which face was clicked.
    uint32_t ClientPlayerController::OurFaceToMcFace(int ourFace) {
        switch (ourFace) {
            case 0: return 5;  // +X -> east
            case 1: return 4;  // -X -> west
            case 2: return 1;  // +Y -> top
            case 3: return 0;  // -Y -> bottom
            case 4: return 3;  // +Z -> south
            case 5: return 2;  // -Z -> north
            default: return 1;
        }
    }

    BlockID ClientPlayerController::HeldFillBlock() const {
        if (!player) return BlockID::Air;
        const BlockID block = player->GetSelectedBlock();
        if (block != BlockID::Air) return block;
        const Game::ItemID item = player->inventory.GetSelectedItem();
        if (item == Game::Items::WaterBucket) return BlockID::Water;
        if (item == Game::Items::LavaBucket)  return BlockID::Lava;
        return BlockID::Air;
    }

    bool ClientPlayerController::FluidCanFill(Game::BlockState existing, BlockID fluidBlock) const {
        const BlockID id = existing.Block();
        if (id == BlockID::Air) return true;
        // The same fluid, source or flowing, is a valid pour target — MC's
        // emptyContents succeeds over an existing source and overwrites a
        // flowing cell — so a corner may sit in the pool being filled.
        if (Game::FluidStateOf(existing).IsSame(
                fluidBlock == BlockID::Lava ? Game::FluidType::Lava : Game::FluidType::Water)) {
            return true;
        }
        // Water into a dry waterloggable block waterlogs it.
        if (fluidBlock == BlockID::Water && BlockRegistry::IsWaterloggable(id) &&
            !BlockRegistry::ContainsWater(existing)) {
            return true;
        }
        // BlockBehaviour.canBeReplaced(state, fluid): the replaceable flag or
        // no collision at all.
        return BlockRegistry::Get(id).replaceable || !BlockRegistry::HasCollision(id);
    }

    bool ClientPlayerController::ComputeFillCorner(const RaycastHit& hit, glm::ivec3& outPos,
                                                   Game::BlockState& outState) const {
        if (!player) return false;
        if (player->GetSelectedBlock() != BlockID::Air) {
            BlockID block = BlockID::Air;
            return ComputePredictedPlacement(hit, outPos, block, outState);
        }
        const BlockID fluidBlock = HeldFillBlock();
        if (fluidBlock != BlockID::Water && fluidBlock != BlockID::Lava) return false;

        // MC BucketItem.use: the cell in front of the clicked face, or the
        // clicked cell itself when the bucket would pour into it (a
        // LiquidBlockContainer for water; a cell the fluid may replace —
        // the tall grass or the flowing water the crosshair is on).
        const BlockState clickedState = ReadBlockState(hit.blockPos);
        const BlockID    clickedId    = clickedState.Block();
        const bool intoClicked =
            (fluidBlock == BlockID::Water && BlockRegistry::IsWaterloggable(clickedId) &&
             !BlockRegistry::ContainsWater(clickedState)) ||
            BlockRegistry::Get(clickedId).replaceable || !BlockRegistry::HasCollision(clickedId);
        const glm::ivec3 target = intoClicked ? hit.blockPos : hit.adjacentPos;
        if (!FluidCanFill(ReadBlockState(target), fluidBlock)) return false;
        outPos   = target;
        outState = Game::BlockStates::Default(fluidBlock);
        return true;
    }

    bool ClientPlayerController::HandleFillClick(const std::optional<RaycastHit>& hit) {
        if (!player) return false;
        const bool alt = Input::IsKeyDown(Input::Key::LeftAlt);
        if (!alt && !m_fill.armed) return false;
        const Game::DimensionId dim = Client::ClientLevels::ActiveDimension();
        glm::ivec3 pos{0};
        BlockState state;
        const bool can = hit.has_value() && ComputeFillCorner(*hit, pos, state);
        if (alt) {
            if (!can) {
                // Alt on air or on something that cannot take a block:
                // the mark is dropped, and the click is spent.
                m_fill.armed = false;
                return true;
            }
            m_fill.armed     = true;
            m_fill.cell      = pos;
            m_fill.dimension = dim;
            m_fill.state     = state;
            return true;
        }
        // A plain click with a corner marked. On a block, the cell a block
        // would go into; on nothing, a cell out in the air along the look.
        // Where a block would not go, the click keeps its usual meaning (a
        // chest opens, a door swings).
        if (m_fill.dimension != dim) return false;
        if (!can) {
            if (hit.has_value() || !FillAirCell(pos, state)) return false;
        }
        SendFillBlocks(m_fill.cell, pos, state);
        m_fill.armed = false;
        return true;
    }

    bool ClientPlayerController::FillAirCell(glm::ivec3& outCell, Game::BlockState& outState) const {
        if (!player || !m_fill.armed) return false;
        // The block (or bucket) in hand must still be the marked one; its
        // state (orientation) is the mark's.
        const BlockID held = HeldFillBlock();
        if (held == BlockID::Air || held != m_fill.state.Block()) return false;
        constexpr float kAirCornerDistance = 5.0f;
        const glm::dvec3 eye = player->GetEyePosition();
        const glm::vec3 dir = glm::normalize(player->lookDir);
        const glm::dvec3 point = eye + glm::dvec3(dir * kAirCornerDistance);
        const glm::ivec3 cell(static_cast<int>(std::floor(point.x)),
                              static_cast<int>(std::floor(point.y)),
                              static_cast<int>(std::floor(point.z)));
        if (held == BlockID::Water || held == BlockID::Lava) {
            if (!FluidCanFill(ReadBlockState(cell), held)) return false;
        } else {
            Game::PlacementClick click;
            click.replacingClickedOnBlock = false;
            if (!Game::CanBeReplacedByPlacement(ReadBlockState(cell), held,
                                                player->physics.isSneaking, click)) {
                return false;
            }
        }
        outCell  = cell;
        outState = m_fill.state;
        return true;
    }

    void ClientPlayerController::SendFillBlocks(const glm::ivec3& a, const glm::ivec3& b,
                                                Game::BlockState state) {
        if (!networkClient || !networkClient->IsConnected()) return;
        auto connection = networkClient->GetConnection();
        if (!connection) return;
        Network::FillBlocksC2SPacket packet;
        packet.hand = 0;
        packet.x0 = a.x; packet.y0 = a.y; packet.z0 = a.z;
        packet.x1 = b.x; packet.y1 = b.y; packet.z1 = b.z;
        packet.rawState    = state.RawId();
        packet.dimensionId = static_cast<int8_t>(Game::DimensionToRaw(Client::ClientLevels::ActiveDimension()));
        connection->SendPacket(static_cast<uint8_t>(Network::PacketId::FillBlocksC2S),
                               Network::Serialization::Serialize(packet));
    }

    bool ClientPlayerController::FillPreview(const std::optional<RaycastHit>& hit, glm::ivec3& outLo,
                                             glm::ivec3& outHi, Game::BlockState& outState) const {
        if (!m_fill.armed || !player) return false;
        if (m_fill.dimension != Client::ClientLevels::ActiveDimension()) return false;
        glm::ivec3 pos = m_fill.cell;
        BlockState state = m_fill.state;
        if (hit.has_value()) {
            if (!ComputeFillCorner(*hit, pos, state)) {
                pos   = m_fill.cell;
                state = m_fill.state;
            }
        } else if (!FillAirCell(pos, state)) {
            pos   = m_fill.cell;
            state = m_fill.state;
        }
        outLo    = glm::min(m_fill.cell, pos);
        outHi    = glm::max(m_fill.cell, pos);
        outState = state;
        return true;
    }

    bool ClientPlayerController::ComputePredictedPlacement(const RaycastHit& hit,
                                                           glm::ivec3& outPos,
                                                           BlockID& outBlock,
                                                           BlockState& outState,
                                                           bool fromUse) const {
        if (!player) return false;

        // --- Only plain block items are predictable ---------------------
        // Anything with an item behaviour (flint & steel, buckets, hoes,
        // bone meal…) resolves server-side in ways we don't model here.
        const Game::ItemID held = player->inventory.GetSelectedItem();
        if (held == Game::Items::Air) return false;
        if (Game::StackUseOn(player->inventory.GetSelectedStack()) != nullptr) return false;
        // ItemStack.useOn's adventure gate: BlockItem.useOn runs only when the
        // stack's CAN_PLACE_ON matches the clicked block.
        if (player->gameMode == 2 &&
            !ClientAdventureAllows(player->inventory.GetSelectedStack().get(Game::DataComponents::CAN_PLACE_ON),
                                   ReadBlockState(hit.blockPos))) {
            return false;
        }

        const BlockID toPlace = player->GetSelectedBlock();
        if (toPlace == BlockID::Air) return false;

        // PlaceOnWaterBlockItem: its useOn is PASS, so a lily pad or frogspawn
        // places only through its own `use` clip (the `fromUse` placement),
        // and that placement is the only thing `fromUse` may predict — the
        // server holds both sides to the same rule.
        if (Game::IsPlaceOnWaterBlock(toPlace) != fromUse) return false;

        // --- Would the clicked block swallow the click? ------------------
        // Mirrors HandleUseItemOn's suppressBlockUse + block-use dispatch:
        // a door/lever/chest consumes the interaction and nothing is placed.
        // `somethingInHands` is implied — held != Air was checked above. A
        // `fromUse` placement is BlockItem.useOn alone: no block reacts.
        const BlockID clickedId = ReadBlock(hit.blockPos);
        const bool suppressBlockUse = player->physics.isSneaking || fromUse;
        if (!suppressBlockUse) {
            const Block& clicked = BlockRegistry::Get(clickedId);
            // CandleBlock.useItemOn acts on an EMPTY hand only (it puts the
            // candle out); with a block in hand it hands the click on, which
            // is how a candle grows the clump it is clicked onto. Predicting
            // through it is exact, where the other useItemOn hooks are not.
            const bool declinesHeldItem = Game::Candles::IsCandle(clickedId);
            if ((clicked.useItemOn && !declinesHeldItem) || clicked.useWithoutItem) return false;
        }

        // --- Resolve the target cell (server: step 6a) -------------------
        // MC BlockPlaceContext's order: decide whether the CLICKED block is
        // replaceable, resolve the position from that, then re-ask at the
        // resolved cell. Clicking the grass under a leaf litter clump lands on
        // the litter's own cell, which is what lets the growth below see it.
        const BlockState clickedState = ReadBlockState(hit.blockPos);
        // Same click data the server builds, so the slab merge predicts
        // identically instead of flashing a slab into the neighbouring cell.
        Game::PlacementClick click;
        click.clickedFace = static_cast<Game::Direction>(OurFaceToMcFace(hit.hitFace));
        click.hitY        = hit.cursorPos.y;
        click.replacingClickedOnBlock = true;

        const bool replaceClicked =
            Game::CanBeReplacedByPlacement(clickedState, toPlace,
                                           player->physics.isSneaking, click);

        const glm::ivec3 target = replaceClicked ? hit.blockPos : hit.adjacentPos;
        const BlockID targetId    = ReadBlock(target);
        const BlockState targetState = ReadBlockState(target);
        Game::PlacementClick resolvedClick = click;
        resolvedClick.replacingClickedOnBlock = false;
        if (!replaceClicked &&
            !Game::CanBeReplacedByPlacement(targetState, toPlace,
                                            player->physics.isSneaking, resolvedClick)) {
            return false;
        }

        // --- Segmented ground cover grows in place (server: step 6b) ------
        // MC SegmentableBlock: same item on an existing clump raises its
        // segment count and KEEPS the facing it already has. Predicting the
        // grow avoids a round trip in which the clump visibly stays put and
        // then jumps a segment.
        // Growing raises the `segment_amount`/`flower_amount` state; the
        // BlockID never changes, so `resolved` stays put and the new state is
        // carried to outState below.
        BlockID resolved = toPlace;
        // Skull items are MC StandingAndWallBlockItems: a click on a
        // horizontal face resolves to the WALL variant. Same rule, same
        // stage as the server (PlayerSession::HandleUseItemOn), so the
        // prediction never flips block when the authoritative update lands.
        if (!replaceClicked) {
            resolved = Game::SkullPlacementBlock(resolved, click.clickedFace);
            resolved = Game::SignPlacementBlock(resolved, click.clickedFace);
            if (blockAccess) {
                resolved = Game::TorchPlacementBlock(*blockAccess, resolved, target, click.clickedFace);
            }
        }
        bool grewInPlace = false;
        BlockState grownState;
        // The segmented clumps, candles, sea pickles and turtle eggs — the
        // server's StackedPlacementState, verbatim.
        if (Game::StackedPlacementState(targetState, toPlace, grownState)) {
            grewInPlace = true;
        }
        // MC SnowLayerBlock.getStateForPlacement — the server's step 6b twin:
        // a snow layer in the resolved cell grows by one layer.
        if (targetId == BlockID::SnowLayer && toPlace == BlockID::SnowLayer) {
            const BlockState grown = Game::SnowLayer::GrownState(targetState);
            if (grown != targetState) {
                grownState  = grown;
                grewInPlace = true;
            }
        }

        // --- Slab half (server: SlabBlock.getStateForPlacement mirror) ---
        // Same inputs (clicked face + cursor Y), so the same answer — without
        // this a slab would predict as bottom and visibly flip on the ack.
        // The half is a `type` state, so unlike the segmented clumps this
        // cannot resolve to a different BlockID — it is applied to the state
        // once ComputePlacementState has run, below.
        std::optional<Game::BlockRegistry::SlabType> slabHalf;
        if (!grewInPlace && Game::BlockRegistry::IsSlabBlock(toPlace)) {
            using SlabType = Game::BlockRegistry::SlabType;
            // The merge comes first, exactly as on the server: a slab already
            // in the resolved cell turns the placement into a double instead of
            // a second half. `targetId == toPlace` is MC's `state.is(this)`.
            if (targetId == toPlace &&
                Game::BlockRegistry::SlabTypeOf(targetState) != SlabType::Double) {
                slabHalf = SlabType::Double;
            } else {
                bool placeAsTop;
                switch (hit.hitFace) {
                    case 3:  placeAsTop = true;  break;               // -Y (bottom face)
                    case 2:  placeAsTop = false; break;               // +Y (top face)
                    default: placeAsTop = (hit.cursorPos.y > 0.5f); break;  // sides
                }
                slabHalf = placeAsTop ? SlabType::Top : SlabType::Bottom;
            }
        }

        // --- Can the block survive there? (server: step 7) ----------------
        // The SAME CanSurviveAt the server calls, so we never predict a
        // placement it is about to reject — e.g. a 4-segment clump resolving to
        // the cell above itself, where leaf litter has nothing sturdy to sit
        // on, or a seed clicked onto plain grass instead of farmland. Calling a
        // weaker check here is what would make a rejected planting flash into
        // existence for one round trip.
        if (blockAccess) {
            if (!Game::CanSurviveAt(*blockAccess, target, resolved)) return false;
        }

        // --- Would the block land inside an entity? ----------------------
        // Asked at the very end, once the final state is known (its collision
        // shape is what counts — a grown clump, a slab half, a snow pile one
        // layer below its top): see PlacementUnobstructedLocally below.

        // --- Orientation (server: Block.getStateForPlacement mirror) -----
        // Same shared table the server calls, fed the same inputs, so a
        // furnace predicts facing the right way instead of appearing north-
        // facing for a round trip and then snapping.
        Game::UseOnContext ctx;
        ctx.world  = nullptr;   // the placement rules read no world state
        ctx.player = nullptr;
        ctx.hand   = 0;
        ctx.hitResult.blockPos = hit.blockPos;
        ctx.hitResult.face     = OurFaceToMcFace(hit.hitFace);
        ctx.hitResult.hitPoint = glm::dvec3(hit.blockPos) + glm::dvec3(hit.cursorPos);
        // LookAngles(), not player->yaw/pitch — those are only written on
        // teleports (mouse-look updates camera.yaw/pitch instead), so reading
        // them here made every predicted placement use a stale angle. The block
        // appeared facing the wrong way until the server's authoritative state
        // arrived a round trip later, which is exactly the flicker prediction
        // exists to avoid. Same trap SpawnPortalProjectile documents.
        // HitLookAngles: through a portal, the look as seen on the far side
        // (the same angles SendUseItemOn hands the server).
        HitLookAngles(hit, ctx.playerYaw, ctx.playerPitch);

        outPos   = target;
        outBlock = resolved;
        // Growing a clump keeps the facing it already has (MC's
        // `state.setValue(segment, n + 1)`); everything else derives its
        // orientation from how the player is standing.
        outState = grewInPlace ? grownState : Game::ComputePlacementState(resolved, ctx);
        // Applied after ComputePlacementState so it composes with, rather than
        // overwrites, the waterlogged bit that placing into a fluid sets.
        if (slabHalf) {
            using SlabType = Game::BlockRegistry::SlabType;
            outState = Game::BlockRegistry::SlabStateWithType(outState, *slabHalf);
            // MC clears WATERLOGGED when merging to a double. The server does
            // the same; predicting otherwise would flip the block on the ack.
            if (*slabHalf == SlabType::Double) {
                outState = Game::BlockRegistry::WithWaterlogged(outState, false);
            }
        }
        // Neighbour-derived orientation (redstone dust). Uses the SAME function
        // the server calls, against the client's own block access, so the wire
        // predicts with the exact connections the server is about to send.
        if (!grewInPlace && blockAccess) {
            outState = Game::ComputeWorldPlacementState(*blockAccess, target, outState);
            // The block can change here, not just its state — concrete powder
            // placed into water comes back as CONCRETE. Mirrors the server
            // (PlayerSession::HandleUseItemOn); predicting the powder would
            // flash the wrong block for a tick.
            outBlock = outState.Block();
            // BlockItem.updateBlockStateFromTag: the stack's BLOCK_STATE, as
            // the server applies it.
            outState = Game::ApplyBlockItemStateProperties(player->inventory.GetSelectedStack(), outState);
            // State-aware survival, mirroring the server's second gate: a
            // button's support depends on the face it ends up attached to, so
            // this can only be asked once the state is known. Predicting a
            // placement the server will refuse would flash a floating button.
            if (!Game::CanSurviveAt(*blockAccess, target, outState)) return false;
        }
        // A door: the cell above must be free as well, and the hinge is
        // chosen the way the server chooses it, so the pair predicts true.
        if (Game::IsDoorBlock(outBlock)) {
            const glm::ivec3 above = target + glm::ivec3(0, 1, 0);
            Game::PlacementClick aboveClick;
            aboveClick.replacingClickedOnBlock = false;
            if (!Game::CanBeReplacedByPlacement(ReadBlockState(above), outBlock,
                                                player->physics.isSneaking, aboveClick)) {
                return false;
            }
            if (blockAccess) {
                outState = Game::DoorPlacementState(*blockAccess, target, outState, ctx.hitResult.hitPoint);
            }
        }
        // A bed: the foot goes here and the head in the facing direction,
        // which must be free as well (mirrors PlayerSession::HandleUseItemOn).
        if (Game::IsBedBlock(outBlock)) {
            outState = Game::BedFootPlacementState(outState);
            const glm::ivec3 head = Game::BedOtherHalfPos(target, outState);
            Game::PlacementClick headClick;
            headClick.replacingClickedOnBlock = false;
            if (!Game::CanBeReplacedByPlacement(ReadBlockState(head), outBlock,
                                                player->physics.isSneaking, headClick)) {
                return false;
            }
        }
        // --- Would the block land inside an entity? (server: step 8) -----
        // MC BlockItem.canPlace's isUnobstructed: the server refuses a block
        // whose collision shape overlaps this player, another player or a
        // mob, and predicting it anyway would trap the body for a round trip
        // and then yank the block away. No-collision blocks (flowers, ground
        // cover) have an empty shape and always pass.
        if (!PlacementUnobstructedLocally(outState, target)) return false;
        return true;
    }

    bool ClientPlayerController::PlacementUnobstructedLocally(BlockState state,
                                                              const glm::ivec3& pos) const {
        const BlockRegistry::BlockShapeSet shape = Game::PlacementCollisionShape(state);
        if (shape.count == 0) return true;

        // A player's box by pose, as the server builds it from its live
        // ServerPlayer (PlayerSession.cpp LivePlayerBox): a morph wears its
        // mob's box, a sleeper is 0.2 x 0.2, a crouch is PlayerPhysics'
        // sneaking height; all at the body's scale.
        auto playerBox = [](const glm::dvec3& feet, uint32_t morph, bool sleeping, bool crouching,
                            float scale) {
            float width  = Game::PlayerPhysics::WIDTH;
            float height = Game::PlayerPhysics::HEIGHT_STANDING;
            if (!Game::Morph::IsNone(morph)) {
                const Game::Morph::Dims dims = Game::Morph::DimsOf(morph);
                width  = dims.width;
                height = dims.height;
            } else if (sleeping) {
                width = height = 0.2f;
            } else if (crouching) {
                height = Game::PlayerPhysics::HEIGHT_SNEAKING;
            }
            const double hw = 0.5 * static_cast<double>(width * scale);
            const double h  = static_cast<double>(height * scale);
            return Game::AABBd::FromMinMax(feet - glm::dvec3(hw, 0.0, hw), feet + glm::dvec3(hw, h, hw));
        };

        // This player — CollisionGetter.isUnobstructed passes no entity to
        // skip, so the placer's own body counts. A spectator has none.
        if (player && !player->IsSpectator()) {
            // A morphed body's box is the physics' own (SetMorph carried the
            // mob's size across), so it is read from there.
            const Game::AABBd box = player->physics.morphed
                ? Game::ToAABBd(player->physics.GetAABB())
                : playerBox(player->physics.position, Game::Morph::kNone, player->IsSleeping(),
                            player->physics.isSneaking, player->physics.scale);
            if (Game::PlacementShapeOverlaps(shape, pos, box)) return false;
        }

        // The other players this client draws, in the level being edited.
        if (Client::g_remotePlayerManager) {
            for (const auto& [pid, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                (void)pid;
                if (!rp.positionInitialized) continue;
                if (!Client::IsRemotePlayerInBoundLevel(rp)) continue;
                const Game::AABBd box = playerBox(rp.position, rp.morph, rp.sleepingPos.has_value(),
                                                  rp.isCrouching, rp.scale);
                if (Game::PlacementShapeOverlaps(shape, pos, box)) return false;
            }
        }

        // The mob mirror: armor stands, primed TNT, falling blocks, animals,
        // monsters — whatever blocksBuilding.
        if (Client::g_clientMobManager) {
            if (Game::EntitiesObstructPlacement(Client::g_clientMobManager->Level(), shape, pos,
                                                /*skipPlayers=*/false)) {
                return false;
            }
        }
        return true;
    }

    bool ClientPlayerController::HoldsPlaceOnWaterItem() const {
        if (!player) return false;
        const Game::ItemID held = player->inventory.GetSelectedItem();
        return Game::ItemRegistry::IsBlockItem(held) &&
               Game::IsPlaceOnWaterBlock(Game::ItemRegistry::ToBlock(held));
    }

    std::optional<RaycastHit> ClientPlayerController::ClipPlaceOnWater() const {
        if (!player || !blockAccess) return std::nullopt;

        // Item.getPlayerPOVHitResult(level, player, Fluid.SOURCE_ONLY): from
        // the eye along the view, out to the block interaction range,
        // against every block's OUTLINE and every fluid SOURCE's shape.
        const glm::dvec3 eye = player->GetEyePosition();
        const glm::dvec3 dir = glm::dvec3(glm::normalize(player->lookDir));
        const double reach = player->GetBlockInteractionRange() * static_cast<double>(player->physics.scale);

        // Entry distance of the ray into one box, and the axis it entered
        // through; nullopt when it misses or starts inside (AABB.clip only
        // reports an entry).
        struct Entry { double t; int axis; };
        auto clipBox = [&](const glm::dvec3& lo, const glm::dvec3& hi) -> std::optional<Entry> {
            double tNear = -std::numeric_limits<double>::infinity();
            double tFar  = std::numeric_limits<double>::infinity();
            int axis = -1;
            for (int a = 0; a < 3; ++a) {
                if (std::abs(dir[a]) < 1e-12) {
                    if (eye[a] < lo[a] || eye[a] > hi[a]) return std::nullopt;
                    continue;
                }
                double t1 = (lo[a] - eye[a]) / dir[a];
                double t2 = (hi[a] - eye[a]) / dir[a];
                if (t1 > t2) std::swap(t1, t2);
                if (t1 > tNear) { tNear = t1; axis = a; }
                tFar = std::min(tFar, t2);
                if (tNear > tFar) return std::nullopt;
            }
            if (axis < 0 || tNear <= 0.0 || tNear > reach) return std::nullopt;
            return Entry{tNear, axis};
        };

        auto isFluidOnlyBlock = [](BlockID id) {
            return id == BlockID::Air || id == BlockID::Water || id == BlockID::Lava ||
                   id == BlockID::BubbleColumn || id == BlockID::ResonantWater;
        };

        // BlockGetter.traverseBlocks: the cells along the ray in order; the
        // first cell with a hit ends the walk, the nearer of its block and
        // fluid hits winning (ClipContext's `blockDistance <= fluidDistance`).
        glm::ivec3 cell(static_cast<int>(std::floor(eye.x)),
                        static_cast<int>(std::floor(eye.y)),
                        static_cast<int>(std::floor(eye.z)));
        const glm::ivec3 step(dir.x > 0.0 ? 1 : -1, dir.y > 0.0 ? 1 : -1, dir.z > 0.0 ? 1 : -1);
        glm::dvec3 tMax, tDelta;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(dir[a]) < 1e-12) {
                tMax[a] = tDelta[a] = std::numeric_limits<double>::infinity();
            } else {
                const double bound = dir[a] > 0.0 ? std::floor(eye[a]) + 1.0 : std::floor(eye[a]);
                tMax[a]   = (bound - eye[a]) / dir[a];
                tDelta[a] = 1.0 / std::abs(dir[a]);
            }
        }

        double travelled = 0.0;
        while (travelled <= reach) {
            // A cell outside the world (above the build limit, an unloaded
            // column) holds nothing to hit; the walk goes on to the reach.
            if (blockAccess->IsValidPosition(cell.x, cell.y, cell.z)) {
                std::optional<Entry> best;
                const BlockState state = ReadBlockState(cell);
                if (!isFluidOnlyBlock(state.Block())) {
                    for (const auto& b : BlockRegistry::GetBlockShapeSet(state)) {
                        const auto e = clipBox(glm::dvec3(cell) + glm::dvec3(b.min),
                                               glm::dvec3(cell) + glm::dvec3(b.max));
                        if (e && (!best || e->t < best->t)) best = e;
                    }
                }
                // ClipContext.Fluid.SOURCE_ONLY: FluidState::isSource, any fluid.
                const Game::FluidState fluid = Game::GetFluidState(*blockAccess, cell);
                if (!fluid.IsEmpty() && fluid.IsSource()) {
                    // FluidState.getShape: the full cell under the same fluid,
                    // else the fluid's own height (8/9 for a source).
                    const Game::FluidState above =
                        Game::GetFluidState(*blockAccess, cell + glm::ivec3(0, 1, 0));
                    const double height = above.IsSame(fluid.type) ? 1.0 : fluid.OwnHeight();
                    const auto e = clipBox(glm::dvec3(cell),
                                           glm::dvec3(cell) + glm::dvec3(1.0, height, 1.0));
                    // blockDistance <= fluidDistance ? block : fluid
                    if (e && (!best || e->t < best->t)) best = e;
                }
                if (best) {
                    const int a = best->axis;
                    // Our face numbering: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z; the
                    // face entered is the one facing back along the ray.
                    const int face = a * 2 + (dir[a] > 0.0 ? 1 : 0);
                    glm::ivec3 normal(0);
                    normal[a] = dir[a] > 0.0 ? -1 : 1;

                    // hitResult.withPosition(hitResult.getBlockPos().above()):
                    // the same face and location, the cell above.
                    RaycastHit hit{};
                    hit.blockPos    = cell + glm::ivec3(0, 1, 0);
                    hit.adjacentPos = hit.blockPos + normal;
                    hit.hitPoint    = eye + dir * best->t;
                    hit.normal      = glm::vec3(normal);
                    hit.cursorPos   = glm::vec3(hit.hitPoint - glm::dvec3(hit.blockPos));
                    hit.state       = ReadBlockState(hit.blockPos);
                    hit.blockId     = hit.state.Block();
                    hit.distance    = static_cast<float>(best->t);
                    hit.hitFace     = face;
                    hit.insideBlock = false;
                    return hit;
                }
            }
            if (tMax.x < tMax.y && tMax.x < tMax.z) {
                cell.x += step.x; travelled = tMax.x; tMax.x += tDelta.x;
            } else if (tMax.y < tMax.z) {
                cell.y += step.y; travelled = tMax.y; tMax.y += tDelta.y;
            } else {
                cell.z += step.z; travelled = tMax.z; tMax.z += tDelta.z;
            }
        }
        return std::nullopt;   // a MISS: BlockItem.useOn places nothing there
    }

    bool ClientPlayerController::UsePlaceOnWaterItem() {
        if (!HoldsPlaceOnWaterItem()) return false;
        const std::optional<RaycastHit> hit = ClipPlaceOnWater();
        if (!hit) return true;

        OnHotbarChanged(player->inventory.GetSelectedSlot());
        // Resolve the prediction before sending, as every placement does —
        // once predicted, the cell is no longer free.
        glm::ivec3 predictPos{};
        BlockID    predictBlock = BlockID::Air;
        Game::BlockState predictState;
        const bool predictable = ComputePredictedPlacement(*hit, predictPos, predictBlock, predictState,
                                                           /*fromUse=*/true);
        const uint32_t sequence = SendUseItemOn(*hit, /*hand=*/0, /*altInteract=*/false,
                                                Client::ClientLevels::ActiveDimension(),
                                                /*fromUse=*/true);
        if (predictable) {
            PredictBlock(predictPos, predictBlock, sequence, predictState);
            PlayBlockPlaceSound(predictPos, predictState);
            if (!player->IsCreative()) player->inventory.ConsumeSelectedBlock();
            armSwingPending = true;
        }
        return true;
    }

    bool ClientPlayerController::PredictUseItemOn(const RaycastHit& hit,
                                                  uint32_t hand,
                                                  uint32_t sequence) {
        if (!player || !Client::g_clientBlockAccess) return false;

        const Game::ItemID heldId = player->inventory.GetSelectedItem();

#if ENABLE_PORTAL_GUN
        // PortalGun's useOn drives the SERVER portal registry (spawns the
        // projectile, moves the pair). It has its own client-side path via
        // SpawnPortalProjectile, so running it here would double-fire.
        if (heldId != Game::Items::Air && heldId == Game::Items::PortalGun) return false;
#endif
#if ENABLE_IMMERSIVE_PORTALS
        // The portal wand's useOn is server state only (it talks to the
        // immersive registry and casts its player to the server's); the
        // client sends the click and shows nothing until the reply.
        if (heldId != Game::Items::Air && heldId == Game::Items::PortalWand) return false;
#endif
        if (heldId != Game::Items::Air && heldId == Game::Items::AoWand) return false;   // server state only

        Client::ClientUsePlayer usePlayer(player);
        float yawDeg, pitchDeg; LookAngles(yawDeg, pitchDeg);
        usePlayer.setRotation(yawDeg, pitchDeg);

        Game::BlockHitResult bhr(hit.blockPos, hit.hitFace, hit.hitPoint, hit.insideBlock);
        Game::UseOnContext ctx(Client::g_clientBlockAccess, &usePlayer, hand, bhr);
        // The placement orients by the look in the clicked block's space —
        // mapped through the portal the ray crossed, if any — exactly as
        // the server will (UseItemOnC2S's look rotation). The use-player
        // keeps the player's own rotation, as the server's ServerPlayer does.
        HitLookAngles(hit, ctx.playerYaw, ctx.playerPitch);

        // Same dispatch order as PlayerSession::HandleUseItemOn, which in turn
        // mirrors ServerPlayerGameMode.useItemOn:
        //   block.useItemOn → block.useWithoutItem → item.useOn
        const bool somethingInHands = (heldId != Game::Items::Air);
        const bool suppressBlockUse = player->physics.isSneaking && somethingInHands;

        Client::g_clientBlockAccess->BeginPrediction(sequence);
        bool consumed = false;

        const Block& clicked = BlockRegistry::Get(ReadBlock(hit.blockPos));
        Game::ItemStack& heldStack =
            player->inventory.MutableSlot(usePlayer.handSlotIndex(hand));

        if (!suppressBlockUse) {
            if (clicked.useItemOn) {
                Game::UseResult r = clicked.useItemOn(
                    heldStack, Client::g_clientBlockAccess, hit.blockPos, &usePlayer, hand, bhr);
                if (Game::ConsumesAction(r)) {
                    consumed = true;
                } else if (r == Game::UseResult::TryEmptyHandInteraction && hand == 0
                           && clicked.useWithoutItem) {
                    Game::UseResult r2 = clicked.useWithoutItem(
                        Client::g_clientBlockAccess, hit.blockPos, &usePlayer, bhr);
                    consumed = Game::ConsumesAction(r2);
                }
            } else if (clicked.useWithoutItem) {
                if (!somethingInHands || hand == 0) {
                    Game::UseResult r = clicked.useWithoutItem(
                        Client::g_clientBlockAccess, hit.blockPos, &usePlayer, bhr);
                    consumed = Game::ConsumesAction(r);
                }
            }
        }

        // ItemStack.useOn: a player who may not build (adventure) uses the
        // item on this block only when its CAN_PLACE_ON matches it.
        if (!consumed && somethingInHands && player->gameMode == 2 &&
            !ClientAdventureAllows(heldStack.get(Game::DataComponents::CAN_PLACE_ON), ReadBlockState(hit.blockPos))) {
            Client::g_clientBlockAccess->EndPrediction();
            return false;
        }

        if (!consumed && somethingInHands) {
            // Item.useOn — the item's own, or its BLOCK_TRANSFORMER's.
            const Game::ItemUseOnFn heldUseOn = Game::StackUseOn(heldStack);
            if (heldUseOn) {
                // Creative stack preservation, mirroring the server (and MC's
                // ServerPlayerGameMode.useItemOn lines 365-371) — only the
                // COUNT, so a callback's component writes survive. See the
                // long note at the server's copy in
                // PlayerSession::HandleUseItemOn for why the whole-stack
                // restore is kept for the emptied case only.
                const Game::ItemStack stackBefore = heldStack;
                Game::UseResult r = heldUseOn(ctx, heldStack);
                if (player->IsCreative()) {
                    if (heldStack.IsEmpty()) heldStack = stackBefore;
                    else                     heldStack.count = stackBefore.count;
                }
                // Fail also stops the chain — the server won't fall through to
                // placement either, so neither should our caller.
                consumed = Game::ConsumesAction(r) || (r == Game::UseResult::Fail);
            }
        }

        Client::g_clientBlockAccess->EndPrediction();
        return consumed;
    }

    void ClientPlayerController::PredictUseItem(uint32_t hand, uint32_t sequence) {
        if (!player || !Client::g_clientBlockAccess) return;

        const Game::ItemID heldId = player->inventory.GetSelectedItem();
        if (heldId == Game::Items::Air) return;

        const Game::Item& heldItem = Game::ItemRegistry::Get(heldId);
        // ONLY items with an explicit `use` override are predicted (buckets).
        // The Item_DefaultUse fallback is the CONSUMABLE / EQUIPPABLE /
        // BLOCKS_ATTACKS chain — eating, armour swaps, shield raising — which
        // is server-authoritative lifecycle state, not a block edit. Its
        // client-visible half is already predicted by StartPredictedUse.
        if (!heldItem.use) return;

        Client::ClientUsePlayer usePlayer(player);
        float yawDeg, pitchDeg; LookAngles(yawDeg, pitchDeg);
        usePlayer.setRotation(yawDeg, pitchDeg);

        Game::ItemStack& heldStack =
            player->inventory.MutableSlot(usePlayer.handSlotIndex(hand));

        // MC MultiPlayerGameMode.useItem: an item whose cooldown group is
        // resting is not used on the client (the packet still goes; the
        // server refuses it on its own table).
        if (Client::LocalItemCooldowns::IsOnCooldown(heldStack)) return;

        Client::g_clientBlockAccess->BeginPrediction(sequence);
        // Same whole-stack restore as the useOn path above: an emptied stack
        // has had its item id cleared, so putting the count back is not enough.
        const Game::ItemStack stackBefore = heldStack;
        heldItem.use(Client::g_clientBlockAccess, &usePlayer, hand, heldStack);
        if (player->IsCreative()) heldStack = stackBefore;
        Client::g_clientBlockAccess->EndPrediction();
    }

    uint32_t ClientPlayerController::SendUseItem(int hand) {
        // Mirrors MC MultiPlayerGameMode.useItem's packet send
        // (ServerboundUseItemPacket: hand, sequence, yRot, xRot).
        if (!networkClient || !networkClient->IsConnected()) {
            return 0;
        }

        // Derive fresh yaw/pitch from the live look vector (the player's
        // yaw/pitch members are stale — mouse-look writes the camera
        // directly; see the lookDir comment in Player.hpp).
        float yRot = 0.0f, xRot = 0.0f;
        if (player) {
            yRot = Game::Mth::YRotFromVector(player->lookDir);
            xRot = Game::Mth::XRotFromVector(player->lookDir);
        }

        Network::UseItemC2SPacket packet;
        packet.hand     = static_cast<uint32_t>(hand);
        packet.sequence = static_cast<uint32_t>(++interactSeq);
        packet.yRot     = yRot;
        packet.xRot     = xRot;

        auto data = Network::Serialization::Serialize(packet);
        if (auto connection = networkClient->GetConnection()) {
            FlushMovement();
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::UseItem), data);
            Log::Debug("Sent UseItemC2S: hand=%d seq=%d yRot=%.1f xRot=%.1f",
                       hand, interactSeq, yRot, xRot);
        }
        return packet.sequence;
    }

    void ClientPlayerController::SendPlayerAction(Network::PlayerAction action) {
        // Mirrors MC's ServerboundPlayerActionPacket sends (BlockPos.ZERO +
        // Direction.DOWN for the non-block actions — MultiPlayerGameMode.java:485).
        if (!networkClient || !networkClient->IsConnected()) {
            return;
        }
        Network::PlayerActionC2SPacket packet;
        packet.action   = action;
        packet.sequence = static_cast<uint32_t>(++interactSeq);
        auto data = Network::Serialization::Serialize(packet);
        if (auto connection = networkClient->GetConnection()) {
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::PlayerAction), data);
            Log::Debug("Sent PlayerActionC2S: action=%u seq=%d",
                       static_cast<unsigned>(action), interactSeq);
        }
    }

    uint32_t ClientPlayerController::PickUseHand() const {
        // Mirrors MC Minecraft.startUseItem's MAIN_HAND → OFF_HAND loop:
        // prefer the main hand when it has anything usable; otherwise fall
        // back to the offhand when THAT holds a hold-to-use item (shield).
        if (!player) return 0;
        const ItemStack& main = player->inventory.GetSlot(
            Inventory::HotbarToIndex(player->inventory.GetSelectedSlot()));
        if (!main.IsEmpty()
            && (GetUseDuration(main) > 0
                || ItemRegistry::Get(main.itemId).use != nullptr)) {
            return 0;
        }
        const ItemStack& off = player->inventory.GetSlot(Inventory::OFFHAND_BEGIN);
        // (A firework rocket too: its use boosts a glider from either hand.)
        if (!off.IsEmpty() && (GetUseDuration(off) > 0 || off.itemId == Game::Items::FireworkRocket)) {
            return 1;
        }
        // A fishing rod in the off hand casts when the main hand has nothing
        // to use (FishingRodItem.use answers for either hand).
        if (!off.IsEmpty() && off.itemId == Items::FishingRod) {
            return 1;
        }
        return 0;
    }

    void ClientPlayerController::SwingForRodUse(uint32_t hand) {
        // MC Minecraft.startUseItem swings the arm for an item use that
        // answers SUCCESS client-side, which FishingRodItem.use always does:
        // the cast and the reel-in flick. (Only the main arm swings in the
        // first-person hand.)
        if (!player || hand != 0) return;
        if (player->inventory.GetSelectedStack().itemId == Items::FishingRod) armSwingPending = true;
    }

    void ClientPlayerController::StartPredictedUse(uint32_t hand, bool fromUseOn) {
        // Client-side mirror of LivingEntity.startUsingItem — only fires when
        // the held stack actually has a use duration (food, shield, …).
        // Components are known client-side (item defaults + synced per-stack
        // patches), so GetUseDuration gives the same answer as the server.
        if (!player) return;
        const int slot = (hand == 0)
            ? Inventory::HotbarToIndex(player->inventory.GetSelectedSlot())
            : Inventory::OFFHAND_BEGIN;
        const ItemStack& stack = player->inventory.GetSlot(slot);
        if (stack.IsEmpty()) return;
        const int duration = GetUseDuration(stack);
        if (duration <= 0) return;

        // BrushItem: only useOn starts the hold (Item.use's default passes).
        if (stack.itemId == Game::Items::Brush && !fromUseOn) return;
        // MultiPlayerGameMode.useItem: an item whose cooldown group is
        // resting is not used (a goat horn inside its 7 s, a shield an axe
        // just disabled) — so no hold starts either.
        if (Client::LocalItemCooldowns::IsOnCooldown(stack)) return;

        // CrossbowItem.use: a loaded crossbow shoots (no hold), and one with
        // nothing to load fails — only an empty crossbow with ammunition in
        // reach (or infinite materials) starts the draw.
        if (stack.itemId == Game::Items::Crossbow) {
            if (Game::FireworkItems::IsCrossbowCharged(stack)) return;
            if (!Game::FireworkItems::CanDrawCrossbow(player->inventory, player->IsCreative())) return;
        }

        // TridentItem.use (it runs on the client too): no draw when the next
        // point of wear would break it, nor for Riptide out of water and
        // rain.
        if (stack.itemId == Game::Items::Trident) {
            if (Game::NextDamageWillBreak(stack)) return;
            if (Game::EnchantmentHelper::GetTridentSpinAttackStrength(stack) > 0.0f &&
                !LocalInWaterOrRain(*player)) {
                return;
            }
        }

        // Food gate — MC's client runs the same Consumable.startConsuming
        // canEat check (Player.canEat = canAlwaysEat || foodData.needsFood),
        // so at FULL hunger the eat animation never starts. Without this
        // mirror the client played the whole 1.6 s animation while the
        // server correctly refused — "eating looks broken". The food level
        // is server-synced (SetHealthS2C), so the answer matches.
        if (stack.get(DataComponents::CONSUMABLE)) {
            if (auto food = stack.get(DataComponents::FOOD)) {
                if (!food->canAlwaysEat && player->food >= 20) {
                    return;
                }
            }
        }

        player->usingItem        = true;
        player->usingHand        = hand;
        player->useItemRemaining = duration;
        player->useItemDuration  = duration;
        player->useAnim          = GetUseAnimation(stack);
        player->useItemId        = stack.itemId;

        // Item.use for a KINETIC_WEAPON: kineticWeapon.makeSound(player) —
        // level.playSound(player, …), which on the client is the player's
        // own copy (the server sends it to everyone else).
        if (const auto kinetic = Game::Spear::Kinetic(stack); kinetic && !kinetic->sound.empty()) {
            Client::Sounds::PlayLocal(player->physics.position, kinetic->sound, Game::SoundSource::Players,
                                      1.0f, 1.0f);
        }
    }

    void ClientPlayerController::StopPredictedUse() {
        if (!player) return;
        player->usingItem        = false;
        player->useItemRemaining = 0;
        player->useItemDuration  = 0;
        player->useAnim          = ItemUseAnimation::NONE;
    }

    void ClientPlayerController::UpdateUsingTick() {
        // Predicted-use countdown at 20 TPS — the client-side shadow of
        // ServerPlayer::updateUsingItem. On zero the server's
        // completeUsingItem fires (its slot broadcast updates our stack);
        // locally we just clear the pose state.
        if (!player || !player->usingItem) return;

        // MC LivingEntity.updateUsingItem runs on the client too:
        // ItemStack.onUseTick → Consumable.emitParticlesAndSounds every 4th
        // tick past the first 21.875% — the eater hears its own chewing from
        // its own prediction (Player.playSound: the server sends it to
        // everyone else). The last bite is MC's entity event 9
        // (completeUsingItem → onConsume's emit) at the end of the countdown.
        const auto consumeSound = [this](const Game::Consumable& consumable) {
            if (consumable.sound.empty()) return;
            static Game::JavaRandom s_random(static_cast<int64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()));
            const bool drink = consumable.animation == ItemUseAnimation::DRINK;
            const float eatVolume  = s_random.NextBool() ? 0.5f : 1.0f;
            const float eatPitch   = 1.0f + 0.2f * (s_random.NextFloat() - s_random.NextFloat());
            const float drinkPitch = 0.9f + s_random.NextFloat() * 0.1f;
            Client::Sounds::PlayLocal(player->physics.position, consumable.sound, Game::SoundSource::Players,
                                      drink ? 0.5f : eatVolume, drink ? drinkPitch : eatPitch);
        };
        const int slot = (player->usingHand == 0)
            ? Inventory::HotbarToIndex(player->inventory.GetSelectedSlot())
            : Inventory::OFFHAND_BEGIN;
        const auto& usedStack = player->inventory.GetSlot(slot);
        const auto consumable = usedStack.get(DataComponents::CONSUMABLE);
        // emitParticlesAndSounds' particle half: LivingEntity.spawnItemParticles
        // (5 crumbs per bite, 16 on the last) when the item has them.
        const auto consumeParticles = [this, &usedStack](const Game::Consumable& consumable, int count) {
            if (!consumable.hasConsumeParticles || usedStack.IsEmpty()) return;
            float yRot = 0.0f, xRot = 0.0f;
            LookAngles(yRot, xRot);
            Client::LevelEvents::SpawnItemParticles(player->GetEyePosition(), yRot, xRot, usedStack.itemId, count);
        };
        if (consumable &&
            Game::ConsumableBehavior::ShouldEmitParticlesAndSounds(*consumable, player->useItemRemaining)) {
            consumeParticles(*consumable, 5);
            consumeSound(*consumable);
        }

        if (--player->useItemRemaining <= 0) {
            if (consumable) {
                consumeParticles(*consumable, 16);
                consumeSound(*consumable);
            }
            StopPredictedUse();
        }
    }

    void ClientPlayerController::UpdateBreakingTick() {
        if (!player) return;
        // MultiPlayerGameMode.startDestroyBlock / continueDestroyBlock:
        // Player.blockActionRestricted — a spectator never digs.
        if (player->IsSpectator()) {
            if (digState.isDestroying) AbortDig();
            return;
        }

        // Post-break delay (MC: 5 ticks after a successful break before the
        // next click can re-arm a dig).
        if (digState.destroyDelay > 0) {
            --digState.destroyDelay;
            return;
        }

        if (!breakButtonHeld) {
            if (digState.isDestroying) AbortDig();
            return;
        }
        // The press already hit an entity — see pressHitEntity.
        if (pressHitEntity) {
            if (digState.isDestroying) AbortDig();
            return;
        }
        // MC Minecraft.continueAttack: `if (!heldItem.has(PIERCING_WEAPON))`
        // — a spear never mines.
        if (Game::Spear::IsPiercing(player->inventory.GetSelectedStack())) {
            if (digState.isDestroying) AbortDig();
            return;
        }

        // The fill tool's cancel: Alt + left-click drops the marked corner
        // and breaks nothing.
        if (m_fill.armed && Input::IsKeyDown(Input::Key::LeftAlt)) {
            m_fill.armed = false;
            if (digState.isDestroying) AbortDig();
            return;
        }

#if ENABLE_PORTAL_GUN
        // The PortalGun hijacks left-click for the blue portal, and
        // StartAttack returns early for it — but it sets breakButtonHeld
        // BEFORE that return, so this tick still saw a held mine button and
        // went on to break the block the portal had just been shot at
        // (instantly, in creative). The guard has to be here as well, not only
        // on the click edge.
        {
            const Game::ItemID held = player->inventory.GetSelectedItem();
            if (held != Game::ItemID(0) && held == Game::Items::PortalGun) {
                if (digState.isDestroying) AbortDig();
                return;
            }
        }
#endif

        // MC Minecraft.continueAttack only continues a dig when
        // `hitResult.getType() == BLOCK`, and hitResult is ONE result — an
        // entity nearer than the block replaces it outright. So a mob standing
        // in front of a wall makes the wall unmineable for as long as you are
        // aiming at the mob.
        //
        // Without this, StartAttack's `TryAttackEntity() -> return` was not
        // enough: it sets breakButtonHeld BEFORE that return, so the very next
        // tick started digging the block behind the mob anyway — instantly, in
        // creative.
        if (PickEntity() != 0) {
            if (digState.isDestroying) AbortDig();
            return;
        }

        const auto& currentHit = player->lastBlockHit;
        const bool haveTarget = currentHit.has_value();
        const glm::ivec3 hitPos = haveTarget ? currentHit->blockPos : glm::ivec3(0, -1024, 0);

        // No target while held — nothing to do this tick.
        if (!haveTarget) {
            if (digState.isDestroying) AbortDig();
            return;
        }

        // Creative held-mining: MC's continueDestroyBlock checks instabuild
        // right after the destroyDelay countdown, so held-LMB just destroys
        // whatever is under the crosshair once every CREATIVE_BREAK_DELAY_TICKS
        // — no progress state, no per-block hold.
        if (player->IsCreative()) {
            // A dig left over from a mid-mine gamemode switch has to be torn
            // down (and its ABORT packet sent) before we start instant-breaking.
            if (digState.isDestroying) AbortDig();
            CreativeDestroy(hitPos, currentHit->hitFace);
            return;
        }

        // MultiPlayerGameMode.startDestroyBlock / continueDestroyBlock:
        // Player.blockActionRestricted's adventure half — without a main-hand
        // CAN_BREAK matching the block, an adventure player does not dig.
        if (player->gameMode == 2) {
            const Game::ItemStack& mainHand = player->inventory.GetSelectedStack();
            if (mainHand.IsEmpty() ||
                !ClientAdventureAllows(mainHand.get(Game::DataComponents::CAN_BREAK), ReadBlockState(hitPos))) {
                if (digState.isDestroying) AbortDig();
                return;
            }
        }

        // Target changed mid-mine: abort and restart on the new block.
        if (digState.isDestroying && hitPos != digState.destroyBlockPos) {
            AbortDig();
        }

        // (Re-)start dig if not currently mining.
        if (!digState.isDestroying) {
            StartDig(hitPos, currentHit->hitFace);
            // Fall through so we ALSO get one tick of progress this frame.
        }

        // Look up the block; refresh the cached ID if it changed (rare —
        // server might have set a different block during the mine).
        BlockID currentBlock = digState.destroyingBlockId;
        {
            const BlockID worldBlock = ReadBlock(hitPos);
            if (worldBlock != BlockID::Air) {
                currentBlock = worldBlock;
                digState.destroyingBlockId    = worldBlock;
                // Refresh the state with it — a crop that finished growing
                // mid-dig should still drop as the age it was broken at.
                digState.destroyingBlockState = ReadBlockState(hitPos);
            }
        }
        const Block& block = BlockRegistry::Get(currentBlock);

        // Per-tick progress increment (MC's BlockBehaviour.getDestroyProgress).
        const Game::ItemStack& held = player->inventory.GetSelectedStack();
        const bool onGround = player->physics.isOnGround;
        const float inc = GetDestroyProgressPerTick(held, block, onGround,
                                                    player->GetDigSpeedMultiplier(),
                                                    player->physics.isEyeInWater,
                                                    player->GetMiningEfficiency(),
                                                    player->GetSubmergedMiningSpeed());

        digState.destroyProgress += inc;
        // MC continueDestroyBlock: `if (destroyTicks % 4.0F == 0.0F)` the hit
        // sound, BEFORE the tick count advances.
        if (digState.destroyTicks % 4 == 0) {
            PlayBlockHitSound(hitPos, ReadBlockState(hitPos));
        }
        // The crack particle on the face being mined, every tick of the dig
        // (MC's levelEvent 2019 / 2020 → ClientLevel.addBreakingBlockEffects;
        // the server sends it to everyone else, this client shows its own).
        {
            // RaycastHit::hitFace (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z) as a
            // Direction ordinal (DOWN, UP, NORTH, SOUTH, WEST, EAST).
            static constexpr int kDirection[6] = {5, 4, 1, 0, 3, 2};
            const int face = digState.destroyFace >= 0 && digState.destroyFace < 6
                ? kDirection[digState.destroyFace] : 1;
            Client::LevelEvents::AddBreakingBlockEffects(hitPos, face, /*playSound=*/false);
        }
        digState.destroyTicks    += 1;

        // Continuous-mine arm swing (MC: every 4 ticks while mining).
        if (digState.destroyTicks - digState.lastSwingTick >= MINE_SWING_TICKS) {
            armSwingPending = true;
            digState.lastSwingTick = digState.destroyTicks;
        }

        if (digState.destroyProgress >= 1.0f) {
            FinishDig();
        }
    }

    void ClientPlayerController::UpdatePlacingTick() {
        if (!player) return;
        // A spectator's use is one packet per press (StartUseItem).
        if (player->IsSpectator()) return;

        // MC handleKeybinds:1996 —
        //   if (keyUse.isDown() && rightClickDelay == 0 && !player.isUsingItem())
        //       startUseItem();
        // The gap between held-RMB re-fires is rightClickDelay, which
        // StartUseItem resets to 4 and Tick decrements — the same counter the
        // first click sets, so the edge and the repeats share one cadence
        // instead of the two separate ones this used to keep.
        if (!placeButtonHeld) return;
        // Mid-use suppression — MC's Minecraft.startUseItem is a no-op while
        // player.isUsingItem() (Minecraft.java:1656), so held-RMB during an
        // eat/block must not spam placements.
        if (player->usingItem) return;
        if (rightClickDelay > 0) return;

        const auto& currentHit = player->lastBlockHit;

        // A lily pad / frogspawn repeat is startUseItem again: the crosshair
        // block's use first (if the crosshair is on one), then the item's own
        // SOURCE_ONLY clip — which finds the open water the crosshair ray
        // passes straight through, so a held right-click lays pads across a
        // pond with nothing under the crosshair at all.
        if (HoldsPlaceOnWaterItem()) {
            bool usedOn = false;
            if (currentHit.has_value()) {
                OnHotbarChanged(player->inventory.GetSelectedSlot());
                const uint32_t sequence = SendUseItemOn(*currentHit, 0);
                usedOn = PredictUseItemOn(*currentHit, 0, sequence);
            }
            if (!usedOn) UsePlaceOnWaterItem();
            rightClickDelay = PLACE_REFIRE_TICKS;
            return;
        }

        if (!currentHit.has_value()) return;

        // Only re-fire for items that actually place blocks; for tools we
        // already fired on the edge and shouldn't keep spamming.
        //
        // `IsBlockItem` alone is not that question: a seed is a pure item that
        // nonetheless places a block (Item::placesBlock — see ItemBehaviors'
        // seed table). Testing only the former would let you hold right-click
        // to lay a row of stone but not a row of wheat, which is the sort of
        // inconsistency nobody reports and everybody feels.
        const Game::ItemID held = player->inventory.GetSelectedItem();
        if (held == Game::Items::Air) return;
        // A brush whose 200-tick hold ran out while RMB stayed down: MC's
        // startUseItem repeat runs BrushItem.useOn again and the brushing
        // goes on.
        if (held == Game::Items::Brush) {
            OnHotbarChanged(player->inventory.GetSelectedSlot());
            const uint32_t sequence = SendUseItemOn(*currentHit, 0);
            if (PredictUseItemOn(*currentHit, 0, sequence)) StartPredictedUse(0, /*fromUseOn=*/true);
            rightClickDelay = PLACE_REFIRE_TICKS;
            return;
        }
        if (!ItemRegistry::IsBlockItem(held) &&
            ItemRegistry::Get(held).placesBlock == BlockID::Air) {
            return;
        }

#if ENABLE_PORTAL_GUN
        // PortalGun: continuous-RMB does NOT spam projectiles (its StartUseItem edge
        // already started the projectile + viewmodel anim). Skip.
        if (held == Game::Items::PortalGun) return;
#endif

        OnHotbarChanged(player->inventory.GetSelectedSlot());
        // Resolve the prediction BEFORE sending — once the block is predicted
        // into the chunk cache the target cell is no longer Air and the
        // predictor would refuse it.
        glm::ivec3 predictPos{};
        BlockID    predictBlock = BlockID::Air;
        Game::BlockState predictState;
        const bool predictable = ComputePredictedPlacement(*currentHit, predictPos, predictBlock,
                                                           predictState);
        const uint32_t sequence = SendUseItemOn(*currentHit, 0);
        const bool usedOn = PredictUseItemOn(*currentHit, 0, sequence);
        if (!usedOn && predictable) {
            PredictBlock(predictPos, predictBlock, sequence, predictState);
            PlayBlockPlaceSound(predictPos, predictState);
            if (Game::IsDoorBlock(predictBlock)) {
                PredictBlock(predictPos + glm::ivec3(0, 1, 0), predictBlock, sequence,
                             Game::DoorUpperState(predictState));
            }
            if (Game::IsBedBlock(predictBlock)) {
                PredictBlock(Game::BedOtherHalfPos(predictPos, predictState), predictBlock, sequence,
                             Game::BedHeadState(predictState));
            }
        }
        // Same guards as the edge path in StartUseItem: a block that swallowed
        // the click consumes nothing server-side, creative never consumes at
        // all, and a placement the server will reject consumes nothing either
        // (which `predictable` is exactly the test for — see the longer note
        // there about carrots). Without them, holding RMB on a crafting table
        // walks the held stack down until the server corrects it.
        if (!usedOn && predictable && !player->IsCreative()) {
            player->inventory.ConsumeSelectedBlock();
        }
        rightClickDelay = PLACE_REFIRE_TICKS;
        // Each repeat-place during held-RMB plays the arm swing, matching
        // MC. The first place is handled by StartUseItem's edge feeding
        // placeEdge in PlatformMain; this covers every subsequent one.
        armSwingPending = true;
    }

    void ClientPlayerController::OnHotbarChanged(int slot) {
        if (!player) return;

        player->SelectSlot(slot);

        // Switching slots cancels a predicted use — the server's
        // updatingUsingItem sees the hand-item mismatch and stops on its own
        // (LivingEntity.java:3256-3261), so no release packet is needed.
        if (player->usingItem && player->usingHand == 0) {
            StopPredictedUse();
        }

        // Send slot change + block type to server (MC: ServerboundSetCarriedItemPacket)
        if (networkClient && networkClient->IsConnected()) {
            BlockID block = player->GetSelectedBlock();
            Network::HeldItemChangeC2SPacket packet(
                static_cast<int16_t>(slot),
                static_cast<uint16_t>(block));
            auto data = Network::Serialization::Serialize(packet);
            auto connection = networkClient->GetConnection();
            if (connection) {
                connection->SendPacket(static_cast<uint8_t>(Network::PacketId::HeldItemChange), data);
            }
        }
    }

    void ClientPlayerController::SendPickItem(const Network::PickItemC2SPacket& packet) {
        // No local prediction: the server decides which slot the item lands
        // in (it may swap a stack out of the inventory), and its
        // SetHeldSlotS2C plus the slot diff bring the hotbar up to date.
        if (!networkClient || !networkClient->IsConnected()) return;
        auto data = Network::Serialization::Serialize(packet);
        if (auto conn = networkClient->GetConnection()) {
            conn->SendPacket(static_cast<uint8_t>(Network::PacketId::PickItemC2S), data);
        }
    }

    void ClientPlayerController::OnRespawnRequest() {
        // TODO: Implement respawn request for multiplayer
        // This would send a client command packet to respawn
        // net->SendClientCommand(RESPAWN);
        
        Log::Debug("Respawn request (TODO: Implement for multiplayer)");
    }

    int32_t ClientPlayerController::PickEntityAlong(const glm::dvec3& origin, const glm::vec3& dir,
                                                    float range, float blockLimit,
                                                    int* outDragonPart, glm::dvec3* outHit,
                                                    float minRange, float margin) const {
        if (outDragonPart) *outDragonPart = -1;
        if (!player) return 0;
        if (!Client::g_clientMobManager) return 0;

        // MC's entity pick distance in survival is 3.0 blocks
        // (Attributes.ENTITY_INTERACTION_RANGE). The server re-checks with a
        // more generous 6.0 to absorb latency — see HandleInteract.
        // `range` is the pick distance left along this ray, `blockLimit` the
        // distance at which the crosshair's block stops it.

        // Ray-vs-AABB over the entities in range, nearest wins, each box
        // inflated by its own MC getPickRadius (ProjectileUtil.
        // getEntityHitResult: getBoundingBox().inflate(getPickRadius())) —
        // 0 for players and mobs, 1 for a punchable projectile. (A blanket
        // 0.3 margin here reached a painting's 1/16-deep box round the edge
        // of the block it hangs on, so a hit on that block's top or side
        // broke the painting.)

        int32_t bestId = 0;
        float bestT = range;

        // Slab test of the ray against a DOUBLE world box, taken in a frame
        // anchored at the ray's origin: the box's faces are a few blocks
        // from the eye, so float is exact once the world-sized numbers have
        // cancelled in double. Returns the entry distance, or a negative
        // number for a miss.
        const auto slab = [&](const glm::dvec3& mn, const glm::dvec3& mx) -> float {
            float tMin = minRange, tMax = bestT;
            for (int a = 0; a < 3; ++a) {
                const float d  = dir[a];
                const float lo = static_cast<float>(mn[a] - origin[a]) - margin;
                const float hi = static_cast<float>(mx[a] - origin[a]) + margin;
                if (std::abs(d) < 1e-8f) {
                    if (0.0f < lo || 0.0f > hi) return -1.0f;
                    continue;
                }
                float t1 = lo / d, t2 = hi / d;
                if (t1 > t2) std::swap(t1, t2);
                tMin = std::max(tMin, t1);
                tMax = std::min(tMax, t2);
                if (tMin > tMax) return -1.0f;
            }
            return tMin;
        };

        // Other players first, so the loop below can only beat them on
        // distance. A remote player's id IS its connection id, which is what
        // lets the server tell it from a mob without a kind byte on the wire.
        if (Client::g_remotePlayerManager) {
            for (const auto& [pid, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (!rp.positionInitialized) continue;
                if (!Client::IsRemotePlayerInBoundLevel(rp)) continue;
                // MC Player.isPickable: never a spectator. Nor the entity the
                // view is coming from (getEntityHitResult excludes the camera).
                if (rp.IsSpectator()) continue;
                if (static_cast<int32_t>(pid) == player->cameraEntityId) continue;

                // MC's player box (0.6 wide, 1.8 tall, feet at the position),
                // or the morph's — a snow golem's pumpkin sits above 1.8 and
                // a bee is a hand's width — at the player's scale. The server
                // reaches the same box (PlayerEntityView::BaseBbWidth/Height).
                const Game::Morph::Dims dims = Game::Morph::DimsOf(rp.morph);
                const double hw = static_cast<double>(dims.width) * rp.scale * 0.5;
                const double h  = static_cast<double>(dims.height) * rp.scale;
                // Player.getPickRadius: 0.
                const glm::dvec3 mn = rp.position - glm::dvec3(hw, 0.0, hw);
                const glm::dvec3 mx = rp.position + glm::dvec3(hw, h, hw);
                const float tMin = slab(mn, mx);
                if (tMin < 0.0f || tMin >= bestT) continue;
                if (blockLimit < tMin) continue;
                bestT = tMin;
                bestId = static_cast<int32_t>(pid);
            }
        }

        // The manager's per-tick candidate list, not the full mob map: with a
        // hundred thousand primed TNT in the level, walking every mob per
        // frame was half the main thread. The list holds the non-TNT mobs
        // within 48 blocks of the player, rebuilt each client tick; ids
        // rather than pointers because a removal packet can land mid-frame.
        for (const int32_t id : Client::g_clientMobManager->PickCandidates()) {
            const Client::ClientMob* centry = Client::g_clientMobManager->GetMob(id);
            if (!centry || !centry->mob) continue;
            if (id == player->cameraEntityId) continue;   // the spectated entity
            const Game::Mob& mob = *centry->mob;
            if (!mob.IsAlive()) continue;
            // MC ProjectileUtil.getEntityHitResult: nothing that shares the
            // player's root vehicle — the boat they sit in, the mob sitting
            // behind them — is picked from the seat.
            if (player->vehicleId != 0) {
                const Game::Entity* root = &mob;
                while (root->GetVehicle()) root = root->GetVehicle();
                const Client::ClientMob* own = Client::g_clientMobManager->GetMob(player->vehicleId);
                const Game::Entity* ownRoot = own && own->mob ? own->mob.get() : nullptr;
                while (ownRoot && ownRoot->GetVehicle()) ownRoot = ownRoot->GetVehicle();
                if (root->GetId() == player->vehicleId || (ownRoot && root == ownRoot)) continue;
            }
            // ── Ender dragon: pick the PART boxes, never the whole box ──
            //
            // MC's dragon body is not pickable; its eight EnderDragonPart
            // entities are, each tested with the same 0.3 pick margin. The
            // parts are not entities here, so the ray is tested against
            // ComputePartBoxes' layout and the winning part index rides the
            // interact packet for the server's head-vs-body routing.
            if (const auto* dragon =
                    dynamic_cast<const Game::EnderDragon*>(&mob)) {
                Game::AABB parts[Game::EnderDragon::kDragonPartCount];
                dragon->ComputePartBoxes(parts);
                for (int pi = 0; pi < Game::EnderDragon::kDragonPartCount; ++pi) {
                    // The part boxes are float world boxes (ComputePartBoxes);
                    // their grid error far from the origin is well inside
                    // the 0.3 pick margin.
                    const Game::AABB& pbox = parts[pi];
                    // EnderDragonPart.getPickRadius: 0.
                    const float tMin = slab(glm::dvec3(pbox.min), glm::dvec3(pbox.max));
                    if (tMin < 0.0f || tMin >= bestT) continue;
                    if (blockLimit < tMin) continue;
                    bestT = tMin;
                    bestId = id;
                    if (outDragonPart) *outDragonPart = pi;
                }
                continue;
            }

            // MC Entity.isPickable — the gate MC's GameRenderer.pick applies
            // through its ProjectileUtil.getEntityHitResult predicate. Without
            // it every entity steals the crosshair, including the ones you
            // obviously want to build through.
            if (!mob.IsPickable()) continue;

            const Game::AABBd box = mob.GetAABBd();
            const double pick = static_cast<double>(mob.GetPickRadius());
            const float tMin = slab(box.min - pick, box.max + pick);
            if (tMin < 0.0f || tMin >= bestT) continue;

            // A block between us and the mob wins. lastBlockHit is the
            // per-frame block raycast, already in the same units.
            if (blockLimit < tMin) continue;

            bestT = tMin;
            bestId = id;
            // A nearer non-dragon hit outranks an earlier dragon-part hit.
            if (outDragonPart) *outDragonPart = -1;
        }

        // The entry point on the box, for the interaction's click location.
        if (outHit && bestId != 0) *outHit = origin + glm::dvec3(dir) * static_cast<double>(bestT);
        return bestId;
    }

    int32_t ClientPlayerController::PickEntity(int* outDragonPart, glm::dvec3* outHit) const {
        if (outDragonPart) *outDragonPart = -1;
        if (!player) return 0;
        // MC's entity pick distance: Player.entityInteractionRange (the
        // ENTITY_INTERACTION_RANGE attribute, 3.0 — +2.0 in creative). The
        // server re-checks with 3.0 more to absorb latency — see
        // HandleInteract.
        float kPickRange = static_cast<float>(player->GetEntityInteractionRange());
        const glm::dvec3 origin = player->GetEyePosition();
        const glm::vec3 dir = glm::normalize(player->lookDir);
        // LocalPlayer.raycastHitResult: the active item's ATTACK_RANGE picks
        // instead (AttackRange.getClosesetHit → ProjectileUtil.
        // getHitEntitiesAlong): from its minimum reach to its maximum plus
        // the known movement along the look, boxes inflated by its margin.
        float minRange = 0.0f, margin = 0.0f;
        {
            const Game::ItemStack& active = player->usingItem && player->usingHand == 1
                ? player->inventory.GetSlot(Game::Inventory::OFFHAND_BEGIN)
                : player->inventory.GetSelectedStack();
            if (const auto range = active.IsEmpty() ? std::nullopt : active.get(Game::DataComponents::ATTACK_RANGE)) {
                const bool creative = player->IsCreative();
                minRange = creative ? range->minCreativeReach : range->minReach;
                const float maxRange = creative ? range->maxCreativeReach : range->maxReach;
                const float along = glm::dot(player->physics.velocity / 20.0f, dir);
                kPickRange = maxRange + std::max(0.0f, along);
                margin = range->hitboxMargin;
            }
        }
        // The block under the crosshair caps the pick, as a distance along
        // the ray: a hit through a portal is in another level's coordinates,
        // so its point is no use here but its distance is.
        const float blockLimit = player->lastBlockHit.has_value()
            ? player->lastBlockHit->distance : std::numeric_limits<float>::max();

#if ENABLE_IMMERSIVE_PORTALS
        // The ray starts in the level the player STANDS in, whatever level
        // happens to be bound: the dig logic ticks with the far level bound
        // while the crosshair reaches through a portal, and this pick is
        // what stops a dig when a mob is in the way.
        if (!Client::ClientLevels::HasSession()) {
            return PickEntityAlong(origin, dir, kPickRange, blockLimit, outDragonPart, outHit, minRange, margin);
        }
        int32_t nearId = 0;
        Client::ClientLevels::WithLevel(Client::ClientLevels::ActiveDimension(), [&]() {
            nearId = PickEntityAlong(origin, dir, kPickRange, blockLimit, outDragonPart, outHit, minRange, margin);
        });
        if (nearId != 0) return nearId;

        // Through a portal: the rest of the ray continues in the far level,
        // the same way the block raycast does (ClientPlayer::UpdateRaycast).
        namespace PortalFlag = Game::Immersive::PortalFlag;
        const glm::dvec3 from = origin;
        const glm::dvec3 to = from + glm::dvec3(dir) * static_cast<double>(kPickRange);
        const Game::Immersive::Portal* best = nullptr;
        double bestT = 2.0;
        glm::dvec3 pierce{0.0};
        Client::ClientLevels::Active().Portals().ForEach([&](const Game::Immersive::Portal& p) {
            if (!p.Has(PortalFlag::Interactable) || !p.Has(PortalFlag::Visible)) return;
            const auto hit = p.RaytraceSegment(from, to, p.CrossingLeniency());   // whole opening for a gun portal
            if (!hit || hit->t >= bestT) return;
            best = &p; bestT = hit->t; pierce = hit->point;
        });
        if (!best || best->IsMirror()) return 0;
        const Game::Immersive::Portal portal = *best;   // copy: the level rebinds below
        const float travelled = static_cast<float>(glm::length(pierce - from));
        if (travelled >= kPickRange || travelled >= blockLimit) return 0;
        const glm::dvec3 farOrigin = portal.TransformPoint(pierce) + portal.ContentDirection() * 0.001;
        const glm::vec3 farDir(glm::normalize(portal.TransformLocalVecNonScale(glm::dvec3(dir))));
        int32_t farId = 0;
        Client::ClientLevels::WithLevel(portal.destDimension, [&]() {
            farId = PickEntityAlong(farOrigin, farDir, kPickRange - travelled,
                                    blockLimit - travelled, outDragonPart, outHit,
                                    std::max(0.0f, minRange - travelled), margin);
        });
        return farId;
#else
        return PickEntityAlong(origin, dir, kPickRange, blockLimit, outDragonPart, outHit, minRange, margin);
#endif
    }

    bool ClientPlayerController::TryAttackEntity() {
        if (!player || !networkClient) return false;

        int dragonPart = -1;
        glm::dvec3 hitLocation{0.0};
        const int32_t bestId = PickEntity(&dragonPart, &hitLocation);
        if (bestId == 0) return false;

        // Minecraft.startAttack's ENTITY case: with a held ATTACK_RANGE, the
        // hit must be within it (AttackRange.isInRange(player, location)) —
        // otherwise the arm swings at nothing.
        {
            const Game::ItemStack& held = player->inventory.GetSelectedStack();
            if (const auto range = held.IsEmpty() ? std::nullopt : held.get(Game::DataComponents::ATTACK_RANGE)) {
                const bool creative = player->IsCreative();
                const double distance = glm::length(hitLocation - player->GetEyePosition());
                const double minReach = static_cast<double>((creative ? range->minCreativeReach : range->minReach) -
                                                            range->hitboxMargin);
                const double maxReach = static_cast<double>((creative ? range->maxCreativeReach : range->maxReach) +
                                                            range->hitboxMargin);
                if (distance < minReach || distance > maxReach) {
                    armSwingPending = true;
                    return true;
                }
            }
        }

        Network::InteractC2SPacket packet;
        packet.entityId = bestId;
        packet.action = Network::InteractC2SPacket::Action::Attack;
        packet.sneaking = player->sneakPressed;
        packet.sprinting = player->physics.isSprinting;
        packet.dragonPart = static_cast<int8_t>(dragonPart);

        // Mirror the server's ticker so the attack indicator reads the same
        // charge the server will use. MC does exactly this — the bar is the
        // CLIENT's copy of attackStrengthTicker, reset locally on the swing.
        player->attackStrengthTicker = 0;

        auto connection = networkClient->GetConnection();
        if (connection) {
            FlushMovement();   // the server measures the attack's reach from here
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::InteractC2S),
                                   Network::Serialization::Serialize(packet));
        }

        armSwingPending = true;
        return true;
    }

    void ClientPlayerController::StartAttack() {
        if (!player) return;

        // MC Minecraft.startAttack:1595-1597 — a screen was open recently
        // enough that this click can't be trusted; swallow it.
        if (missTime > 0) return;

        // MC startAttack's `gameMode.isSpectator()` branch: no swing, no dig —
        // an entity under the crosshair is spectated (MultiPlayerGameMode
        // .spectate), anything else is the empty spectatorNoAction.
        if (player->IsSpectator()) {
            breakButtonHeld = true;
            pressHitEntity  = false;
            Network::SpectatorActionC2SPacket packet;
            if (const int32_t target = PickEntity(); target != 0) {
                packet.hasEntity = true;
                packet.entityId  = target;
            }
            if (networkClient) {
                if (auto connection = networkClient->GetConnection()) {
                    FlushMovement();
                    connection->SendPacket(static_cast<uint8_t>(Network::PacketId::SpectatorActionC2S),
                                           Network::Serialization::Serialize(packet));
                }
            }
            return;
        }

        {
            breakButtonHeld = true;
            pressHitEntity = false;   // a fresh press decides for itself

#if ENABLE_PORTAL_GUN
            // PortalGun hijacks left-click for blue-portal placement.
            const Game::ItemID held = player->inventory.GetSelectedItem();
            if (held != Game::ItemID(0) && held == Game::Items::PortalGun) {
                SpawnPortalProjectile(/*isOrange=*/false);
                return;  // skip the normal block-break path
            }
#endif

            // MC Minecraft.startAttack: a PIERCING_WEAPON (a spear) jabs —
            // whatever lies along its reach, entity or not, and never digs —
            // once its attack charge is full (cannotAttackWithItem(held, 0)).
            // MultiPlayerGameMode.piercingAttack: STAB to the server, then
            // onAttack, the STAB swing and the jab sound (makeSound — the
            // player's own copy).
            {
                const ItemStack& heldStack = player->inventory.GetSelectedStack();
                if (const auto piercing = Game::Spear::Piercing(heldStack)) {
                    pressHitEntity = true;   // the press never digs
                    if (player->usingItem) return;
                    // cannotAttackWithItem(held, 0): MINIMUM_ATTACK_CHARGE.
                    if (Game::CannotAttackWithItem(heldStack, player->GetAttackStrengthScale(0.0f))) return;
                    FlushMovement();
                    SendPlayerAction(Network::PlayerAction::STAB);
                    player->attackStrengthTicker = 0;
                    armSwingPending = true;
                    if (!piercing->sound.empty()) {
                        Client::Sounds::PlayLocal(player->physics.position, piercing->sound,
                                                  Game::SoundSource::Players, 1.0f, 1.0f);
                    }
                    return;
                }
            }

            // MC Minecraft.startAttack picks an ENTITY before a block: the
            // crosshair target is whichever is nearer, and an entity in front
            // of a wall must be hittable. Doing this after the block path
            // would make mobs unhittable whenever anything was behind them.
            if (TryAttackEntity()) {
                pressHitEntity = true;
                return;
            }

            // MC parity: startDestroyBlock runs SYNCHRONOUSLY on the click —
            // it doesn't wait for the next continueDestroyBlock tick AND it
            // doesn't check destroyDelay. So a fresh click right after a
            // break starts the new dig instantly. (destroyDelay only blocks
            // held-mining continuation; releasing LMB ends the sequence it
            // belongs to — see ContinueAttack(false) below.)
            digState.destroyDelay = 0;
            const auto& currentHit = player->lastBlockHit;
            if (currentHit.has_value()) {
                // Creative: the click destroys the block outright — no
                // hold-to-mine, no destroyTime gate (MC checks
                // getAbilities().instabuild first thing in startDestroyBlock,
                // before any progress math).
                if (player->IsCreative()) {
                    CreativeDestroy(currentHit->blockPos, currentHit->hitFace);
                    return;
                }
                // Instant-break check (MC's `if (f >= 1.0F) destroyBlock(pos)`):
                // grass/flowers/torches break inside startDestroyBlock without
                // entering the held-mining state. Mirror that.
                const BlockID hereBlock = ReadBlock(currentHit->blockPos);
                if (hereBlock != BlockID::Air) {
                    const Block& block = BlockRegistry::Get(hereBlock);
                    const float inc = GetDestroyProgressPerTick(
                        player->inventory.GetSelectedStack(), block,
                        player->physics.isOnGround,
                        player->GetDigSpeedMultiplier(),
                        player->physics.isEyeInWater,
                        player->GetMiningEfficiency(),
                        player->GetSubmergedMiningSpeed());
                    if (inc >= 1.0f) {
                        // Instant break — set up minimal state so FinishDig's
                        // packet/inventory path runs, then fire it.
                        digState.destroyBlockPos      = currentHit->blockPos;
                        digState.destroyFace          = currentHit->hitFace;
                        digState.destroyingBlockId    = hereBlock;
                        digState.destroyingBlockState = ReadBlockState(currentHit->blockPos);
                        digState.destroyProgress      = 1.0f;
                        digState.isDestroying         = true;
                        armSwingPending            = true;
                        FinishDig();
                        return;
                    }
                }
                StartDig(currentHit->blockPos, currentHit->hitFace);
            }
            // No target yet (player aiming at sky / past raycast range) —
            // UpdateBreakingTick will start the dig as soon as a target
            // appears under the crosshair.
        }
    }

    void ClientPlayerController::ContinueAttack(bool down) {
        if (!player) return;

        // MC Minecraft.continueAttack:1568-1571 — releasing clears missTime, so
        // the block a UI frame put in place lifts as soon as the button is up.
        if (!down) {
            missTime = 0;
        }

        if (down == breakButtonHeld) return;   // no transition

        breakButtonHeld = down;
        if (!down) {
            pressHitEntity = false;
            // Releasing LMB ends the held-mining sequence the destroyDelay
            // belongs to. Without this, the player gets a 5-tick "first
            // click after a break" lag every time they tap LMB.
            digState.destroyDelay = 0;
            if (digState.isDestroying) AbortDig();
        }
    }

    void ClientPlayerController::StartUseItem() {
        if (!player) return;

        // MC Minecraft.startUseItem:1656-1658 sets rightClickDelay = 4, which
        // is what paces a held-RMB strip of blocks.
        rightClickDelay = PLACE_REFIRE_TICKS;

        // Spectator: nothing is predicted and nothing swings.
        //   entity → MultiPlayerGameMode.interact sends the packet and
        //            answers PASS (the server opens a MenuProvider entity's
        //            menu, and nothing else);
        //   block  → useItemOn sends the packet, performUseItemOn is CONSUME
        //            (the server opens the block's menu or uses its portal);
        //   air    → useItem is PASS before any packet.
        if (player->IsSpectator()) {
            placeButtonHeld = true;
            if (const int32_t picked = PickEntity(); picked != 0) {
                if (networkClient) {
                    Network::InteractC2SPacket packet;
                    packet.entityId = picked;
                    packet.action   = Network::InteractC2SPacket::Action::Interact;
                    packet.sneaking = player->sneakPressed;
                    if (auto connection = networkClient->GetConnection()) {
                        FlushMovement();
                        connection->SendPacket(static_cast<uint8_t>(Network::PacketId::InteractC2S),
                                               Network::Serialization::Serialize(packet));
                    }
                }
                return;
            }
            if (player->lastBlockHit.has_value()) {
                SendUseItemOn(*player->lastBlockHit, /*hand=*/0);
            }
            return;
        }

        // RMB EDGE — fire one placement / use immediately. While-held re-fires
        // are handled by UpdatePlacingTick at MC's 4-tick cadence (no
        // wall-clock throttle).
        {
                const auto& currentHit = player->lastBlockHit;

#if ENABLE_PORTAL_GUN
                // PortalGun branches BEFORE the normal block-hit path so
                // it can fire even when nothing is in melee range.
                //   • Shift + RMB → clear-portals gesture: still needs a
                //     block hit (server detects sneak+!altInteract). No
                //     projectile.
                //   • Plain RMB → fire orange projectile. Server placement
                //     happens on impact, not at fire time.
                {
                    const Game::ItemID heldRMB = player->inventory.GetSelectedItem();
                    if (heldRMB != Game::ItemID(0) && heldRMB == Game::Items::PortalGun) {
                        // The RAW sneak key, not physics.isSneaking: that one
                        // is false while flying (shift descends), yet the
                        // move packet — and so the server's clear/fire
                        // choice — carries the key. Reading a different
                        // thing here is what fired a projectile client-side
                        // while the server was clearing the pair.
                        if (Input::IsDown(*Input::Binds::Sneak)) {
                            OnHotbarChanged(player->inventory.GetSelectedSlot());
                            if (currentHit.has_value()) {
                                SendUseItemOn(*currentHit, /*hand=*/0, /*altInteract=*/false);
                            } else {
                                // No block in sight (player is staring at sky
                                // or out past raycast range). The clear gesture
                                // shouldn't require a target — fabricate a
                                // zero-distance "hit" at the player's eye so
                                // the server's UseItemOn path still dispatches
                                // OnGunUseOn. suppressBlockUse = sneaking &&
                                // somethingInHands skips the block-use branch,
                                // so the synthetic-hit air block is never
                                // actually queried — OnGunUseOn runs and the
                                // sneak+!alt branch calls ClearPair.
                                const glm::dvec3 eye = player->physics.GetEyePosition();
                                const glm::ivec3 ipos(
                                    static_cast<int>(std::floor(eye.x)),
                                    static_cast<int>(std::floor(eye.y)),
                                    static_cast<int>(std::floor(eye.z)));
                                RaycastHit sky{};
                                sky.blockPos    = ipos;
                                sky.adjacentPos = ipos;
                                sky.hitPoint    = eye;
                                sky.normal      = glm::vec3(0.0f, 1.0f, 0.0f);
                                sky.cursorPos   = glm::vec3(0.5f);
                                sky.blockId     = BlockID::Air;
                                sky.distance    = 0.0f;
                                sky.hitFace     = 2;   // +Y, arbitrary
                                sky.insideBlock = true;
                                SendUseItemOn(sky, /*hand=*/0, /*altInteract=*/false);
                            }
                        } else {
                            SpawnPortalProjectile(/*isOrange=*/true);
                        }
                        rightClickDelay = PLACE_REFIRE_TICKS;
                        placeButtonHeld = true;
                        return;
                    }
                }
#endif

                // MC Minecraft.startUseItem: `if (hitResult.getType() == ENTITY)`
                // the interact goes to the ENTITY and the block branch never
                // runs. PickEntity already clips against the block hit, so a
                // mob in front of a wall wins and a mob behind it does not.
                //
                // This is what dye-on-sheep (and every future saddle / name
                // tag / shears interaction) arrives through.
                int interactDragonPart = -1;
                glm::dvec3 interactHit(0.0);
                if (const int32_t pickedEntity = PickEntity(&interactDragonPart, &interactHit);
                    pickedEntity != 0) {
                    if (networkClient) {
                        Network::InteractC2SPacket packet;
                        packet.entityId = pickedEntity;
                        packet.action   = Network::InteractC2SPacket::Action::Interact;
                        packet.sneaking = player->sneakPressed;
                        packet.dragonPart = static_cast<int8_t>(interactDragonPart);
                        // MC InteractionAtLocationAction: the hit relative to
                        // the entity's position (Player.interactOn subtracts
                        // it). A mob's position is its feet.
                        if (Client::g_clientMobManager) {
                            if (const Client::ClientMob* picked = Client::g_clientMobManager->GetMob(pickedEntity);
                                picked && picked->mob) {
                                packet.hasLocation = true;
                                packet.location = glm::vec3(interactHit - picked->mob->position);
                            }
                        }
                        if (auto connection = networkClient->GetConnection()) {
                            FlushMovement();
                            connection->SendPacket(
                                static_cast<uint8_t>(Network::PacketId::InteractC2S),
                                Network::Serialization::Serialize(packet));
                        }
                    }
                    // MC Cushion.interact answers CONSUME on the client —
                    // Success with no swing source — so sitting down (or
                    // clicking a taken seat) does not swing the arm.
                    bool swing = true;
                    if (Client::g_clientMobManager) {
                        if (const Client::ClientMob* picked = Client::g_clientMobManager->GetMob(pickedEntity);
                            picked && picked->mob && picked->mob->GetType() == Game::EntityTypeId::Cushion &&
                            !player->sneakPressed) {
                            swing = false;
                        }
                    }
                    if (swing) armSwingPending = true;
                    rightClickDelay = PLACE_REFIRE_TICKS;
                    placeButtonHeld = true;
                    return;
                }

                // The fill tool first: with Alt held the click marks a
                // corner; with a corner marked it sends the box.
                if (HandleFillClick(currentHit)) {
                    armSwingPending = true;
                    rightClickDelay = PLACE_REFIRE_TICKS;
                    placeButtonHeld = true;
                    return;
                }

                if (currentHit.has_value()) {
                    // Targeting a block — send UseItemOn regardless of what we
                    // hold. The server's dispatch order (mirroring MC's
                    // ServerPlayerGameMode.useItemOn) decides what happens:
                    //   block.useItemOn → block.useWithoutItem → item.useOn
                    //   → BlockItem placement
                    // Previously this branch was gated on "holding a block",
                    // which meant flint_and_steel / hoe / shovel / bone_meal /
                    // shears / etc. fell into the air-use path and the server
                    // never ran their useOn callback even though the player
                    // clicked on a block.
                    OnHotbarChanged(player->inventory.GetSelectedSlot());

                    // Predict the placement before sending — the predictor
                    // requires the target cell to still read as Air, which
                    // stops being true the moment we write the prediction.
                    glm::ivec3 predictPos{};
                    BlockID    predictBlock = BlockID::Air;
                    Game::BlockState predictState;
                    const bool predictable =
                        ComputePredictedPlacement(*currentHit, predictPos, predictBlock,
                                                  predictState);

                    const uint32_t sequence = SendUseItemOn(*currentHit, 0);  // 0 = main hand

                    // Run the block-use / item-useOn chain locally first, the
                    // same order the server will. If it consumes the click
                    // (door opened, hoe tilled, flint lit a fire) the server
                    // won't reach the placement fallback either, so neither do
                    // we — predicting a placement on top would put a phantom
                    // block down for a round trip.
                    const bool usedOn = PredictUseItemOn(*currentHit, 0, sequence);
                    // BrushItem.useOn → player.startUsingItem(hand), on the
                    // client too: the brushing pose and the hold whose
                    // release the server waits for.
                    if (usedOn && player->inventory.GetSelectedStack().itemId == Game::Items::Brush) {
                        StartPredictedUse(0, /*fromUseOn=*/true);
                    }

                    // The block appears this frame instead of a round trip
                    // later; the server's ack confirms it or rolls it back.
                    if (!usedOn && predictable) {
                        PredictBlock(predictPos, predictBlock, sequence, predictState);
                        PlayBlockPlaceSound(predictPos, predictState);
                        if (Game::IsDoorBlock(predictBlock)) {
                            PredictBlock(predictPos + glm::ivec3(0, 1, 0), predictBlock, sequence,
                                         Game::DoorUpperState(predictState));
                        }
                        if (Game::IsBedBlock(predictBlock)) {
                            PredictBlock(Game::BedOtherHalfPos(predictPos, predictState), predictBlock, sequence,
                                         Game::BedHeadState(predictState));
                        }
                    }

                    // Predictive consumption — ONLY for block placement, and
                    // only when the block did NOT swallow the click. A block
                    // with a use action (a crafting table opening its menu)
                    // ends the server's dispatch right there: nothing is
                    // placed and nothing is consumed, so predicting either
                    // shows the held stack ticking down until the server's
                    // correction arrives.
                    //
                    // The server's placement-fallback path consumes one block
                    // from the stack; for non-block items (tools, food, etc.)
                    // it doesn't, so we mustn't predict consumption either.
                    // Server is authoritative — it'll re-sync our inventory
                    // either way. Creative never consumes (MC
                    // ItemStack.consume no-ops with infinite materials).
                    //
                    // Gated on `predictable`, not on "am I holding a block":
                    // ComputePredictedPlacement returns false for exactly the
                    // cases the server rejects (cell occupied, can't survive
                    // there, would trap the player), so a rejected placement no
                    // longer predicts a consumption it will have to take back.
                    //
                    // That distinction became load-bearing with seeds. A carrot
                    // is both a block item and a food: clicking anything but
                    // farmland with one fails to place and the server eats it
                    // instead, so the old "holding a block → consume, else eat"
                    // split would have ticked the stack down and never played
                    // the eat.
                    if (!usedOn && HoldsPlaceOnWaterItem()) {
                        // PlaceOnWaterBlockItem: useOn was PASS (nothing was
                        // placed at the crosshair), so the client falls
                        // through to useItem — the item's own SOURCE_ONLY
                        // clip, placed on the cell above what it hits.
                        UsePlaceOnWaterItem();
                    } else if (!usedOn) {
                        if (predictable) {
                            if (!player->IsCreative()) {
                                player->inventory.ConsumeSelectedBlock();
                            }
                        } else {
                            // Nothing will be placed: either a non-block item
                            // aimed at a block, or a block item whose placement
                            // the server is going to reject. Both end in the
                            // server's use-item fallthrough — main hand first,
                            // then offhand (mirroring MC's hand loop in
                            // Minecraft.startUseItem, and BlockItem.useOn's
                            // own fallthrough to `use` for consumables).
                            // Mirror the condition by predicting the same hand.
                            //
                            // That fallthrough is gameMode.useItem, which
                            // runs Item.use on the client as well; for a book
                            // and quill that is LocalPlayer.openItemGui — its
                            // editor opens even with a block in the crosshair.
                            {
                                const uint32_t useHand = PickUseHand();
                                Client::ClientUsePlayer usePlayer(player);
                                Game::ItemStack& held =
                                    player->inventory.MutableSlot(usePlayer.handSlotIndex(useHand));
                                if (held.itemId == Game::Items::WritableBook) usePlayer.OpenItemGui(held, useHand);
                            }
                            StartPredictedUse(PickUseHand());
                            SwingForRodUse(PickUseHand());
                        }
                    }
                    rightClickDelay = PLACE_REFIRE_TICKS;
                } else {
                    // No block target — use item in air (food eat, shield
                    // raise, later Bow draw / EnderPearl throw). Server-side
                    // `Item.use` handles it; we start the matching predicted
                    // use for the viewmodel pose. Hand picked like MC's
                    // MAIN_HAND→OFF_HAND loop (offhand shield raises even
                    // with a pickaxe in the main hand).
                    //
                    // A lily pad or frogspawn is the air use that places a
                    // block: the crosshair ray passes through water, so open
                    // water reads as "nothing" here, and the item's own
                    // SOURCE_ONLY clip is what finds the surface.
                    if (UsePlaceOnWaterItem()) {
                        rightClickDelay = PLACE_REFIRE_TICKS;
                        placeButtonHeld = true;
                        return;
                    }
                    const uint32_t useHand = PickUseHand();
                    const uint32_t useSeq = SendUseItem(static_cast<int>(useHand));
                    // Buckets are the air-use case that edits the world; run
                    // it locally so the water appears/disappears immediately.
                    PredictUseItem(useHand, useSeq);
                    // Diagnostics: a gliding rocket use should attach one.
                    if (player->physics.isFallFlying &&
                        player->inventory.GetSlot(useHand == 0
                            ? Inventory::HotbarToIndex(player->inventory.GetSelectedSlot())
                            : Inventory::OFFHAND_BEGIN).itemId == Game::Items::FireworkRocket) {
                        Client::Fireworks::NoteBoostUse();
                    }
                    StartPredictedUse(useHand);
                    SwingForRodUse(useHand);
                    rightClickDelay = PLACE_REFIRE_TICKS;
                }
            }
        placeButtonHeld = true;
    }

    void ClientPlayerController::StopUseItem() {
        if (!player) return;
        if (!placeButtonHeld) return;   // no transition

        placeButtonHeld = false;
        // RELEASE_USE_ITEM — mirrors MC MultiPlayerGameMode.releaseUsingItem
        // (BlockPos.ZERO / Direction.DOWN, MultiPlayerGameMode.java:485).
        // The ONLY place the release packet is sent, so every path that stops
        // using an item (button release, UI opening) funnels through here.
        if (player->usingItem) {
            SendPlayerAction(Network::PlayerAction::RELEASE_USE_ITEM);
            // LocalPlayer.releaseUsingItem → TridentItem.releaseUsing: a
            // Riptide trident launches its thrower here — the push, the hop
            // and the spin are this client's own movement.
            {
                const int slot = (player->usingHand == 0)
                    ? Inventory::HotbarToIndex(player->inventory.GetSelectedSlot())
                    : Inventory::OFFHAND_BEGIN;
                const ItemStack& stack = player->inventory.GetSlot(slot);
                if (stack.itemId == Game::Items::Trident) {
                    const float strength = Game::EnchantmentHelper::GetTridentSpinAttackStrength(stack);
                    const int ticksHeld = player->useItemDuration - player->useItemRemaining;
                    if (strength > 0.0f &&
                        Game::WeaponItems::CanReleaseTrident(stack, ticksHeld, LocalInWaterOrRain(*player),
                                                             player->IsPassenger())) {
                        float yRot = 0.0f, xRot = 0.0f;
                        LookAngles(yRot, xRot);
                        player->StartRiptide(strength, yRot, xRot, Client::g_clientBlockAccess);
                    }
                }
            }
            StopPredictedUse();
        }
    }

    void ClientPlayerController::FinishBreaking(uint32_t sequence) {
        if (!player) {
            Log::Warning("Cannot break block - missing references");
            return;
        }

        // Use the cached block ID from StartDig — the world position may already
        // be Air if the server processed BlockActionC2S before we got here.
        BlockID brokenBlock = digState.destroyingBlockId;
        const glm::ivec3 pos = digState.destroyBlockPos;

        if (brokenBlock == BlockID::Air) {
            return;
        }
        // Bedrock is unbreakable in survival (destroyTime -1 means the dig
        // never completes anyway — this is the belt-and-braces guard), but
        // creative destroys it like anything else.
        if (brokenBlock == BlockID::Bedrock && !player->IsCreative()) {
            return;
        }

        // DecoratedPotBlock.playerWillDestroy runs here too: a pot broken
        // with a #breaks_decorated_pots tool without Silk Touch cracks first,
        // so its break is the shatter (the cracked pot's sound type).
        Game::BlockState brokenState = digState.destroyingBlockState;
        if (brokenBlock == BlockID::DecoratedPot) {
            const ItemStack& tool = player->inventory.GetSelectedStack();
            if (!tool.IsEmpty() &&
                Game::DataTags::HasTag(Game::DataTags::Registry::Item, Game::ItemRegistry::Slug(tool.itemId),
                                       "minecraft:breaks_decorated_pots") &&
                Game::EnchantmentHelper::GetItemEnchantmentLevel(Game::Enchantments::SilkTouch, tool) <= 0) {
                brokenState = brokenState.SetName(Game::PropertyId::CRACKED, "true");
            }
        }
        PlayBlockBreakSound(pos, brokenState);
        // ... and its debris (the particle half of levelEvent 2001, which the
        // server sends to everyone but this player).
        Client::LevelEvents::AddDestroyBlockEffect(pos, brokenState);

        // MC Level.destroyBlock:266 — the cell becomes the FLUID that was in
        // it, not air. Breaking a waterlogged fence or a kelp stalk under an
        // ocean has to leave water behind, or the prediction punches a dry
        // hole that the server's echo then has to un-punch a tick later.
        // Mirrors the identical rule in PlayerSession's break handler.
        const BlockID replacement =
            Game::BlockRegistry::ContainsWater(digState.destroyingBlockState)
                ? BlockID::Water
                : BlockID::Air;

        // Predict the break into the client's own chunk data. This is what
        // makes breaking feel instant on a remote server; on the integrated
        // host it lands a tick earlier than the echo would.
        PredictBlock(pos, replacement, sequence);

        // The host used to ALSO write the break straight into the server's
        // Game::World from here, on the grounds that its raycast and physics
        // read that World rather than the client cache. Both halves of that
        // are gone: the host reads ClientBlockAccess now (so the prediction
        // above is all it needs), and the write itself was a client-thread
        // mutation of a world owned by the server tick — a data race that
        // also, once there were three Worlds, always hit the overworld's.
        // The authoritative clear happens in PlayerSession::HandleBlockAction
        // on the server thread, in the right dimension, and comes back as
        // BlockChangeS2C.

        // NO predicted pickup. Drops come from the block's loot table, which
        // is random — uniform counts, random_chance, table_bonus — so the
        // client cannot guess the outcome without sharing the server's RNG
        // stream, and a wrong guess would flash the wrong item in the HUD
        // until the next sync corrected it. MC's client doesn't predict
        // drops either: the server spawns them and tells the client.
        //
        // The authoritative roll lives in PlayerSession::HandleBlockAction,
        // and its inventory delta arrives via BroadcastContainerChanges in
        // the same server tick (sub-frame on the integrated server).
        player->stats.blocksBroken++;
        player->stats.lastBrokenBlockId = static_cast<int>(brokenBlock);

        // Remeshing is triggered by the server's BlockChangeS2C via
        // ProcessBlockChange (which handles neighbor boundaries correctly).
        // Don't mark here — avoids race where neighbors remesh before
        // the client chunk cache is updated.

        const Block& block = BlockRegistry::Get(brokenBlock);
        Log::Info("Broke %s at (%d, %d, %d)",
                 block.name.c_str(), pos.x, pos.y, pos.z);
    }

#if ENABLE_PORTAL_GUN
    // Portal's BLAST_SPEED from weapon_portalgun.cpp:71 — 3000 HU/s
    // = 57.15 m/s. For long-range shots (server reach = 256 m) we
    // need ~4.5 s of flight at that speed, so we lift the lifetime
    // cap well above sv_portal_projectile_delay's 0.5 s.
    static constexpr float kPortalProjSpeed_m_per_s = 57.15f;
    static constexpr float kPortalProjMaxLifetime  = 4.5f;  // ≈ 257 m

    void ClientPlayerController::UpdateShooterVelocity(float deltaTime) {
        if (!player) { m_shooterMotionValid = false; return; }
        const glm::dvec3 pos = player->physics.position;
        if (!m_shooterMotionValid || deltaTime <= 0.0f) {
            m_shooterMotionPrevPos = pos;
            m_shooterVelocity      = glm::dvec3(0.0);
            m_shooterMotionValid   = true;
            return;
        }
        const glm::dvec3 step = pos - m_shooterMotionPrevPos;
        m_shooterMotionPrevPos = pos;
        // A jump of more than 8 blocks in one frame is a teleport (a portal
        // crossing, /tp, a respawn), not motion: keep the last estimate.
        if (glm::dot(step, step) > 64.0) return;
        const glm::dvec3 instant = step / static_cast<double>(deltaTime);
        // Smoothed over ~a tick: physics steps at 20 Hz while frames come
        // faster, so single-frame deltas alternate between a step and none.
        const double blend = 1.0 - std::exp(-static_cast<double>(deltaTime) / 0.05);
        m_shooterVelocity += (instant - m_shooterVelocity) * blend;
    }

    void ClientPlayerController::SpawnPortalProjectile(bool isOrange) {
        if (!player) return;

        // Use the cached camera-space forward written by UpdateRaycast
        // each frame. player->yaw/pitch are stale (mouse-look writes
        // camera.yaw/pitch and only syncs back on teleports), so reading
        // them here makes every shot fly the same direction.
        const glm::vec3 aim = player->lookDir;

        // MC Projectile.shootFromRotation: the shot inherits the shooter's
        // motion — horizontally always, vertically only while airborne
        // (`shooter.onGround() ? 0 : movement.y`). A rider's motion is its
        // vehicle's, so it is taken whole. The impact is decided here, on
        // the client (the server opens the portal at the face this flight
        // reports), so the logical sweep and the visible bolt below fly
        // the same vector and land where the bolt lands. The portal gun
        // has no hitscan mode; every shot is this projectile.
        constexpr double kMaxInheritedSpeed = 100.0;   // blocks/s: past elytra-rocket speed
        glm::dvec3 inherited = m_shooterVelocity;
        if (player->physics.isOnGround && player->vehicleId == 0) inherited.y = 0.0;
        const double inheritedLen = glm::length(inherited);
        if (inheritedLen > kMaxInheritedSpeed) inherited *= kMaxInheritedSpeed / inheritedLen;
        const glm::dvec3 launch = glm::dvec3(aim) * static_cast<double>(kPortalProjSpeed_m_per_s) + inherited;
        const double launchSpeed = glm::length(launch);
        const glm::vec3 front = launchSpeed > 1.0e-6 ? glm::vec3(launch / launchSpeed) : aim;
        const float shotSpeed = launchSpeed > 1.0e-6 ? static_cast<float>(launchSpeed) : kPortalProjSpeed_m_per_s;

        const glm::dvec3 origin = player->physics.GetEyePosition();

        // Logical projectile — the collision raycast stays anchored to
        // the eye so the shot lands EXACTLY where the crosshair points
        // (independent of the visual muzzle offset). Without this, the
        // shot would consistently impact a few centimetres right/down
        // of where you aimed.
        PendingPortalProjectile p;
        p.origin     = origin;
        p.direction  = front;
        p.currentPos = origin;
        p.age        = 0.0f;
        p.isOrange   = isOrange;
        p.hand       = 0;
        p.dimension  = Client::ClientLevels::ActiveDimension();
        p.speed      = shotSpeed;
        m_pendingPortalProjectiles.push_back(p);

        // Visual bolt — spawn at the gun's muzzle, not the eye. The
        // offsets here are the muzzle position in camera-space *as
        // rendered by the viewmodel*, then FOV-corrected to the world
        // projection so the bolt actually appears at the gun's tip on
        // screen instead of drifting toward the centre.
        //
        // The viewmodel renders with a 54° narrow FOV (PortalGun-
        // Viewmodel::Render — matches Portal's v_viewmodel_fov ConVar).
        // The world projection uses 70° (Render::Camera::fov default).
        // For a point at world-space (x, y, -z) to project to the same
        // NDC under 70° as under 54°, x and y must scale by
        // tan(35°)/tan(27°) ≈ 1.374. Without that the muzzle's
        // on-screen position under the world projection sits much
        // closer to the centre than where the gun is actually drawn,
        // and the bolt visibly "spawns from the air" beside the gun.
        //
        // Raw viewmodel-space muzzle (right, down, forward):
        //     hold offset (+0.18, -0.16, -0.32)
        //   + gun extent past grip after 180° Y rotation ≈ -0.53 z
        //   = (+0.27, -0.12, +0.85)
        // Scaled by 1.374 in x & y:
        constexpr glm::vec3 kMuzzleOffsetCameraSpace{0.371f, -0.165f, 0.85f};
        const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
        // Look almost-straight-up/down breaks the cross-with-worldUp
        // basis (right collapses to zero). Fall back to world-X in
        // that degenerate case so the muzzle still has a defined
        // position. Threshold of 0.999 ≈ within ~2.5° of vertical.
        glm::vec3 right;
        if (std::abs(glm::dot(front, worldUp)) > 0.999f) {
            right = glm::vec3(1.0f, 0.0f, 0.0f);
        } else {
            right = glm::normalize(glm::cross(front, worldUp));
        }
        const glm::vec3 up = glm::normalize(glm::cross(right, front));
        const glm::dvec3 muzzle =
            origin
            + glm::dvec3(right * kMuzzleOffsetCameraSpace.x
                       + up    * kMuzzleOffsetCameraSpace.y
                       + front * kMuzzleOffsetCameraSpace.z);

        // Aim the visual bolt at the crosshair point — find the
        // logical projectile's first impact within max range, and
        // use that as the visual endpoint. If no hit (sky shot),
        // the bolt streaks to the max-lifetime point in the air.
        // This makes the bolt appear to converge from the muzzle to
        // where you aimed, hiding the small parallax between the
        // muzzle and the crosshair.
        const float reachM = shotSpeed * kPortalProjMaxLifetime;
        auto aimHit = Raycast::CastRay(origin, front, reachM);
        glm::dvec3 aimPoint = aimHit.has_value()
            ? aimHit->hitPoint
            : (origin + glm::dvec3(front * reachM));
#if ENABLE_IMMERSIVE_PORTALS
        // An aim line that pierces an immersive portal before its block:
        // the bolt ends at the surface, and a second bolt carries on from
        // the far surface, in the far level, to where the shot will land.
        {
            const float aimDist = static_cast<float>(glm::length(aimPoint - origin));
            const Game::Immersive::Portal* crossed = nullptr;
            double bestT = 1.0;
            glm::dvec3 crossPoint{0.0};
            const glm::dvec3 from(origin), to(origin + glm::dvec3(front * aimDist));
            Client::ClientLevels::Active().Portals().ForEach([&](const Game::Immersive::Portal& portal) {
                if (!portal.Has(Game::Immersive::PortalFlag::Teleportable)) return;
                const auto s = portal.RaytraceSegment(from, to, portal.CrossingLeniency());
                if (!s || s->t >= bestT) return;
                bestT = s->t; crossed = &portal; crossPoint = s->point;
            });
            if (crossed) {
                aimPoint = crossPoint;
                const glm::dvec3 farDir = glm::normalize(crossed->TransformLocalVecNonScale(glm::dvec3(front)));
                const glm::dvec3 farStart = crossed->TransformPoint(crossPoint) + farDir * 0.01;
                const Game::DimensionId farDim =
                    crossed->IsMirror() ? Client::ClientLevels::ActiveDimension() : crossed->destDimension;
                const float farReach = reachM - static_cast<float>(bestT) * aimDist;
                glm::dvec3 farEnd = farStart + farDir * static_cast<double>(farReach);
                Client::ClientLevels::WithLevel(farDim, [&]() {
                    if (auto farHit = Raycast::CastRay(farStart, glm::vec3(farDir), farReach)) {
                        farEnd = farHit->hitPoint;
                    }
                });
                Render::g_portalParticleSystem.EmitProjectileIn(farDim, farStart, farEnd, isOrange, shotSpeed);
            }
        }
#endif
        Render::g_portalParticleSystem.EmitProjectile(muzzle, aimPoint, isOrange, shotSpeed);

        // Play the real Source @fire1 animation — 15-frame, 0.625s
        // skeletal clip from v_portalgun.mdl (the prongs spin out and
        // back). Returns to @idle automatically when done.
        Render::g_portalGunViewmodel.OnFire();
    }

    void ClientPlayerController::UpdatePendingPortalProjectiles(float deltaTime) {
        if (m_pendingPortalProjectiles.empty()) return;

        auto it = m_pendingPortalProjectiles.begin();
        while (it != m_pendingPortalProjectiles.end()) {
            PendingPortalProjectile& p = *it;
            p.age += deltaTime;
            if (p.age > kPortalProjMaxLifetime) {
                it = m_pendingPortalProjectiles.erase(it);
                continue;
            }

            // Sweep this frame's segment with the shared block raycast (the
            // one the look-aim uses), in the level the projectile is in. An
            // immersive portal pierced before the block hands the rest of
            // the segment to the far level with position and direction
            // mapped through — the same crossing the player makes. Bounded:
            // two facing portals must not bounce the sweep forever inside
            // one frame.
            float remaining = p.speed * deltaTime;
            bool impacted = false;
            for (int hop = 0; hop < 8 && remaining > 1.0e-4f; ++hop) {
                std::optional<RaycastHit> hit;
                Client::ClientLevels::WithLevel(p.dimension, [&]() {
                    hit = Raycast::CastRay(p.currentPos, p.direction, remaining);
                });
                const float blockDist = hit ? static_cast<float>(glm::length(hit->hitPoint - p.currentPos)) : remaining;

#if ENABLE_IMMERSIVE_PORTALS
                const Game::Immersive::Portal* crossed = nullptr;
                double bestT = 1.0;
                glm::dvec3 crossPoint{0.0};
                if (Client::ClientLevel* level = Client::ClientLevels::Get(p.dimension)) {
                    const glm::dvec3 from = p.currentPos, to = p.currentPos + glm::dvec3(p.direction * remaining);
                    level->Portals().ForEach([&](const Game::Immersive::Portal& portal) {
                        if (!portal.Has(Game::Immersive::PortalFlag::Teleportable)) return;
                        const auto s = portal.RaytraceSegment(from, to, portal.CrossingLeniency());
                        if (!s || s->t >= bestT) return;
                        bestT = s->t; crossed = &portal; crossPoint = s->point;
                    });
                }
                if (crossed && static_cast<float>(bestT) * remaining < blockDist) {
                    const glm::dvec3 dir = glm::normalize(crossed->TransformLocalVecNonScale(glm::dvec3(p.direction)));
                    p.currentPos = crossed->TransformPoint(crossPoint) + dir * 0.01;
                    p.direction  = glm::vec3(dir);
                    if (!crossed->IsMirror()) p.dimension = crossed->destDimension;
                    remaining -= static_cast<float>(bestT) * remaining;
                    continue;
                }
#endif
                if (hit.has_value()) {
                    // altInteract=true → blue portal (matches LMB semantics).
                    SendUseItemOn(*hit, p.hand, /*altInteract=*/!p.isOrange, p.dimension);
                    impacted = true;
                    break;
                }
                p.currentPos += glm::dvec3(p.direction * remaining);
                remaining = 0.0f;
            }

            if (impacted) it = m_pendingPortalProjectiles.erase(it);
            else          ++it;
        }
    }
#endif

    void ClientPlayerController::MarkSurroundingSectionsForRemesh(const glm::ivec3& worldPos) {
        if (!Client::g_clientChunkManager) {
            return;
        }

        // Convert world position to chunk coordinates
        int chunkX = static_cast<int>(std::floor(static_cast<float>(worldPos.x) / Game::Math::CHUNK_SIZE_X));
        int chunkZ = static_cast<int>(std::floor(static_cast<float>(worldPos.z) / Game::Math::CHUNK_SIZE_Z));

        // Convert world Y to section index
        int sectionY = (worldPos.y - Config::MinY) / Game::Math::SECTION_HEIGHT;

        Game::Math::ChunkPos chunkPos{chunkX, chunkZ};

        // Mark the section containing the changed block
        Client::g_clientChunkManager->MarkSectionDirty(chunkPos, sectionY);

        // Check if we need to mark neighboring sections/chunks
        int localX = worldPos.x - (chunkX * Game::Math::CHUNK_SIZE_X);
        int localZ = worldPos.z - (chunkZ * Game::Math::CHUNK_SIZE_Z);
        int localY = (worldPos.y - Config::MinY) % Game::Math::SECTION_HEIGHT;

        // Mark neighboring chunks if block is on chunk boundary
        if (localX == 0) {
            Game::Math::ChunkPos westChunk{chunkX - 1, chunkZ};
            Client::g_clientChunkManager->MarkSectionDirty(westChunk, sectionY);
        }
        if (localX == Game::Math::CHUNK_SIZE_X - 1) {
            Game::Math::ChunkPos eastChunk{chunkX + 1, chunkZ};
            Client::g_clientChunkManager->MarkSectionDirty(eastChunk, sectionY);
        }
        if (localZ == 0) {
            Game::Math::ChunkPos northChunk{chunkX, chunkZ - 1};
            Client::g_clientChunkManager->MarkSectionDirty(northChunk, sectionY);
        }
        if (localZ == Game::Math::CHUNK_SIZE_Z - 1) {
            Game::Math::ChunkPos southChunk{chunkX, chunkZ + 1};
            Client::g_clientChunkManager->MarkSectionDirty(southChunk, sectionY);
        }

        // Mark neighboring sections if block is on section boundary
        if (localY == 0 && sectionY > 0) {
            Client::g_clientChunkManager->MarkSectionDirty(chunkPos, sectionY - 1);
        }
        if (localY == Game::Math::SECTION_HEIGHT - 1 && sectionY < Game::Math::SECTIONS_PER_CHUNK - 1) {
            Client::g_clientChunkManager->MarkSectionDirty(chunkPos, sectionY + 1);
        }
    }

} // namespace Game