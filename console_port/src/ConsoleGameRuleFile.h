#pragma once
#include <map>
#include <span>
#include <string>
#include <vector>
namespace console {
struct ConsoleGameRuleNode {
 std::wstring name;
 std::map<std::wstring,std::wstring> attributes;
 std::vector<ConsoleGameRuleNode> children;
};
// Original GRF string table, embedded files and recursive rule records.
// This decodes definitions; runtime rule execution remains separate.
struct ConsoleGameRuleFile {
 std::map<std::wstring,std::vector<unsigned char>> files;
 std::vector<ConsoleGameRuleNode> rules;
 static ConsoleGameRuleFile read(std::span<const unsigned char> bytes);
};
}
