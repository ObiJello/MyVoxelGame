// File: src/client/input/PlayerController.cpp
#include "PlayerController.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/MiningSpeed.hpp"
#include "common/world/level/World.hpp"
#include "common/network/PacketTypes.hpp"
#include "../network/NetworkClient.hpp"
#include "../network/ClientConnection.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "../world/ClientChunkManager.hpp"
#include "common/core/Features.hpp"
#include "common/core/Log.hpp"
#include "common/entity/Item.hpp"
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

    void ClientPlayerController::SendDigPacket(Network::BlockActionType action,
                                               const glm::ivec3& pos, BlockID blockId) {
        if (!networkClient || !networkClient->IsConnected()) return;
        Network::BlockActionC2SPacket packet;
        packet.worldX = pos.x;
        packet.worldY = pos.y;
        packet.worldZ = pos.z;
        packet.action = action;
        packet.blockId = blockId;
        packet.face = 0;
        packet.sequenceNumber = ++interactSeq;
        auto data = Network::Serialization::Serialize(packet);
        auto connection = networkClient->GetConnection();
        if (connection) {
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::BlockActionC2S), data);
        }
    }

    void ClientPlayerController::SetPlayer(ClientPlayer* playerPtr) {
        player = playerPtr;
        Log::Debug("ClientPlayerController player reference set");
    }

    void ClientPlayerController::SetWorld(World* worldPtr) {
        world = worldPtr;
        Log::Debug("ClientPlayerController world reference set");
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
            UpdateBreakingTick();
            UpdatePlacingTick();
        }

#if ENABLE_PORTAL_GUN
        // Tick any in-flight portal-gun projectiles; on impact each one
        // turns into a UseItemOnC2S at the hit block face.
        UpdatePendingPortalProjectiles(deltaTime);
#endif

        // Send movement packets if due (TODO: Implement for networking)
        SendMovementIfDue();
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

        // Cache block ID at start — the world may already be Air by the time
        // we want to finalise (integrated server shares the world).
        digState.destroyingBlockId = BlockID::Air;
        if (world) {
            try { digState.destroyingBlockId = world->GetBlock(pos.x, pos.y, pos.z); }
            catch (...) {}
        }

        SendDigPacket(Network::BlockActionType::START_DESTROY, pos, digState.destroyingBlockId);
        // First swing fires immediately on press.
        armSwingPending = true;
    }

    void ClientPlayerController::AbortDig() {
        if (!digState.isDestroying) return;
        SendDigPacket(Network::BlockActionType::ABORT_DESTROY,
                      digState.destroyBlockPos, digState.destroyingBlockId);
        digState.isDestroying    = false;
        digState.destroyProgress = 0.0f;
        digState.destroyTicks    = 0;
    }

    void ClientPlayerController::FinishDig() {
        // STOP_DESTROY is the MC finish action. The server clears the block
        // and credits the player's inventory.
        SendDigPacket(Network::BlockActionType::STOP_DESTROY,
                      digState.destroyBlockPos, digState.destroyingBlockId);

        // Local prediction (integrated server convenience — gives instant
        // visual feedback before the BlockChangeS2C echo).
        FinishBreaking();

        digState.isDestroying    = false;
        digState.destroyProgress = 0.0f;
        digState.destroyTicks    = 0;
        digState.destroyDelay    = POST_BREAK_DELAY_TICKS;
    }

    void ClientPlayerController::SendUseItemOn(const RaycastHit& hit, int hand, bool altInteract) {
        // Build and send BlockPlaceC2S packet (Minecraft-compatible)
        Log::Debug("SendUseItemOn called for block (%d,%d,%d), hand=%d alt=%d",
                  hit.blockPos.x, hit.blockPos.y, hit.blockPos.z, hand, altInteract ? 1 : 0);
        
        if (!networkClient) {
            Log::Debug("SendUseItemOn: networkClient is null - not set on controller");
            return;
        }
        
        if (!networkClient->IsConnected()) {
            Log::Debug("SendUseItemOn: networkClient not connected to server");
            return;
        }
        
        Log::Debug("SendUseItemOn: Building BlockPlaceC2S packet...");
        
        // Convert hit face to Minecraft direction format
        // Our format: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z
        // MC format: 0=bottom(-Y), 1=top(+Y), 2=north(-Z), 3=south(+Z), 4=west(-X), 5=east(+X)
        uint32_t direction = 0;
        switch (hit.hitFace) {
            case 0: direction = 5; break;  // +X -> east
            case 1: direction = 4; break;  // -X -> west
            case 2: direction = 1; break;  // +Y -> top
            case 3: direction = 0; break;  // -Y -> bottom
            case 4: direction = 3; break;  // +Z -> south
            case 5: direction = 2; break;  // -Z -> north
        }
        
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
    }

    void ClientPlayerController::SendUseItem(int hand) {
        // TODO: Implement UseItem packet for using items in air
        // This would send a packet for eating food, using bow, shield, etc.
        // When entity system exists, also check for entity interactions
        
        if (!networkClient || !networkClient->IsConnected()) {
            return;
        }
        
        // Future implementation:
        // Network::UseItemC2SPacket packet(hand, ++interactSeq);
        // auto data = Network::Serialization::Serialize(packet);
        // networkClient->GetConnection()->SendPacket(PacketId::UseItemC2S, data);
        
        Log::Debug("TODO: SendUseItem not yet implemented (hand=%d)", hand);
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

        const auto& currentHit = player->lastBlockHit;
        const bool haveTarget = currentHit.has_value();
        const glm::ivec3 hitPos = haveTarget ? currentHit->blockPos : glm::ivec3(0, -1024, 0);

        // No target while held — nothing to do this tick.
        if (!haveTarget) {
            if (digState.isDestroying) AbortDig();
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
        if (world) {
            try {
                BlockID worldBlock = world->GetBlock(hitPos.x, hitPos.y, hitPos.z);
                if (worldBlock != BlockID::Air) {
                    currentBlock = worldBlock;
                    digState.destroyingBlockId = worldBlock;
                }
            } catch (...) {}
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
        if (ticksSincePlace < 1000) ++ticksSincePlace;

        if (!placeButtonHeld) return;
        // First-click placement already happens on the RMB edge in OnRMB.
        // Continuous-RMB re-fires every PLACE_REFIRE_TICKS while held.
        if (ticksSincePlace < PLACE_REFIRE_TICKS) return;

        const auto& currentHit = player->lastBlockHit;
        if (!currentHit.has_value()) return;

        // Only re-fire for items that actually place blocks; for tools we
        // already fired on the edge and shouldn't keep spamming.
        const Game::ItemID held = player->inventory.GetSelectedItem();
        if (held == Game::Items::Air) return;
        if (!ItemRegistry::IsBlockItem(held)) return;

#if ENABLE_PORTAL_GUN
        // PortalGun: continuous-RMB does NOT spam projectiles (its OnRMB edge
        // already started the projectile + viewmodel anim). Skip.
        if (held == Game::Items::PortalGun) return;
#endif

        OnHotbarChanged(player->inventory.GetSelectedSlot());
        SendUseItemOn(*currentHit, 0);
        if (player->GetSelectedBlock() != BlockID::Air) {
            player->inventory.ConsumeSelectedBlock();
        }
        ticksSincePlace = 0;
        // Each repeat-place during held-RMB plays the arm swing, matching
        // MC. The first place is handled by OnRMB's edge-trigger feeding
        // placeEdge in PlatformMain; this covers every subsequent one.
        armSwingPending = true;
    }

    void ClientPlayerController::OnHotbarChanged(int slot) {
        if (!player) return;

        player->SelectSlot(slot);

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

    void ClientPlayerController::OnLMB(bool pressed) {
        if (!player) return;

        if (pressed) {
            breakButtonHeld = true;

#if ENABLE_PORTAL_GUN
            // PortalGun hijacks left-click for blue-portal placement.
            const Game::ItemID held = player->inventory.GetSelectedItem();
            if (held != Game::ItemID(0) && held == Game::Items::PortalGun) {
                SpawnPortalProjectile(/*isOrange=*/false);
                return;  // skip the normal block-break path
            }
#endif

            // MC parity: startDestroyBlock runs SYNCHRONOUSLY on the click —
            // it doesn't wait for the next continueDestroyBlock tick AND it
            // doesn't check destroyDelay. So a fresh click right after a
            // break starts the new dig instantly. (destroyDelay only blocks
            // held-mining continuation; releasing LMB ends the sequence it
            // belongs to — see OnLMB(false) below.)
            digState.destroyDelay = 0;
            const auto& currentHit = player->lastBlockHit;
            if (currentHit.has_value()) {
                // Instant-break check (MC's `if (f >= 1.0F) destroyBlock(pos)`):
                // grass/flowers/torches break inside startDestroyBlock without
                // entering the held-mining state. Mirror that.
                BlockID hereBlock = BlockID::Air;
                if (world) {
                    try { hereBlock = world->GetBlock(currentHit->blockPos.x,
                                                       currentHit->blockPos.y,
                                                       currentHit->blockPos.z); }
                    catch (...) {}
                }
                if (hereBlock != BlockID::Air) {
                    const Block& block = BlockRegistry::Get(hereBlock);
                    const float inc = GetDestroyProgressPerTick(
                        player->inventory.GetSelectedItem(), block,
                        player->physics.isOnGround);
                    if (inc >= 1.0f) {
                        // Instant break — set up minimal state so FinishDig's
                        // packet/inventory path runs, then fire it.
                        digState.destroyBlockPos    = currentHit->blockPos;
                        digState.destroyingBlockId  = hereBlock;
                        digState.destroyProgress   = 1.0f;
                        digState.isDestroying      = true;
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
        } else {
            breakButtonHeld = false;
            // Releasing LMB ends the held-mining sequence the destroyDelay
            // belongs to. Without this, the player gets a 5-tick "first
            // click after a break" lag every time they tap LMB.
            digState.destroyDelay = 0;
            if (digState.isDestroying) AbortDig();
        }
    }

    void ClientPlayerController::OnRMB(bool pressed) {
        if (!player) return;

        if (pressed) {
            // RMB EDGE — fire one placement / use immediately. While-held
            // re-fires are handled by UpdatePlacingTick at MC's 4-tick cadence
            // (no wall-clock throttle).
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
                        ticksSincePlace = 0;
                        placeButtonHeld = true;
                        return;
                    }
                }
#endif

                // TODO: Check for entity hit first when entity system exists
                // if (player->lastEntityHit.has_value()) {
                //     SendUseEntity(*player->lastEntityHit, 0);  // Interact with entity
                //     ticksSincePlace = 0;
                // } else
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
                    SendUseItemOn(*currentHit, 0);  // 0 = main hand

                    // Predictive consumption — ONLY for block placement.
                    // The server's placement-fallback path consumes one block
                    // from the stack; for non-block items (tools, food, etc.)
                    // it doesn't, so we mustn't predict consumption either.
                    // Server is authoritative — it'll re-sync our inventory
                    // either way.
                    if (player->GetSelectedBlock() != BlockID::Air) {
                        player->inventory.ConsumeSelectedBlock();
                    }
                    ticksSincePlace = 0;
                } else {
                    // No block target — use item in air (Bow draw, EnderPearl
                    // throw, food eat, etc.). Server-side `Item.use` handles it.
                    SendUseItem(0);  // 0 = main hand
                    ticksSincePlace = 0;
                }
            }
            placeButtonHeld = true;
        } else {
            placeButtonHeld = false;
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

        if (!player->inventory.ConsumeSelectedBlock()) {
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
            ticksSincePlace = 0;

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

    void ClientPlayerController::FinishBreaking() {
        if (!world || !player) {
            Log::Warning("Cannot break block - missing references");
            return;
        }

        // Use the cached block ID from StartDig — the world position may already
        // be Air if the server processed BlockActionC2S before we got here.
        BlockID brokenBlock = digState.destroyingBlockId;
        const glm::ivec3 pos = digState.destroyBlockPos;

        if (brokenBlock == BlockID::Bedrock || brokenBlock == BlockID::Air) {
            return;
        }

        bool breakingSuccessful = false;
        try {
            breakingSuccessful = world->SetBlock(pos.x, pos.y, pos.z, BlockID::Air);
        } catch (const std::exception& e) {
            Log::Error("Exception during block breaking: %s", e.what());
            breakingSuccessful = false;
        }

        if (breakingSuccessful) {
            int remaining = player->inventory.AddBlocks(brokenBlock, 1);

            if (remaining > 0) {
                Log::Warning("Inventory full - dropped %d %s",
                           remaining, BlockRegistry::Get(brokenBlock).name.c_str());
            }

            // Sync server with updated inventory (block was added to a slot)
            OnHotbarChanged(player->inventory.GetSelectedSlot());

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