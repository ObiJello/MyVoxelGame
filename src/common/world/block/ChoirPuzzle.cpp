// File: src/common/world/block/ChoirPuzzle.cpp
#include "common/world/block/ChoirPuzzle.hpp"
#include "common/world/block/AurelithQuestBlocks.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/sound/SoundSource.hpp"
#include "common/entity/EntityLevel.hpp"
#include "common/entity/GeneratedItemList.hpp"
#include "common/entity/IUsePlayer.hpp"
#include "common/entity/Item.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/mobs/GenericMobs.hpp"
#include "common/physics/Physics.hpp"
#include "common/world/block/BlockInteraction.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/RedstoneStateUtil.hpp"
#include "common/world/level/ILevelWrite.hpp"
#include "common/world/level/World.hpp"
#include "common/world/ticks/ScheduledTickAccess.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Game {

    namespace {

        using namespace ChoirPuzzle;

        // ── The song state (server) ──────────────────────────────────────

        struct Song {
            DimensionId dim;
            glm::ivec3 altar;
            std::vector<glm::ivec3> ring;   // clockwise from north
            std::vector<int> sequence;      // ring indices
            bool singing = true;            // false = listening for the player
            int step = 0;                   // next note to sing / to hear
            int64_t deadline = 0;           // listening: game time to answer by
            int64_t expires = 0;            // stale-guard for a lost block tick
            bool refundShard = true;        // a creative starter paid nothing
        };

        std::mutex& SongsMutex() {
            static std::mutex m;
            return m;
        }
        std::vector<Song>& Songs() {
            static std::vector<Song> songs;
            return songs;
        }

        Song* FindSong(DimensionId dim, const glm::ivec3& altar) {
            for (Song& s : Songs()) {
                if (s.dim == dim && s.altar == altar) return &s;
            }
            return nullptr;
        }

        void EraseSong(DimensionId dim, const glm::ivec3& altar) {
            auto& songs = Songs();
            songs.erase(std::remove_if(songs.begin(), songs.end(),
                                       [&](const Song& s) { return s.dim == dim && s.altar == altar; }),
                        songs.end());
        }

        // ── Chimes ───────────────────────────────────────────────────────

        // Each ring position has its own pitch (MC note-block pitch for
        // semitones 6..): a sung sequence is a tune, not one note repeated.
        float ChimePitch(int index) {
            return std::pow(2.0f, (static_cast<float>(index % 12) * 2.0f - 6.0f) / 12.0f);
        }

        // Light the chime and book its dimming. No puzzle bookkeeping — the
        // altar's singing uses this too.
        void Sound(ILevelWrite& level, const glm::ivec3& pos, int ringIndex) {
            const BlockState state = level.GetBlockState(pos.x, pos.y, pos.z);
            if (!state.Is(BlockID::ResonantChime)) return;
            if (!LitOf(state)) {
                level.SetBlock(pos.x, pos.y, pos.z, WithLit(state, true),
                               World::UpdateFlags::UpdateClients);
            }
            if (ScheduledTickAccess* ticks = level.Ticks()) {
                ticks->ScheduleTick(pos, BlockID::ResonantChime, kChimeLitTicks);
            }
            // The chime's note: an engine event (assets/sound_overlays/
            // obeycraft/world.json — the amethyst block's resonance), pitched
            // by its place in the ring. Server-only (the uses gate on it), so
            // except = null: the striker hears it with everyone else.
            level.PlaySound(nullptr, pos, "obeycraft:block.resonant_chime.strike", SoundSource::Blocks, 1.5f,
                            ChimePitch(ringIndex < 0 ? 0 : ringIndex));
        }

        std::vector<glm::ivec3> GatherRing(const IBlockAccess& level, const glm::ivec3& altar) {
            struct Entry { glm::ivec3 pos; double bearing; int dist2; };
            std::vector<Entry> found;
            for (int dy = -kRingHeight; dy <= kRingHeight; ++dy) {
                for (int dx = -kRingRadius; dx <= kRingRadius; ++dx) {
                    for (int dz = -kRingRadius; dz <= kRingRadius; ++dz) {
                        if (dx * dx + dz * dz > kRingRadius * kRingRadius) continue;
                        const glm::ivec3 p = altar + glm::ivec3(dx, dy, dz);
                        if (level.GetBlock(p.x, p.y, p.z) != BlockID::ResonantChime) continue;
                        // Bearing clockwise from north (-Z) seen from above.
                        double b = std::atan2(static_cast<double>(dx), static_cast<double>(-dz));
                        if (b < 0.0) b += 6.283185307179586;
                        found.push_back({ p, b, dx * dx + dy * dy + dz * dz });
                    }
                }
            }
            std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) {
                if (std::abs(a.bearing - b.bearing) > 1.0e-9) return a.bearing < b.bearing;
                return a.dist2 < b.dist2;
            });
            std::vector<glm::ivec3> out;
            out.reserve(found.size());
            for (const Entry& e : found) out.push_back(e.pos);
            return out;
        }

        int RingIndexOf(const Song& s, const glm::ivec3& pos) {
            for (size_t i = 0; i < s.ring.size(); ++i) {
                if (s.ring[i] == pos) return static_cast<int>(i);
            }
            return -1;
        }

        void FlashRing(ILevelWrite& level, const Song& s) {
            for (size_t i = 0; i < s.ring.size(); ++i) Sound(level, s.ring[i], static_cast<int>(i));
        }

        glm::dvec3 AltarTop(const glm::ivec3& altar) {
            return glm::dvec3(altar.x + 0.5, altar.y + 1.1, altar.z + 0.5);
        }

        // Wrong note or out of time: the ring flashes, the shard comes back.
        void FailSong(ILevelWrite& level, const Song& s) {
            FlashRing(level, s);
            level.PlaySound(nullptr, s.altar, "obeycraft:block.choir_altar.fail", SoundSource::Blocks, 1.0f, 0.5f);
            if (s.refundShard) {
                if (EntityLevel* entities = level.Entities()) {
                    entities->SpawnItemDrop(AltarTop(s.altar), Items::EchoShard, 1);
                }
            }
            Log::Info("[Hush] The choir at (%d, %d, %d) falls silent", s.altar.x, s.altar.y,
                      s.altar.z);
        }

        bool MotherNear(EntityLevel& entities, const glm::ivec3& altar) {
            const glm::vec3 c(altar.x + 0.5f, altar.y + 0.5f, altar.z + 0.5f);
            std::vector<Entity*> nearby;
            entities.GetEntitiesInBox(AABB::FromMinMax(c - glm::vec3(48.0f), c + glm::vec3(48.0f)),
                                      nullptr, nearby);
            for (Entity* e : nearby) {
                if (e && !e->IsRemoved() && e->GetType() == EntityTypeId::ChoirMother) return true;
            }
            return false;
        }

        // The song is right: the Choir Mother rises over the altar.
        void SummonMother(ILevelWrite& level, const Song& s) {
            FlashRing(level, s);
            EntityLevel* entities = level.Entities();
            if (!entities) return;
            std::unique_ptr<Mob> mother = MakeGenericMob(EntityTypeId::ChoirMother, entities);
            if (!mother) {
                Log::Warning("[Hush] Choir solved at (%d, %d, %d) but the Choir Mother could "
                             "not be built", s.altar.x, s.altar.y, s.altar.z);
                return;
            }
            mother->position = glm::dvec3(s.altar.x + 0.5, s.altar.y + 1.0 + kMotherLift,
                                          s.altar.z + 0.5);
            if (JavaRandom* rng = level.Random()) {
                mother->yRot = mother->yBodyRot = mother->yHeadRot = rng->NextFloat() * 360.0f;
            }
            mother->FinalizeSpawn(SpawnReason::Triggered, nullptr);
            // The Choir Mother rises: a warden's emergence, pitched up
            // (obeycraft:entity.choir_mother.emerge → entity.warden.emerge).
            entities->PlaySound(nullptr, mother->position, "obeycraft:entity.choir_mother.emerge",
                                SoundSource::Hostile, 5.0f, 1.4f);
            Log::Info("[Hush] The choir at (%d, %d, %d) is answered: the Choir Mother rises",
                      s.altar.x, s.altar.y, s.altar.z);
            entities->AddFreshEntity(std::move(mother));
        }

        // ── resonant_chime ───────────────────────────────────────────────

        UseResult ChimeUseWithoutItem(ILevelWrite* world, const glm::ivec3& pos,
                                      IUsePlayer* player, const BlockHitResult& hit) {
            (void)hit;
            if (!world) return UseResult::Pass;
            // Block ticks and the song are server authority; the client just
            // swings (the lit state arrives with the server's write).
            if (world->IsClientSide()) return UseResult::Success;

            enum class Outcome { None, Wrong, Solved } outcome = Outcome::None;
            Song done;
            int ringIndex = -1;
            {
                std::lock_guard<std::mutex> lock(SongsMutex());
                const DimensionId dim = world->GetDimension();
                for (Song& s : Songs()) {
                    if (s.dim != dim) continue;
                    const int idx = RingIndexOf(s, pos);
                    if (idx < 0) continue;
                    ringIndex = idx;
                    // Striking while the altar is still singing is ignored —
                    // listen first.
                    if (s.singing) break;
                    if (idx == s.sequence[s.step]) {
                        if (++s.step >= static_cast<int>(s.sequence.size())) {
                            outcome = Outcome::Solved;
                            done = s;
                            EraseSong(dim, done.altar);   // `s` dies here
                        }
                    } else {
                        outcome = Outcome::Wrong;
                        done = s;
                        EraseSong(dim, done.altar);   // `s` dies here
                    }
                    break;
                }
            }

            Sound(*world, pos, ringIndex);
            // Aurelith's tuned cabinets listen to the chimes before them
            // (AurelithQuestBlocks.hpp): the struck chime's plinth is a note.
            Aurelith::OnChimeStruck(*world, pos);
            if (outcome == Outcome::Wrong) {
                FailSong(*world, done);
                if (player) player->DisplayClientMessage("A wrong note - the choir falls silent.", true);
            } else if (outcome == Outcome::Solved) {
                SummonMother(*world, done);
                if (player) player->DisplayClientMessage("The Choir Mother answers the song.", true);
            }
            return UseResult::Success;
        }

        UseResult ChimeUseItemOn(ItemStack& stack, ILevelWrite* world, const glm::ivec3& pos,
                                 IUsePlayer* player, uint32_t hand, const BlockHitResult& hit) {
            (void)stack; (void)world; (void)pos; (void)player; (void)hand; (void)hit;
            // Whatever is in hand, a click strikes the chime (sneak to place
            // against it instead — PlayerSession's suppressUsingBlock).
            return UseResult::TryEmptyHandInteraction;
        }

        void ChimeTick(ILevelWrite& level, const glm::ivec3& pos, BlockState state, JavaRandom&) {
            if (LitOf(state)) {
                level.SetBlock(pos.x, pos.y, pos.z, WithLit(state, false),
                               World::UpdateFlags::UpdateClients);
            }
        }

        void ChimeAnimateTick(EntityLevel& level, const glm::ivec3& pos, BlockState state,
                              JavaRandom& random) {
            if (!LitOf(state)) return;
            // A sounding chime sheds motes from every face.
            for (int i = 0; i < 6; ++i) {
                level.AddParticle(ParticleKind::HushMote,
                                  pos.x + random.NextDouble(), pos.y + random.NextDouble() * 1.2,
                                  pos.z + random.NextDouble(),
                                  (random.NextDouble() - 0.5) * 0.04, 0.03,
                                  (random.NextDouble() - 0.5) * 0.04);
            }
        }

        // ── choir_altar ──────────────────────────────────────────────────

        UseResult AltarUseItemOn(ItemStack& stack, ILevelWrite* world, const glm::ivec3& pos,
                                 IUsePlayer* player, uint32_t hand, const BlockHitResult& hit) {
            (void)hand; (void)hit;
            if (stack.IsEmpty() || stack.itemId != Items::EchoShard) {
                return UseResult::TryEmptyHandInteraction;
            }
            if (!world) return UseResult::Pass;
            if (world->IsClientSide()) return UseResult::Success;

            const auto say = [&](const char* text) {
                if (player) player->DisplayClientMessage(text, true);
            };
            EntityLevel* entities = world->Entities();
            if (!entities) return UseResult::Pass;
            const DimensionId dim = world->GetDimension();
            const int64_t now = world->GameTime();

            {
                std::lock_guard<std::mutex> lock(SongsMutex());
                if (Song* s = FindSong(dim, pos)) {
                    // A song whose block tick was lost (chunk unloaded) is
                    // dropped once stale; a live one keeps the altar busy.
                    if (now < s->expires) {
                        say("The choir is already singing.");
                        return UseResult::Consume;
                    }
                    EraseSong(dim, pos);
                }
            }
            if (MotherNear(*entities, pos)) {
                say("The Choir Mother is already here.");
                return UseResult::Consume;
            }
            std::vector<glm::ivec3> ring = GatherRing(*world, pos);
            if (static_cast<int>(ring.size()) < kMinChimes) {
                say("Too few chimes answer the altar.");
                return UseResult::Consume;
            }

            JavaRandom* rng = world->Random();
            Song song;
            song.dim = dim;
            song.altar = pos;
            song.ring = std::move(ring);
            const int n = static_cast<int>(song.ring.size());
            const int length = 4 + (rng ? rng->NextInt(3) : 1);
            int last = -1;
            for (int i = 0; i < length; ++i) {
                // No note twice in a row: a repeat reads as one long note.
                int k = rng ? rng->NextInt(n) : (i * 2) % n;
                if (k == last) k = (k + 1 + (rng ? rng->NextInt(n - 1) : 0)) % n;
                song.sequence.push_back(k);
                last = k;
            }
            song.singing = true;
            song.step = 0;
            song.refundShard = !(player && player->isCreative());
            // Singing + the answer window + slack; refreshed on each tick.
            song.expires = now + kLeadInTicks + length * kNoteInterval + 20 * (5 + 2 * length) + 200;

            // The altar keeps the shard while the choir sings.
            if (song.refundShard) {
                stack.count -= 1;
                if (stack.count <= 0) stack.Clear();
            }
            {
                std::lock_guard<std::mutex> lock(SongsMutex());
                Songs().push_back(std::move(song));
            }
            if (ScheduledTickAccess* ticks = world->Ticks()) {
                ticks->ScheduleTick(pos, BlockID::ChoirAltar, kLeadInTicks);
            }
            world->PlaySound(nullptr, pos, "obeycraft:block.choir_altar.sing", SoundSource::Blocks, 1.0f, 1.3f);
            say("The choir begins to sing. Listen...");
            return UseResult::Success;
        }

        UseResult AltarUseWithoutItem(ILevelWrite* world, const glm::ivec3& pos,
                                      IUsePlayer* player, const BlockHitResult& hit) {
            (void)pos; (void)hit;
            if (!world || world->IsClientSide()) return UseResult::Pass;
            if (player) player->DisplayClientMessage("The altar waits for an echo shard.", true);
            return UseResult::Consume;
        }

        void AltarTick(ILevelWrite& level, const glm::ivec3& pos, BlockState, JavaRandom&) {
            const DimensionId dim = level.GetDimension();
            const int64_t now = level.GameTime();
            int noteRing = -1;
            glm::ivec3 notePos(0);
            bool timedOut = false;
            Song done;
            int nextDelay = 0;
            {
                std::lock_guard<std::mutex> lock(SongsMutex());
                Song* s = FindSong(dim, pos);
                if (!s) return;
                if (s->singing) {
                    noteRing = s->sequence[s->step];
                    notePos = s->ring[noteRing];
                    if (++s->step >= static_cast<int>(s->sequence.size())) {
                        // Sung: now the player's turn.
                        s->singing = false;
                        s->step = 0;
                        s->deadline = now + 20 * (5 + 2 * static_cast<int64_t>(s->sequence.size()));
                        nextDelay = 20;
                    } else {
                        nextDelay = kNoteInterval;
                    }
                    s->expires = std::max(s->expires, now + 600);
                } else if (now >= s->deadline) {
                    timedOut = true;
                    done = *s;
                    EraseSong(dim, pos);
                } else {
                    nextDelay = 20;
                }
            }
            if (noteRing >= 0) Sound(level, notePos, noteRing);
            if (timedOut) {
                FailSong(level, done);
                return;
            }
            if (nextDelay > 0) {
                if (ScheduledTickAccess* ticks = level.Ticks()) {
                    ticks->ScheduleTick(pos, BlockID::ChoirAltar, nextDelay);
                }
            }
        }

    } // namespace

    void BlockRegistry_RegisterChoirBlocks(std::array<Block, BlockRegistry::Size>& blocks) {
        Block& chime = blocks[static_cast<size_t>(BlockID::ResonantChime)];
        chime.useWithoutItem = &ChimeUseWithoutItem;
        chime.useItemOn      = &ChimeUseItemOn;
        chime.tick           = &ChimeTick;
        chime.animateTick    = &ChimeAnimateTick;
        chime.emissive       = true;

        Block& altar = blocks[static_cast<size_t>(BlockID::ChoirAltar)];
        altar.useItemOn      = &AltarUseItemOn;
        altar.useWithoutItem = &AltarUseWithoutItem;
        altar.tick           = &AltarTick;
        altar.emissive       = true;
    }

} // namespace Game
