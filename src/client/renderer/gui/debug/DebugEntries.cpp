// File: src/client/renderer/gui/debug/DebugEntries.cpp
//
// The text entries of the F3 screen — MC 26.3's DebugEntry* classes
// (client/gui/components/debug), each reading this engine's equivalent of
// what vanilla reads. Where the engine has no equivalent the line says so
// honestly rather than inventing a number (no light engine → block light
// is 0).
#include "DebugScreenEntries.hpp"

#include "client/entity/Player.hpp"
#include "client/entity/ClientMobManager.hpp"
#include "client/entity/RemotePlayerManager.hpp"
#include "client/entity/ItemEntityManager.hpp"
#include "client/entity/XpOrbManager.hpp"
#include "client/entity/ClientFallingBlocks.hpp"
#include "client/input/PlayerController.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/renderer/core/Camera.hpp"
#include "client/renderer/backend/RenderBackend.hpp"
#include "client/renderer/environment/EnvironmentState.hpp"
#include "client/sound/AmbientSoundHandlers.hpp"
#include "client/sound/SoundManager.hpp"
#include "client/renderer/gui/screens/Screen.hpp"
#include "client/renderer/mesh/ChunkRenderer.hpp"
#include "client/renderer/mesh/ClientMeshManager.hpp"
#include "client/renderer/mesh/MeshUploadPermits.hpp"
#include "client/renderer/particle/MobParticleSystem.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "client/world/ClientChunkManager.hpp"
#include "client/world/ClientLevel.hpp"
#include "common/core/HardwareProfile.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"
#include "common/entity/Mob.hpp"
#include "common/entity/MobCategory.hpp"
#include "common/physics/RayCast.hpp"
#include "common/world/biome/Biomes.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/world/block/Direction.hpp"
#include "common/world/chunk/Chunk.hpp"
#include "common/world/chunk/Heightmap.hpp"
#include "common/world/level/BlockClip.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/math/WorldCoordinates.hpp"
#include "common/world/tags/DataTags.hpp"
#include "platform/GameDirectory.hpp"
#include "server/IntegratedServer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#else
#include <unistd.h>
#endif

namespace Render::DebugScreen {

    // ── Shared helpers ──────────────────────────────────────────────────

    namespace {

        template <typename... Args>
        std::string Fmt(const char* fmt, Args... args) {
            char buf[512];
            std::snprintf(buf, sizeof(buf), fmt, args...);
            return buf;
        }

        // MC ChatFormatting codes, UTF-8 "§".
        const char* kUnderline = "\xC2\xA7n";
        const char* kGreen     = "\xC2\xA7" "a";
        const char* kRed       = "\xC2\xA7" "c";

        glm::ivec3 FeetBlock(const Context& ctx) {
            const glm::vec3 p = ctx.player->physics.position;
            return { static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                     static_cast<int>(std::floor(p.z)) };
        }

        Server::IntegratedServer::DebugSample ServerSample() {
            if (Server::g_integratedServer) return Server::g_integratedServer->GetDebugSample();
            return {};
        }

        // MC Entity.pick(20, 0, withFluids): the block the camera looks at
        // within 20 blocks; with fluids, the first fluid cell on the way wins.
        struct Pick { bool hit = false; glm::ivec3 pos{0}; };

        Pick PickBlock(const Context& ctx, bool withFluids) {
            Pick out;
            if (!ctx.player || !ctx.camera) return out;
            const glm::dvec3 eye = ctx.player->GetEyePosition();
            const glm::vec3 dir = ctx.camera->GetForward();
            float range = 20.0f;
            const auto hit = Game::Raycast::CastRay(eye, dir, range);
            if (hit) { out.hit = true; out.pos = hit->blockPos; range = std::min(range, hit->distance); }
            if (withFluids && Client::g_clientBlockAccess) {
                glm::ivec3 cell;
                const glm::dvec3 from(eye);
                const glm::dvec3 to = from + glm::dvec3(dir) * static_cast<double>(range);
                const bool found = Game::TraverseBlocks(from, to,
                    [&](const glm::ivec3& c, const glm::dvec3&, const glm::dvec3&) {
                        return Client::g_clientBlockAccess->IsBlockFluid(c.x, c.y, c.z) ||
                               Client::g_clientBlockAccess->ContainsWater(c.x, c.y, c.z);
                    }, &cell);
                if (found) { out.hit = true; out.pos = cell; }
            }
            return out;
        }

        std::string BlockRegistryName(Game::BlockID id) {
            return "minecraft:" + Game::BlockRegistry::Get(id).registrySlug;
        }

        // MC DebugEntryLookingAtState.addStateProperties: "name: value",
        // booleans coloured green/red.
        void AddStateProperties(std::vector<std::string>& out, Game::BlockState state) {
            const Game::BlockID id = state.Block();
            const uint16_t n = Game::BlockStates::PropertyCount(id);
            for (uint16_t slot = 0; slot < n; ++slot) {
                const Game::PropertyId prop = Game::BlockStates::PropertyAt(id, slot);
                const std::string value(state.GetName(prop));
                std::string shown = value;
                if (value == "true")       shown = std::string(kGreen) + value;
                else if (value == "false") shown = std::string(kRed) + value;
                out.push_back(std::string(Game::BlockStates::PropertyName(prop)) + ": " + shown);
            }
        }

        // The fluid a block cell holds, MC FluidState style: the still fluid
        // for a source, the flowing one with `level`/`falling` otherwise,
        // "minecraft:empty" for none. Water is a block with a level property
        // here (0 = source, 1-7 flowing, 8+ falling), waterlogging counts as
        // a source.
        struct FluidInfo { std::string name; std::vector<std::string> props; std::string tagId; bool water = false, lava = false; };

        FluidInfo FluidAt(const glm::ivec3& p) {
            FluidInfo f;
            f.name = "minecraft:empty";
            if (!Client::g_clientBlockAccess) return f;
            const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(p.x, p.y, p.z);
            const Game::BlockID id = state.Block();
            const bool isWater = id == Game::BlockID::Water;
            const bool isLava  = id == Game::BlockID::Lava;
            if (!isWater && !isLava) {
                if (Client::g_clientBlockAccess->ContainsWater(p.x, p.y, p.z)) {
                    f.water = true; f.name = "minecraft:water"; f.tagId = "minecraft:water";
                    f.props.push_back(std::string("falling: ") + kRed + "false");
                }
                return f;
            }
            int level = 0;
            const std::string_view lv = state.GetValueByName("level");
            if (!lv.empty()) level = std::atoi(std::string(lv).c_str());
            f.water = isWater; f.lava = isLava;
            const char* still = isWater ? "water" : "lava";
            const char* flowing = isWater ? "flowing_water" : "flowing_lava";
            f.tagId = std::string("minecraft:") + still;
            if (level == 0) {
                f.name = std::string("minecraft:") + still;
                f.props.push_back(std::string("falling: ") + kRed + "false");
            } else {
                f.name = std::string("minecraft:") + flowing;
                const bool falling = level >= 8;
                f.props.push_back(std::string("falling: ") + (falling ? kGreen : kRed) + (falling ? "true" : "false"));
                f.props.push_back("level: " + std::to_string(falling ? 8 : 8 - level));
            }
            // Tags are keyed on the still fluid AND the flowing one in MC's
            // data (#minecraft:water lists both); the flowing id is what the
            // state carries, so look up by that.
            f.tagId = f.name;
            return f;
        }

        // The entity under the crosshair (MC Minecraft.crosshairPickEntity):
        // its registry name, or empty. Mobs and other players are the only
        // pickable entities here.
        std::string CrosshairEntityName(const Context& ctx) {
            if (!ctx.controller) return {};
            const int32_t id = ctx.controller->PickEntity();
            if (id == 0) return {};
            if (Client::ClientLevels::HasSession()) {
                if (Client::ClientMobManager* mobs = Client::ClientLevels::Active().Mobs()) {
                    auto it = mobs->All().find(id);
                    if (it != mobs->All().end() && it->second.mob) {
                        return "minecraft:" + std::string(Game::GetEntityTypeInfo(it->second.mob->GetType()).slug);
                    }
                }
            }
            if (Client::g_remotePlayerManager &&
                Client::g_remotePlayerManager->GetPlayers().count(static_cast<uint32_t>(id))) {
                return "minecraft:player";
            }
            return {};
        }

        // ── Process memory / CPU, per platform ──────────────────────────
        struct ProcessMemory { uint64_t resident = 0, peakResident = 0, virtualSize = 0; };

        ProcessMemory ReadProcessMemory() {
            ProcessMemory m;
#if defined(__APPLE__)
            mach_task_basic_info info{};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
                m.resident = info.resident_size;
                m.peakResident = info.resident_size_max;
                m.virtualSize = info.virtual_size;
            }
#elif defined(_WIN32)
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                m.resident = pmc.WorkingSetSize;
                m.peakResident = pmc.PeakWorkingSetSize;
                m.virtualSize = pmc.PrivateUsage;
            }
#else
            if (std::ifstream in("/proc/self/statm"); in) {
                uint64_t pages = 0, resident = 0;
                in >> pages >> resident;
                const long pageSize = sysconf(_SC_PAGESIZE);
                m.virtualSize = pages * static_cast<uint64_t>(pageSize);
                m.resident = resident * static_cast<uint64_t>(pageSize);
            }
            if (std::ifstream st("/proc/self/status"); st) {
                std::string line;
                while (std::getline(st, line)) {
                    if (line.rfind("VmHWM:", 0) == 0) { m.peakResident = std::strtoull(line.c_str() + 6, nullptr, 10) * 1024; break; }
                }
            }
#endif
            return m;
        }

        // MC DebugEntrySystemSpecs.getCpuInfo: "<n>x <brand>", whitespace collapsed.
        const std::string& CpuInfo() {
            static std::string s_info;
            if (!s_info.empty()) return s_info;
            std::string brand;
#if defined(__APPLE__)
            char buf[256] = {};
            size_t len = sizeof(buf);
            if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) brand = buf;
#elif defined(_WIN32)
            int regs[4] = {};
            char b[49] = {};
            __cpuid(regs, 0x80000000);
            if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
                for (unsigned i = 0; i < 3; ++i) {
                    __cpuid(regs, static_cast<int>(0x80000002u + i));
                    std::memcpy(b + i * 16, regs, 16);
                }
                brand = b;
            }
#else
            if (std::ifstream in("/proc/cpuinfo"); in) {
                std::string line;
                while (std::getline(in, line)) {
                    if (line.rfind("model name", 0) == 0) {
                        const size_t colon = line.find(':');
                        if (colon != std::string::npos) brand = line.substr(colon + 1);
                        break;
                    }
                }
            }
#endif
            if (brand.empty()) brand = "<unknown>";
            std::string collapsed;
            bool space = true;
            for (char c : brand) {
                if (std::isspace(static_cast<unsigned char>(c))) { if (!space) collapsed += ' '; space = true; }
                else { collapsed += c; space = false; }
            }
            while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
            s_info = Fmt("%zux %s", Core::HardwareProfile::Get().logicalCores, collapsed.c_str());
            return s_info;
        }

        long long ToMiB(uint64_t bytes) { return static_cast<long long>(bytes / 1024 / 1024); }

        // MC Level.getMoonBrightness: DimensionType.MOON_BRIGHTNESS_PER_PHASE[phase].
        float MoonBrightness(int64_t dayTime) {
            static const float kTable[8] = { 1.0f, 0.75f, 0.5f, 0.25f, 0.0f, 0.25f, 0.5f, 0.75f };
            const int phase = static_cast<int>((dayTime / 24000 % 8 + 8) % 8);
            return kTable[phase];
        }

        // MC DifficultyInstance.calculateDifficulty.
        float EffectiveDifficulty(int base, int64_t totalGameTime, int64_t localGameTime, float moonBrightness) {
            if (base == 0) return 0.0f;
            const bool hard = base == 3;
            float scale = 0.75f;
            const float globalScale = std::clamp((static_cast<float>(totalGameTime) - 72000.0f) / 1440000.0f, 0.0f, 1.0f) * 0.25f;
            scale += globalScale;
            float localScale = std::clamp(static_cast<float>(localGameTime) / 3600000.0f, 0.0f, 1.0f) * (hard ? 1.0f : 0.75f);
            localScale += std::clamp(moonBrightness * 0.25f, 0.0f, globalScale);
            if (base == 1) localScale *= 0.5f;
            scale += localScale;
            return static_cast<float>(base) * scale;
        }

        float SpecialMultiplier(float effective) {
            if (effective < 2.0f) return 0.0f;
            return effective > 4.0f ? 1.0f : (effective - 2.0f) / 2.0f;
        }

        const char* kGroupMemory        = "memory";
        const char* kGroupSystem        = "system";
        const char* kGroupBlock         = "looking_at_block";
        const char* kGroupFluid         = "looking_at_fluid";
        const char* kGroupEntity        = "looking_at_entity";
        const char* kGroupChunkGen      = "chunk_generation";
        const char* kGroupPosition      = "position";
        const char* kGroupLight         = "light";
        const char* kGroupHeightmaps    = "heightmaps";
    }

    // ── Entries ─────────────────────────────────────────────────────────

    // "MyVoxelGame 0.1.42 (0.1.42/vanilla)" — MC prints the version, the
    // launched version and the client brand.
    class VersionEntry : public Entry {
    public:
        void Display(Displayer& out, const Context&) override {
            std::string v = Render::GetScreenManager().GetVersionString();   // "MyVoxelGame 0.1.N"
            std::string number = v;
            if (const size_t sp = v.rfind(' '); sp != std::string::npos) number = v.substr(sp + 1);
            out.AddPriorityLine(v + " (" + number + "/vanilla)");
        }
        bool IsAllowed(bool) const override { return true; }
    };

    class FpsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            const bool unlimited = ctx.maxFps <= 0 || ctx.maxFps >= 260;
            const std::string limit = unlimited ? "inf" : std::to_string(ctx.maxFps);
            const char* present = ctx.vsync ? " (fifo)" : " (immediate)";
            out.AddPriorityLine(Fmt("%d fps T: %s%s @%dHz", ctx.fps, limit.c_str(), present, ctx.refreshRate));
        }
        bool IsAllowed(bool) const override { return true; }
    };

    class TpsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!Client::g_networkClient || !Client::g_networkClient->IsConnected()) return;
            const auto s = ServerSample();
            std::string runStatus;
            if (s.valid) {
                if (s.stepping) runStatus = " (frozen - stepping)";
                else if (s.frozen) runStatus = " (frozen)";
            }
            if (!ctx.isRemoteClient && s.valid) {
                if (s.sprinting) runStatus = " (sprinting)";
                const std::string target = s.sprinting ? "-" : Fmt("%.1f", s.msPerTick);
                out.AddLine(Fmt("Integrated server @ %.1f/%s ms%s, %.0f tx, %.0f rx",
                                s.smoothedTickMs, target.c_str(), runStatus.c_str(),
                                ctx.avgSentPackets, ctx.avgReceivedPackets));
            } else {
                out.AddLine(Fmt("\"%s\" server%s, %.0f tx, %.0f rx", ctx.serverBrand.c_str(),
                                runStatus.c_str(), ctx.avgSentPackets, ctx.avgReceivedPackets));
            }
        }
        bool IsAllowed(bool) const override { return true; }
    };

    // MC DebugEntryMemory, on the process instead of a JVM heap: resident
    // set against physical RAM, the resident growth rate, and the peak.
    class MemoryEntry : public Entry {
    public:
        void Display(Displayer& out, const Context&) override {
            const ProcessMemory m = ReadProcessMemory();
            const uint64_t max = std::max<uint64_t>(1, Core::HardwareProfile::Get().physicalMemoryBytes);
            out.AddToGroup(kGroupMemory, std::vector<std::string>{
                Fmt("Mem: %2lld%% %03lld/%03lldMiB", static_cast<long long>(m.resident * 100 / max), ToMiB(m.resident), ToMiB(max)),
                Fmt("Allocation rate: %03lldMiB/s", ToMiB(static_cast<uint64_t>(std::max<int64_t>(0, RatePerSecond(m.resident))))),
                Fmt("Allocated: %2lld%% %03lldMiB", static_cast<long long>(m.peakResident * 100 / max), ToMiB(m.peakResident)) });
        }
        bool IsAllowed(bool) const override { return true; }
    private:
        // MC AllocationRateCalculator: re-sampled every 500 ms.
        int64_t RatePerSecond(uint64_t resident) {
            const auto now = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTime).count();
            if (m_lastUsage != 0 && ms < 500) return m_lastRate;
            if (m_lastUsage != 0 && ms > 0) {
                m_lastRate = static_cast<int64_t>((static_cast<double>(resident) - static_cast<double>(m_lastUsage)) * 1000.0 / static_cast<double>(ms));
            }
            m_lastTime = now;
            m_lastUsage = resident;
            return m_lastRate;
        }
        std::chrono::steady_clock::time_point m_lastTime = std::chrono::steady_clock::now();
        uint64_t m_lastUsage = 0;
        int64_t m_lastRate = 0;
    };

    // MC prints the JVM's heap and non-heap pools; the native equivalents
    // are the process and the GPU.
    class DetailedMemoryEntry : public Entry {
    public:
        void Display(Displayer& out, const Context&) override {
            const ProcessMemory m = ReadProcessMemory();
            std::vector<std::string> lines;
            lines.push_back(Fmt("Memory (process): r=%03lldMiB p=%03lldMiB v=%03lldMiB",
                                ToMiB(m.resident), ToMiB(m.peakResident), ToMiB(m.virtualSize)));
            if (Render::g_renderBackend) {
                const GPUMemoryStats g = Render::g_renderBackend->GetMemoryStats();
                lines.push_back(Fmt("Memory (gpu): u=%03lldMiB b=%03lldMiB t=%03lldMiB p=%03lldMiB",
                                    ToMiB(g.totalAllocated), ToMiB(g.bufferMemory), ToMiB(g.textureMemory), ToMiB(g.peakUsage)));
            }
            out.AddToGroup(kGroupMemory, std::move(lines));
        }
        bool IsAllowed(bool) const override { return true; }
    };

    class SystemSpecsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            GpuDeviceInfo dev;
            if (Render::g_renderBackend) dev = Render::g_renderBackend->GetDeviceInfo();
            const char* type = "";
            switch (dev.type) {
                case GpuDeviceInfo::Type::Integrated: type = " (iGPU)"; break;
                case GpuDeviceInfo::Type::Discrete:   type = " (dGPU)"; break;
                case GpuDeviceInfo::Type::Virtual:    type = " (vGPU)"; break;
                case GpuDeviceInfo::Type::Cpu:        type = " (software)"; break;
                default: break;
            }
            std::string driver = dev.driverInfo;
            if (const size_t nl = driver.find('\n'); nl != std::string::npos) driver.resize(nl);
            out.AddToGroup(kGroupSystem, std::vector<std::string>{
                Fmt("Build: %s %s", CompilerName(), BuildConfig()),
                Fmt("CPU: %s", CpuInfo().c_str()),
                Fmt("Display: %dx%d (%s)", ctx.framebufferWidth, ctx.framebufferHeight, dev.vendorName.c_str()),
                Fmt("%s%s", dev.name.c_str(), type),
                Fmt("%s %s", dev.backendName.c_str(), driver.c_str()) });
        }
        bool IsAllowed(bool) const override { return true; }
    private:
        static const char* CompilerName() {
#if defined(__clang__)
            return "clang " __clang_version__;
#elif defined(_MSC_VER)
            return "msvc";
#elif defined(__GNUC__)
            return "gcc " __VERSION__;
#else
            return "c++";
#endif
        }
        static const char* BuildConfig() {
#ifdef NDEBUG
            return "release";
#else
            return "debug";
#endif
        }
    };

    class LookingAtBlockStateEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            std::vector<std::string> lines;
            const Pick p = PickBlock(ctx, false);
            if (p.hit && Client::g_clientBlockAccess) {
                const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(p.pos.x, p.pos.y, p.pos.z);
                lines.push_back(Fmt("%sTargeted Block: %d, %d, %d", kUnderline, p.pos.x, p.pos.y, p.pos.z));
                lines.push_back(BlockRegistryName(state.Block()));
                AddStateProperties(lines, state);
            }
            out.AddToGroup(kGroupBlock, std::move(lines));
        }
    };

    class LookingAtBlockTagsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            std::vector<std::string> lines;
            const Pick p = PickBlock(ctx, false);
            if (p.hit && Client::g_clientBlockAccess) {
                const Game::BlockID id = Client::g_clientBlockAccess->GetBlock(p.pos.x, p.pos.y, p.pos.z);
                lines = Game::DataTags::TagsFor(Game::DataTags::Registry::Block, BlockRegistryName(id));
            }
            out.AddToGroup(kGroupBlock, std::move(lines));
        }
    };

    class LookingAtFluidStateEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            std::vector<std::string> lines;
            const Pick p = PickBlock(ctx, true);
            if (p.hit) {
                const FluidInfo f = FluidAt(p.pos);
                lines.push_back(Fmt("%sTargeted Fluid: %d, %d, %d", kUnderline, p.pos.x, p.pos.y, p.pos.z));
                lines.push_back(f.name);
                lines.insert(lines.end(), f.props.begin(), f.props.end());
            }
            out.AddToGroup(kGroupFluid, std::move(lines));
        }
    };

    class LookingAtFluidTagsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            std::vector<std::string> lines;
            const Pick p = PickBlock(ctx, true);
            if (p.hit) {
                const FluidInfo f = FluidAt(p.pos);
                if (!f.tagId.empty()) lines = Game::DataTags::TagsFor(Game::DataTags::Registry::Fluid, f.tagId);
            }
            out.AddToGroup(kGroupFluid, std::move(lines));
        }
    };

    class LookingAtEntityEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            std::vector<std::string> lines;
            const std::string name = CrosshairEntityName(ctx);
            if (!name.empty()) {
                lines.push_back(std::string(kUnderline) + "Targeted Entity");
                lines.push_back(name);
            }
            out.AddToGroup(kGroupEntity, std::move(lines));
        }
    };

    class LookingAtEntityTagsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            std::vector<std::string> lines;
            const std::string name = CrosshairEntityName(ctx);
            if (!name.empty()) lines = Game::DataTags::TagsFor(Game::DataTags::Registry::EntityType, name);
            out.AddToGroup(kGroupEntity, std::move(lines));
        }
    };

    // MC LevelExtractor.sectionStatistics: "C: rendered/total (s) D: dist,
    // pC: pending compiles, aB: available buffers".
    class ChunkRenderStatsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!Client::ClientLevels::HasSession()) return;
            size_t total = 0, ready = 0, meshing = 0, dirty = 0;
            if (Client::ClientChunkManager* chunks = Client::ClientLevels::Active().Chunks()) {
                chunks->GetSectionStats(total, ready, meshing, dirty);
            }
            int rendered = 0;
            bool smartCull = false;
            if (Render::g_chunkRenderer) {
                rendered = static_cast<int>(Render::g_chunkRenderer->GetStats().sectionsRendered);
                smartCull = Render::g_chunkRenderer->IsEnabledSmartCull();
            }
            const size_t pending = Render::ClientMeshManager::GetPendingMeshBuildCount();
            const size_t available = Render::GetMeshUploadPermits().Available();
            out.AddLine(Fmt("C: %d/%zu %sD: %d, pC: %03zu, aB: %02zu", rendered, total, smartCull ? "(s) " : "",
                            ctx.effectiveRenderDistance, pending, available));
        }
        bool IsAllowed(bool) const override { return true; }
    };

    // MC DebugEntryChunkGeneration: the noise router's samples and the biome
    // builder's bands at the feet, sampled on the server thread when the
    // feet block moves (IntegratedServer::SampleDebugInfo).
    class ChunkGenerationEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (ctx.isRemoteClient || !Server::g_integratedServer) return;
            const auto s = ServerSample();
            if (!s.valid) return;
            out.AddToGroup(kGroupChunkGen, s.chunkGenLines);
        }
    };

    class EntityRenderStatsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!Client::ClientLevels::HasSession()) return;
            const Client::ClientLevel& level = Client::ClientLevels::Active();
            size_t total = 0;
            if (level.Mobs()) total += level.Mobs()->Count();
            if (level.Items()) total += level.Items()->Count();
            if (level.Orbs()) total += level.Orbs()->Count();
            if (level.FallingBlocks()) total += level.FallingBlocks()->Count();
            if (Client::g_remotePlayerManager) total += Client::g_remotePlayerManager->GetPlayers().size();
            out.AddLine(Fmt("E: %d/%zu, SD: %d", ctx.entitiesRendered, total, ctx.simulationDistance));
        }
        bool IsAllowed(bool) const override { return true; }
    };

    class ParticleRenderStatsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context&) override {
            out.AddLine("P: " + std::to_string(Render::g_mobParticleSystem.Count()));
        }
    };

    // MC ClientLevel/ServerLevel.gatherChunkSourceStats: "Chunks[C] W: <cache>,
    // <loaded> E: <entities>,<sections>,<ticking chunks>".
    class ChunkSourceStatsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!Client::ClientLevels::HasSession()) return;
            const Client::ClientLevel& level = Client::ClientLevels::Active();
            size_t loaded = 0, retained = 0, mobs = 0, others = 0;
            if (level.Chunks()) { loaded = level.Chunks()->GetLoadedChunkCount(); retained = level.Chunks()->GetRetainedChunkCount(); }
            if (level.Mobs()) mobs = level.Mobs()->Count();
            if (level.Items()) others += level.Items()->Count();
            if (level.Orbs()) others += level.Orbs()->Count();
            if (level.FallingBlocks()) others += level.FallingBlocks()->Count();
            const size_t players = Client::g_remotePlayerManager ? Client::g_remotePlayerManager->GetPlayers().size() : 0;
            out.AddLine(Fmt("Chunks[C] W: %zu, %zu E: %zu,%zu,%zu", loaded + retained, loaded, mobs, others, players));
            if (!ctx.isRemoteClient) {
                const auto s = ServerSample();
                if (s.valid) {
                    out.AddLine(Fmt("Chunks[S] W: %zu E: %d,%zu,%zu", s.loadedChunks, s.mobCount, s.tickets, s.entityTickingChunks));
                }
            }
        }
        bool IsAllowed(bool) const override { return true; }
    };

    class PositionEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player || !ctx.camera) return;
            const glm::vec3 p = ctx.player->physics.position;
            const glm::ivec3 feet = FeetBlock(ctx);
            const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(feet.x, feet.z);
            const Game::Direction dir = Game::FromYRot(ctx.camera->yaw);
            const char* facing = "Invalid";
            switch (dir) {
                case Game::Direction::North: facing = "Towards negative Z"; break;
                case Game::Direction::South: facing = "Towards positive Z"; break;
                case Game::Direction::West:  facing = "Towards negative X"; break;
                case Game::Direction::East:  facing = "Towards positive X"; break;
                default: break;
            }
            const int sectionY = static_cast<int>(std::floor(static_cast<float>(feet.y) / 16.0f));
            // MC Mth.wrapDegrees: (-180, 180].
            auto wrap = [](float d) { d = std::fmod(d, 360.0f); if (d >= 180.0f) d -= 360.0f; if (d < -180.0f) d += 360.0f; return d; };
            int forceLoaded = 0;
            if (!ctx.isRemoteClient) forceLoaded = ServerSample().forceLoadedChunks;
            out.AddToGroup(kGroupPosition, std::vector<std::string>{
                Fmt("XYZ: %.3f / %.5f / %.3f", p.x, p.y, p.z),
                Fmt("Block: %d %d %d", feet.x, feet.y, feet.z),
                Fmt("Chunk: %d %d %d [%d %d in r.%d.%d.mca]", cpos.x, sectionY, cpos.z,
                    cpos.x & 31, cpos.z & 31, cpos.x >> 5, cpos.z >> 5),
                Fmt("Facing: %s (%s) (%.1f / %.1f)", std::string(Game::NameOf(dir)).c_str(), facing,
                    wrap(ctx.camera->yaw), wrap(ctx.camera->pitch)),
                Fmt("minecraft:%s FC: %d", std::string(Game::DimensionName(Client::ClientLevels::ActiveDimension())).c_str(), forceLoaded) });
        }
    };

    // "x y z" — the feet block, nothing else. A plain line so it sits alone
    // at the top left when it is the only enabled entry.
    class CoordinatesEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player) return;
            const glm::ivec3 feet = FeetBlock(ctx);
            out.AddLine(Fmt("%d %d %d", feet.x, feet.y, feet.z));
        }
    };

    class SectionPositionEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player) return;
            const glm::ivec3 feet = FeetBlock(ctx);
            out.AddToGroup(kGroupPosition, Fmt("Section-relative: %02d %02d %02d", feet.x & 15, feet.y & 15, feet.z & 15));
        }
        bool IsAllowed(bool) const override { return true; }
    };

    // MC Entity.getKnownSpeed: the position delta of the last tick. Sampled
    // here on the client-tick cadence.
    class PlayerSpeedEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player) return;
            const glm::dvec3 now(ctx.player->physics.position);
            const auto t = std::chrono::steady_clock::now();
            if (!m_have) { m_last = now; m_lastTime = t; m_have = true; }
            const double dt = std::chrono::duration<double>(t - m_lastTime).count();
            if (dt >= 0.05) {
                m_speed = glm::length(now - m_last) * (0.05 / dt);
                m_last = now;
                m_lastTime = t;
            }
            out.AddToGroup(kGroupPosition, Fmt("Speed: %.3f blocks/tick", m_speed));
        }
    private:
        bool m_have = false;
        glm::dvec3 m_last{0.0};
        std::chrono::steady_clock::time_point m_lastTime;
        double m_speed = 0.0;
    };

    // No light engine: sky light is the open-to-sky stand-in the terrain
    // uses (15 or 0), block light does not exist.
    class LightEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player || !Client::g_clientBlockAccess) return;
            const glm::ivec3 feet = FeetBlock(ctx);
            const int sky = Client::g_clientBlockAccess->GetRawBrightness(feet.x, feet.y, feet.z);
            const int block = 0;
            out.AddToGroup(kGroupLight, Fmt("Client Light: %d (%d sky, %d block)", std::max(sky, block), sky, block));
        }
    };

    // MC DebugEntryHeightmap: "CH" from the client chunk (the types the
    // server sends: WORLD_SURFACE, MOTION_BLOCKING), "SH" from the server
    // chunk (the four live ones). The client never receives heightmaps, so
    // its two are computed from the column on the spot.
    class HeightmapEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player || !Client::ClientLevels::HasSession()) return;
            Client::ClientChunkManager* chunks = Client::ClientLevels::Active().Chunks();
            if (!chunks) return;
            const glm::ivec3 feet = FeetBlock(ctx);
            const auto cpos = Game::Math::WorldCoordinates::WorldToChunkPos(feet.x, feet.z);
            const Client::ClientChunk* chunk = chunks->GetChunk(cpos);
            if (!chunk || !chunk->chunkData) return;
            const int lx = feet.x - cpos.x * Game::Math::CHUNK_SIZE_X;
            const int lz = feet.z - cpos.z * Game::Math::CHUNK_SIZE_Z;
            auto height = [&](Game::HeightmapType type) {
                for (int y = Game::Heightmap::MaxY(); y >= Game::Heightmap::MinY(); --y) {
                    if (Game::HeightmapIsOpaque(type, chunk->chunkData->GetBlock(lx, y, lz))) return y;
                }
                return Game::Heightmap::MinY() - 1;
            };
            std::vector<std::string> lines;
            lines.push_back(Fmt("CH S: %d M: %d", height(Game::HeightmapType::WorldSurface),
                                height(Game::HeightmapType::MotionBlocking)));
            const auto s = ctx.isRemoteClient ? Server::IntegratedServer::DebugSample{} : ServerSample();
            if (s.valid && s.chunkLoaded && s.heightmapsPrimed) {
                lines.push_back(Fmt("SH S: %d O: %d M: %d ML: %d", s.heightWorldSurface, s.heightOceanFloor,
                                    s.heightMotionBlocking, s.heightMotionBlockingNoLeaves));
            } else {
                lines.push_back("SH S: ?? O: ?? M: ?? ML: ??");
            }
            out.AddToGroup(kGroupHeightmaps, std::move(lines));
        }
    };

    class BiomeEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player || !Client::g_clientBlockAccess) return;
            const glm::ivec3 feet = FeetBlock(ctx);
            if (feet.y < Game::Heightmap::MinY() || feet.y > Game::Heightmap::MaxY()) return;
            const uint16_t biome = Client::g_clientBlockAccess->GetBiome(feet.x, feet.y, feet.z);
            out.AddLine("Biome: minecraft:" + std::string(Game::BiomeRegistry::Get(biome).name));
        }
    };

    class LocalDifficultyEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player || ctx.isRemoteClient) return;
            const auto s = ServerSample();
            if (!s.valid || !s.chunkLoaded) return;
            const glm::ivec3 feet = FeetBlock(ctx);
            if (feet.y < Game::Heightmap::MinY() || feet.y > Game::Heightmap::MaxY()) return;
            const float effective = EffectiveDifficulty(s.difficulty, s.dayTime, s.inhabitedTime, MoonBrightness(s.dayTime));
            out.AddLine(Fmt("Local Difficulty: %.2f // %.2f", effective, SpecialMultiplier(effective)));
        }
    };

    class DayCountEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            int64_t dayTime = Render::EnvironmentState::Get().DayTime();
            if (!ctx.isRemoteClient) {
                const auto s = ServerSample();
                if (s.valid) dayTime = s.dayTime;
            }
            out.AddLine("Day #" + std::to_string(dayTime / 24000));
        }
    };

    // MC DebugEntrySpawnCounts: "SC: chunks, MO: n, C: n, ..." — the last
    // spawn pass's census, from the server.
    class SpawnCountsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (ctx.isRemoteClient) return;
            const auto s = ServerSample();
            if (!s.valid) return;
            static const char* kAbbrev[8] = { "MO", "C", "AM", "AX", "UWC", "WC", "WA", "MI" };
            std::string line = "SC: " + std::to_string(s.spawnableChunks);
            for (int i = 0; i < 8; ++i) line += Fmt(", %s: %d", kAbbrev[i], s.categoryCounts[i]);
            out.AddLine(line);
        }
    };

    // MC DebugEntrySoundMood: the channel pools in use and the cave-mood
    // counter (LocalPlayer.getCurrentMood).
    class SoundMoodEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            if (!ctx.player) return;
            out.AddLine(Client::GetSoundManager().GetChannelDebugString() +
                        Fmt(" (Mood %d%%)", static_cast<int>(std::lround(
                                Client::AmbientSounds::GetCurrentMood() * 100.0f))));
        }
    };

    // MC DebugEntrySoundCache: decoded static buffers and their size.
    class SoundCacheEntry : public Entry {
    public:
        void Display(Displayer& out, const Context&) override {
            const auto stats = Client::GetSoundManager().GetSoundCacheStats();
            out.AddLine(Fmt("Sound cache: %d buffers, %lld MiB", stats.count,
                            static_cast<long long>(std::ceil(static_cast<double>(stats.bytes) / 1024.0 / 1024.0))));
        }
        bool IsAllowed(bool) const override { return true; }
    };

    // MC prints "Post: <effects>" only when a post effect is applied; none exist.
    class PostEffectsEntry : public Entry {
    public:
        void Display(Displayer&, const Context&) override {}
    };

    class GpuUtilizationEntry : public Entry {
    public:
        void Display(Displayer& out, const Context& ctx) override {
            std::string value;
            if (ctx.gpuUtilization < 0.0) value = "n/a";
            else if (ctx.gpuUtilization > 100.0) value = std::string(kRed) + "100%";
            else value = std::to_string(static_cast<int>(std::lround(ctx.gpuUtilization))) + "%";
            out.AddLine("GPU: " + value);
        }
        bool IsAllowed(bool) const override { return true; }
    };

    class SimplePerformanceImpactorsEntry : public Entry {
    public:
        void Display(Displayer& out, const Context&) override {
            const auto& settings = Platform::g_gameSettings;
            const std::string clouds = settings.GetRenderClouds();
            const char* cloudText = clouds == "false" ? "" : (clouds == "fast" ? "fast-clouds " : "fancy-clouds ");
            out.AddLine(Fmt("%s%sB: %d", "", cloudText, settings.GetBiomeBlendRadius()));
            const int mips = settings.GetMipmapLevels();
            out.AddLine(mips > 0 ? Fmt("Filtering: Trilinear (%d mipmap levels)", mips) : std::string("Filtering: Nearest"));
            const bool mdi = Render::g_renderBackend && Render::g_renderBackend->DebugGetMultiDrawIndirect();
            out.AddLine(Fmt("Terrain Rendering: %s", mdi ? "multidrawindirect" : "naive"));
        }
        bool IsAllowed(bool) const override { return true; }
    };

    void RegisterTextEntries(std::map<std::string, std::unique_ptr<Entry>>& out) {
        out[Ids::GameVersion]           = std::make_unique<VersionEntry>();
        out[Ids::Fps]                   = std::make_unique<FpsEntry>();
        out[Ids::Tps]                   = std::make_unique<TpsEntry>();
        out[Ids::Memory]                = std::make_unique<MemoryEntry>();
        out[Ids::DetailedMemory]        = std::make_unique<DetailedMemoryEntry>();
        out[Ids::SystemSpecs]           = std::make_unique<SystemSpecsEntry>();
        out[Ids::LookingAtBlockState]   = std::make_unique<LookingAtBlockStateEntry>();
        out[Ids::LookingAtBlockTags]    = std::make_unique<LookingAtBlockTagsEntry>();
        out[Ids::LookingAtFluidState]   = std::make_unique<LookingAtFluidStateEntry>();
        out[Ids::LookingAtFluidTags]    = std::make_unique<LookingAtFluidTagsEntry>();
        out[Ids::LookingAtEntity]       = std::make_unique<LookingAtEntityEntry>();
        out[Ids::LookingAtEntityTags]   = std::make_unique<LookingAtEntityTagsEntry>();
        out[Ids::ChunkRenderStats]      = std::make_unique<ChunkRenderStatsEntry>();
        out[Ids::ChunkGenerationStats]  = std::make_unique<ChunkGenerationEntry>();
        out[Ids::EntityRenderStats]     = std::make_unique<EntityRenderStatsEntry>();
        out[Ids::ParticleRenderStats]   = std::make_unique<ParticleRenderStatsEntry>();
        out[Ids::ChunkSourceStats]      = std::make_unique<ChunkSourceStatsEntry>();
        out[Ids::PlayerPosition]        = std::make_unique<PositionEntry>();
        out[Ids::PlayerSectionPosition] = std::make_unique<SectionPositionEntry>();
        out[Ids::PlayerSpeed]           = std::make_unique<PlayerSpeedEntry>();
        out[Ids::Coordinates]           = std::make_unique<CoordinatesEntry>();
        out[Ids::LightLevels]           = std::make_unique<LightEntry>();
        out[Ids::Heightmap]             = std::make_unique<HeightmapEntry>();
        out[Ids::Biome]                 = std::make_unique<BiomeEntry>();
        out[Ids::LocalDifficulty]       = std::make_unique<LocalDifficultyEntry>();
        out[Ids::DayCount]              = std::make_unique<DayCountEntry>();
        out[Ids::EntitySpawnCounts]     = std::make_unique<SpawnCountsEntry>();
        out[Ids::SoundMood]             = std::make_unique<SoundMoodEntry>();
        out[Ids::SoundCache]            = std::make_unique<SoundCacheEntry>();
        out[Ids::PostEffects]           = std::make_unique<PostEffectsEntry>();
        out[Ids::GpuUtilization]        = std::make_unique<GpuUtilizationEntry>();
        out[Ids::SimplePerformanceImpactors] = std::make_unique<SimplePerformanceImpactorsEntry>();
    }

} // namespace Render::DebugScreen
