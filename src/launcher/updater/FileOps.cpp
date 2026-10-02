// File: src/launcher/updater/FileOps.cpp
#include "launcher/updater/FileOps.hpp"
#include "common/core/Log.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef _WIN32
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

namespace Launcher::FileOps {

    std::string RelativePath(std::string_view entryName) {
        std::string out;
        out.reserve(entryName.size());
        size_t i = 0;
        const size_t n = entryName.size();
        if (n > 0 && (entryName[0] == '/' || entryName[0] == '\\')) return "";
        while (i < n) {
            size_t j = i;
            while (j < n && entryName[j] != '/' && entryName[j] != '\\') j++;
            const std::string_view part = entryName.substr(i, j - i);
            if (part == "..") return "";
            // ':' would be a drive ("C:") or an NTFS stream ("file:stream").
            if (part.find(':') != std::string_view::npos) return "";
            if (!part.empty() && part != ".") {
                if (!out.empty()) out += '/';
                out.append(part);
            }
            i = j + 1;
        }
        return out;
    }

    std::string ContentPrefix(const std::vector<Core::ZipArchive::Entry>& entries) {
        std::string top;
        bool isDirectory = false;
        for (const auto& e : entries) {
            const std::string rel = RelativePath(e.name);
            if (rel.empty()) continue;
            const size_t slash = rel.find('/');
            const std::string first = rel.substr(0, slash);
            if (top.empty()) {
                top = first;
            } else if (first != top) {
                return "";
            }
            if (slash != std::string::npos || e.IsDirectory()) isDirectory = true;
        }
        if (top.empty() || !isDirectory || top.find(".app") != std::string::npos) return "";
        return top + "/";
    }

    std::string StripPrefix(const std::string& relative, const std::string& prefix) {
        if (prefix.empty()) return relative;
        if (relative.size() <= prefix.size() || relative.compare(0, prefix.size(), prefix) != 0) return "";
        return relative.substr(prefix.size());
    }

    bool WriteFile(const fs::path& path, const uint8_t* data, size_t size, uint32_t unixMode) {
#ifdef _WIN32
        (void)unixMode;
        std::error_code ec;
        fs::remove(path, ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
#else
        ::unlink(path.c_str());
        const mode_t perms = (unixMode & 0777) ? static_cast<mode_t>(unixMode & 0777) : 0644;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, perms);
        if (fd < 0) return false;
        size_t done = 0;
        while (done < size) {
            const ssize_t w = ::write(fd, data + done, size - done);
            if (w < 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                return false;
            }
            done += static_cast<size_t>(w);
        }
        // open() is subject to the umask; the archive's bits are what ship.
        if (unixMode & 0777) ::fchmod(fd, perms);
        return ::close(fd) == 0;
#endif
    }

    bool CloneTree(const fs::path& src, const fs::path& dst) {
        std::error_code ec;
#ifdef __APPLE__
        if (::clonefile(src.c_str(), dst.c_str(), CLONE_NOFOLLOW) == 0) return true;
        Log::Warning("[Update] clonefile %s failed (%s); linking instead", src.string().c_str(), std::strerror(errno));
        fs::remove_all(dst, ec);
#endif
        fs::create_directories(dst, ec);
        if (ec) return false;
        for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            const fs::path to = dst / it->path().lexically_relative(src);
            const fs::file_status st = it->symlink_status(ec);
            if (ec) return false;
            if (fs::is_directory(st)) {
                fs::create_directory(to, ec);
            } else if (fs::is_symlink(st)) {
                fs::copy_symlink(it->path(), to, ec);
            } else {
                fs::create_hard_link(it->path(), to, ec);
                if (ec) {
                    ec.clear();
                    fs::copy_file(it->path(), to, ec);
                }
            }
            if (ec) {
                Log::Error("[Update] cannot copy %s: %s", it->path().string().c_str(), ec.message().c_str());
                return false;
            }
        }
        return !ec;
    }

    namespace {

        class Deleter {
        public:
            static Deleter& Get() {
                static Deleter d;
                return d;
            }
            ~Deleter() { Shutdown(); }

            void Queue(fs::path p) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_stopped) return;   // left for the next launch's sweep
                m_queue.push_back(std::move(p));
                if (!m_thread.joinable()) m_thread = std::thread([this] { Run(); });
                m_cv.notify_one();
            }

            void Shutdown() {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stopped = true;
                    m_stop = true;
                }
                m_cv.notify_one();
                if (m_thread.joinable()) m_thread.join();
            }

        private:
            void Run() {
                for (;;) {
                    fs::path p;
                    {
                        std::unique_lock<std::mutex> lock(m_mutex);
                        m_cv.wait(lock, [this] { return m_stop.load() || !m_queue.empty(); });
                        if (m_stop) return;
                        p = std::move(m_queue.front());
                        m_queue.pop_front();
                    }
                    const auto start = std::chrono::steady_clock::now();
                    if (!DeleteTree(p)) return;   // stopped part-way
                    Log::Info("[Update] deleted %s in %.0f ms", p.filename().string().c_str(),
                              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                }
            }

            // Files first, checking for shutdown between them, then the (now
            // file-less) directories in one go.
            bool DeleteTree(const fs::path& root) {
                std::error_code ec;
                std::vector<fs::path> files;
                for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                     !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    if (!it->is_directory(ec) || it->is_symlink(ec)) files.push_back(it->path());
                }
                for (size_t i = 0; i < files.size(); i++) {
                    if ((i & 255) == 0 && m_stop) return false;
                    fs::remove(files[i], ec);
                }
                fs::remove_all(root, ec);
                return true;
            }

            std::mutex m_mutex;
            std::condition_variable m_cv;
            std::deque<fs::path> m_queue;
            std::thread m_thread;
            std::atomic<bool> m_stop{false};
            bool m_stopped = false;
        };

        fs::path TrashName(const fs::path& dir) {
            static std::atomic<uint32_t> counter{0};
            const auto now = std::chrono::system_clock::now().time_since_epoch().count();
            return dir / ("_trash_" + std::to_string(now) + "_" + std::to_string(counter++));
        }

    } // namespace

    void Discard(const fs::path& path) {
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(path, ec))) return;
        const fs::path trash = TrashName(path.parent_path());
        fs::rename(path, trash, ec);
        if (ec) {
            Log::Warning("[Update] cannot move %s aside (%s); deleting in place", path.string().c_str(),
                         ec.message().c_str());
            fs::remove_all(path, ec);
            return;
        }
        Deleter::Get().Queue(trash);
    }

    void SweepTrash(const fs::path& dir) {
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->path().filename().string().rfind("_trash_", 0) == 0) Deleter::Get().Queue(it->path());
        }
    }

    void ShutdownDeleter() { Deleter::Get().Shutdown(); }

    bool SwapIntoPlace(const fs::path& newContent, const fs::path& target, std::string& error) {
        std::error_code ec;
        fs::path old;
        if (fs::exists(target, ec)) {
            old = TrashName(target.parent_path());
            fs::rename(target, old, ec);
            if (ec) {
                error = "cannot move the old install aside: " + ec.message();
                return false;
            }
        }
        fs::create_directories(target.parent_path(), ec);
        fs::rename(newContent, target, ec);
        if (ec) {
            error = "cannot move the new install into place: " + ec.message();
            if (!old.empty()) {
                std::error_code rollback;
                fs::rename(old, target, rollback);
            }
            return false;
        }
        if (!old.empty()) Deleter::Get().Queue(old);
        return true;
    }

} // namespace Launcher::FileOps
