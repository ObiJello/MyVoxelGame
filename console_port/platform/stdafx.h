#pragma once
// Portable precompiled-header boundary for the selected original world modules.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <chrono>
#include <bit>
#include <string>
#include <vector>
#include "MinecraftColours.h"
using std::shared_ptr;
using std::vector;
using std::wstring;
using __int64 = std::int64_t;
using byte = unsigned char;
constexpr double PI = 3.14159265358979323846;
inline void MemSect(int) {}
// Copies retain the original shared-buffer calling convention, but own storage.
struct doubleArray {
#ifndef CONSOLE_REFERENCE_RAW_DOUBLE
    std::shared_ptr<double[]> owner;
#endif
    double* data = nullptr;
    unsigned int length = 0;
    doubleArray() = default;
    explicit doubleArray(unsigned int n)
#ifdef CONSOLE_REFERENCE_RAW_DOUBLE
        : data(new double[n]()), length(n) {}
#else
        : owner(new double[n]()), data(owner.get()), length(n) {}
#endif
    double& operator[](unsigned int i) { return data[i]; }
    const double& operator[](unsigned int i) const { return data[i]; }
};
#include "Random.h"
