// File: src/client/world/FastLaneConfig.hpp
#pragma once

#include <cstddef>

namespace Threading {

    // Configuration for the fast lane priority system
    struct FastLaneConfig {
        // Distance thresholds (in blocks)
        static constexpr float NEAR_DISTANCE = 48.0f;    // 3 chunks
        static constexpr float MEDIUM_DISTANCE = 128.0f; // 8 chunks
        static constexpr float FAR_DISTANCE = 256.0f;    // 16 chunks
        
        // Job submission limits per frame
        static constexpr size_t MAX_NEAR_JOBS_PER_FRAME = 5;    // High priority nearby chunks
        static constexpr size_t MAX_NORMAL_JOBS_PER_FRAME = 10; // Total including nearby
        static constexpr size_t MAX_JOBS_PER_CHUNK = 4;         // Limit per chunk to prevent monopolization
        
        // GPU upload budgets (milliseconds)
        static constexpr float MAX_GPU_UPLOAD_TIME_MS = 2.0f;   // Per frame GPU upload time budget
        static constexpr size_t MAX_GPU_UPLOADS_PER_FRAME = 8;  // Maximum mesh uploads per frame
        
        // Queue size limits
        static constexpr size_t HIGH_PRIORITY_QUEUE_SIZE = 100;
        static constexpr size_t NORMAL_PRIORITY_QUEUE_SIZE = 400;
        static constexpr size_t MAX_TOTAL_QUEUE_SIZE = 500;
        
        // Worker thread configuration
        static constexpr size_t MIN_WORKER_THREADS = 1;
        static constexpr size_t MAX_WORKER_THREADS = 8;
        static constexpr size_t DEFAULT_WORKER_THREADS = 2;
        
        // Priority weights for distance calculation
        static constexpr float DISTANCE_WEIGHT = 1.0f;
        static constexpr float HEIGHT_WEIGHT = 0.5f;     // Sections at player height get priority
        static constexpr float DIRECTION_WEIGHT = 0.3f;  // Sections in view direction get priority
        
        // Calculate priority score (lower = higher priority)
        static float CalculatePriority(float distance, float heightDiff, float dotProduct) {
            float priority = distance * DISTANCE_WEIGHT;
            priority += heightDiff * HEIGHT_WEIGHT;
            priority -= dotProduct * DIRECTION_WEIGHT; // Subtract because higher dot = more in view
            return priority;
        }
        
        // Check if a job should go in the fast lane
        static bool IsHighPriority(float distance) {
            return distance < NEAR_DISTANCE;
        }
        
        // Check if a job should be considered medium priority
        static bool IsMediumPriority(float distance) {
            return distance < MEDIUM_DISTANCE;
        }
        
        // Check if a job is too far and should be deferred
        static bool ShouldDefer(float distance) {
            return distance > FAR_DISTANCE;
        }
    };

} // namespace Threading