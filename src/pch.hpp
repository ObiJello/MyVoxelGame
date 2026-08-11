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

// Only put headers here that are BOTH widely included and expensive to parse,
// and that essentially never change — a PCH is rebuilt (and every TU with it)
// whenever anything it pulls in is touched, so a project header in here would
// cost far more than it saves.
//
// The counts below are how many files in src/ include each one directly; they
// are what earns a header its place.

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
#include <array>
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

// Header-only template libraries — the expensive half of a typical TU here.
// glm alone is included by ~69 files and re-instantiates its whole vector and
// matrix template set every time; nlohmann/json by ~17 and is one of the
// heaviest single headers in common use. Both are vendored and pinned, so they
// never invalidate the PCH in normal work.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>