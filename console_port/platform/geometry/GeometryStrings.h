#pragma once
#include <sstream>
// Original StringHelpers.h template, used only for geometry diagnostic strings.
template <class T> std::wstring _toString(T t)
{
    std::wostringstream oss;
    oss << std::dec << t;
    return oss.str();
}
