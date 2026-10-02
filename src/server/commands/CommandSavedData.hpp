// File: src/server/commands/CommandSavedData.hpp
//
// Two of MC's server SavedData that only commands touch, saved the way
// vanilla saves them into the overworld's data folder:
//
//   Stopwatches     (MC net.minecraft.world.Stopwatches)      data/stopwatches.dat
//       {data:{stopwatches:{<id>: <elapsed ms as long>}}}
//       A stopwatch runs on the wall clock (Util.getMillis), and keeps
//       running while the world is closed only in the sense that the saved
//       elapsed time is resumed from on load — MC's pack/unpack.
//   RandomSequences (MC net.minecraft.world.RandomSequences)  data/random_sequences.dat
//       {data:{salt, include_world_seed, include_sequence_id,
//              sequences:{<id>:{source:[L; seedLo, seedHi]}}}}
//       Named Xoroshiro128++ streams /random draws from; created on first
//       use from the world seed, the salt and the id (RandomSequence).
//
// Loaded lazily on first use, written with the world (autosave and
// shutdown, next to CommandStorage), forgotten on Close.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace minecraft { class XoroshiroRandomSource; }

namespace Server::CommandSavedData {

    // ── Stopwatches ─────────────────────────────────────────────────────────
    // MC Stopwatches.currentTime: milliseconds on a monotonic clock.
    int64_t StopwatchNow();
    // add: false when `id` already exists.
    bool AddStopwatch(const std::string& id);
    // The elapsed seconds, or nullopt when `id` does not exist.
    std::optional<double> StopwatchElapsedSeconds(const std::string& id);
    // update(id, now): restart from zero; false when `id` does not exist.
    bool RestartStopwatch(const std::string& id);
    bool RemoveStopwatch(const std::string& id);
    std::vector<std::string> StopwatchIds();

    // ── Random sequences ────────────────────────────────────────────────────
    // RandomSequences.get(id, worldSeed): the stream, created on first use.
    minecraft::XoroshiroRandomSource& RandomSequence(const std::string& id, int64_t worldSeed);
    // reset(id, worldSeed): a fresh stream from the current defaults.
    void ResetRandomSequence(const std::string& id, int64_t worldSeed);
    // reset(id, worldSeed, salt, includeWorldSeed, includeSequenceId).
    void ResetRandomSequence(const std::string& id, int64_t worldSeed, int salt,
                             bool includeWorldSeed, bool includeSequenceId);
    // setSeedDefaults — what streams created from now on are seeded with.
    void SetRandomSequenceDefaults(int salt, bool includeWorldSeed, bool includeSequenceId);
    // clear: drops every stream; how many there were.
    int ClearRandomSequences();
    std::vector<std::string> RandomSequenceIds();

    // Write what changed (the world save) / write and forget (world close).
    void Save();
    void Close();

} // namespace Server::CommandSavedData
