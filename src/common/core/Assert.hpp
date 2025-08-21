// File: src/common/core/Assert.hpp
#pragma once

#include <thread>
#include <cassert>
#include <cstdio>

// Server thread assertion support
namespace Server {
    extern std::thread::id g_serverThreadId;
}

// Thread assertion macros for debug builds
#ifdef NDEBUG
    // Release build - no assertions
    #define ASSERT_SERVER_THREAD() ((void)0)
    #define ASSERT_CLIENT_THREAD() ((void)0)
    #define ASSERT_NOT_SERVER_THREAD() ((void)0)
#else
    // Debug build - perform assertions
    #define ASSERT_SERVER_THREAD() \
        do { \
            if (std::this_thread::get_id() != Server::g_serverThreadId) { \
                fprintf(stderr, "ASSERTION FAILED: Not on server thread! File: %s, Line: %d\n", \
                        __FILE__, __LINE__); \
                assert(false && "Not on server thread"); \
            } \
        } while(0)
    
    #define ASSERT_NOT_SERVER_THREAD() \
        do { \
            if (std::this_thread::get_id() == Server::g_serverThreadId) { \
                fprintf(stderr, "ASSERTION FAILED: On server thread when shouldn't be! File: %s, Line: %d\n", \
                        __FILE__, __LINE__); \
                assert(false && "Should not be on server thread"); \
            } \
        } while(0)
    
    // Client thread assertion would require client thread tracking
    #define ASSERT_CLIENT_THREAD() ((void)0)
#endif

// General assertion macro
#ifndef ASSERT
    #ifdef NDEBUG
        #define ASSERT(condition) ((void)0)
    #else
        #define ASSERT(condition) assert(condition)
    #endif
#endif