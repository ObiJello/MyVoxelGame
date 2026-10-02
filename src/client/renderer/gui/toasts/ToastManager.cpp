// File: src/client/renderer/gui/toasts/ToastManager.cpp
#include "ToastManager.hpp"

#include "client/renderer/gui/GuiGraphics.hpp"
#include "client/sound/ClientSounds.hpp"

#include "common/text/Language.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <string>

namespace Render {

    namespace {

        int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        constexpr int64_t kSlideAnimationMs = 600;   // ToastInstance.SLIDE_ANIMATION_DURATION_MS

        const char* VisibilitySound(Toast::Visibility visibility) {
            return visibility == Toast::Visibility::Show ? "ui.toast.in" : "ui.toast.out";
        }

        uint32_t WithAlpha(int alpha, uint32_t rgb) {
            return (static_cast<uint32_t>(std::clamp(alpha, 0, 255)) << 24) | (rgb & 0xFFFFFFu);
        }

    } // namespace

    // ── Text wrapping ────────────────────────────────────────────────────

    std::vector<std::string> WrapText(const GuiGraphics& g, const std::string& text, int maxWidth) {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= text.size()) {
            const size_t newline = text.find('\n', start);
            const std::string paragraph = text.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
            // Greedy fill, breaking after the last space that still fits.
            size_t pos = 0;
            while (pos < paragraph.size() || (pos == 0 && paragraph.empty())) {
                if (paragraph.empty()) { lines.emplace_back(); break; }
                size_t end = pos;
                size_t lastSpace = std::string::npos;
                while (end < paragraph.size()) {
                    // Whole UTF-8 sequences only.
                    size_t next = end + 1;
                    while (next < paragraph.size() && (static_cast<unsigned char>(paragraph[next]) & 0xC0) == 0x80) ++next;
                    if (g.GetStringWidth(paragraph.substr(pos, next - pos)) > maxWidth) break;
                    if (paragraph[end] == ' ') lastSpace = end;
                    end = next;
                }
                if (end >= paragraph.size()) {
                    lines.push_back(paragraph.substr(pos));
                    break;
                }
                if (lastSpace != std::string::npos && lastSpace > pos) {
                    lines.push_back(paragraph.substr(pos, lastSpace - pos));
                    pos = lastSpace + 1;
                } else {
                    if (end == pos) {   // one glyph wider than the line
                        ++end;
                        while (end < paragraph.size() && (static_cast<unsigned char>(paragraph[end]) & 0xC0) == 0x80) ++end;
                    }
                    lines.push_back(paragraph.substr(pos, end - pos));
                    pos = end;
                }
            }
            if (newline == std::string::npos) break;
            start = newline + 1;
        }
        return lines;
    }

    // ── AdvancementToast ─────────────────────────────────────────────────

    AdvancementToast::AdvancementToast(Game::Advancements::DisplayInfo display) : m_display(std::move(display)) {}

    void AdvancementToast::Update(ToastManager& manager, int64_t fullyVisibleForMs) {
        m_wantedVisibility = static_cast<double>(fullyVisibleForMs) >=
                                     static_cast<double>(kDisplayTimeMs) * manager.NotificationDisplayTimeMultiplier()
                                 ? Visibility::Hide
                                 : Visibility::Show;
    }

    const char* AdvancementToast::GetSoundEvent() const {
        return m_display.type == Game::Advancements::FrameType::Challenge ? "ui.toast.challenge_complete" : nullptr;
    }

    void AdvancementToast::Render(GuiGraphics& g, int x, int y, int64_t fullyVisibleForMs) {
        g.BlitSprite("toast/advancement", x, y, Width(), Height());
        const std::vector<std::string> lines = WrapText(g, Game::Text::GetString(m_display.title), 125);
        // -30465 (0xFF88FF, pink) for a challenge, -256 (0xFFFF00, yellow) otherwise.
        const uint32_t titleRgb = m_display.type == Game::Advancements::FrameType::Challenge ? 0xFF88FFu : 0xFFFF00u;
        const std::string header = Game::Language::Get(std::string("advancements.toast.") +
                                                       Game::Advancements::FrameName(m_display.type));
        if (lines.size() == 1) {
            g.DrawString(header, x + 30, y + 7, WithAlpha(255, titleRgb), false);
            g.DrawString(lines[0], x + 30, y + 18, 0xFFFFFFFFu, false);
        } else if (fullyVisibleForMs < 1500) {
            const float t = std::clamp(static_cast<float>(1500 - fullyVisibleForMs) / 300.0f, 0.0f, 1.0f);
            const int alpha = static_cast<int>(std::floor(t * 255.0f));
            g.DrawString(header, x + 30, y + 11, WithAlpha(alpha, titleRgb), false);
        } else {
            const float t = std::clamp(static_cast<float>(fullyVisibleForMs - 1500) / 300.0f, 0.0f, 1.0f);
            const int alpha = static_cast<int>(std::floor(t * 252.0f));
            int lineY = Height() / 2 - static_cast<int>(lines.size()) * 9 / 2;
            for (const std::string& line : lines) {
                g.DrawString(line, x + 30, y + lineY, WithAlpha(alpha, 0xFFFFFFu), false);
                lineY += 9;
            }
        }
        g.RenderItem(m_display.icon, x + 8, y + 8);
    }

    // ── ToastManager ─────────────────────────────────────────────────────

    ToastManager& GetToastManager() {
        static ToastManager s_instance;
        return s_instance;
    }

    int ToastManager::FindFreeSlotsIndex(int requiredCount) const {
        if (FreeSlotCount() < requiredCount) return -1;
        int consecutive = 0;
        for (int i = 0; i < kSlotCount; ++i) {
            if (m_occupiedSlots.test(static_cast<size_t>(i))) {
                consecutive = 0;
            } else if (++consecutive == requiredCount) {
                return i + 1 - consecutive;
            }
        }
        return -1;
    }

    void ToastManager::UpdateInstance(Instance& instance, int64_t now) {
        // ToastInstance.update.
        if (instance.animationStartTime == -1) {
            instance.animationStartTime = now;
            instance.visibility = Toast::Visibility::Show;
        }
        if (instance.visibility == Toast::Visibility::Show && now - instance.animationStartTime <= kSlideAnimationMs) {
            instance.becameFullyVisibleAt = now;
        }
        instance.fullyVisibleFor = now - instance.becameFullyVisibleAt;
        float progress = std::clamp(static_cast<float>(now - instance.animationStartTime) /
                                        static_cast<float>(kSlideAnimationMs), 0.0f, 1.0f);
        progress *= progress;
        instance.visiblePortion = instance.visibility == Toast::Visibility::Hide ? 1.0f - progress : progress;
        instance.toast->Update(*this, instance.fullyVisibleFor);
        const Toast::Visibility wanted = instance.toast->GetWantedVisibility();
        if (wanted != instance.visibility) {
            instance.animationStartTime = now - static_cast<int64_t>((1.0f - instance.visiblePortion) *
                                                                    static_cast<float>(kSlideAnimationMs));
            instance.visibility = wanted;
        }
        instance.hasFinishedRendering = instance.visibility == Toast::Visibility::Hide &&
                                        now - instance.animationStartTime > kSlideAnimationMs;
    }

    void ToastManager::Update() {
        const int64_t now = NowMs();
        bool visibilityChangeSoundPlayed = false;
        for (auto it = m_visible.begin(); it != m_visible.end();) {
            const Toast::Visibility previous = it->visibility;
            UpdateInstance(*it, now);
            if (it->visibility != previous && !visibilityChangeSoundPlayed) {
                visibilityChangeSoundPlayed = true;
                Client::Sounds::PlayUI(VisibilitySound(it->visibility), 1.0f, 1.0f);
            }
            if (it->hasFinishedRendering) {
                for (int i = 0; i < it->occupiedSlotCount; ++i) {
                    m_occupiedSlots.reset(static_cast<size_t>(it->firstSlotIndex + i));
                }
                it = m_visible.erase(it);
            } else {
                ++it;
            }
        }
        if (!m_queued.empty() && FreeSlotCount() > 0) {
            std::set<std::string> playedSounds;
            for (auto it = m_queued.begin(); it != m_queued.end();) {
                const int count = (*it)->OccupiedSlotCount();
                const int first = FindFreeSlotsIndex(count);
                if (first == -1) { ++it; continue; }
                Instance instance;
                instance.toast = std::move(*it);
                instance.firstSlotIndex = first;
                instance.occupiedSlotCount = count;
                for (int i = 0; i < count; ++i) m_occupiedSlots.set(static_cast<size_t>(first + i));
                if (const char* sound = instance.toast->GetSoundEvent(); sound && playedSounds.insert(sound).second) {
                    Client::Sounds::PlayUI(sound, 1.0f, 1.0f);
                }
                m_visible.push_back(std::move(instance));
                it = m_queued.erase(it);
            }
        }
    }

    void ToastManager::Render(GuiGraphics& g) {
        if (m_visible.empty()) return;
        g.NextStratum();
        const int screenWidth = g.GuiWidth();
        for (Instance& instance : m_visible) {
            if (instance.hasFinishedRendering) continue;
            // Toast.xPos / yPos.
            const float x = static_cast<float>(screenWidth) -
                            static_cast<float>(instance.toast->Width()) * instance.visiblePortion;
            const int y = instance.firstSlotIndex * instance.toast->Height();
            instance.toast->Render(g, static_cast<int>(std::lround(x)), y, instance.fullyVisibleFor);
        }
    }

    void ToastManager::AddToast(std::unique_ptr<Toast> toast) {
        if (toast) m_queued.push_back(std::move(toast));
    }

    void ToastManager::Clear() {
        m_occupiedSlots.reset();
        m_visible.clear();
        m_queued.clear();
    }

} // namespace Render
