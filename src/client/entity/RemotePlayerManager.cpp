// File: src/client/entity/RemotePlayerManager.cpp
#include "RemotePlayerManager.hpp"
#include "common/entity/SpearItem.hpp"

namespace Client {

    int RemotePlayerManager::SwingDurationOf(Game::ItemID item) {
        return Game::Spear::AttackAnimationDuration(item);
    }
    std::unique_ptr<RemotePlayerManager> g_remotePlayerManager = nullptr;
} // namespace Client
