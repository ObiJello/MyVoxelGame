// File: src/launcher/appearance/CapeLibrary.cpp
#include "CapeLibrary.hpp"
#include "launcher/updater/HttpSession.hpp"
#include "common/core/Log.hpp"
#include "common/entity/PlayerCapes.hpp"

#include <deque>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace Launcher::Appearance {

    struct CapeLibrary::Shared {
        std::mutex mutex;
        std::deque<std::string> queue;               // slugs to load
        struct Done { std::string slug; bool ok; Game::SkinImage image; };
        std::vector<Done> done;
        bool workerRunning = false;
        bool closing = false;
        std::string cacheDir;
    };

    namespace {

        // One cape: the cache, else the texture server.
        bool LoadCape(Http::Session& session, const std::string& cacheDir, const std::string& slug,
                      Game::SkinImage& out) {
            const Game::CapeInfo* info = Game::FindCape(slug);
            if (!info) return false;
            const std::string cachePath = cacheDir + "/" + slug + ".png";
            std::error_code ec;
            if (std::filesystem::is_regular_file(cachePath, ec) && LoadPngFile(cachePath, out) &&
                NormalizeCape(out)) {
                return true;
            }
            Http::Request r;
            // https: textures.minecraft.net answers on both; the hash is the id.
            r.url = "https://textures.minecraft.net/texture/" + std::string(info->hash);
            session.Fetch({ &r }, 1);
            if (!r.Ok()) {
                Log::Warning("[Capes] %s: %s", slug.c_str(),
                             r.error.empty() ? ("HTTP " + std::to_string(r.status)).c_str() : r.error.c_str());
                return false;
            }
            if (!Game::DecodePng(r.body, out) || !NormalizeCape(out)) {
                Log::Warning("[Capes] %s: not a cape image", slug.c_str());
                return false;
            }
            if (!cacheDir.empty()) SavePngFile(cachePath, out);
            return true;
        }

        void Worker(std::shared_ptr<CapeLibrary::Shared> shared) {
            Http::Session session;
            for (;;) {
                std::string slug, dir;
                {
                    std::lock_guard<std::mutex> lock(shared->mutex);
                    if (shared->closing || shared->queue.empty()) {
                        shared->workerRunning = false;
                        return;
                    }
                    slug = std::move(shared->queue.front());
                    shared->queue.pop_front();
                    dir = shared->cacheDir;
                }
                Game::SkinImage image;
                const bool ok = LoadCape(session, dir, slug, image);
                std::lock_guard<std::mutex> lock(shared->mutex);
                shared->done.push_back({ slug, ok, std::move(image) });
            }
        }

    } // namespace

    CapeLibrary::CapeLibrary() : m_shared(std::make_shared<Shared>()) {}

    CapeLibrary::~CapeLibrary() {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->closing = true;
        m_shared->queue.clear();
    }

    void CapeLibrary::SetCacheDir(const std::string& dir) {
        m_cacheDir = dir;
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        m_shared->cacheDir = dir;
    }

    void CapeLibrary::Kick() {
        // Called with the queue already appended; starts a worker unless one
        // is draining it.
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        if (m_shared->workerRunning || m_shared->queue.empty()) return;
        m_shared->workerRunning = true;
        std::thread(Worker, m_shared).detach();
    }

    CapeLibrary::Status CapeLibrary::Request(const std::string& slug) {
        Entry& e = m_entries[slug];
        if (e.status != Status::Unrequested) return e.status;
        if (!Game::FindCape(slug)) {
            e.status = Status::Failed;
            return e.status;
        }
        e.status = Status::Loading;
        {
            std::lock_guard<std::mutex> lock(m_shared->mutex);
            m_shared->queue.push_back(slug);
        }
        Kick();
        return e.status;
    }

    CapeLibrary::Status CapeLibrary::StatusOf(const std::string& slug) const {
        const auto it = m_entries.find(slug);
        return it == m_entries.end() ? Status::Unrequested : it->second.status;
    }

    const Game::SkinImage* CapeLibrary::Image(const std::string& slug) const {
        const auto it = m_entries.find(slug);
        if (it == m_entries.end() || it->second.status != Status::Ready) return nullptr;
        return &it->second.image;
    }

    GLuint CapeLibrary::Texture(const std::string& slug) const {
        const auto it = m_entries.find(slug);
        if (it == m_entries.end() || it->second.status != Status::Ready) return 0;
        return it->second.texture.Id();
    }

    void CapeLibrary::Poll() {
        std::vector<Shared::Done> done;
        {
            std::lock_guard<std::mutex> lock(m_shared->mutex);
            done.swap(m_shared->done);
        }
        for (Shared::Done& d : done) {
            Entry& e = m_entries[d.slug];
            if (d.ok) {
                e.image = std::move(d.image);
                e.texture.Upload(e.image);
                e.status = Status::Ready;
            } else {
                e.status = Status::Failed;
            }
        }
    }

    void CapeLibrary::Retry(const std::string& slug) {
        const auto it = m_entries.find(slug);
        if (it != m_entries.end() && it->second.status == Status::Failed) {
            it->second.status = Status::Unrequested;
        }
    }

    void CapeLibrary::ReleaseGpu() {
        for (auto& [slug, entry] : m_entries) entry.texture.Release();
    }

} // namespace Launcher::Appearance
