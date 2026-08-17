// File: src/client/input/PlayerController.cpp
#include "PlayerController.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "common/core/Mth.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/MiningSpeed.hpp"
#include "common/world/level/World.hpp"
#include "common/network/PacketTypes.hpp"
#include "../network/NetworkClient.hpp"
#include "../network/ClientConnection.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "../world/ClientChunkManager.hpp"
#include "../world/ClientBlockAccess.hpp"
#include "../world/ClientUsePlayer.hpp"
#include "../entity/ClientMobManager.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/core/Features.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
#include "common/data/DataComponents.hpp"
#if ENABLE_PORTAL_GUN
#include "../renderer/portal/PortalParticleSystem.hpp"
#include "../renderer/viewmodel/PortalGunViewmodel.hpp"
#endif
#include <glm/glm.hpp>
#include <cmath>
#include <thread>

namespace Game {

    ClientPlayerController::ClientPlayerController()
        : player(nullptr)
        , world(nullptr)
        , networkClient(nullptr)
        , lastMoveSend(std::chrono::steady_clock::now())
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
                                                   uint8_t blockState) {
        if (!networkClient || !networkClient->IsConnected()) return 0;
        Network::BlockActionC2SPacket packet;
        packet.worldX = pos.x;
        packet.worldY = pos.y;
        packet.worldZ = pos.z;
        packet.action = action;
        packet.blockId = blockId;
        packet.blockState = blockState;
        packet.face = 0;
        packet.sequenceNumber = ++interactSeq;
        auto data = Network::Serialization::Serialize(packet);
        auto connection = networkClient->GetConnection();
        if (connection) {
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::BlockActionC2S), data);
        }
        return packet.sequenceNumber;
    }

    void ClientPlayerController::SetPlayer(ClientPlayer* playerPtr) {
        player = playerPtr;
        Log::Debug("ClientPlayerController player reference set");
    }

    void ClientPlayerController::SetWorld(World* worldPtr) {
        world = worldPtr;
        Log::Debug("ClientPlayerController world reference set");
    }

    void ClientPlayerController::SetBlockAccess(const IBlockAccess* access) {
        blockAccess = access;
        Log::Debug("ClientPlayerController block access set");
    }

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

    BlockID ClientPlayerController::ReadBlock(const glm::ivec3& pos) const {
        try {
            if (blockAccess) return blockAccess->GetBlock(pos.x, pos.y, pos.z);
            if (world)       return world->GetBlock(pos.x, pos.y, pos.z);
        } catch (...) {}
        return BlockID::Air;
    }

    uint8_t ClientPlayerController::ReadBlockState(const glm::ivec3& pos) const {
        try {
            if (blockAccess) return blockAccess->GetBlockState(pos.x, pos.y, pos.z);
            if (world)       return world->GetBlockState(pos.x, pos.y, pos.z);
        } catch (...) {}
        return 0;
    }

    void ClientPlayerController::PredictBlock(const glm::ivec3& pos,
                                              BlockID newBlock,
                                              uint32_t sequence,
                                              uint8_t stateIndex) {
        // The client's chunk cache is what the renderer meshes from AND what
        // ClientBlockAccess reads for raycast/physics, so a single write here
        // makes the change visible, targetable and solid on the same frame.
        if (Client::g_clientChunkManager) {
            Client::g_clientChunkManager->PredictBlockChange(pos, newBlock, sequence, stateIndex);
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
        UpdatePendingPortalProjectiles(deltaTime);
#endif

        // Send movement packets if due (TODO: Implement for networking)
        SendMovementIfDue();

        // Fly-state sync — MC LocalPlayer.sendIsSprintingIfNeeded-style
        // dirty check: whenever the local fly flag changes (double-tap
        // toggle, landing auto-cancel, server revoke), ship the new state
        // via PlayerAbilitiesC2S (MC ServerboundPlayerAbilitiesPacket).
        if (player->physics.isFlying != lastSentFlying) {
            lastSentFlying = player->physics.isFlying;
            if (networkClient && networkClient->IsConnected()) {
                Network::PlayerAbilitiesC2SPacket packet;
                if (lastSentFlying) packet.flags |= Network::PlayerAbilitiesC2SPacket::FLAG_FLYING;
                auto data = Network::Serialization::Serialize(packet);
                if (auto connection = networkClient->GetConnection()) {
                    connection->SendPacket(
                        static_cast<uint8_t>(Network::PacketId::PlayerAbilitiesC2S), data);
                }
            }
        }
    }

    void ClientPlayerController::SendMovementIfDue() {
        // TODO: Implement network movement sending at 20Hz (50ms intervals)
        // This would check if 50ms have passed since lastMoveSend
        // and send a movement packet with player->predictedPos, yaw, pitch
        
        // auto now = std::chrono::steady_clock::now();
        // auto timeSinceLastSend = std::chrono::duration_cast<std::chrono::milliseconds>
        //                          (now - lastMoveSend);
        // if (timeSinceLastSend.count() >= 50) {
        //     // Send movement packet
        //     // net->SendPlayerMove(player->predictedPos, player->yaw, player->pitch, 
        //     //                     player->physics.isOnGround, ++moveSeq);
        //     lastMoveSend = now;
        // }
    }

    void ClientPlayerController::StartDig(const glm::ivec3& pos, int /*face*/) {
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

    void ClientPlayerController::CreativeDestroy(const glm::ivec3& pos) {
        // MC MultiPlayerGameMode's `instabuild` branch: the block is destroyed
        // outright — destroyProgress is never accumulated, so the block's
        // destroyTime (including bedrock's -1 "unbreakable" sentinel) is never
        // consulted. The only bail is "the block is already air"
        // (MultiPlayerGameMode.destroyBlock's `oldState.isAir()` check).
        const BlockID target = ReadBlock(pos);
        if (target == BlockID::Air) return;

        // Set up the minimal dig state FinishDig's packet + local-prediction
        // path expects, then finish immediately.
        digState.destroyBlockPos      = pos;
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

    uint32_t ClientPlayerController::SendUseItemOn(const RaycastHit& hit, int hand, bool altInteract) {
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
        
        // Serialize and send
        auto data = Network::Serialization::Serialize(packet);
        auto connection = networkClient->GetConnection();
        if (connection) {
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

    bool ClientPlayerController::ComputePredictedPlacement(const RaycastHit& hit,
                                                           glm::ivec3& outPos,
                                                           BlockID& outBlock,
                                                           uint8_t& outState) const {
        if (!player) return false;

        // --- Only plain block items are predictable ---------------------
        // Anything with an item behaviour (flint & steel, buckets, hoes,
        // bone meal…) resolves server-side in ways we don't model here.
        const Game::ItemID held = player->inventory.GetSelectedItem();
        if (held == Game::Items::Air) return false;
        if (Game::ItemRegistry::Get(held).useOn != nullptr) return false;

        const BlockID toPlace = player->GetSelectedBlock();
        if (toPlace == BlockID::Air) return false;

        // --- Would the clicked block swallow the click? ------------------
        // Mirrors HandleUseItemOn's suppressBlockUse + block-use dispatch:
        // a door/lever/chest consumes the interaction and nothing is placed.
        // `somethingInHands` is implied — held != Air was checked above.
        const BlockID clickedId = ReadBlock(hit.blockPos);
        const bool suppressBlockUse = player->physics.isSneaking;
        if (!suppressBlockUse) {
            const Block& clicked = BlockRegistry::Get(clickedId);
            if (clicked.useItemOn || clicked.useWithoutItem) return false;
        }

        // --- Resolve the target cell (server: step 6a) -------------------
        // MC BlockPlaceContext's order: decide whether the CLICKED block is
        // replaceable, resolve the position from that, then re-ask at the
        // resolved cell. Clicking the grass under a leaf litter clump lands on
        // the litter's own cell, which is what lets the growth below see it.
        const uint8_t clickedState = ReadBlockState(hit.blockPos);
        const bool replaceClicked =
            Game::CanBeReplacedByPlacement(clickedId, clickedState, toPlace,
                                           player->physics.isSneaking);

        const glm::ivec3 target = replaceClicked ? hit.blockPos : hit.adjacentPos;
        const BlockID targetId    = ReadBlock(target);
        const uint8_t targetState = ReadBlockState(target);
        if (!replaceClicked &&
            !Game::CanBeReplacedByPlacement(targetId, targetState, toPlace,
                                            player->physics.isSneaking)) {
            return false;
        }

        // --- Segmented ground cover grows in place (server: step 6b) ------
        // MC SegmentableBlock: same item on an existing clump raises its
        // segment count and KEEPS the facing it already has. Predicting the
        // grow avoids a round trip in which the clump visibly stays put and
        // then jumps a segment.
        BlockID resolved = toPlace;
        bool grewInPlace = false;
        {
            const BlockID base = Game::SegmentedFamilyBase(targetId);
            if (base != BlockID::Air && base == Game::SegmentedFamilyBase(toPlace)) {
                const BlockID grown = Game::SegmentedGrowth(targetId);
                if (grown != BlockID::Air) {
                    resolved     = grown;
                    grewInPlace  = true;
                }
            }
        }

        // --- Slab half (server: SlabBlock.getStateForPlacement mirror) ---
        // Same inputs (clicked face + cursor Y), so the same answer — without
        // this a slab would predict as bottom and visibly flip on the ack.
        if (!grewInPlace) {
            const BlockID topVariant = Game::BlockRegistry::SlabTopVariant(toPlace);
            if (topVariant != BlockID::Air) {
                bool placeAsTop;
                switch (hit.hitFace) {
                    case 3:  placeAsTop = true;  break;               // -Y (bottom face)
                    case 2:  placeAsTop = false; break;               // +Y (top face)
                    default: placeAsTop = (hit.cursorPos.y > 0.5f); break;  // sides
                }
                if (placeAsTop) resolved = topVariant;
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

        // --- Would the block land inside the player? ---------------------
        // The server rejects this (and resyncs); predicting it would place a
        // block, trap the player for a round trip, then yank it away.
        // Skipped for `.noCollision()` blocks, matching the server — MC's
        // isUnobstructed tests the collision shape, which is empty for flowers
        // and ground cover, so those place even inside the player.
        const glm::vec3 playerPos = player->physics.position;
        const glm::vec3 blockCenter = glm::vec3(target) + glm::vec3(0.5f);
        if (BlockRegistry::HasCollision(resolved) &&
            std::abs(playerPos.x - blockCenter.x) < 0.8f &&
            std::abs(playerPos.z - blockCenter.z) < 0.8f &&
            playerPos.y < target.y + 1.0f &&
            playerPos.y + 1.8f > target.y) {
            return false;
        }

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
        ctx.hitResult.hitPoint = glm::vec3(hit.blockPos) + hit.cursorPos;
        // LookAngles(), not player->yaw/pitch — those are only written on
        // teleports (mouse-look updates camera.yaw/pitch instead), so reading
        // them here made every predicted placement use a stale angle. The block
        // appeared facing the wrong way until the server's authoritative state
        // arrived a round trip later, which is exactly the flicker prediction
        // exists to avoid. Same trap SpawnPortalProjectile documents.
        LookAngles(ctx.playerYaw, ctx.playerPitch);

        outPos   = target;
        outBlock = resolved;
        // Growing a clump keeps the facing it already has (MC's
        // `state.setValue(segment, n + 1)`); everything else derives its
        // orientation from how the player is standing.
        outState = grewInPlace ? targetState : Game::ComputePlacementState(resolved, ctx);
        // Neighbour-derived orientation (redstone dust). Uses the SAME function
        // the server calls, against the client's own block access, so the wire
        // predicts with the exact connections the server is about to send.
        if (!grewInPlace && blockAccess) {
            outState = Game::ComputeWorldPlacementState(*blockAccess, target, resolved, outState);
            // State-aware survival, mirroring the server's second gate: a
            // button's support depends on the face it ends up attached to, so
            // this can only be asked once the state is known. Predicting a
            // placement the server will refuse would flash a floating button.
            if (!Game::CanSurviveAt(*blockAccess, target, resolved, outState)) return false;
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

        Client::ClientUsePlayer usePlayer(player);
        float yawDeg, pitchDeg; LookAngles(yawDeg, pitchDeg);
        usePlayer.setRotation(yawDeg, pitchDeg);

        Game::BlockHitResult bhr(hit.blockPos, hit.hitFace, hit.hitPoint, hit.insideBlock);
        Game::UseOnContext ctx(Client::g_clientBlockAccess, &usePlayer, hand, bhr);
        ctx.playerYaw   = yawDeg;
        ctx.playerPitch = pitchDeg;

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

        if (!consumed && somethingInHands) {
            const Game::Item& heldItem = Game::ItemRegistry::Get(heldId);
            if (heldItem.useOn) {
                // Creative stack preservation, mirroring the server (and MC's
                // ServerPlayerGameMode.useItemOn lines 365-371) — only the
                // COUNT, so a callback's component writes survive. See the
                // long note at the server's copy in
                // PlayerSession::HandleUseItemOn for why the whole-stack
                // restore is kept for the emptied case only.
                const Game::ItemStack stackBefore = heldStack;
                Game::UseResult r = heldItem.useOn(ctx, heldStack);
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
        if (!off.IsEmpty() && GetUseDuration(off) > 0) {
            return 1;
        }
        return 0;
    }

    void ClientPlayerController::StartPredictedUse(uint32_t hand) {
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
        if (--player->useItemRemaining <= 0) {
            StopPredictedUse();
        }
    }

    void ClientPlayerController::UpdateBreakingTick() {
        if (!player) return;

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
            CreativeDestroy(hitPos);
            return;
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
        const Game::ItemID held = player->inventory.GetSelectedItem();
        const bool onGround = player->physics.isOnGround;
        const float inc = GetDestroyProgressPerTick(held, block, onGround);

        digState.destroyProgress += inc;
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
        uint8_t predictState = 0;
        const bool predictable = ComputePredictedPlacement(*currentHit, predictPos, predictBlock,
                                                           predictState);
        const uint32_t sequence = SendUseItemOn(*currentHit, 0);
        const bool usedOn = PredictUseItemOn(*currentHit, 0, sequence);
        if (!usedOn && predictable) PredictBlock(predictPos, predictBlock, sequence, predictState);
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

    void ClientPlayerController::OnPickBlock(BlockID picked) {
        if (!player) return;
        if (picked == BlockID::Air) return;

        // MC normalises blockstate-only variants back to the canonical block
        // before giving the player an item — picking a TOP slab yields the
        // base slab item (placement-time logic re-derives the orientation
        // from the click). Without this, pick-block on a top slab would put
        // a "top" item in the inventory which renders weird in the HUD and
        // bypasses the normal SlabBlock.getStateForPlacement decision.
        if (Game::BlockRegistry::IsSlabTop(picked)) {
            picked = Game::BlockRegistry::SlabBottomVariant(picked);
        }

        const int slot = player->inventory.GetSelectedSlot();
        const int unifiedSlot = Game::Inventory::HotbarToIndex(slot);
        const Game::ItemID itemId = Game::ItemRegistry::FromBlock(picked);

        // Predictive client-side fill so the HUD updates instantly. The server
        // will echo back an InventorySetSlotS2C that either confirms or
        // corrects this. (Without the predictive update the HUD would lag a
        // round-trip behind every pick-block.)
        const int maxStack = Game::ItemRegistry::Get(itemId).maxStackSize;
        player->inventory.SetSlot(unifiedSlot, itemId, maxStack);

        // Authoritative request — server will mutate its own inventory state.
        if (networkClient && networkClient->IsConnected()) {
            Network::InventoryClickC2SPacket pkt;
            pkt.slotIndex      = static_cast<int16_t>(unifiedSlot);
            pkt.button         = 0;
            pkt.action         = static_cast<uint8_t>(Network::ContainerInput::CREATIVE_FILL_SLOT);
            pkt.flags          = 0;
            pkt.creativeItemId = static_cast<uint32_t>(itemId);
            auto data = Network::Serialization::Serialize(pkt);
            if (auto conn = networkClient->GetConnection()) {
                conn->SendPacket(static_cast<uint8_t>(Network::PacketId::InventoryClickC2S), data);
            }
        }
    }

    void ClientPlayerController::OnRespawnRequest() {
        // TODO: Implement respawn request for multiplayer
        // This would send a client command packet to respawn
        // net->SendClientCommand(RESPAWN);
        
        Log::Debug("Respawn request (TODO: Implement for multiplayer)");
    }

    int32_t ClientPlayerController::PickEntity() const {
        if (!player) return 0;
        if (!Client::g_clientMobManager) return 0;

        // MC's entity pick distance in survival is 3.0 blocks
        // (Attributes.ENTITY_INTERACTION_RANGE). The server re-checks with a
        // more generous 6.0 to absorb latency — see HandleInteract.
        constexpr float kPickRange = 3.0f;

        const glm::vec3 origin = player->GetEyePosition();
        const glm::vec3 dir = glm::normalize(player->lookDir);

        // Ray-vs-AABB over the mobs in range, nearest wins. MC inflates each
        // candidate box by 0.3 (EntityHitResult's pick margin) so a target is
        // hittable slightly outside its collision box, which is what makes
        // combat feel responsive rather than pixel-perfect.
        constexpr float kPickInflate = 0.3f;

        int32_t bestId = 0;
        float bestT = kPickRange;

        // Other players first, so the loop below can only beat them on
        // distance. A remote player's id IS its connection id, which is what
        // lets the server tell it from a mob without a kind byte on the wire.
        if (Client::g_remotePlayerManager) {
            for (const auto& [pid, rp] : Client::g_remotePlayerManager->GetPlayers()) {
                if (!rp.positionInitialized) continue;

                // MC's player box: 0.6 wide, 1.8 tall, feet at the position.
                Game::AABB box;
                box.min = rp.position - glm::vec3(0.3f, 0.0f, 0.3f)
                        - glm::vec3(kPickInflate);
                box.max = rp.position + glm::vec3(0.3f, 1.8f, 0.3f)
                        + glm::vec3(kPickInflate);

                float tMin = 0.0f, tMax = bestT;
                bool hit = true;
                for (int a = 0; a < 3 && hit; ++a) {
                    const float o = origin[a], d = dir[a];
                    const float lo = box.min[a], hi = box.max[a];
                    if (std::abs(d) < 1e-8f) {
                        if (o < lo || o > hi) { hit = false; break; }
                        continue;
                    }
                    float t1 = (lo - o) / d, t2 = (hi - o) / d;
                    if (t1 > t2) std::swap(t1, t2);
                    tMin = std::max(tMin, t1);
                    tMax = std::min(tMax, t2);
                    if (tMin > tMax) { hit = false; break; }
                }
                if (!hit || tMin >= bestT) continue;
                if (player->lastBlockHit.has_value()) {
                    const float blockDist =
                        glm::length(player->lastBlockHit->hitPoint - origin);
                    if (blockDist < tMin) continue;
                }
                bestT = tMin;
                bestId = static_cast<int32_t>(pid);
            }
        }

        for (const auto& [id, entry] : Client::g_clientMobManager->All()) {
            const Game::Mob& mob = *entry.mob;
            if (!mob.IsAlive()) continue;

            Game::AABB box = mob.GetAABB();
            box.min -= glm::vec3(kPickInflate);
            box.max += glm::vec3(kPickInflate);

            // Slab method. A zero direction component is handled by the
            // infinities that division produces, which compare correctly
            // against the finite bounds.
            float tMin = 0.0f;
            float tMax = bestT;
            bool hit = true;
            for (int axis = 0; axis < 3; ++axis) {
                const float o = origin[axis];
                const float d = dir[axis];
                const float lo = box.min[axis];
                const float hi = box.max[axis];

                if (std::abs(d) < 1e-8f) {
                    if (o < lo || o > hi) { hit = false; break; }
                    continue;
                }
                float t1 = (lo - o) / d;
                float t2 = (hi - o) / d;
                if (t1 > t2) std::swap(t1, t2);
                tMin = std::max(tMin, t1);
                tMax = std::min(tMax, t2);
                if (tMin > tMax) { hit = false; break; }
            }

            if (!hit || tMin >= bestT) continue;

            // A block between us and the mob wins. lastBlockHit is the
            // per-frame block raycast, already in the same units.
            if (player->lastBlockHit.has_value()) {
                const float blockDist = glm::length(player->lastBlockHit->hitPoint - origin);
                if (blockDist < tMin) continue;
            }

            bestT = tMin;
            bestId = id;
        }

        return bestId;
    }

    bool ClientPlayerController::TryAttackEntity() {
        if (!player || !networkClient) return false;

        const int32_t bestId = PickEntity();
        if (bestId == 0) return false;

        Network::InteractC2SPacket packet;
        packet.entityId = bestId;
        packet.action = Network::InteractC2SPacket::Action::Attack;
        packet.sneaking = player->sneakPressed;
        packet.sprinting = player->physics.isSprinting;

        // Mirror the server's ticker so the attack indicator reads the same
        // charge the server will use. MC does exactly this — the bar is the
        // CLIENT's copy of attackStrengthTicker, reset locally on the swing.
        player->attackStrengthTicker = 0;

        auto connection = networkClient->GetConnection();
        if (connection) {
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

        {
            breakButtonHeld = true;

#if ENABLE_PORTAL_GUN
            // PortalGun hijacks left-click for blue-portal placement.
            const Game::ItemID held = player->inventory.GetSelectedItem();
            if (held != Game::ItemID(0) && held == Game::Items::PortalGun) {
                SpawnPortalProjectile(/*isOrange=*/false);
                return;  // skip the normal block-break path
            }
#endif

            // MC Minecraft.startAttack picks an ENTITY before a block: the
            // crosshair target is whichever is nearer, and an entity in front
            // of a wall must be hittable. Doing this after the block path
            // would make mobs unhittable whenever anything was behind them.
            if (TryAttackEntity()) return;

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
                    CreativeDestroy(currentHit->blockPos);
                    return;
                }
                // Instant-break check (MC's `if (f >= 1.0F) destroyBlock(pos)`):
                // grass/flowers/torches break inside startDestroyBlock without
                // entering the held-mining state. Mirror that.
                const BlockID hereBlock = ReadBlock(currentHit->blockPos);
                if (hereBlock != BlockID::Air) {
                    const Block& block = BlockRegistry::Get(hereBlock);
                    const float inc = GetDestroyProgressPerTick(
                        player->inventory.GetSelectedItem(), block,
                        player->physics.isOnGround);
                    if (inc >= 1.0f) {
                        // Instant break — set up minimal state so FinishDig's
                        // packet/inventory path runs, then fire it.
                        digState.destroyBlockPos      = currentHit->blockPos;
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
                        if (player->physics.isSneaking) {
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
                                const glm::vec3 eye  = player->physics.GetEyePosition();
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
                if (const int32_t pickedEntity = PickEntity(); pickedEntity != 0) {
                    if (networkClient) {
                        Network::InteractC2SPacket packet;
                        packet.entityId = pickedEntity;
                        packet.action   = Network::InteractC2SPacket::Action::Interact;
                        packet.sneaking = player->sneakPressed;
                        if (auto connection = networkClient->GetConnection()) {
                            connection->SendPacket(
                                static_cast<uint8_t>(Network::PacketId::InteractC2S),
                                Network::Serialization::Serialize(packet));
                        }
                    }
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
                    uint8_t    predictState = 0;
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

                    // The block appears this frame instead of a round trip
                    // later; the server's ack confirms it or rolls it back.
                    if (!usedOn && predictable) {
                        PredictBlock(predictPos, predictBlock, sequence, predictState);
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
                    if (!usedOn) {
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
                            StartPredictedUse(PickUseHand());
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
                    const uint32_t useHand = PickUseHand();
                    const uint32_t useSeq = SendUseItem(static_cast<int>(useHand));
                    // Buckets are the air-use case that edits the world; run
                    // it locally so the water appears/disappears immediately.
                    PredictUseItem(useHand, useSeq);
                    StartPredictedUse(useHand);
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
            StopPredictedUse();
        }
    }

    void ClientPlayerController::TryPlaceBlock() {
        if (!world || !player) {
            Log::Warning("Cannot place block - missing references");
            return;
        }

        const auto& currentHit = player->lastBlockHit;
        if (!currentHit.has_value()) {
            return;
        }

        BlockID selectedBlock = player->GetSelectedBlock();
        if (selectedBlock == BlockID::Air) {
            return;
        }

        const glm::ivec3& placePos = currentHit->adjacentPos;
        if (!CanPlaceBlockAt(placePos)) {
            return;
        }

        // Check if placing block would intersect with player
        AABB blockAABB(
            glm::vec3(placePos) + glm::vec3(0.5f),
            glm::vec3(1.0f)
        );

        if (player->physics.GetAABB().Intersects(blockAABB)) {
            Log::Debug("Cannot place block - would intersect with player");
            return;
        }

        // Creative keeps infinite stacks — only survival consumes.
        if (!player->IsCreative() && !player->inventory.ConsumeSelectedBlock()) {
            Log::Debug("Cannot place block - none left in inventory");
            return;
        }

        bool placementSuccessful = false;
        try {
            placementSuccessful = world->SetBlock(placePos.x, placePos.y, placePos.z, selectedBlock);
        } catch (const std::exception& e) {
            Log::Error("Exception during block placement: %s", e.what());
            placementSuccessful = false;
        }

        if (placementSuccessful) {
            player->stats.blocksPlaced++;
            player->stats.lastPlacedBlockId = static_cast<int>(selectedBlock);
            rightClickDelay = PLACE_REFIRE_TICKS;

            // Remeshing triggered by server's BlockChangeS2C → ProcessBlockChange
            // (handles neighbor boundaries correctly, avoids race conditions).

            const Block& block = BlockRegistry::Get(selectedBlock);
            Log::Info("Placed %s at (%d, %d, %d)",
                     block.name.c_str(), placePos.x, placePos.y, placePos.z);
        } else {
            player->inventory.AddBlocks(selectedBlock, 1);
            Log::Warning("Failed to place block at (%d, %d, %d)",
                        placePos.x, placePos.y, placePos.z);
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

        // Predict the break into the client's own chunk data. This is what
        // makes breaking feel instant on a remote server; on the integrated
        // host it lands a tick earlier than the echo would.
        PredictBlock(pos, BlockID::Air, sequence);

        // Integrated host only: also clear the shared server World so the
        // host's raycast/physics (which read the server World, not the client
        // cache) agree with what was just predicted visually. A remote client
        // has no World — its ClientBlockAccess reads the same chunk cache the
        // prediction just wrote, so it's already consistent.
        bool breakingSuccessful = true;
        if (world) {
            try {
                breakingSuccessful = world->SetBlock(pos.x, pos.y, pos.z, BlockID::Air);
            } catch (const std::exception& e) {
                Log::Error("Exception during block breaking: %s", e.what());
                breakingSuccessful = false;
            }
        }

        if (breakingSuccessful) {
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
        } else {
            Log::Warning("Failed to break block at (%d, %d, %d)",
                        pos.x, pos.y, pos.z);
        }
    }

    bool ClientPlayerController::CanPlaceBlockAt(const glm::ivec3& pos) {
        if (!world) {
            return false;
        }

        if (!world->IsValidPosition(pos.x, pos.y, pos.z)) {
            return false;
        }

        BlockID existing = BlockID::Air;
        try {
            existing = world->GetBlock(pos.x, pos.y, pos.z);
        } catch (const std::exception& e) {
            Log::Error("Exception checking block at placement position: %s", e.what());
            return false;
        }

        if (existing != BlockID::Air) {
            return false;
        }

        return true;
    }


    BlockID ClientPlayerController::GetBreakingBlockType(const glm::ivec3& pos) {
        if (!world) {
            return BlockID::Air;
        }

        try {
            return world->GetBlock(pos.x, pos.y, pos.z);
        } catch (const std::exception& e) {
            Log::Error("Exception getting breaking block type: %s", e.what());
            return BlockID::Air;
        }
    }

#if ENABLE_PORTAL_GUN
    // Portal's BLAST_SPEED from weapon_portalgun.cpp:71 — 3000 HU/s
    // = 57.15 m/s. For long-range shots (server reach = 256 m) we
    // need ~4.5 s of flight at that speed, so we lift the lifetime
    // cap well above sv_portal_projectile_delay's 0.5 s.
    static constexpr float kPortalProjSpeed_m_per_s = 57.15f;
    static constexpr float kPortalProjMaxLifetime  = 4.5f;  // ≈ 257 m

    void ClientPlayerController::SpawnPortalProjectile(bool isOrange) {
        if (!player) return;

        // Use the cached camera-space forward written by UpdateRaycast
        // each frame. player->yaw/pitch are stale (mouse-look writes
        // camera.yaw/pitch and only syncs back on teleports), so reading
        // them here makes every shot fly the same direction.
        const glm::vec3 front = player->lookDir;

        const glm::vec3 origin = player->physics.GetEyePosition();

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
        const glm::vec3 muzzle =
            origin
            + right * kMuzzleOffsetCameraSpace.x
            + up    * kMuzzleOffsetCameraSpace.y
            + front * kMuzzleOffsetCameraSpace.z;

        // Aim the visual bolt at the crosshair point — find the
        // logical projectile's first impact within max range, and
        // use that as the visual endpoint. If no hit (sky shot),
        // the bolt streaks to the max-lifetime point in the air.
        // This makes the bolt appear to converge from the muzzle to
        // where you aimed, hiding the small parallax between the
        // muzzle and the crosshair.
        const float reachM = kPortalProjSpeed_m_per_s * kPortalProjMaxLifetime;
        auto aimHit = Raycast::CastRay(origin, front, reachM);
        const glm::vec3 aimPoint = aimHit.has_value()
            ? aimHit->hitPoint
            : (origin + front * reachM);
        Render::g_portalParticleSystem.EmitProjectile(muzzle, aimPoint, isOrange);

        // Play the real Source @fire1 animation — 15-frame, 0.625s
        // skeletal clip from v_portalgun.mdl (the prongs spin out and
        // back). Returns to @idle automatically when done.
        Render::g_portalGunViewmodel.OnFire();
    }

    void ClientPlayerController::UpdatePendingPortalProjectiles(float deltaTime) {
        if (m_pendingPortalProjectiles.empty()) return;

        const float segmentDist = kPortalProjSpeed_m_per_s * deltaTime;
        auto it = m_pendingPortalProjectiles.begin();
        while (it != m_pendingPortalProjectiles.end()) {
            PendingPortalProjectile& p = *it;
            p.age += deltaTime;
            if (p.age > kPortalProjMaxLifetime) {
                it = m_pendingPortalProjectiles.erase(it);
                continue;
            }

            // Sweep one short segment per frame using the shared block
            // raycast (same one the look-aim uses, just stepped forward
            // from the projectile's current position).
            auto hit = Raycast::CastRay(p.currentPos, p.direction, segmentDist);
            if (hit.has_value()) {
                // altInteract=true → blue portal (matches LMB semantics).
                SendUseItemOn(*hit, p.hand, /*altInteract=*/!p.isOrange);
                it = m_pendingPortalProjectiles.erase(it);
                continue;
            }

            p.currentPos += p.direction * segmentDist;
            ++it;
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