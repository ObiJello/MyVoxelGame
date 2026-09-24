// File: src/client/renderer/gui/debug/DebugScreenEntries.hpp
//
// The F3 screen's entry registry — a port of MC 26.3's
// client/gui/components/debug package (DebugScreenEntries, DebugScreenEntry,
// DebugScreenEntryList, DebugScreenEntryStatus, DebugScreenProfile,
// DebugScreenDisplayer, DebugEntryCategory).
//
//   • Every line group on the F3 screen and every debug renderer is an
//     ENTRY with an id ("fps", "chunk_borders", "visualize_water_levels").
//   • Each entry has a STATUS: always on, only while the overlay is up, or
//     never. F3 toggles the overlay; F3+F6 edits the statuses; F3+B/G flip
//     the hitbox / chunk-border entries the way MC's toggleStatus does.
//   • The statuses persist to <gamedir>/debug-profile.json in vanilla's
//     shape: {"profile":"default"} or {"custom":{"minecraft:fps":"alwaysOn"}}.
//
// The entries themselves (what each one prints) live in DebugEntries.cpp.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Game { class ClientPlayer; class ClientPlayerController; }

namespace Render {

    class Camera;

    namespace DebugScreen {

        enum class EntryStatus : uint8_t { AlwaysOn, InOverlay, Never };
        enum class Profile     : uint8_t { Default, Performance };
        // MC DebugEntryCategory: sortKey 1 = screen text, 2 = renderers.
        enum class Category    : uint8_t { ScreenText = 1, Renderer = 2 };

        const char* StatusName(EntryStatus s);        // "alwaysOn" / "inOverlay" / "never"
        bool        StatusFromName(std::string_view name, EntryStatus& out);
        const char* ProfileName(Profile p);           // "default" / "performance"
        const char* ProfileLabel(Profile p);          // "Default profile" / "Performance profile"
        bool        ProfileFromName(std::string_view name, Profile& out);
        const char* CategoryLabel(Category c);        // "Debug Screen Text" / "Debug Renderers"

        // What an entry writes into. MC DebugScreenDisplayer: priority lines
        // alternate left/right at the top, plain lines are split half and
        // half below them, groups keep their lines together and are dealt
        // out left/right in insertion order.
        class Displayer {
        public:
            virtual ~Displayer() = default;
            virtual void AddPriorityLine(std::string line) = 0;
            virtual void AddLine(std::string line) = 0;
            virtual void AddToGroup(const std::string& group, std::vector<std::string> lines) = 0;
            virtual void AddToGroup(const std::string& group, std::string line) = 0;
        };

        // Per-frame facts an entry may need that live in PlatformMain's frame
        // loop rather than behind a global (MC reads them off Minecraft.getInstance()).
        struct Context {
            const Game::ClientPlayer*           player     = nullptr;
            const Game::ClientPlayerController* controller = nullptr;
            const Render::Camera*               camera     = nullptr;
            int   fps = 0;
            int   windowWidth = 0, windowHeight = 0;
            int   framebufferWidth = 0, framebufferHeight = 0;
            int   refreshRate = 0;          // 0 = unknown
            bool  vsync = false;
            int   maxFps = 0;               // 0 or >= 260 = unlimited
            bool  isRemoteClient = false;
            std::string serverBrand;        // remote server's brand ("MyVoxelGame")
            double gpuUtilization = -1.0;   // < 0 = not measured this frame
            int   effectiveRenderDistance = 0;
            int   simulationDistance = 0;
            int   entitiesRendered = 0;     // this frame, across the entity renderers
            float avgSentPackets = 0.0f;    // per second (Connection.averageSentPackets)
            float avgReceivedPackets = 0.0f;
            bool  freeCamActive = false;
        };

        class Entry {
        public:
            virtual ~Entry() = default;
            virtual void Display(Displayer& out, const Context& ctx) = 0;
            // MC DebugScreenEntry.isAllowed(reducedDebugInfo): most entries
            // hide under "Reduced Debug Info"; the ones vanilla keeps override.
            virtual bool IsAllowed(bool reducedDebugInfo) const { return !reducedDebugInfo; }
            virtual Category GetCategory() const { return Category::ScreenText; }
        };

        // Entry that prints nothing — a renderer toggle (MC DebugEntryNoop).
        class NoopEntry : public Entry {
        public:
            explicit NoopEntry(bool allowedWithReducedInfo = false) : m_allowed(allowedWithReducedInfo) {}
            void Display(Displayer&, const Context&) override {}
            bool IsAllowed(bool reducedDebugInfo) const override { return m_allowed || !reducedDebugInfo; }
            Category GetCategory() const override { return Category::Renderer; }
        private:
            bool m_allowed;
        };

        // MC DebugScreenEntries' registered ids (paths; the namespace is
        // always "minecraft" and is added when written to disk).
        namespace Ids {
            inline constexpr const char* GameVersion            = "game_version";
            inline constexpr const char* Fps                    = "fps";
            inline constexpr const char* Tps                    = "tps";
            inline constexpr const char* Memory                 = "memory";
            inline constexpr const char* DetailedMemory         = "detailed_memory";
            inline constexpr const char* SystemSpecs            = "system_specs";
            inline constexpr const char* LookingAtBlockState    = "looking_at_block_state";
            inline constexpr const char* LookingAtBlockTags     = "looking_at_block_tags";
            inline constexpr const char* LookingAtFluidState    = "looking_at_fluid_state";
            inline constexpr const char* LookingAtFluidTags     = "looking_at_fluid_tags";
            inline constexpr const char* LookingAtEntity        = "looking_at_entity";
            inline constexpr const char* LookingAtEntityTags    = "looking_at_entity_tags";
            inline constexpr const char* ChunkRenderStats       = "chunk_render_stats";
            inline constexpr const char* ChunkGenerationStats   = "chunk_generation_stats";
            inline constexpr const char* EntityRenderStats      = "entity_render_stats";
            inline constexpr const char* ParticleRenderStats    = "particle_render_stats";
            inline constexpr const char* ChunkSourceStats       = "chunk_source_stats";
            inline constexpr const char* PlayerPosition         = "player_position";
            inline constexpr const char* PlayerSectionPosition  = "player_section_position";
            inline constexpr const char* PlayerSpeed            = "player_speed";
            // Not vanilla: the block position alone ("x y z"), for people who
            // want a coordinate readout without the whole F3 screen. Off by
            // default; set it to Always in F3+F6.
            inline constexpr const char* Coordinates            = "coordinates";
            inline constexpr const char* LightLevels            = "light_levels";
            inline constexpr const char* Heightmap              = "heightmap";
            inline constexpr const char* Biome                  = "biome";
            inline constexpr const char* LocalDifficulty        = "local_difficulty";
            inline constexpr const char* DayCount               = "day_count";
            inline constexpr const char* EntitySpawnCounts      = "entity_spawn_counts";
            inline constexpr const char* SoundMood              = "sound_mood";
            inline constexpr const char* SoundCache             = "sound_cache";
            inline constexpr const char* PostEffects            = "post_effects";
            inline constexpr const char* EntityHitboxes         = "entity_hitboxes";
            inline constexpr const char* ChunkBorders           = "chunk_borders";
            inline constexpr const char* ThreeDimensionalCrosshair = "3d_crosshair";
            inline constexpr const char* ChunkSectionPaths      = "chunk_section_paths";
            inline constexpr const char* GpuUtilization         = "gpu_utilization";
            inline constexpr const char* SimplePerformanceImpactors = "simple_performance_impactors";
            inline constexpr const char* ChunkSectionOctree     = "chunk_section_octree";
            inline constexpr const char* VisualizeWaterLevels   = "visualize_water_levels";
            inline constexpr const char* VisualizeHeightmap     = "visualize_heightmap";
            inline constexpr const char* VisualizeCollisionBoxes = "visualize_collision_boxes";
            inline constexpr const char* VisualizeEntitySupportingBlocks = "visualize_entity_supporting_blocks";
            inline constexpr const char* VisualizeBlockLightLevels = "visualize_block_light_levels";
            inline constexpr const char* VisualizeSkyLightLevels = "visualize_sky_light_levels";
            inline constexpr const char* VisualizeSolidFaces    = "visualize_solid_faces";
            inline constexpr const char* VisualizeChunksOnServer = "visualize_chunks_on_server";
            inline constexpr const char* VisualizeSkyLightSections = "visualize_sky_light_sections";
            inline constexpr const char* ChunkSectionVisibility = "chunk_section_visibility";
        }

        // The registry (MC DebugScreenEntries.ENTRIES_BY_ID). Ordered by id,
        // which is also the display order.
        const std::map<std::string, std::unique_ptr<Entry>>& AllEntries();
        Entry* GetEntry(std::string_view id);
        // The two built-in profiles' status tables.
        const std::map<std::string, EntryStatus>& ProfileStatuses(Profile p);

        // MC DebugScreenEntryList — the live statuses + the overlay flag.
        class EntryList {
        public:
            EntryList();

            void Load();                                    // from debug-profile.json (or the default profile)
            void Save() const;
            void LoadProfile(Profile p);

            EntryStatus GetStatus(std::string_view id) const;
            bool IsCurrentlyEnabled(std::string_view id) const;
            void SetStatus(std::string_view id, EntryStatus status);
            // MC toggleStatus: the F3+B / F3+G cycle. Returns the new "shown" state.
            bool ToggleStatus(std::string_view id);
            const std::vector<std::string>& GetCurrentlyEnabled() const { return m_currentlyEnabled; }

            void ToggleDebugOverlay() { SetOverlayVisible(!m_overlayVisible); }
            void SetOverlayVisible(bool visible);
            bool IsOverlayVisible() const { return m_overlayVisible; }

            void RebuildCurrentList();
            uint64_t GetCurrentlyEnabledVersion() const { return m_version; }
            bool IsUsingProfile(Profile p) const { return m_hasProfile && m_profile == p; }

            // "Reduced Debug Info" — the options.txt flag. MC also honours the
            // server's reducedDebugInfo game rule; this engine has no such rule.
            static bool ShowOnlyReducedInfo();

        private:
            std::string ProfileFilePath() const;
            void ResetToProfile(Profile p);

            std::map<std::string, EntryStatus> m_statuses;
            std::vector<std::string> m_currentlyEnabled;
            bool m_overlayVisible = false;
            bool m_hasProfile = false;
            Profile m_profile = Profile::Default;
            uint64_t m_version = 0;
        };

        EntryList& Entries();   // the one instance (MC Minecraft.debugEntries)

    } // namespace DebugScreen
} // namespace Render
