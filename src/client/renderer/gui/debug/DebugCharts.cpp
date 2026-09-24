// File: src/client/renderer/gui/debug/DebugCharts.cpp
#include "DebugCharts.hpp"
#include "../GuiGraphics.hpp"
#include "../GuiRenderState.hpp"
#include "../FontRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Render::DebugScreen {

    namespace {
        // MC ARGB.srgbLerp: per-channel lerp in gamma space (what the charts use).
        uint32_t SrgbLerp(float t, uint32_t a, uint32_t b) {
            auto ch = [&](int shift) {
                const float ca = static_cast<float>((a >> shift) & 0xFF);
                const float cb = static_cast<float>((b >> shift) & 0xFF);
                return static_cast<uint32_t>(std::lround(ca + (cb - ca) * t)) & 0xFF;
            };
            return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
        std::string Fmt(const char* fmt, double v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), fmt, v);
            return buf;
        }
        std::string FmtInt(const char* fmt, long long v) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), fmt, v);
            return buf;
        }
        constexpr uint32_t kTextColor  = 0xFFE0E0E0;   // -2039584
        constexpr uint32_t kShadeColor = 0x90505050;   // -1873784752
    }

    // ── LocalSampleLogger ───────────────────────────────────────────────

    LocalSampleLogger::LocalSampleLogger(int dimensions)
        : m_dimensions(std::max(1, dimensions)),
          m_sample(static_cast<size_t>(m_dimensions), 0),
          m_samples(static_cast<size_t>(CAPACITY) * m_dimensions, 0) {}

    void LocalSampleLogger::UseSample() {
        const int next = Wrap(m_start + m_size);
        std::copy(m_sample.begin(), m_sample.end(), m_samples.begin() + static_cast<size_t>(next) * m_dimensions);
        if (m_size < CAPACITY) ++m_size;
        else m_start = Wrap(m_start + 1);
        std::fill(m_sample.begin(), m_sample.end(), 0);
    }

    void LocalSampleLogger::LogFullSample(const int64_t* sample, int count) {
        for (int i = 0; i < std::min(count, m_dimensions); ++i) m_sample[static_cast<size_t>(i)] = sample[i];
        UseSample();
    }

    void LocalSampleLogger::LogSample(int64_t sample) {
        m_sample[0] = sample;
        UseSample();
    }

    void LocalSampleLogger::LogPartialSample(int64_t sample, int dimension) {
        if (dimension >= 1 && dimension < m_dimensions) m_sample[static_cast<size_t>(dimension)] = sample;
    }

    int64_t LocalSampleLogger::Get(int index, int dimension) const {
        if (index < 0 || index >= m_size || dimension < 0 || dimension >= m_dimensions) return 0;
        return m_samples[static_cast<size_t>(Wrap(m_start + index)) * m_dimensions + dimension];
    }

    // ── AbstractDebugChart ──────────────────────────────────────────────

    int AbstractDebugChart::GetWidth(int maxWidth) const {
        return std::min(m_storage.Capacity() + 2, maxWidth);
    }

    void AbstractDebugChart::Render(GuiGraphics& g, int left, int width, int bottom) {
        g.Fill(left, bottom - CHART_HEIGHT, left + width, bottom, kShadeColor);
        int64_t avg = 0, mn = INT64_MAX, mx = INT64_MIN;
        const int startIndex = std::max(0, m_storage.Capacity() - (width - 2));
        const int sampleCount = m_storage.Size() - startIndex;
        for (int i = 0; i < sampleCount; ++i) {
            const int x = left + i + 1;
            const int sampleIndex = startIndex + i;
            const int64_t v = GetValueForAggregation(sampleIndex);
            mn = std::min(mn, v);
            mx = std::max(mx, v);
            avg += v;
            RenderSampleBars(g, bottom, x, sampleIndex);
        }
        g.HLine(left, left + width - 1, bottom - CHART_HEIGHT, 0xFFFFFFFF);
        g.HLine(left, left + width - 1, bottom - 1, 0xFFFFFFFF);
        g.VLine(left, bottom - CHART_HEIGHT, bottom, 0xFFFFFFFF);
        g.VLine(left + width - 1, bottom - CHART_HEIGHT, bottom, 0xFFFFFFFF);
        if (sampleCount > 0) {
            const std::string minText = ToDisplayString(static_cast<double>(mn)) + " min";
            const std::string avgText = ToDisplayString(static_cast<double>(avg) / sampleCount) + " avg";
            const std::string maxText = ToDisplayString(static_cast<double>(mx)) + " max";
            const int y = bottom - CHART_HEIGHT - 9;
            g.DrawString(minText, left + 2, y, kTextColor, true);
            g.DrawCenteredString(avgText, left + width / 2, y, kTextColor);
            g.DrawString(maxText, left + width - g.GetStringWidth(maxText) - 2, y, kTextColor, true);
        }
        RenderAdditionalLinesAndLabels(g, left, width, bottom);
    }

    void AbstractDebugChart::RenderSampleBars(GuiGraphics& g, int bottom, int x, int sampleIndex) {
        RenderMainSampleBar(g, bottom, x, sampleIndex);
        RenderAdditionalSampleBars(g, bottom, x, sampleIndex);
    }

    void AbstractDebugChart::RenderMainSampleBar(GuiGraphics& g, int bottom, int x, int sampleIndex) {
        const int64_t v = m_storage.Get(sampleIndex);
        const int h = GetSampleHeight(static_cast<double>(v));
        g.Fill(x, bottom - h, x + 1, bottom, GetSampleColor(v));
    }

    void AbstractDebugChart::RenderStringWithShade(GuiGraphics& g, const std::string& s, int x, int y) {
        g.Fill(x, y, x + g.GetStringWidth(s) + 1, y + 9, kShadeColor);
        g.DrawString(s, x + 1, y + 1, kTextColor, false);
    }

    uint32_t AbstractDebugChart::GetSampleColor(double sample, double min, uint32_t minColor,
                                                double mid, uint32_t midColor, double max, uint32_t maxColor) const {
        sample = std::clamp(sample, min, max);
        return sample < mid ? SrgbLerp(static_cast<float>((sample - min) / (mid - min)), minColor, midColor)
                            : SrgbLerp(static_cast<float>((sample - mid) / (max - mid)), midColor, maxColor);
    }

    // ── FpsDebugChart ───────────────────────────────────────────────────

    void FpsDebugChart::RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int width, int bottom) {
        RenderStringWithShade(g, "30 fps", left + 1, bottom - CHART_HEIGHT + 1);
        RenderStringWithShade(g, "60 fps", left + 1, bottom - 30 + 1);
        g.HLine(left, left + width - 1, bottom - 30, 0xFFFFFFFF);
        if (m_framerateLimit > 0 && m_framerateLimit <= 250) {
            g.HLine(left, left + width - 1,
                    bottom - GetSampleHeight(1.0e9 / static_cast<double>(m_framerateLimit)) - 1, 0xFF00FFFF);
        }
    }

    std::string FpsDebugChart::ToDisplayString(double nanos) const {
        return FmtInt("%lld ms", std::llround(nanos / 1.0e6));
    }

    int FpsDebugChart::GetSampleHeight(double nanos) const {
        return static_cast<int>(std::lround(nanos / 1.0e6 * 60.0 / 33.333333333333336));
    }

    uint32_t FpsDebugChart::GetSampleColor(int64_t nanos) const {
        return AbstractDebugChart::GetSampleColor(static_cast<double>(nanos) / 1.0e6,
                                                  0.0, 0xFF00FF00, 28.0, 0xFFFFFF00, 56.0, 0xFFFF0000);
    }

    // ── TpsDebugChart ───────────────────────────────────────────────────

    void TpsDebugChart::RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int width, int bottom) {
        (void)width;
        const float tps = 1000.0f / std::max(0.001f, m_mspt());
        RenderStringWithShade(g, Fmt("%.1f tps", tps), left + 1, bottom - CHART_HEIGHT + 1);
    }

    void TpsDebugChart::RenderAdditionalSampleBars(GuiGraphics& g, int bottom, int x, int sampleIndex) {
        const int64_t tickMethodTime = m_storage.Get(sampleIndex, TICK_SERVER_METHOD);
        const int tickMethodHeight = GetSampleHeight(static_cast<double>(tickMethodTime));
        g.Fill(x, bottom - tickMethodHeight, x + 1, bottom, 0xFF9911D1);          // -6745839
        const int64_t tasksTime = m_storage.Get(sampleIndex, SCHEDULED_TASKS);
        const int tasksHeight = GetSampleHeight(static_cast<double>(tasksTime));
        g.Fill(x, bottom - tickMethodHeight - tasksHeight, x + 1, bottom - tickMethodHeight, 0xFFBA98DF); // -4548257
        const int64_t otherTime = m_storage.Get(sampleIndex) - m_storage.Get(sampleIndex, IDLE) - tickMethodTime - tasksTime;
        const int otherHeight = GetSampleHeight(static_cast<double>(otherTime));
        g.Fill(x, bottom - otherHeight - tasksHeight - tickMethodHeight, x + 1,
               bottom - tasksHeight - tickMethodHeight, 0xFF5F0C8C);                 // -10547572
    }

    int64_t TpsDebugChart::GetValueForAggregation(int sampleIndex) const {
        return m_storage.Get(sampleIndex) - m_storage.Get(sampleIndex, IDLE);
    }

    std::string TpsDebugChart::ToDisplayString(double nanos) const {
        return FmtInt("%lld ms", std::llround(nanos / 1.0e6));
    }

    int TpsDebugChart::GetSampleHeight(double nanos) const {
        return static_cast<int>(std::lround(nanos / 1.0e6 * 60.0 / static_cast<double>(std::max(0.001f, m_mspt()))));
    }

    uint32_t TpsDebugChart::GetSampleColor(int64_t nanos) const {
        const double mspt = std::max(0.001f, m_mspt());
        return AbstractDebugChart::GetSampleColor(static_cast<double>(nanos) / 1.0e6,
                                                  mspt, 0xFF00FF00, mspt * 1.125, 0xFFFFFF00, mspt * 1.25, 0xFFFF0000);
    }

    // ── PingDebugChart ──────────────────────────────────────────────────

    void PingDebugChart::RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int, int bottom) {
        RenderStringWithShade(g, "500 ms", left + 1, bottom - CHART_HEIGHT + 1);
    }

    std::string PingDebugChart::ToDisplayString(double millis) const {
        return FmtInt("%lld ms", std::llround(millis));
    }

    int PingDebugChart::GetSampleHeight(double millis) const {
        return static_cast<int>(std::lround(millis * 60.0 / 500.0));
    }

    uint32_t PingDebugChart::GetSampleColor(int64_t millis) const {
        return AbstractDebugChart::GetSampleColor(static_cast<double>(millis),
                                                  0.0, 0xFF00FF00, 250.0, 0xFFFFFF00, 500.0, 0xFFFF0000);
    }

    // ── BandwidthDebugChart ─────────────────────────────────────────────

    namespace {
        constexpr double kMegabyte = 1048576.0;
        std::string BandwidthDisplay(double bytesPerSecond) {
            if (bytesPerSecond >= kMegabyte) return Fmt("%.1f MiB/s", bytesPerSecond / kMegabyte);
            if (bytesPerSecond >= 1024.0)   return Fmt("%.1f KiB/s", bytesPerSecond / 1024.0);
            return FmtInt("%lld B/s", static_cast<long long>(std::floor(bytesPerSecond)));
        }
        int BandwidthHeight(double bytesPerSecond) {
            return static_cast<int>(std::lround(std::log(bytesPerSecond + 1.0) * 60.0 / std::log(kMegabyte)));
        }
    }

    void BandwidthDebugChart::RenderLabeledLineAtValue(GuiGraphics& g, int left, int width, int bottom, int bytesPerSecond) {
        const int y = bottom - BandwidthHeight(bytesPerSecond);
        RenderStringWithShade(g, BandwidthDisplay(bytesPerSecond), left + 1, y + 1);
        g.HLine(left, left + width - 1, y, 0xFFFFFFFF);
    }

    void BandwidthDebugChart::RenderAdditionalLinesAndLabels(GuiGraphics& g, int left, int width, int bottom) {
        RenderLabeledLineAtValue(g, left, width, bottom, 64);
        RenderLabeledLineAtValue(g, left, width, bottom, 1024);
        RenderLabeledLineAtValue(g, left, width, bottom, 16384);
        RenderStringWithShade(g, BandwidthDisplay(kMegabyte), left + 1, bottom - BandwidthHeight(kMegabyte) + 1);
    }

    std::string BandwidthDebugChart::ToDisplayString(double bytesPerTick) const {
        return BandwidthDisplay(bytesPerTick * 20.0);
    }

    int BandwidthDebugChart::GetSampleHeight(double bytesPerTick) const {
        return BandwidthHeight(bytesPerTick * 20.0);
    }

    uint32_t BandwidthDebugChart::GetSampleColor(int64_t bytesPerTick) const {
        return AbstractDebugChart::GetSampleColor(static_cast<double>(bytesPerTick) * 20.0,
                                                  0.0, 0xFF00FFFF, 8192.0, 0xFFA0A0FF, 1.048576e7, 0xFFFF0000);
    }

    // ── Profiler pie chart ──────────────────────────────────────────────

    uint32_t ResultField::Color() const {
        // MC ResultField.getColor: (name.hashCode() & 0xAAAAAA) + 0x444444, opaque.
        int32_t h = 0;
        for (unsigned char c : name) h = 31 * h + static_cast<int32_t>(c);
        const uint32_t rgb = (static_cast<uint32_t>(h) & 0xAAAAAAu) + 0x444444u;
        return 0xFF000000u | (rgb & 0xFFFFFFu);
    }

    const ProfileNode* ProfileResults::Find(const std::string& path) const {
        if (m_root.name.empty()) return nullptr;
        const ProfileNode* node = &m_root;
        size_t pos = 0;
        // "root" then \x1E-separated child names, as MC's profilerTreePath.
        std::vector<std::string> parts;
        while (pos <= path.size()) {
            const size_t next = path.find('\x1E', pos);
            parts.push_back(path.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        for (size_t i = 1; i < parts.size(); ++i) {
            const ProfileNode* found = nullptr;
            for (const ProfileNode& c : node->children) if (c.name == parts[i]) { found = &c; break; }
            if (!found) return nullptr;
            node = found;
        }
        return node;
    }

    std::vector<ResultField> ProfileResults::GetTimes(const std::string& path) const {
        std::vector<ResultField> out;
        const ProfileNode* node = Find(path);
        if (!node) return out;
        const double rootTime = std::max(m_root.timeMs, 1e-9);
        const double nodeTime = std::max(node->timeMs, 1e-9);
        // The node itself: MC's first entry, percentage of the parent is not
        // used by the chart, the global share is.
        ResultField self;
        self.name = node->name;
        self.percentage = 100.0;
        self.globalPercentage = node->timeMs * 100.0 / rootTime;
        out.push_back(self);
        double accounted = 0.0;
        std::vector<ResultField> children;
        for (const ProfileNode& c : node->children) {
            ResultField f;
            f.name = c.name;
            f.percentage = c.timeMs * 100.0 / nodeTime;
            f.globalPercentage = c.timeMs * 100.0 / rootTime;
            accounted += c.timeMs;
            children.push_back(f);
        }
        if (nodeTime - accounted > 0.0 && !node->children.empty()) {
            ResultField f;
            f.name = "unspecified";
            f.percentage = (nodeTime - accounted) * 100.0 / nodeTime;
            f.globalPercentage = (nodeTime - accounted) * 100.0 / rootTime;
            children.push_back(f);
        }
        std::stable_sort(children.begin(), children.end(),
                         [](const ResultField& a, const ResultField& b) { return a.percentage > b.percentage; });
        out.insert(out.end(), children.begin(), children.end());
        return out;
    }

    std::vector<std::string> ProfilerPieChart::SplitNodeName(GuiGraphics& g, const std::string& nodeName,
                                                             int firstLineMaxWidth, int maxWidth) const {
        std::vector<std::string> parts;
        size_t pos = 0;
        while (pos <= nodeName.size()) {
            const size_t next = nodeName.find('.', pos);
            parts.push_back(nodeName.substr(pos, next == std::string::npos ? std::string::npos : next - pos));
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        std::vector<std::string> lines;
        std::string current;
        size_t i = 0;
        while (i < parts.size()) {
            const std::string withPeriod = (i != 0 ? "." : "") + parts[i];
            const std::string newLine = current + withPeriod;
            const int newWidth = g.GetStringWidth(newLine);
            if (newWidth > (!lines.empty() ? maxWidth : firstLineMaxWidth)) {
                if (current.empty()) { lines.push_back(withPeriod); ++i; }
                else { lines.push_back(current); current.clear(); }
            } else {
                current = newLine;
                ++i;
            }
        }
        if (!current.empty()) lines.push_back(current);
        if (lines.empty()) lines.push_back(nodeName);
        return lines;
    }

    void ProfilerPieChart::RenderPie(GuiGraphics& g, const std::vector<ResultField>& list, int x0, int y0, int x1, int y1) {
        // MC GuiProfilerChartRenderer: a fan of sin/cos wedges (105 wide,
        // half as tall) with a 10 px darker band under the front half. The
        // texture it renders into is centred in the given rect.
        const float cx = static_cast<float>(x0 + x1) * 0.5f;
        const float cy = static_cast<float>(y0 + y1) * 0.5f - 5.0f;
        double total = 0.0;
        GuiRenderState* rs = g.GetRenderState();
        if (!rs) return;
        auto tri = [&](float ax, float ay, float bx, float by, float ccx, float ccy, uint32_t color) {
            QuadCommand q;
            q.texture = INVALID_TEXTURE;
            q.px[0] = ax;  q.py[0] = ay;
            q.px[1] = bx;  q.py[1] = by;
            q.px[2] = ccx; q.py[2] = ccy;
            q.px[3] = ccx; q.py[3] = ccy;
            q.color = color;
            rs->SubmitQuad(q);
        };
        auto quad = [&](float ax, float ay, float bx, float by, float ccx, float ccy, float dx, float dy, uint32_t color) {
            QuadCommand q;
            q.texture = INVALID_TEXTURE;
            q.px[0] = ax;  q.py[0] = ay;
            q.px[1] = bx;  q.py[1] = by;
            q.px[2] = ccx; q.py[2] = ccy;
            q.px[3] = dx;  q.py[3] = dy;
            q.color = color;
            rs->SubmitQuad(q);
        };
        for (const ResultField& result : list) {
            const double slice = result.percentage;
            const double start = total;
            total += slice;
            const int steps = static_cast<int>(std::floor(slice / 4.0)) + 1;
            const uint32_t color = result.Color();
            // ARGB.multiply(color, 0xFF808080): halve every channel.
            const uint32_t shade = 0xFF000000u | (((color >> 16) & 0xFF) / 2 << 16) |
                                   (((color >> 8) & 0xFF) / 2 << 8) | ((color & 0xFF) / 2);
            float prevX = 0.0f, prevY = 0.0f;
            for (int j = steps; j >= 0; --j) {
                const float dir = static_cast<float>((start + slice * j / steps) * 6.2831854820251465 / 100.0);
                const float xx = std::sin(dir) * RADIUS;
                const float yy = std::cos(dir) * RADIUS * 0.5f;
                if (j != steps) tri(cx, cy, cx + prevX, cy + prevY, cx + xx, cy + yy, color);
                prevX = xx; prevY = yy;
            }
            for (int j = steps; j > 0; --j) {
                const float dir0 = static_cast<float>((start + slice * j / steps) * 6.2831854820251465 / 100.0);
                const float x0f = std::sin(dir0) * RADIUS;
                const float y0f = std::cos(dir0) * RADIUS * 0.5f;
                const float dir1 = static_cast<float>((start + slice * (j - 1) / steps) * 6.2831854820251465 / 100.0);
                const float x1f = std::sin(dir1) * RADIUS;
                const float y1f = std::cos(dir1) * RADIUS * 0.5f;
                if ((y0f + y1f) / 2.0f < 0.0f) continue;
                quad(cx + x0f, cy + y0f, cx + x0f, cy + y0f + PIE_CHART_THICKNESS,
                     cx + x1f, cy + y1f + PIE_CHART_THICKNESS, cx + x1f, cy + y1f, shade);
            }
        }
    }

    void ProfilerPieChart::Render(GuiGraphics& g, int scaledScreenWidth, int scaledScreenHeight) {
        if (!m_results || m_results->Empty()) return;
        std::vector<ResultField> list = m_results->GetTimes(m_treePath);
        if (list.empty()) return;
        const ResultField currentNode = list.front();
        list.erase(list.begin());

        const int chartCenterX = scaledScreenWidth - 130 - 10;
        const int left = chartCenterX - 130;
        const int right = chartCenterX + 130;
        const int textUnderChartHeight = static_cast<int>(list.size()) * 9;
        const int bottom = scaledScreenHeight - m_bottomOffset - 5;
        const int textStartY = bottom - textUnderChartHeight;
        const int chartCenterY = textStartY - 62 - 5;
        const std::string globalPercentage = Fmt("%.2f", currentNode.globalPercentage) + "%";
        const int globalPercentageWidth = g.GetStringWidth(globalPercentage);
        const int zeroPrefixWidth = g.GetStringWidth("[0] ");
        const int topTextMaxWidth = right - globalPercentageWidth - 5 - left - zeroPrefixWidth;
        const std::string currentNodeName = currentNode.name;
        const std::vector<std::string> nameLines = SplitNodeName(g, currentNodeName, topTextMaxWidth, topTextMaxWidth - 10);
        const int currentNodeNameTop = chartCenterY - 62 - (static_cast<int>(nameLines.size()) - 1) * 9;

        g.Fill(left - 5, currentNodeNameTop - 5, right + 5, bottom + 5, kShadeColor);
        RenderPie(g, list, left, chartCenterY - 62 + 10, right, chartCenterY + 62);

        std::string firstLine;
        if (currentNodeName != "unspecified" && currentNodeName != "root") firstLine += "[0] ";
        firstLine += nameLines.front();
        g.DrawString(firstLine, left, currentNodeNameTop, 0xFFFFFFFF, true);
        for (size_t i = 1; i < nameLines.size(); ++i) {
            g.DrawString(nameLines[i], left + 10 + zeroPrefixWidth, currentNodeNameTop + static_cast<int>(i) * 9, 0xFFFFFFFF, true);
        }
        g.DrawString(globalPercentage, right - globalPercentageWidth, currentNodeNameTop, 0xFFFFFFFF, true);
        for (size_t i = 0; i < list.size(); ++i) {
            const ResultField& result = list[i];
            std::string msg = result.name == "unspecified" ? "[?] " : "[" + std::to_string(i + 1) + "] ";
            msg += result.name;
            const int textY = textStartY + static_cast<int>(i) * 9;
            const uint32_t color = result.Color();
            g.DrawString(msg, left, textY, color, true);
            msg = Fmt("%.2f", result.percentage) + "%";
            g.DrawString(msg, right - 50 - g.GetStringWidth(msg), textY, color, true);
            msg = Fmt("%.2f", result.globalPercentage) + "%";
            g.DrawString(msg, right - g.GetStringWidth(msg), textY, color, true);
        }
    }

    void ProfilerPieChart::KeyPress(int key) {
        if (!m_results || m_results->Empty()) return;
        std::vector<ResultField> list = m_results->GetTimes(m_treePath);
        if (list.empty()) return;
        const ResultField node = list.front();
        list.erase(list.begin());
        if (key == 0) {
            if (!node.name.empty()) {
                const size_t pos = m_treePath.rfind('\x1E');
                if (pos != std::string::npos) m_treePath = m_treePath.substr(0, pos);
            }
            return;
        }
        --key;
        if (key < static_cast<int>(list.size()) && list[static_cast<size_t>(key)].name != "unspecified") {
            if (!m_treePath.empty()) m_treePath += '\x1E';
            m_treePath += list[static_cast<size_t>(key)].name;
        }
    }

} // namespace Render::DebugScreen
