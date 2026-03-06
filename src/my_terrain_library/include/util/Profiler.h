#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>

/**
 * Profiler - Simple thread-safe timing collection for performance analysis
 *
 * Usage:
 *   // Time a scope:
 *   { PROFILE_SCOPE("ChunkGenerationTask.runUntilWait"); }
 *
 *   // Or manually:
 *   Profiler::Timer timer("dispatcher.submit");
 *   // ... code ...
 *   timer.stop();
 *
 *   // Print results:
 *   Profiler::report();
 *   Profiler::reset();
 */

namespace minecraft {
namespace util {

class Profiler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    struct Stats {
        std::atomic<int64_t> totalNanos{0};
        std::atomic<int64_t> count{0};
        std::atomic<int64_t> maxNanos{0};

        void record(int64_t nanos) {
            totalNanos.fetch_add(nanos, std::memory_order_relaxed);
            count.fetch_add(1, std::memory_order_relaxed);

            // Update max (lock-free)
            int64_t current = maxNanos.load(std::memory_order_relaxed);
            while (nanos > current && !maxNanos.compare_exchange_weak(
                current, nanos, std::memory_order_relaxed)) {}
        }
    };

    /**
     * RAII Timer - times a scope and records on destruction
     */
    class Timer {
    public:
        explicit Timer(const std::string& name)
            : m_name(name)
            , m_start(Clock::now())
            , m_stopped(false)
        {}

        ~Timer() {
            if (!m_stopped) {
                stop();
            }
        }

        void stop() {
            if (!m_stopped) {
                auto end = Clock::now();
                auto nanos = std::chrono::duration_cast<Duration>(end - m_start).count();
                Profiler::record(m_name, nanos);
                m_stopped = true;
            }
        }

        // Get elapsed time without stopping
        int64_t elapsedNanos() const {
            auto end = Clock::now();
            return std::chrono::duration_cast<Duration>(end - m_start).count();
        }

    private:
        std::string m_name;
        TimePoint m_start;
        bool m_stopped;
    };

    /**
     * Record a timing measurement
     */
    static void record(const std::string& name, int64_t nanos) {
        if (!s_enabled.load(std::memory_order_relaxed)) return;

        std::lock_guard<std::mutex> lock(s_mutex);
        s_stats[name].record(nanos);
    }

    /**
     * Enable/disable profiling
     */
    static void setEnabled(bool enabled) {
        s_enabled.store(enabled, std::memory_order_relaxed);
    }

    static bool isEnabled() {
        return s_enabled.load(std::memory_order_relaxed);
    }

    /**
     * Reset all statistics
     */
    static void reset() {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_stats.clear();
    }

    /**
     * Print profiling report to stdout
     */
    static void report() {
        std::lock_guard<std::mutex> lock(s_mutex);

        if (s_stats.empty()) {
            std::cout << "No profiling data collected." << std::endl;
            return;
        }

        // Collect and sort by total time
        std::vector<std::pair<std::string, Stats*>> sorted;
        int64_t grandTotal = 0;
        for (auto& pair : s_stats) {
            sorted.push_back({pair.first, &pair.second});
            grandTotal += pair.second.totalNanos.load();
        }

        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
                return a.second->totalNanos.load() > b.second->totalNanos.load();
            });

        std::cout << "\n=== C++ Async Pipeline Profiling Results ===" << std::endl;
        std::cout << std::fixed << std::setprecision(1);

        // Header
        std::cout << std::left << std::setw(45) << "Name"
                  << std::right << std::setw(12) << "Total(s)"
                  << std::setw(8) << "%"
                  << std::setw(10) << "Count"
                  << std::setw(12) << "Avg(ms)"
                  << std::setw(12) << "Max(ms)"
                  << std::endl;
        std::cout << std::string(99, '-') << std::endl;

        for (const auto& pair : sorted) {
            const std::string& name = pair.first;
            const Stats* stats = pair.second;

            int64_t totalNanos = stats->totalNanos.load();
            int64_t count = stats->count.load();
            int64_t maxNanos = stats->maxNanos.load();

            double totalSecs = totalNanos / 1e9;
            double percentage = grandTotal > 0 ? (100.0 * totalNanos / grandTotal) : 0;
            double avgMs = count > 0 ? (totalNanos / 1e6 / count) : 0;
            double maxMs = maxNanos / 1e6;

            std::cout << std::left << std::setw(45) << name
                      << std::right << std::setw(12) << totalSecs
                      << std::setw(7) << percentage << "%"
                      << std::setw(10) << count
                      << std::setw(12) << avgMs
                      << std::setw(12) << maxMs
                      << std::endl;
        }

        std::cout << std::string(99, '-') << std::endl;
        std::cout << "Grand Total: " << (grandTotal / 1e9) << " seconds" << std::endl;
    }

    /**
     * Get report as string (for logging)
     */
    static std::string getReportString() {
        std::ostringstream oss;

        std::lock_guard<std::mutex> lock(s_mutex);

        if (s_stats.empty()) {
            return "No profiling data collected.";
        }

        // Collect and sort
        std::vector<std::pair<std::string, Stats*>> sorted;
        for (auto& pair : s_stats) {
            sorted.push_back({pair.first, &pair.second});
        }

        std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) {
                return a.second->totalNanos.load() > b.second->totalNanos.load();
            });

        oss << "Profiling Results:\n";
        for (const auto& pair : sorted) {
            int64_t totalNanos = pair.second->totalNanos.load();
            int64_t count = pair.second->count.load();
            oss << "  " << pair.first << ": "
                << (totalNanos / 1e9) << "s, "
                << count << " calls, "
                << (count > 0 ? (totalNanos / 1e6 / count) : 0) << "ms avg\n";
        }

        return oss.str();
    }

private:
    static std::mutex s_mutex;
    static std::unordered_map<std::string, Stats> s_stats;
    static std::atomic<bool> s_enabled;
};

// Static member definitions (inline for header-only)
inline std::mutex Profiler::s_mutex;
inline std::unordered_map<std::string, Profiler::Stats> Profiler::s_stats;
inline std::atomic<bool> Profiler::s_enabled{true};

} // namespace util
} // namespace minecraft

// Macro for easy scope profiling
#define PROFILE_SCOPE(name) minecraft::util::Profiler::Timer _profiler_timer_##__LINE__(name)

// Macro with condition
#define PROFILE_SCOPE_IF(condition, name) \
    std::unique_ptr<minecraft::util::Profiler::Timer> _profiler_timer_##__LINE__; \
    if (condition) _profiler_timer_##__LINE__ = std::make_unique<minecraft::util::Profiler::Timer>(name)
