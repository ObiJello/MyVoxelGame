// File: src/client/renderer/gui/BossBarState.hpp
//
// The client's boss bars — MC BossHealthOverlay's `events` map of
// LerpingBossEvent, keyed here by the BossEventS2C bar id (0: the dragon
// fight's and the Hush bosses' bar; 1..255: one per wither) in insertion
// order (MC's LinkedHashMap), plus GameRenderer's bossOverlayWorldDarkening
// clock that a darken-screen bar (the wither's) drives. Written by
// ClientPacketHandler::onBossEventS2C and ticked from the client tick (main
// thread); read by HudRenderer::RenderBossBar, the lightmap and the fog
// colour (also main thread) — the same single-threaded contract as the rest
// of the HUD state.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Client {

    struct BossBarState {
        float       progress = 1.0f;
        // BossEventS2CPacket::Color ordinal — doubles as the sprite prefix
        // under assets/textures/gui/sprites/boss_bar/ (pink, blue, ...).
        uint8_t     color = 0;
        // 0 = smooth bar, else the notch count (6/10/12/20).
        uint8_t     notches = 0;
        std::string name;
        // MC BossEvent.darkenScreen / playBossMusic / createWorldFog.
        bool        darkenScreen = false;
        bool        playBossMusic = false;
        bool        createWorldFog = false;
    };

    class BossBarOverlay {
    public:
        // ClientboundBossEventPacket ADD: (re)places the bar with this id —
        // a re-add keeps its place in the order, as MC's map put does.
        void Add(uint8_t id, BossBarState state) {
            if (BossBarState* bar = Find(id)) { *bar = std::move(state); return; }
            m_bars.emplace_back(id, std::move(state));
        }
        void Remove(uint8_t id) {
            m_bars.erase(std::remove_if(m_bars.begin(), m_bars.end(),
                                        [id](const auto& entry) { return entry.first == id; }),
                         m_bars.end());
        }
        BossBarState* Find(uint8_t id) {
            for (auto& entry : m_bars) if (entry.first == id) return &entry.second;
            return nullptr;
        }
        // MC BossHealthOverlay.reset — a new level or a disconnect.
        void Clear() { m_bars.clear(); }

        bool Empty() const { return m_bars.empty(); }
        const std::vector<std::pair<uint8_t, BossBarState>>& Bars() const { return m_bars; }

        // MC BossHealthOverlay.shouldPlayMusic / shouldDarkenScreen /
        // shouldCreateWorldFog: any bar asking for it.
        bool ShouldPlayMusic() const {
            for (const auto& entry : m_bars) if (entry.second.playBossMusic) return true;
            return false;
        }
        bool ShouldDarkenScreen() const {
            for (const auto& entry : m_bars) if (entry.second.darkenScreen) return true;
            return false;
        }
        bool ShouldCreateWorldFog() const {
            for (const auto& entry : m_bars) if (entry.second.createWorldFog) return true;
            return false;
        }

        // MC GameRenderer.tick's bossOverlayWorldDarkening: +0.05 a tick up
        // to 1 while a darken-screen bar shows, else -0.0125 a tick back.
        void Tick() {
            m_worldDarkeningO = m_worldDarkening;
            if (ShouldDarkenScreen()) {
                m_worldDarkening = std::min(1.0f, m_worldDarkening + 0.05f);
            } else if (m_worldDarkening > 0.0f) {
                m_worldDarkening -= 0.0125f;
            }
        }
        // MC GameRenderer.bossOverlayWorldDarkening(a): lerp(a, old, now).
        float WorldDarkening(float partialTick) const {
            return m_worldDarkeningO + (m_worldDarkening - m_worldDarkeningO) * partialTick;
        }

    private:
        std::vector<std::pair<uint8_t, BossBarState>> m_bars;
        float m_worldDarkening = 0.0f;
        float m_worldDarkeningO = 0.0f;
    };

    // Defined in ClientPacketHandler.cpp.
    extern BossBarOverlay g_bossBars;

} // namespace Client
