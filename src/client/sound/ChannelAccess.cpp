// File: src/client/sound/ChannelAccess.cpp
#include "client/sound/ChannelAccess.hpp"

#include <utility>

namespace Client {

    void ChannelHandle::Execute(std::function<void(Audio::Channel&)> action) {
        std::shared_ptr<ChannelHandle> self = shared_from_this();
        m_owner.Executor().Execute([self, action = std::move(action)] {
            if (self->m_channel) action(*self->m_channel);
        });
    }

    void ChannelHandle::Release() {
        m_owner.Library().ReleaseChannel(m_pool, std::move(m_channel));
        m_channel.reset();
        m_stopped.store(true, std::memory_order_release);
    }

    std::shared_ptr<ChannelHandle> ChannelAccess::CreateHandle(Audio::Library::Pool pool) {
        if (!m_library.TryReserve(pool)) return nullptr;
        auto handle = std::make_shared<ChannelHandle>(*this, pool);
        m_executor.Execute([this, handle, pool] {
            handle->m_channel = m_library.CreateReserved(pool);
            if (handle->m_channel) {
                m_channels.push_back(handle);
            } else {
                // No AL source to give (the reservation is already returned):
                // the handle reads stopped and nothing it is asked runs.
                handle->m_stopped.store(true, std::memory_order_release);
            }
        });
        return handle;
    }

    void ChannelAccess::ExecuteOnChannels(std::function<void(Audio::Channel&)> action) {
        m_executor.Execute([this, action = std::move(action)] {
            for (const auto& handle : m_channels) {
                if (handle->m_channel) action(*handle->m_channel);
            }
        });
    }

    void ChannelAccess::ScheduleTick() {
        m_executor.Execute([this] {
            for (size_t i = 0; i < m_channels.size();) {
                ChannelHandle& handle = *m_channels[i];
                if (handle.m_channel) handle.m_channel->UpdateStream();
                if (!handle.m_channel || handle.m_channel->Stopped()) {
                    handle.Release();
                    m_channels[i] = std::move(m_channels.back());
                    m_channels.pop_back();
                } else {
                    ++i;
                }
            }
        });
    }

    void ChannelAccess::Clear() {
        for (const auto& handle : m_channels) handle->Release();
        m_channels.clear();
        // Creations still queued when the executor was shut down were dropped
        // with it; their pool reservations go with them.
        m_library.ResetReservations();
    }

} // namespace Client
