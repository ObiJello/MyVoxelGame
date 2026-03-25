// File: src/client/input/PlayerController.cpp
#include "PlayerController.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/level/World.hpp"
#include "common/network/PacketTypes.hpp"
#include "../network/NetworkClient.hpp"
#include "../network/ClientConnection.hpp"
#include "../renderer/mesh/ClientMeshManager.hpp"
#include "../world/ClientChunkManager.hpp"
#include "common/core/Log.hpp"
#include <glm/glm.hpp>
#include <thread>

namespace Game {

    ClientPlayerController::ClientPlayerController()
        : player(nullptr)
        , world(nullptr)
        , networkClient(nullptr)
        , isBreaking(false)
        , breakButtonHeld(false)
        , breakProgress(0.0f)
        , breakingBlockPos(0)
        , placeButtonHeld(false)
        , placeCooldownTimer(0.0f)
        , rightClickDelayTimer(0.0f)
        , lastMoveSend(std::chrono::steady_clock::now())
    {
        Log::Info("ClientPlayerController initialized");
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

        // Update breaking progress
        UpdateBreaking(deltaTime);

        // Update place cooldown
        if (placeCooldownTimer > 0.0f) {
            placeCooldownTimer -= deltaTime;
        }
        
        // Update right click delay timer (Minecraft-style rate limiting)
        if (rightClickDelayTimer > 0.0f) {
            rightClickDelayTimer -= deltaTime;
        }

        // NOTE: Block placement now only happens via network packets
        // The server will send back block changes that update the world

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

    void ClientPlayerController::StartDig(const glm::ivec3& pos, int face) {
        // TODO: Send start dig packet to server
        // For now, just start the local breaking animation
        isBreaking = true;
        breakProgress = 0.0f;
        breakingBlockPos = pos;

        // Cache block ID now — by the time FinishBreaking runs, the server
        // may have already set this position to Air via BlockChangeS2C.
        breakingBlockId = BlockID::Air;
        if (world) {
            try {
                breakingBlockId = world->GetBlock(pos.x, pos.y, pos.z);
            } catch (...) {}
        }

        Log::Debug("Started breaking block at (%d, %d, %d) type=%d",
                  breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z,
                  static_cast<int>(breakingBlockId));
        
        // TODO: Send packet
        // net->SendBlockDig(START_DESTROY_BLOCK, pos, face, ++interactSeq);
    }

    void ClientPlayerController::AbortDig() {
        if (isBreaking) {
            // TODO: Send abort dig packet to server
            // net->SendBlockDig(ABORT_DESTROY_BLOCK, breakingBlockPos, 0, ++interactSeq);
            
            isBreaking = false;
            breakProgress = 0.0f;
            Log::Debug("Breaking cancelled");
        }
    }

    void ClientPlayerController::FinishDig() {
        // Send block break to server (server-authoritative)
        if (networkClient && networkClient->IsConnected()) {
            Network::BlockActionC2SPacket packet;
            packet.worldX = breakingBlockPos.x;
            packet.worldY = breakingBlockPos.y;
            packet.worldZ = breakingBlockPos.z;
            packet.action = Network::BlockActionType::BREAK;
            packet.face = 0;
            packet.sequenceNumber = ++interactSeq;

            auto data = Network::Serialization::Serialize(packet);
            auto connection = networkClient->GetConnection();
            if (connection) {
                connection->SendPacket(static_cast<uint8_t>(Network::PacketId::BlockActionC2S), data);
            }
        }

        // Also handle locally for immediate feedback
        FinishBreaking();
    }

    void ClientPlayerController::SendUseItemOn(const RaycastHit& hit, int hand) {
        // Build and send BlockPlaceC2S packet (Minecraft-compatible)
        Log::Debug("SendUseItemOn called for block (%d,%d,%d), hand=%d", 
                  hit.blockPos.x, hit.blockPos.y, hit.blockPos.z, hand);
        
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
            ++interactSeq           // Sequence number
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

    void ClientPlayerController::UpdateBreaking(float deltaTime) {
        if (!isBreaking || !breakButtonHeld || !player) {
            return;
        }

        // Check if we still have sight of the breaking block
        const auto& currentHit = player->lastBlockHit;
        if (!currentHit.has_value() || currentHit->blockPos != breakingBlockPos) {
            AbortDig();
            return;
        }

        // Increase break progress
        breakProgress += deltaTime / BREAK_TIME;

        // Check if block is broken
        if (breakProgress >= 1.0f) {
            FinishDig();
        }
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
            
            const auto& currentHit = player->lastBlockHit;
            if (currentHit.has_value() && !isBreaking) {
                StartDig(currentHit->blockPos, currentHit->hitFace);
            }
        } else {
            breakButtonHeld = false;
            
            if (isBreaking) {
                AbortDig();
            }
        }
    }

    void ClientPlayerController::OnRMB(bool pressed) {
        if (!player) return;
        
        if (pressed) {
            Log::Debug("OnRMB pressed, rightClickDelayTimer=%.3f", rightClickDelayTimer);
            
            // Check if we should process this click (rate limiting)
            if (rightClickDelayTimer <= 0.0f) {
                const auto& currentHit = player->lastBlockHit;
                
                // TODO: Check for entity hit first when entity system exists
                // if (player->lastEntityHit.has_value()) {
                //     SendUseEntity(*player->lastEntityHit, 0);  // Interact with entity
                //     rightClickDelayTimer = RIGHT_CLICK_DELAY;
                // } else
                if (currentHit.has_value() && player->GetSelectedBlock() != BlockID::Air) {
                    // Ensure server knows what block we're holding BEFORE the place request
                    OnHotbarChanged(player->inventory.GetSelectedSlot());
                    // Send place request to server
                    SendUseItemOn(*currentHit, 0);  // 0 = main hand
                    // Consume block from client inventory
                    player->inventory.ConsumeSelectedBlock();
                    rightClickDelayTimer = RIGHT_CLICK_DELAY;  // Set delay to prevent spam
                } else {
                    // No hit - use item in air
                    SendUseItem(0);  // 0 = main hand
                    rightClickDelayTimer = RIGHT_CLICK_DELAY;
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
            placeCooldownTimer = PLACE_COOLDOWN;

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

        const auto& currentHit = player->lastBlockHit;
        if (!currentHit.has_value()) {
            return;
        }

        // Use the cached block ID from StartDig — the world position may already
        // be Air if the server processed BlockActionC2S before we got here.
        BlockID brokenBlock = breakingBlockId;

        if (brokenBlock == BlockID::Bedrock) {
            Log::Debug("Cannot break bedrock");
            isBreaking = false;
            breakProgress = 0.0f;
            return;
        }

        if (brokenBlock == BlockID::Air) {
            Log::Debug("Cannot break air");
            isBreaking = false;
            breakProgress = 0.0f;
            return;
        }

        bool breakingSuccessful = false;
        try {
            breakingSuccessful = world->SetBlock(breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z, BlockID::Air);
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
                     block.name.c_str(), breakingBlockPos.x,
                     breakingBlockPos.y, breakingBlockPos.z);
        } else {
            Log::Warning("Failed to break block at (%d, %d, %d)",
                        breakingBlockPos.x, breakingBlockPos.y, breakingBlockPos.z);
        }

        isBreaking = false;
        breakProgress = 0.0f;
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