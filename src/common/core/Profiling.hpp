// File: src/common/core/Profiling.hpp
#pragma once

#include <chrono>

// Profiling Configuration
// Set ENABLE_PROFILING to 0 to completely disable all profiling in release builds
// By default, profiling is enabled in all builds but has minimal overhead
#ifndef ENABLE_PROFILING
    #define ENABLE_PROFILING 1
#endif

// Profile timer macros
#if ENABLE_PROFILING

    // Simple scoped timer - measures time from creation to destruction
    class ScopedTimer {
    public:
        explicit ScopedTimer(float& outputMs) 
            : m_output(outputMs)
            , m_start(std::chrono::high_resolution_clock::now()) 
        {}
        
        ~ScopedTimer() {
            auto end = std::chrono::high_resolution_clock::now();
            m_output = std::chrono::duration<float, std::milli>(end - m_start).count();
        }
        
    private:
        float& m_output;
        std::chrono::high_resolution_clock::time_point m_start;
    };
    
    // Manual timer for more control
    class ManualTimer {
    public:
        ManualTimer() : m_start(std::chrono::high_resolution_clock::now()) {}
        
        void Start() {
            m_start = std::chrono::high_resolution_clock::now();
        }
        
        float GetElapsedMs() const {
            auto now = std::chrono::high_resolution_clock::now();
            return std::chrono::duration<float, std::milli>(now - m_start).count();
        }
        
        float Stop() const {
            return GetElapsedMs();
        }
        
    private:
        std::chrono::high_resolution_clock::time_point m_start;
    };

    // Main profiling macros - use PROFILE_SCOPE for automatic timing
    #define PROFILE_SCOPE(output_var) ScopedTimer _timer##__LINE__(output_var)
    
    // For manual timing, use named timers to avoid conflicts
    #define PROFILE_TIMER_START(name) auto _timer_##name = std::chrono::high_resolution_clock::now()
    #define PROFILE_TIMER_END(name, output_var) do { \
        auto _timerEnd_##name = std::chrono::high_resolution_clock::now(); \
        output_var = std::chrono::duration<float, std::milli>(_timerEnd_##name - _timer_##name).count(); \
    } while(0)
    
    // Simplified versions using a block number to avoid conflicts
    #define PROFILE_BLOCK_1_START() auto _profileStart1 = std::chrono::high_resolution_clock::now()
    #define PROFILE_BLOCK_1_END(output_var) output_var = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - _profileStart1).count()
    #define PROFILE_BLOCK_2_START() auto _profileStart2 = std::chrono::high_resolution_clock::now()
    #define PROFILE_BLOCK_2_END(output_var) output_var = std::chrono::duration<float, std::milli>(std::chrono::high_resolution_clock::now() - _profileStart2).count()
    
    // Accumulating timer (adds to existing value)
    #define PROFILE_ACCUMULATE_START(name) auto _accum_##name = std::chrono::high_resolution_clock::now()
    #define PROFILE_ACCUMULATE_END(name, output_var) do { \
        auto _accumEnd_##name = std::chrono::high_resolution_clock::now(); \
        output_var += std::chrono::duration<float, std::milli>(_accumEnd_##name - _accum_##name).count(); \
    } while(0)

#else
    // When profiling is disabled, all macros become no-ops
    #define PROFILE_SCOPE(output_var) ((void)0)
    #define PROFILE_TIMER_START(name) ((void)0)
    #define PROFILE_TIMER_END(name, output_var) ((void)0)
    #define PROFILE_BLOCK_1_START() ((void)0)
    #define PROFILE_BLOCK_1_END(output_var) ((void)0)
    #define PROFILE_BLOCK_2_START() ((void)0)
    #define PROFILE_BLOCK_2_END(output_var) ((void)0)
    #define PROFILE_ACCUMULATE_START(name) ((void)0)
    #define PROFILE_ACCUMULATE_END(name, output_var) ((void)0)
#endif

// Optional: Debug-only profiling (only in debug builds)
#ifdef NDEBUG
    #define DEBUG_PROFILE_SCOPE(output_var) ((void)0)
    #define DEBUG_PROFILE_TIMER_START(name) ((void)0)
    #define DEBUG_PROFILE_TIMER_END(name, output_var) ((void)0)
#else
    #define DEBUG_PROFILE_SCOPE(output_var) PROFILE_SCOPE(output_var)
    #define DEBUG_PROFILE_TIMER_START(name) PROFILE_TIMER_START(name)
    #define DEBUG_PROFILE_TIMER_END(name, output_var) PROFILE_TIMER_END(name, output_var)
#endif