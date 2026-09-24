// File: src/client/sound/SoundManager.cpp
#include "client/sound/SoundManager.hpp"

#include "client/resource/ResourcePacks.hpp"
#include "common/core/AssetLocator.hpp"
#include "common/core/Assert.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"
#include "platform/GameDirectory.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace Client {

    namespace fs = std::filesystem;

    namespace {
        fs::path U8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

        bool IsMainThread() {
            // Before PlatformMain stamps the id (startup), everything is main.
            return g_clientThreadId == std::thread::id() || std::this_thread::get_id() == g_clientThreadId;
        }

        // Identifier.parse: "ns:path", no colon meaning "minecraft".
        void SplitIdentifier(const std::string& id, std::string& ns, std::string& path) {
            const size_t colon = id.find(':');
            if (colon == std::string::npos) {
                ns = "minecraft";
                path = id;
            } else {
                ns = id.substr(0, colon);
                path = id.substr(colon + 1);
            }
        }

        std::string OptionKey(Game::SoundSource source) {
            return "soundCategory_" + std::string(Game::SoundSourceName(source));
        }
    } // namespace

    SoundManager& SoundManager::Get() {
        static SoundManager instance;
        return instance;
    }

    // ── Registry ────────────────────────────────────────────────────────────

    std::string SoundManager::ResolveSoundFile(const std::string& ns, const std::string& path) const {
        const std::string rel = path + ".ogg";
        if (ns == "minecraft") {
            const auto it = m_soundFiles.find(rel);
            return it == m_soundFiles.end() ? std::string() : it->second;
        }
        // Another namespace: a pack's assets/<ns>/sounds/, then the engine's
        // assets/<ns>/sounds/.
        std::error_code ec;
        for (const std::string& root : Resources::EnabledPackRoots()) {
            const fs::path p = U8Path(root + "assets/" + ns + "/sounds/" + rel);
            if (fs::is_regular_file(p, ec)) return root + "assets/" + ns + "/sounds/" + rel;
        }
        const std::string engine = m_assetsRoot + "/" + ns + "/sounds/" + rel;
        return fs::is_regular_file(U8Path(engine), ec) ? engine : std::string();
    }

    void SoundManager::LoadSoundsJson(const std::string& path, const std::string& ns) {
        std::ifstream in(U8Path(path), std::ios::binary);
        if (!in) return;
        nlohmann::json root;
        try {
            in >> root;
        } catch (const std::exception& e) {
            Log::Warning("[Sound] Invalid sounds.json in %s: %s", path.c_str(), e.what());
            return;
        }
        if (!root.is_object()) return;

        int missingFiles = 0;
        for (auto it = root.begin(); it != root.end(); ++it) {
            const nlohmann::json& entry = it.value();
            if (!entry.is_object()) continue;
            const std::string eventId = ns == "minecraft" ? it.key() : ns + ":" + it.key();
            const bool replace = entry.value("replace", false);
            const std::string subtitle = entry.contains("subtitle") && entry["subtitle"].is_string()
                ? entry["subtitle"].get<std::string>() : std::string();

            // MC Preparations.handleRegistration.
            WeighedSoundEvents* registration = m_registry.Find(eventId);
            if (!registration || replace) registration = &m_registry.Replace(eventId, subtitle);

            if (!entry.contains("sounds") || !entry["sounds"].is_array()) continue;
            for (const nlohmann::json& s : entry["sounds"]) {
                Sound sound;
                std::string name;
                if (s.is_string()) {
                    name = s.get<std::string>();
                } else if (s.is_object() && s.contains("name") && s["name"].is_string()) {
                    name = s["name"].get<std::string>();
                    const std::string type = s.value("type", std::string("file"));
                    if (type == "event") sound.type = Sound::Type::SoundEvent;
                    else if (type != "file") { Log::Warning("[Sound] %s: invalid type '%s'", eventId.c_str(), type.c_str()); continue; }
                    sound.volume = s.value("volume", 1.0f);
                    sound.pitch = s.value("pitch", 1.0f);
                    sound.weight = s.value("weight", 1);
                    sound.preload = s.value("preload", false);
                    sound.stream = s.value("stream", false);
                    sound.attenuationDistance = s.value("attenuation_distance", 16);
                    // MC Validate.isTrue: positive volume, pitch, weight.
                    if (!(sound.volume > 0.0f) || !(sound.pitch > 0.0f) || sound.weight <= 0) {
                        Log::Warning("[Sound] %s: invalid volume/pitch/weight for %s", eventId.c_str(), name.c_str());
                        continue;
                    }
                } else {
                    continue;
                }

                std::string soundNs, soundPath;
                SplitIdentifier(name, soundNs, soundPath);
                if (sound.type == Sound::Type::SoundEvent) {
                    // Event references keep the namespace their name gives
                    // (bare = minecraft), normalised like registry keys.
                    sound.location = SoundRegistry::Normalize(soundNs + ":" + soundPath);
                } else {
                    sound.location = soundNs + ":" + soundPath;
                    sound.path = ResolveSoundFile(soundNs, soundPath);
                    if (sound.path.empty()) {
                        // MC validateSoundResource: "File {} does not exist,
                        // cannot add it to event {}".
                        if (missingFiles < 4) {
                            Log::Warning("[Sound] File %s does not exist, cannot add it to event %s",
                                         sound.location.c_str(), eventId.c_str());
                        }
                        ++missingFiles;
                        continue;
                    }
                }
                registration->AddSound(std::move(sound));
            }
        }
        if (missingFiles > 4) {
            Log::Warning("[Sound] %s: %d sound files missing in total", path.c_str(), missingFiles);
        }
    }

    void SoundManager::LoadRegistry() {
        PROFILE_ZONE_N("SoundManager.LoadRegistry");
        m_registry.Clear();
        m_soundFiles.clear();

        // Every .ogg under sounds/, the packs' copies over the vanilla ones.
        for (const Core::Assets::Entry& e : Core::Assets::ListFiles(m_assetsRoot + "/sounds", ".ogg", true)) {
            m_soundFiles.emplace(e.relative, e.absolute);
        }

        std::error_code ec;
        // 1. The vanilla file — extracted, or out of the Minecraft install.
        const std::string vanilla = m_assetsRoot + "/sounds.json";
        if (fs::is_regular_file(U8Path(vanilla), ec)) {
            LoadSoundsJson(vanilla, "minecraft");
        } else {
            const std::string installJson = FindMinecraftInstallSounds();
            if (!installJson.empty()) LoadSoundsJson(installJson, "minecraft");
        }

        // 2. The engine's overlays: one directory per namespace, each holding
        //    sounds.json-format files applied in name order (split by topic —
        //    blocks, entities, music — so they stay reviewable).
        const fs::path overlays = U8Path(m_assetsRoot + "/sound_overlays");
        if (fs::is_directory(overlays, ec)) {
            std::vector<std::string> namespaces;
            for (const auto& dir : fs::directory_iterator(overlays, ec)) {
                if (dir.is_directory(ec)) namespaces.push_back(dir.path().filename().string());
            }
            std::sort(namespaces.begin(), namespaces.end());
            for (const std::string& ns : namespaces) {
                std::vector<std::string> files;
                for (const auto& f : fs::directory_iterator(overlays / U8Path(ns), ec)) {
                    if (f.is_regular_file(ec) && f.path().extension() == ".json") {
                        files.push_back(f.path().filename().string());
                    }
                }
                std::sort(files.begin(), files.end());
                for (const std::string& name : files) {
                    LoadSoundsJson(m_assetsRoot + "/sound_overlays/" + ns + "/" + name, ns);
                }
            }
        }

        // 3. Resource packs, lowest priority first.
        std::vector<std::string> roots = Resources::EnabledPackRoots();
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            const fs::path assets = U8Path(*it + "assets");
            if (!fs::is_directory(assets, ec)) continue;
            for (const auto& dir : fs::directory_iterator(assets, ec)) {
                if (!dir.is_directory(ec)) continue;
                const std::string ns = dir.path().filename().string();
                const std::string file = *it + "assets/" + ns + "/sounds.json";
                if (fs::is_regular_file(U8Path(file), ec)) LoadSoundsJson(file, ns);
            }
        }

        Log::Info("[Sound] Registered %zu sound events (%zu sound files)", m_registry.Size(), m_soundFiles.size());
    }

    std::string SoundManager::FindMinecraftInstallSounds() {
        std::string home;
#if defined(_WIN32)
        if (const char* appData = std::getenv("APPDATA")) home = std::string(appData) + "/.minecraft";
#elif defined(__APPLE__)
        if (const char* h = std::getenv("HOME")) home = std::string(h) + "/Library/Application Support/minecraft";
#else
        if (const char* h = std::getenv("HOME")) home = std::string(h) + "/.minecraft";
#endif
        if (home.empty()) return {};
        std::error_code ec;
        const fs::path indexes = U8Path(home + "/assets/indexes");
        if (!fs::is_directory(indexes, ec)) return {};

        // The newest numbered index (26.x's is 34.json), as the tool picks it.
        int best = -1;
        fs::path bestPath;
        for (const auto& e : fs::directory_iterator(indexes, ec)) {
            if (e.path().extension() != ".json") continue;
            const std::string stem = e.path().stem().string();
            if (stem.empty() || stem.find_first_not_of("0123456789") != std::string::npos) continue;
            const int n = std::atoi(stem.c_str());
            if (n > best) { best = n; bestPath = e.path(); }
        }
        if (best < 0) return {};

        nlohmann::json index;
        try {
            std::ifstream in(bestPath, std::ios::binary);
            in >> index;
        } catch (const std::exception& e) {
            Log::Warning("[Sound] Unreadable asset index %s: %s", bestPath.string().c_str(), e.what());
            return {};
        }
        if (!index.contains("objects") || !index["objects"].is_object()) return {};

        const std::string objects = home + "/assets/objects/";
        std::string soundsJson;
        constexpr std::string_view kPrefix = "minecraft/sounds/";
        for (auto it = index["objects"].begin(); it != index["objects"].end(); ++it) {
            const std::string& key = it.key();
            if (!it.value().contains("hash") || !it.value()["hash"].is_string()) continue;
            const std::string hash = it.value()["hash"].get<std::string>();
            if (hash.size() < 2) continue;
            const std::string file = objects + hash.substr(0, 2) + "/" + hash;
            if (key == "minecraft/sounds.json") {
                soundsJson = file;
            } else if (key.compare(0, kPrefix.size(), kPrefix) == 0) {
                // A resource pack's copy (already listed) wins.
                m_soundFiles.emplace(key.substr(kPrefix.size()), file);
            }
        }
        if (soundsJson.empty() || !fs::is_regular_file(U8Path(soundsJson), ec)) return {};
        Log::Info("[Sound] No extracted sounds; using the Minecraft install at %s (index %d)", home.c_str(), best);
        return soundsJson;
    }

    // ── Lifecycle ───────────────────────────────────────────────────────────

    void SoundManager::Initialize(const std::string& assetsRoot) {
        if (m_initialized) return;
        m_initialized = true;
        m_assetsRoot = assetsRoot;
        m_engine = std::make_unique<SoundEngine>(*this);
        ApplyOptionsFromSettings();
        ReloadResources();
    }

    void SoundManager::ReloadResources() {
        if (!m_engine) return;
        LoadRegistry();
        if (!HasSounds()) {
            m_engine->Destroy();
            Log::Warning("[Sound] No sound assets (no assets/sounds.json and no Minecraft install found) - "
                         "run tools/extract_mc_sounds.py; the game runs silent");
            return;
        }
        std::vector<std::string> preloads;
        for (const auto& [id, event] : m_registry.All()) {
            (void)id;
            event.CollectPreloads(m_registry, preloads);
        }
        for (const std::string& p : preloads) m_engine->RequestPreload(p);
        m_engine->Reload();
    }

    void SoundManager::Shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_crossThreadMutex);
            m_crossThreadPlays.clear();
        }
        m_engine.reset();
        m_initialized = false;
    }

    void SoundManager::EmergencyShutdown() {
        if (m_engine) m_engine->EmergencyShutdown();
    }

    // ── Play / stop ─────────────────────────────────────────────────────────

    SoundEngine::PlayResult SoundManager::Play(const std::shared_ptr<SoundInstance>& instance) {
        if (!m_engine || !instance) return SoundEngine::PlayResult::NotStarted;
        if (!IsMainThread()) {
            std::lock_guard<std::mutex> lock(m_crossThreadMutex);
            m_crossThreadPlays.push_back(instance);
            return SoundEngine::PlayResult::Started;
        }
        return m_engine->Play(instance);
    }

    void SoundManager::PlayDelayed(const std::shared_ptr<SoundInstance>& instance, int delay) {
        if (m_engine && instance) m_engine->PlayDelayed(instance, delay);
    }

    void SoundManager::QueueTickingSound(const std::shared_ptr<SoundInstance>& instance) {
        if (m_engine && instance) m_engine->QueueTickingSound(instance);
    }

    void SoundManager::Stop(const std::shared_ptr<SoundInstance>& instance) {
        if (m_engine) m_engine->Stop(instance);
    }

    void SoundManager::Stop(const std::string* identifier, std::optional<Game::SoundSource> source) {
        if (m_engine) m_engine->Stop(identifier, source);
    }

    void SoundManager::StopAll() {
        if (m_engine) m_engine->StopAll();
    }

    bool SoundManager::IsActive(const std::shared_ptr<SoundInstance>& instance) const {
        return m_engine && m_engine->IsActive(instance);
    }

    void SoundManager::Tick(bool paused) {
        if (!m_engine) return;
        std::vector<std::shared_ptr<SoundInstance>> pending;
        {
            std::lock_guard<std::mutex> lock(m_crossThreadMutex);
            pending.swap(m_crossThreadPlays);
        }
        for (const auto& instance : pending) m_engine->Play(instance);
        m_engine->Tick(paused);
    }

    void SoundManager::UpdateSource(const glm::dvec3& position, const glm::dvec3& forward, const glm::dvec3& up) {
        m_listenerPosition = position;
        if (m_engine) m_engine->UpdateSource(position, forward, up);
    }

    void SoundManager::PauseAllExcept(std::initializer_list<Game::SoundSource> ignored) {
        if (m_engine) m_engine->PauseAllExcept(ignored);
    }

    void SoundManager::Resume() {
        if (m_engine) m_engine->Resume();
    }

    void SoundManager::UpdateCategoryVolume(Game::SoundSource source, float gain) {
        if (m_engine) m_engine->UpdateCategoryVolume(source, gain);
    }

    void SoundManager::RefreshCategoryVolume(Game::SoundSource source) {
        if (m_engine) m_engine->RefreshCategoryVolume(source);
    }

    // ── Options ─────────────────────────────────────────────────────────────

    void SoundManager::ApplyOptionsFromSettings() {
        if (!m_engine) return;
        const Platform::GameSettings& s = Platform::g_gameSettings;
        for (int i = 0; i < Game::kSoundSourceCount; ++i) {
            const auto source = static_cast<Game::SoundSource>(i);
            m_engine->SetOptionVolume(source, s.GetFloat(OptionKey(source), 1.0f));
        }
        m_engine->SetPreferredDevice(s.GetSoundDevice());
        m_engine->SetDirectionalAudio(s.GetDirectionalAudio());
    }

    void SoundManager::OnCategoryVolumeChanged(Game::SoundSource source) {
        if (!m_engine) return;
        m_engine->SetOptionVolume(source, Platform::g_gameSettings.GetFloat(OptionKey(source), 1.0f));
        m_engine->RefreshCategoryVolume(source);
    }

    void SoundManager::OnDeviceOptionsChanged() {
        if (!m_engine) return;
        ApplyOptionsFromSettings();
        if (HasSounds()) m_engine->Reload();
    }

    std::vector<std::string> SoundManager::GetAvailableSoundDevices() const {
        return m_engine ? m_engine->GetAvailableSoundDevices() : std::vector<std::string>{};
    }

    float SoundManager::GetFinalSoundSourceVolume(Game::SoundSource source) const {
        return m_engine ? m_engine->GetFinalSoundSourceVolume(source) : 0.0f;
    }

    std::string SoundManager::GetChannelDebugString() const {
        return m_engine ? m_engine->GetChannelDebugString() : std::string("Sounds: 0/0 + 0/0");
    }

    SoundBufferLibrary::Stats SoundManager::GetSoundCacheStats() const {
        return m_engine ? m_engine->GetSoundCacheStats() : SoundBufferLibrary::Stats{};
    }

} // namespace Client
