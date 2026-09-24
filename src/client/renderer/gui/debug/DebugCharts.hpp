// File: src/client/renderer/gui/debug/DebugCharts.hpp
//
// The F3+1/2/3 charts — a port of MC's client/gui/components/debugchart
// (AbstractDebugChart, FpsDebugChart, TpsDebugChart, PingDebugChart,
// BandwidthDebugChart, ProfilerPieChart) and util/debugchart
// (LocalSampleLogger, SampleStorage, TpsDebugDimensions).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Render {

    class GuiGraphics;

    namespace DebugScreen {

        // MC TpsDebugDimensions.
        enum TpsDimension : int { FULL_TICK = 0, TICK_SERVER_METHOD = 1, SCHEDULED_TASKS = 2, IDLE = 3, TPS_DIMENSIONS = 4 };

        // MC LocalSampleLogger: a 240-sample ring of N-dimensional samples.
        class LocalSampleLogger {
        public:
            static constexpr int CAPACITY = 240;

            explicit LocalSampleLogger(int dimensions);

            void LogFullSample(const int64_t* sample, int count);
            void LogSample(int64_t sample);                       // dimension 0
            void LogPartialSample(int64_t sample, int dimension); // dimensions 1..n-1, kept until the next LogSample

            int  Capacity() const { return CAPACITY; }
            int  Size() const { return m_size; }
            int64_t Get(int index) const { return Get(index, 0); }
            int64_t Get(int index, int dimension) const;
            void Reset() { m_start = 0; m_size = 0; }

        private:
            void UseSample();
            int  Wrap(int i) const { return i % CAPACITY; }

            int m_dimensions;
            std::vector<int64_t> m_sample;                    // the one being assembled
            std::vector<int64_t> m_samples;                   // CAPACITY * dimensions
            int m_start = 0;
            int m_size = 0;
        };

        // MC AbstractDebugChart.
        class AbstractDebugChart {
        public:
            static constexpr int CHART_HEIGHT = 60;
            static constexpr int LINE_WIDTH = 1;

            explicit AbstractDebugChart(const LocalSampleLogger& storage) : m_storage(storage) {}
            virtual ~AbstractDebugChart() = default;

            int  GetWidth(int maxWidth) const;
            int  GetFullHeight() const { return CHART_HEIGHT + 9; }
            void Render(GuiGraphics& g, int left, int width, int bottom);

        protected:
            virtual void RenderSampleBars(GuiGraphics& g, int bottom, int x, int sampleIndex);
            virtual void RenderMainSampleBar(GuiGraphics& g, int bottom, int x, int sampleIndex);
            virtual void RenderAdditionalSampleBars(GuiGraphics&, int, int, int) {}
            virtual int64_t GetValueForAggregation(int sampleIndex) const { return m_storage.Get(sampleIndex); }
            virtual void RenderAdditionalLinesAndLabels(GuiGraphics&, int, int, int) {}
            void RenderStringWithShade(GuiGraphics& g, const std::string& s, int x, int y);

            virtual std::string ToDisplayString(double sample) const = 0;
            virtual int  GetSampleHeight(double sample) const = 0;
            virtual uint32_t GetSampleColor(int64_t sample) const = 0;
            uint32_t GetSampleColor(double sample, double min, uint32_t minColor,
                                    double mid, uint32_t midColor, double max, uint32_t maxColor) const;

            const LocalSampleLogger& m_storage;
        };

        class FpsDebugChart : public AbstractDebugChart {
        public:
            using AbstractDebugChart::AbstractDebugChart;
            // The framerate-limit line needs the option; supplied by the overlay.
            void SetFramerateLimit(int limit) { m_framerateLimit = limit; }
        protected:
            void RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int width, int bottom) override;
            std::string ToDisplayString(double nanos) const override;
            int  GetSampleHeight(double nanos) const override;
            uint32_t GetSampleColor(int64_t nanos) const override;
        private:
            int m_framerateLimit = 0;
        };

        class TpsDebugChart : public AbstractDebugChart {
        public:
            TpsDebugChart(const LocalSampleLogger& storage, std::function<float()> msptSupplier)
                : AbstractDebugChart(storage), m_mspt(std::move(msptSupplier)) {}
        protected:
            void RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int width, int bottom) override;
            void RenderAdditionalSampleBars(GuiGraphics& g, int bottom, int x, int sampleIndex) override;
            int64_t GetValueForAggregation(int sampleIndex) const override;
            std::string ToDisplayString(double nanos) const override;
            int  GetSampleHeight(double nanos) const override;
            uint32_t GetSampleColor(int64_t nanos) const override;
        private:
            std::function<float()> m_mspt;
        };

        class PingDebugChart : public AbstractDebugChart {
        public:
            using AbstractDebugChart::AbstractDebugChart;
        protected:
            void RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int width, int bottom) override;
            std::string ToDisplayString(double millis) const override;
            int  GetSampleHeight(double millis) const override;
            uint32_t GetSampleColor(int64_t millis) const override;
        };

        class BandwidthDebugChart : public AbstractDebugChart {
        public:
            using AbstractDebugChart::AbstractDebugChart;
        protected:
            void RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int width, int bottom) override;
            std::string ToDisplayString(double bytesPerTick) const override;
            int  GetSampleHeight(double bytesPerTick) const override;
            uint32_t GetSampleColor(int64_t bytesPerTick) const override;
        private:
            void RenderLabeledLineAtValue(GuiGraphics& g, int left, int width, int bottom, int bytesPerSecond);
        };

        // ── Profiler pie chart (F3+1) ────────────────────────────────────
        // MC ProfileResults / ResultField, reduced to what the frame profile
        // here can supply: a tree of named timings under "root".
        struct ResultField {
            std::string name;
            double percentage = 0.0;         // of the parent
            double globalPercentage = 0.0;   // of the root
            uint32_t Color() const;          // MC ResultField.getColor: hash-derived
        };

        // A named node with a self time and children — assembled from the
        // frame's phase timers by the overlay each frame.
        struct ProfileNode {
            std::string name;
            double timeMs = 0.0;
            std::vector<ProfileNode> children;
        };

        class ProfileResults {
        public:
            void SetRoot(ProfileNode root) { m_root = std::move(root); }
            // MC ProfileResults.getTimes(path): the node itself first (with
            // its global %), then its children by time, plus "unspecified".
            std::vector<ResultField> GetTimes(const std::string& path) const;
            bool Empty() const { return m_root.name.empty(); }
        private:
            const ProfileNode* Find(const std::string& path) const;
            ProfileNode m_root;
        };

        class ProfilerPieChart {
        public:
            static constexpr int RADIUS = 105;
            static constexpr int PIE_CHART_THICKNESS = 10;

            void SetPieChartResults(const ProfileResults* results) { m_results = results; }
            void SetBottomOffset(int bottomOffset) { m_bottomOffset = bottomOffset; }
            void Render(GuiGraphics& g, int scaledScreenWidth, int scaledScreenHeight);
            void KeyPress(int key);   // 0 = up one level, 1..9 = descend

        private:
            std::vector<std::string> SplitNodeName(GuiGraphics& g, const std::string& nodeName,
                                                   int firstLineMaxWidth, int maxWidth) const;
            void RenderPie(GuiGraphics& g, const std::vector<ResultField>& list, int x0, int y0, int x1, int y1);

            const ProfileResults* m_results = nullptr;
            std::string m_treePath = "root";
            int m_bottomOffset = 0;
        };

    } // namespace DebugScreen
} // namespace Render
