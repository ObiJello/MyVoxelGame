// File: src/client/renderer/gui/toasts/ToastManager.hpp
//
// MC ToastManager + Toast + AdvancementToast: the notifications that slide in
// at the top right, above the HUD and any open screen.
//
// Five 32-pixel slots; a queued toast takes the first run of free slots it
// fits, slides in over 600 ms (eased by the square of the progress), stays
// until it asks to hide, slides out, and frees its slots. The first toast to
// change visibility in an update plays ui.toast.in / ui.toast.out; a toast's
// own sound (the challenge fanfare) plays once as it is shown.
//
// AdvancementToast: the "toast/advancement" sprite, the frame's header
// ("Advancement Made!" yellow, "Challenge Complete!" pink) over the title —
// or, for a title that needs two lines, the header for 1.5 s fading into the
// wrapped title — and the icon at (8, 8); five seconds on screen.
#pragma once

#include "common/advancements/Advancement.hpp"

#include <bitset>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;
    class ToastManager;

    class Toast {
    public:
        enum class Visibility { Show, Hide };
        static constexpr int kDefaultWidth = 160;
        static constexpr int kSlotHeight   = 32;

        virtual ~Toast() = default;
        virtual Visibility GetWantedVisibility() const = 0;
        virtual void Update(ToastManager& manager, int64_t fullyVisibleForMs) = 0;
        // The sound played once when the toast is first shown, or null.
        virtual const char* GetSoundEvent() const { return nullptr; }
        // Draws with the toast's top-left at (x, y).
        virtual void Render(GuiGraphics& g, int x, int y, int64_t fullyVisibleForMs) = 0;
        virtual int Width() const { return kDefaultWidth; }
        virtual int Height() const { return kSlotHeight; }
        int OccupiedSlotCount() const { return (Height() + kSlotHeight - 1) / kSlotHeight; }
    };

    class AdvancementToast : public Toast {
    public:
        static constexpr int64_t kDisplayTimeMs = 5000;

        explicit AdvancementToast(Game::Advancements::DisplayInfo display);

        Visibility GetWantedVisibility() const override { return m_wantedVisibility; }
        void Update(ToastManager& manager, int64_t fullyVisibleForMs) override;
        const char* GetSoundEvent() const override;
        void Render(GuiGraphics& g, int x, int y, int64_t fullyVisibleForMs) override;

    private:
        Game::Advancements::DisplayInfo m_display;
        Visibility m_wantedVisibility = Visibility::Hide;
    };

    class ToastManager {
    public:
        static constexpr int kSlotCount = 5;

        // Advance every toast's animation; place queued toasts in free slots.
        void Update();
        // Draw the visible toasts against the right edge of the GUI.
        void Render(GuiGraphics& g);
        void AddToast(std::unique_ptr<Toast> toast);
        void Clear();
        // MC options.notificationDisplayTime (1.0 here: no such option yet).
        double NotificationDisplayTimeMultiplier() const { return 1.0; }

    private:
        struct Instance {
            std::unique_ptr<Toast> toast;
            int firstSlotIndex = 0;
            int occupiedSlotCount = 1;
            int64_t animationStartTime = -1;
            int64_t becameFullyVisibleAt = -1;
            Toast::Visibility visibility = Toast::Visibility::Hide;
            int64_t fullyVisibleFor = 0;
            float visiblePortion = 0.0f;
            bool hasFinishedRendering = false;
        };

        void UpdateInstance(Instance& instance, int64_t now);
        int FindFreeSlotsIndex(int requiredCount) const;
        int FreeSlotCount() const { return kSlotCount - static_cast<int>(m_occupiedSlots.count()); }

        std::vector<Instance> m_visible;
        std::bitset<kSlotCount> m_occupiedSlots;
        std::deque<std::unique_ptr<Toast>> m_queued;
    };

    ToastManager& GetToastManager();

    // MC StringSplitter.splitLines for one plain string: breaks after the
    // last space that fits `maxWidth` (or mid-word when none does). Explicit
    // newlines break too.
    std::vector<std::string> WrapText(const GuiGraphics& g, const std::string& text, int maxWidth);

} // namespace Render
