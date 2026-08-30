// File: src/server/world/storage/anvil/SessionLock.cpp
#include "server/world/storage/anvil/SessionLock.hpp"

#include "common/core/Log.hpp"

#include <cstring>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/file.h>
  #include <unistd.h>
#endif

namespace Game::Anvil {

    namespace {
        // U+2603 SNOWMAN, exactly as DirectoryLock writes it. The content is
        // not what locks the file — the OS lock is — but matching it keeps the
        // file identical to one Minecraft produced.
        constexpr unsigned char kSnowman[3] = {0xE2, 0x98, 0x83};
    }

    std::unique_ptr<SessionLock> SessionLock::Acquire(const SaveRoot& root, std::string& error) {
        const auto path = root.SessionLock();
        std::unique_ptr<SessionLock> lock(new SessionLock());

#if defined(_WIN32)
        HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                                      0 /* no sharing: this IS the lock */,
                                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            error = "This world is already open in another window.";
            return nullptr;
        }
        DWORD written = 0;
        ::WriteFile(handle, kSnowman, sizeof(kSnowman), &written, nullptr);
        ::FlushFileBuffers(handle);
        lock->m_handle = handle;
#else
        const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            error = std::string("Could not create session.lock: ") + std::strerror(errno);
            return nullptr;
        }
        // LOCK_NB so a held lock fails immediately rather than blocking the
        // whole startup on another process.
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            error = "This world is already open in another window.";
            return nullptr;
        }
        if (::write(fd, kSnowman, sizeof(kSnowman)) != static_cast<ssize_t>(sizeof(kSnowman))) {
            Log::Warning("[Anvil] could not write session.lock contents (the lock itself is held)");
        }
        ::fsync(fd);
        lock->m_fd = fd;
#endif
        error.clear();
        Log::Info("[Anvil] session lock held on %s", path.string().c_str());
        return lock;
    }

    SessionLock::~SessionLock() {
#if defined(_WIN32)
        if (m_handle) { ::CloseHandle(static_cast<HANDLE>(m_handle)); m_handle = nullptr; }
#else
        if (m_fd >= 0) {
            // Closing releases the flock. Deliberately NOT deleting the file:
            // vanilla leaves session.lock in place between sessions, and
            // removing it would make a copied world look different from one
            // Minecraft wrote.
            ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

} // namespace Game::Anvil
