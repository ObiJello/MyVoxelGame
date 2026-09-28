// File: src/server/world/storage/anvil/TrialChamberNbt.hpp
//
// The trial chambers' block entities in NBT — MC TrialSpawner.load / store
// (TrialSpawnerStateData.Packed.MAP_CODEC + TrialSpawner.FullConfig.MAP_CODEC)
// and VaultBlockEntity.loadAdditional / saveAdditional (VaultConfig,
// VaultServerData, VaultSharedData codecs):
//
//   trial_spawner: { registered_players?: [UUID...], current_mobs?: [UUID...],
//                    cooldown_ends_at?: long, next_mob_spawns_at?: long,
//                    total_mobs_spawned?: int, spawn_data?: <SpawnData>,
//                    ejecting_loot_table?: "…",
//                    normal_config?: "id" | {…}, ominous_config?: "id" | {…},
//                    target_cooldown_length?: int, required_player_range?: int }
//   vault:         { config: {loot_table?, activation_range?,
//                             deactivation_range?, key_item?,
//                             override_loot_table_to_display?},
//                    shared_data: {display_item?, connected_players?,
//                                  connected_particles_range?},
//                    server_data: {rewarded_players?, state_updating_resumes_at?,
//                                  items_to_eject?, total_ejections_needed?} }
//
// Optional fields at their codec default are omitted on write, as the Java
// codecs do. Also installs the trial spawner's server hooks.
#pragma once

#include "common/nbt/NbtWrite.hpp"
#include "server/world/storage/NBTParser.hpp"

namespace Game {
    class TrialSpawnerBlockEntity;
    class VaultBlockEntity;
}

namespace Game::Anvil {

    void ReadTrialSpawner(const ::World::NBTTagCompound& tag, TrialSpawnerBlockEntity& spawner);
    void WriteTrialSpawner(Nbt::Writer& w, const TrialSpawnerBlockEntity& spawner);

    void ReadVault(const ::World::NBTTagCompound& tag, VaultBlockEntity& vault);
    void WriteVault(Nbt::Writer& w, const VaultBlockEntity& vault);

    // TrialSpawnerServerHooks (IntegratedServer startup, beside the monster
    // spawner's).
    void InstallTrialChamberServerHooks();

} // namespace Game::Anvil
