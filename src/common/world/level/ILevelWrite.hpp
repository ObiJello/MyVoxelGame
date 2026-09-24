// File: src/common/world/level/ILevelWrite.hpp
//
// Read+write block access for code that must run on BOTH sides.
//
// Item behaviours (hoe tilling, shovel path, axe stripping, flint & steel,
// bone meal, buckets) are already common code, but they used to take a
// concrete Game::World* — which only the server has. That forced a remote
// client to wait a full round trip before a tilled block appeared. MC has no
// such split: ItemStack.useOn runs client-side inside
// MultiPlayerGameMode.startPrediction (MultiPlayerGameMode.java:347) against
// the client's own level, and the block edit it performs is captured by the
// prediction handler.
//
// Implementations:
//   Game::World          — server authority (writes the real world).
//   Client::ClientBlockAccess — client prediction (writes the client chunk
//                          cache through ClientChunkManager's prediction
//                          handler, so the ack can roll it back).
#pragma once

#include "../chunk/IBlockAccess.hpp"
#include "../block/Direction.hpp"
#include "DimensionId.hpp"
#include "common/sound/LevelSound.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace Game {

    struct ScheduledTickAccess;
    class  BlockEntity;
    class  EntityLevel;
    class  JavaRandom;
    enum class ParticleKind : uint8_t;   // EntityLevel.hpp

    class ILevelWrite : public IBlockAccess {
    public:
        // Which world this is. Behaviours that run on both sides need it
        // because some rules are dimension-gated — a nether portal only
        // lights in the overworld or the nether (MC BaseFireBlock
        // .inPortalDimension), so the client's prediction has to make the
        // same call the server will.
        //
        // Defaulted rather than pure so an accessor that predates dimensions
        // still compiles; every real level overrides it.
        virtual DimensionId GetDimension() const { return DimensionId::Overworld; }

        // Mirrors World::SetBlock's flagged overload. `updateFlags` uses
        // Game::World::UpdateFlags values; the client implementation ignores
        // everything except the fact that a write happened (its remesh and
        // neighbour dirtying are handled by ClientChunkManager).
        virtual bool SetBlock(int worldX, int worldY, int worldZ,
                              BlockID blockId, uint32_t updateFlags) = 0;

        // Same, carrying the block-state index (MC BlockState.getId()). Needed
        // by any behaviour that edits a PROPERTY rather than swapping the
        // block — a shovel dousing a campfire writes the same block back with
        // `lit=false`, so the two-argument form above would reset it to the
        // default state and relight it.
        //
        // Both implementations already had this overload; it is declared here
        // so common code can reach it through the interface.
        virtual bool SetBlock(int worldX, int worldY, int worldZ,
                              BlockID blockId, uint32_t updateFlags,
                              BlockStateIndex stateIndex) = 0;

        // MC's `Level.setBlock(pos, state, flags)` — the form callers should
        // use. Non-virtual on purpose: it forwards to the pair overload above,
        // which is what implementors override and what the storage layer still
        // speaks. Argument order follows vanilla (state, then flags), NOT the
        // pair form's (flags, then index).
        bool SetBlock(int worldX, int worldY, int worldZ,
                      BlockState state, uint32_t updateFlags) {
            return SetBlock(worldX, worldY, worldZ, state.Block(), updateFlags, state.Index());
        }

        // MC Level.isClientSide. Item behaviours run on both sides (see the
        // note above), and most of them WANT that — a tilled block should
        // appear immediately and be rolled back if the server disagrees.
        //
        // Some do not. Anything that spawns an entity has no client-side
        // equivalent to predict: the entity only exists once the server sends
        // it, so running the spawn during prediction would either do nothing
        // or, in single-player where both sides share a process, spawn twice.
        // MC's own SpawnEggItem opens with `if (!(level instanceof ServerLevel))
        // return SUCCESS;` for exactly this reason, and this is the flag that
        // branch needs.
        virtual bool IsClientSide() const = 0;

        // MC LevelAccessor's ScheduledTickAccess half — how a block behaviour
        // books a delayed tick on itself ("fall two ticks from now").
        //
        // NULL is a legitimate answer and callers must check. The client
        // returns null because block ticks are server authority: a predicted
        // client-side tick would run against a world the server has not agreed
        // to yet, and the correction would arrive as a visible rewind. Vanilla
        // says the same thing by handing ClientLevel a BlackholeTickAccess.
        virtual ScheduledTickAccess* Ticks() { return nullptr; }

        // ── Neighbour notification (MC Level / NeighborUpdater) ────────────
        //
        // The writable half of the update machinery, which redstone lives on.
        // Every one of these is a no-op by default, which is exactly what MC's
        // base `Level` does: `updateNeighborsAt`, `neighborChanged` and
        // `blockEvent` are empty there and only ServerLevel fills them in. The
        // client's predicted level inherits the no-ops and stays a pure
        // block store, as vanilla's ClientLevel is.
        //
        // MC Level.updateNeighborsAt(pos, sourceBlock): tell all six
        // neighbours that `sourceBlock` at `pos` changed, in
        // NeighborUpdater.UPDATE_ORDER (west, east, down, up, north, south).
        virtual void UpdateNeighborsAt(const glm::ivec3& pos, BlockID sourceBlock) {
            (void)pos; (void)sourceBlock;
        }

        // MC Level.updateNeighborsAtExceptFromFacing — the same walk with one
        // direction left out. A repeater changing state does not tell the
        // block BEHIND it, or it would re-trigger its own input.
        virtual void UpdateNeighborsAtExceptFromFacing(const glm::ivec3& pos, BlockID sourceBlock,
                                                       Direction skipDirection) {
            (void)pos; (void)sourceBlock; (void)skipDirection;
        }

        // MC Level.neighborChanged(pos, sourceBlock, orientation) — ONE cell,
        // told that `sourceBlock` changed somewhere next to it. Comparators
        // reading a container are reached this way.
        virtual void NeighborChanged(const glm::ivec3& pos, BlockID sourceBlock) {
            (void)pos; (void)sourceBlock;
        }

        // MC Level.updateNeighbourForOutputSignal(pos, block): a container's
        // contents changed, so any comparator reading it — directly beside it
        // or one solid block away — must re-measure.
        virtual void UpdateNeighbourForOutputSignal(const glm::ivec3& pos, BlockID block) {
            (void)pos; (void)block;
        }

        // MC Level.blockEvent(pos, block, b0, b1): book a block event for the
        // end-of-tick drain. Pistons and note blocks act through this rather
        // than on the spot, and the delay is observable (a piston fires one
        // block-event phase after the update that armed it).
        virtual void BlockEvent(const glm::ivec3& pos, BlockID block, int b0, int b1) {
            (void)pos; (void)block; (void)b0; (void)b1;
        }

        // MC Level.destroyBlock(pos, dropResources): break the block as if
        // mined — loot (when asked), then the cell becomes the fluid it held
        // or air, written with flag 3. Returns whether anything was there.
        virtual bool DestroyBlock(const glm::ivec3& pos, bool dropResources) {
            (void)pos; (void)dropResources;
            return false;
        }

        // ── Block entities (MC Level.getBlockEntity / setBlockEntity) ───────
        //
        // Null on a level that keeps no block entities, and for any cell
        // whose chunk is not resident — never loads.
        virtual BlockEntity* GetBlockEntity(const glm::ivec3& pos) {
            (void)pos;
            return nullptr;
        }

        // Install a block entity at `pos`, replacing any that is there, and
        // tell the watchers. How a piston hands the moving-block cell its
        // carried state, and how a comparator's output survives a write.
        virtual void SetBlockEntity(const glm::ivec3& pos, std::unique_ptr<BlockEntity> entity);

        // MC Level.addParticle, for block code that runs on the client — a
        // block entity's client tick or a block event (the spawner's smoke
        // and flame). The client level forwards to its particle system; every
        // other level ignores it, exactly MC's split (Level.addParticle is an
        // empty default that ClientLevel overrides).
        virtual void AddParticle(ParticleKind kind, double x, double y, double z,
                                 double vx, double vy, double vz) {
            (void)kind; (void)x; (void)y; (void)z; (void)vx; (void)vy; (void)vz;
        }

        // The block entity at `pos` changed in a way clients must see (MC
        // BlockEntity.setChanged + getUpdatePacket). Marks it for saving too.
        virtual void BlockEntityChanged(const glm::ivec3& pos) { (void)pos; }

        // MC Level.removeBlockEntity — drop the entity without touching the
        // block. The piston's moving-block entity retires itself this way
        // before writing the block it carried.
        virtual void RemoveBlockEntity(const glm::ivec3& pos) { (void)pos; }

        // MC ServerLevel.isHandlingTick: true while the level is inside its
        // block-tick / block-event phase. PistonBaseBlock.checkIfExtend reads
        // it to decide whether a retraction may drop the carried block.
        virtual bool IsHandlingTick() const { return false; }

        // ── The rest of MC's Level surface that blocks reach for ────────────
        //
        // MC Level.getGameTime. The client's level answers 0, which no block
        // behaviour should ever observe: everything that reads the clock is
        // server-side (torch burnout, block events).
        virtual int64_t GameTime() const { return 0; }

        // MC Level.random. Null where there is none to give (the client's
        // prediction, which never runs a tick).
        virtual JavaRandom* Random() { return nullptr; }

        // The entity view of this level — mobs, players, dropped items — for
        // the blocks that count what stands on them. Null on the client.
        virtual EntityLevel* Entities() { return nullptr; }

        // The CLIENT's own player, which is not a Game::Entity and so is
        // invisible to Entities(). Vanilla's client pushes its LocalPlayer
        // through the same PistonMovingBlockEntity.tick the server runs for
        // everyone else; these two hooks let that tick reach it. False / no-op
        // on the server, where the player is a real entity.
        virtual bool GetLocalPlayerBox(glm::dvec3& outMin, glm::dvec3& outMax) const {
            (void)outMin; (void)outMax;
            return false;
        }
        virtual void MoveLocalPlayerByPiston(const glm::dvec3& delta) { (void)delta; }

        // The same player's motion, for the block entity tickers that push
        // it the way MC's client pushes its LocalPlayer (the potent sulfur
        // geyser's LAUNCH_ENTITY_TICKER runs on both sides; the player is
        // the client's to move). Movement is MC's deltaMovement, blocks per
        // TICK. GetLocalPlayerMovement answers false where there is no such
        // player to push — on the server, and on the client while its player
        // is a spectator or dead (MC's NO_SPECTATORS / ENTITY_STILL_ALIVE).
        virtual bool GetLocalPlayerMovement(glm::dvec3& outDeltaMovement, bool& outFlying) const {
            (void)outDeltaMovement; (void)outFlying;
            return false;
        }
        virtual void AddLocalPlayerDeltaMovement(const glm::dvec3& delta) { (void)delta; }
        // MC Entity.checkFallDistanceAccumulation on that player: rising, or
        // falling slower than half a block a tick, caps the fall at one block.
        virtual void CheckLocalPlayerFallDistanceAccumulation() {}

        // ── Sound (MC Level.playSound / playSeededSound / playLocalSound) ───
        //
        // See common/sound/LevelSound.hpp for what `except` means on each
        // side. The base is silent, which is MC's Level for everything but
        // its two real subclasses: Game::World (server authority) broadcasts
        // through the installed ServerSoundSink, Client::ClientBlockAccess
        // plays the predicted half.
        virtual void PlaySeededSound(const SoundExcept& except, const glm::dvec3& pos,
                                     std::string_view event, SoundSource source,
                                     float volume, float pitch, int64_t seed) {
            (void)except; (void)pos; (void)event; (void)source; (void)volume; (void)pitch; (void)seed;
        }
        // MC Level.playSound(except, x, y, z, sound, source, volume, pitch).
        void PlaySound(const SoundExcept& except, const glm::dvec3& pos, std::string_view event,
                       SoundSource source, float volume = 1.0f, float pitch = 1.0f) {
            PlaySeededSound(except, pos, event, source, volume, pitch, Sound::NextSeed());
        }
        // MC Level.playSound(except, BlockPos, ...): the block's centre.
        void PlaySound(const SoundExcept& except, const glm::ivec3& pos, std::string_view event,
                       SoundSource source, float volume = 1.0f, float pitch = 1.0f) {
            PlaySound(except, Sound::BlockCenter(pos), event, source, volume, pitch);
        }

        // MC Level.playLocalSound — a sound only THIS client hears, never sent
        // anywhere (animateTick's fire crackle, lava pops, portal hum). A
        // no-op everywhere but the client, as in MC.
        virtual void PlayLocalSound(const glm::dvec3& pos, std::string_view event, SoundSource source,
                                    float volume, float pitch, bool distanceDelay) {
            (void)pos; (void)event; (void)source; (void)volume; (void)pitch; (void)distanceDelay;
        }
        void PlayLocalSound(const glm::ivec3& pos, std::string_view event, SoundSource source,
                            float volume, float pitch, bool distanceDelay) {
            PlayLocalSound(Sound::BlockCenter(pos), event, source, volume, pitch, distanceDelay);
        }
    };

} // namespace Game
