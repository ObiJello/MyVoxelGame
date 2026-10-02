// File: src/launcher/updater/Extractor.hpp
//
// Zip extraction for the launcher's installs, on worker threads.
//
// A release zip is ~30k files, and writing them one after another on one
// thread was the bulk of an install (file creation, not inflation, is the
// cost). Writer inflates and writes entries in parallel; StreamingExtractor
// feeds it from the download as the bytes arrive, so a full install finishes
// moments after its download does; ExtractAll is the same over a zip already
// on disk.
#pragma once

#include "common/core/ZipArchive.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Launcher::Extract {

    namespace fs = std::filesystem;

    // Inflates entries and writes them out on worker threads. Submit blocks
    // while more than kMaxQueuedBytes of compressed data are waiting, so a
    // producer faster than the disk holds back instead of buffering the
    // archive in memory.
    class Writer {
    public:
        struct Job {
            fs::path    path;          // destination file
            std::string key;           // what Written() files the result under
            std::string name;          // entry name, for errors
            uint16_t    method = 0;
            uint32_t    crc32 = 0;
            uint64_t    size = 0;
            uint32_t    unixMode = 0;
            std::vector<uint8_t> compressed;
        };
        struct Written {
            uint32_t crc32 = 0;
            uint64_t size = 0;
        };

        static constexpr uint64_t kMaxQueuedBytes = 192ull << 20;

        explicit Writer(int threads = 0);   // 0: by core count, 2..8
        ~Writer();
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        void Submit(Job job);
        // Blocks until every submitted job is done.
        void Wait();

        // After Wait: every job written, by key; failures and the first error.
        std::unordered_map<std::string, Written> TakeWritten();
        size_t Failures() const;
        std::string FirstError() const;

    private:
        void Run();
        bool EnsureParent(const fs::path& file);

        mutable std::mutex m_mutex;
        std::condition_variable m_work, m_space, m_idle;
        std::deque<Job> m_queue;
        uint64_t m_queuedBytes = 0;
        int m_active = 0;
        bool m_stop = false;
        std::vector<std::thread> m_threads;

        std::unordered_map<std::string, Written> m_written;
        size_t m_failures = 0;
        std::string m_firstError;

        std::mutex m_dirMutex;
        std::unordered_set<std::string> m_dirs;
    };

    // Extracts a zip from its bytes in order, as they arrive, into destDir
    // (entry names as they are; unwrapping a top-level folder is the caller's).
    // A zip whose entries put their sizes after the data (flag bit 3) cannot be
    // cut up before its central directory arrives; streaming stops there and
    // Finish extracts the rest from the file.
    class StreamingExtractor {
    public:
        explicit StreamingExtractor(fs::path destDir);

        // Download thread: the archive's next bytes.
        void Feed(const uint8_t* data, size_t size);

        // With the whole archive at zipPath: checks every streamed file against
        // the central directory, extracts whatever the stream did not deliver
        // (or delivered wrong), removes files the directory does not list, and
        // applies Unix modes. `entries` receives the central directory.
        bool Finish(const fs::path& zipPath, std::vector<Core::ZipArchive::Entry>& entries, std::string& error);

        size_t StreamedFiles() const { return m_streamedFiles; }
        uint64_t DirectoryBytes() const { return m_directoryBytes; }   // after Finish

    private:
        enum class State : uint8_t { Header, Data, Stopped };
        void Stop(const char* why);
        void EntryComplete();

        fs::path m_dest;
        Writer m_writer;
        State m_state = State::Header;
        std::vector<uint8_t> m_header;
        Core::ZipArchive::LocalHeader m_local;
        std::vector<uint8_t> m_data;
        uint64_t m_remaining = 0;
        size_t m_streamedFiles = 0;
        uint64_t m_directoryBytes = 0;
    };

    // Every entry of a zip on disk into destDir, read in file order.
    bool ExtractAll(const fs::path& zipPath, const fs::path& destDir,
                    std::vector<Core::ZipArchive::Entry>& entries, std::string& error);

} // namespace Launcher::Extract
