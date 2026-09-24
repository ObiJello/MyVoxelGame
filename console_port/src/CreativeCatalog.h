#pragma once
#include <span>

namespace console {
struct CreativeEntry {int id=0,damage=0;bool operator==(const CreativeEntry&)const=default;};
struct CreativeTab {const char* name;std::span<const CreativeEntry> items;};
int creativeTabCount();
CreativeTab creativeTab(int index);
}
