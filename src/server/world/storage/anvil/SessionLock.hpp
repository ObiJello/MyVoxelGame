// File: src/server/world/storage/anvil/SessionLock.hpp
//
// session.lock — the file that stops two processes writing one world.
//
// Vanilla writes exactly three bytes (U+2603 SNOWMAN, E2 98 83) and holds an
// exclusive lock on the file for as long as the world is open. Minecraft
// refuses to open a world whose lock it cannot take, and so do we: two writers
// interleaving sector allocations in the same region file would corrupt it in
// a way no amount of care elsewhere can prevent.
//
// Takes a SaveRoot, so it can only ever be created for a world we are allowed
// to write. An imported read-only world gets NO lock — which means ObeyCraft
// can open a world Minecraft already has open, and that is fine, because it
// will not write to it.
#pragma once

#include "server/world/storage/anvil/SaveRoot.hpp"

#include <memory>
#include <string>

namespace Game::Anvil {

    class SessionLock {
    public:
        // Returns null when the lock is already held — `error` then carries a
        // message suitable for showing the player, matching MC's wording.
        static std::unique_ptr<SessionLock> Acquire(const SaveRoot& root, std::string& error);

        ~SessionLock();

        SessionLock(const SessionLock&)            = delete;
        SessionLock& operator=(const SessionLock&) = delete;

    private:
        SessionLock() = default;

        // Platform handle: a file descriptor on POSIX, a HANDLE on Windows.
        // Kept open for the whole session — releasing it IS releasing the lock.
        void* m_handle = nullptr;
        int   m_fd     = -1;
    };

} // namespace Game::Anvil
