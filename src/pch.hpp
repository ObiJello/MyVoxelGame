// src/pch.hpp
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
// Undefine Windows macros that clash with our enum names
// (VOID and far/near must NOT be undef'd - Windows SDK headers need them)
#undef TRANSPARENT
#undef OPAQUE
#undef ABSOLUTE
#undef RELATIVE
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <iostream>
#include <functional>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
#include <chrono>