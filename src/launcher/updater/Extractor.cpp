// File: src/launcher/updater/Extractor.cpp
#include "launcher/updater/Extractor.hpp"
#include "launcher/updater/FileOps.hpp"
#include "common/core/Log.hpp"

#include <algorithm>
#include <system_error>

namespace Launcher::Extract {

    // ── Writer ──────────────────────────────────────────────────────────────

    Writer::Writer(int threads) {
        if (threads <= 0) {
            threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 2u, 8u));
        }
        for (int i = 0; i < threads; i++) m_threads.emplace_back([this] { Run(); });
    }

    Writer::~Writer() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_work.notify_all();
        for (auto& t : m_threads) t.join();
    }

    void Writer::Submit(Job job) {
        const uint64_t bytes = job.compressed.size();
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // One entry bigger than the cap still goes through on its own.
            m_space.wait(lock, [&] { return m_queuedBytes == 0 || m_queuedBytes + bytes <= kMaxQueuedBytes; });
            m_queuedBytes += bytes;
            m_queue.push_back(std::move(job));
        }
        m_work.notify_one();
    }

    void Writer::Wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idle.wait(lock, [&] { return m_queue.empty() && m_active == 0; });
    }

    std::unordered_map<std::string, Writer::Written> Writer::TakeWritten() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return std::move(m_written);
    }

    size_t Writer::Failures() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_failures;
    }

    std::string Writer::FirstError() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_firstError;
    }

    bool Writer::EnsureParent(const fs::path& file) {
        const fs::path parent = file.parent_path();
        const std::string key = parent.string();
        {
            std::lock_guard<std::mutex> lock(m_dirMutex);
            if (m_dirs.count(key)) return true;
        }
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec && !fs::is_directory(parent)) return false;
        std::lock_guard<std::mutex> lock(m_dirMutex);
        m_dirs.insert(key);
        return true;
    }

    void Writer::Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_work.wait(lock, [&] { return m_stop || !m_queue.empty(); });
                if (m_queue.empty()) return;
                job = std::move(m_queue.front());
                m_queue.pop_front();
                m_queuedBytes -= job.compressed.size();
                m_active++;
            }
            m_space.notify_all();

            std::vector<uint8_t> out;
            std::string error;
            bool ok = Core::ZipArchive::Decode(job.name, job.method, job.crc32, job.size, job.compressed.data(),
                                               job.compressed.size(), out, error);
            if (ok) {
                job.compressed = {};
                ok = EnsureParent(job.path) && FileOps::WriteFile(job.path, out.data(), out.size(), job.unixMode);
                if (!ok) error = "cannot write " + job.path.string();
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (ok) {
                    m_written[job.key] = {job.crc32, job.size};
                } else {
                    m_failures++;
                    if (m_firstError.empty()) m_firstError = error;
                }
                m_active--;
                if (m_queue.empty() && m_active == 0) m_idle.notify_all();
            }
        }
    }

    // ── StreamingExtractor ─────────────────────────────────────────────────

    StreamingExtractor::StreamingExtractor(fs::path destDir) : m_dest(std::move(destDir)) {}

    void StreamingExtractor::Stop(const char* why) {
        if (m_state == State::Stopped) return;
        m_state = State::Stopped;
        m_header.clear();
        m_data = {};
        if (why) Log::Info("[Update] streaming extraction stopped after %zu files: %s", m_streamedFiles, why);
    }

    void StreamingExtractor::Feed(const uint8_t* data, size_t size) {
        constexpr size_t kFixed = 30;   // local header before its name and extra field
        size_t i = 0;
        while (i < size && m_state != State::Stopped) {
            if (m_state == State::Header) {
                size_t need = kFixed;
                if (m_header.size() >= kFixed) {
                    need += static_cast<size_t>(m_header[26] | (m_header[27] << 8)) +
                            static_cast<size_t>(m_header[28] | (m_header[29] << 8));
                }
                const size_t take = std::min(need - m_header.size(), size - i);
                m_header.insert(m_header.end(), data + i, data + i + take);
                i += take;

                const auto r = Core::ZipArchive::ParseLocalHeader(m_header.data(), m_header.size(), m_local);
                if (r == Core::ZipArchive::HeaderResult::NeedMore) continue;
                if (r == Core::ZipArchive::HeaderResult::NotAHeader) {
                    // The central directory: every entry has gone by.
                    Stop(nullptr);
                    break;
                }
                if (m_local.flags & 0x0001) { Stop("encrypted entry"); break; }
                if (m_local.flags & 0x0008) { Stop("entry sizes follow its data"); break; }
                if (m_local.compressedSize > Core::ZipArchive::kMaxEntryBytes) { Stop("entry too large"); break; }
                m_header.clear();
                m_state = State::Data;
                m_remaining = m_local.compressedSize;
                m_data.clear();
                m_data.reserve(static_cast<size_t>(m_remaining));
                if (m_remaining == 0) EntryComplete();
            } else {
                const size_t take = static_cast<size_t>(std::min<uint64_t>(m_remaining, size - i));
                m_data.insert(m_data.end(), data + i, data + i + take);
                i += take;
                m_remaining -= take;
                if (m_remaining == 0) EntryComplete();
            }
        }
    }

    void StreamingExtractor::EntryComplete() {
        m_state = State::Header;
        const std::string rel = FileOps::RelativePath(m_local.name);
        if (rel.empty()) {
            m_data = {};
            return;   // Finish reports names that would escape the destination
        }
        if (!m_local.name.empty() && (m_local.name.back() == '/' || m_local.name.back() == '\\')) {
            std::error_code ec;
            fs::create_directories(m_dest / rel, ec);
            m_data = {};
            return;
        }
        Writer::Job job;
        job.path = m_dest / rel;
        job.key = rel;
        job.name = m_local.name;
        job.method = m_local.method;
        job.crc32 = m_local.crc32;
        job.size = m_local.size;
        job.compressed = std::move(m_data);
        m_data = {};
        m_writer.Submit(std::move(job));
        m_streamedFiles++;
    }

    bool StreamingExtractor::Finish(const fs::path& zipPath, std::vector<Core::ZipArchive::Entry>& entries,
                                    std::string& error) {
        const bool streamed = m_streamedFiles > 0;
        if (m_state != State::Stopped) Stop(streamed ? "the stream ended inside an entry" : nullptr);
        m_writer.Wait();
        // Streamed files that failed are simply absent here and extracted again below.
        std::unordered_map<std::string, Writer::Written> written = m_writer.TakeWritten();
        const size_t streamFailures = m_writer.Failures();

        Core::ZipArchive zip(zipPath);
        if (!zip.IsOpen()) {
            error = "cannot open " + zipPath.string() + ": " + zip.Error();
            return false;
        }
        entries = zip.Entries();
        m_directoryBytes = zip.DirectoryBytes();

        std::vector<const Core::ZipArchive::Entry*> order;
        order.reserve(entries.size());
        for (const auto& e : entries) order.push_back(&e);
        std::sort(order.begin(), order.end(),
                  [](const auto* a, const auto* b) { return a->localHeaderOffset < b->localHeaderOffset; });

        std::unordered_set<std::string> listed;
        std::vector<std::pair<std::string, uint32_t>> modes;   // streamed files whose mode is not the default
        size_t fromFile = 0;
        for (const Core::ZipArchive::Entry* e : order) {
            const std::string rel = FileOps::RelativePath(e->name);
            if (rel.empty()) {
                Log::Warning("[Update] skipping zip entry outside the install: %s", e->name.c_str());
                continue;
            }
            listed.insert(rel);
            std::error_code ec;
            if (e->IsDirectory() || e->name.back() == '\\') {
                fs::create_directories(m_dest / rel, ec);
                continue;
            }
            const auto it = written.find(rel);
            if (it != written.end() && it->second.crc32 == e->crc32 && it->second.size == e->size) {
                const uint32_t mode = e->UnixMode() & 0777;
                if (mode != 0 && mode != 0644) modes.emplace_back(rel, mode);
                continue;
            }
            Writer::Job job;
            if (!zip.ReadCompressed(*e, job.compressed)) {
                error = zip.Error();
                m_writer.Wait();
                return false;
            }
            job.path = m_dest / rel;
            job.key = rel;
            job.name = e->name;
            job.method = e->method;
            job.crc32 = e->crc32;
            job.size = e->size;
            job.unixMode = e->UnixMode();
            m_writer.Submit(std::move(job));
            fromFile++;
        }
        m_writer.Wait();
        if (m_writer.Failures() > streamFailures) {
            error = m_writer.FirstError();
            return false;
        }

        std::error_code ec;
        for (const auto& [rel, w] : written) {
            if (!listed.count(rel)) fs::remove(m_dest / rel, ec);   // in the stream, not in the directory
        }
#ifndef _WIN32
        for (const auto& [rel, mode] : modes) {
            fs::permissions(m_dest / rel, static_cast<fs::perms>(mode), fs::perm_options::replace, ec);
        }
#endif
        if (streamed || fromFile) {
            Log::Info("[Update] extracted: %zu files while downloading, %zu from the zip", written.size(), fromFile);
        }
        return true;
    }

    bool ExtractAll(const fs::path& zipPath, const fs::path& destDir,
                    std::vector<Core::ZipArchive::Entry>& entries, std::string& error) {
        StreamingExtractor extractor(destDir);
        return extractor.Finish(zipPath, entries, error);
    }

} // namespace Launcher::Extract
