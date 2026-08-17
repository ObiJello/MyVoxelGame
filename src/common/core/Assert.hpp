// File: src/common/core/Assert.hpp
#pragma once

#include <thread>
#include <cassert>
#include <cstdio>

// Server thread assertion support
namespace Server {
    extern std::thread::id g_serverThreadId;
}

// Client main-thread assertion support. Set once in PlatformMain::Run.
// This is the counterpart to g_serverThreadId and exists for the same reason:
// packet handlers must run on the thread that owns the state they touch, and
// an assert is the only way to keep that true as handlers get added.
namespace Client {
    extern std::thread::id g_clientThreadId;
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
    
    #define ASSERT_CLIENT_THREAD() \
        do { \
            if (Client::g_clientThreadId != std::thread::id() && \
                std::this_thread::get_id() != Client::g_clientThreadId) { \
                fprintf(stderr, "ASSERTION FAILED: Not on client main thread! File: %s, Line: %d\n", \
                        __FILE__, __LINE__); \
                assert(false && "Not on client main thread"); \
            } \
        } while(0)
#endif

// General assertion macro
#ifndef ASSERT
    #ifdef NDEBUG
        #define ASSERT(condition) ((void)0)
    #else
        #define ASSERT(condition) assert(condition)
    #endif
#endif