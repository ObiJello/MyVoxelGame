// File: src/common/entity/EntityTags.hpp
//
// MC Entity.tags — the scoreboard tags `/tag` adds and removes, `@e[tag=x]`
// tests, and the "Tags" string list in the entity's saved compound. Held by
// every kind of thing a selector can return here (Game::Entity, the
// ServerPlayer, and the dropped-item and experience-orb values, which are
// not Entities in this engine), so all four behave alike.
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Game {

    class EntityTags {
    public:
        // MC Entity.MAX_ENTITY_TAG_COUNT.
        static constexpr size_t kMaxTags = 1024;

        // MC Entity.addTag: false when the set is full or already holds it.
        bool Add(std::string_view tag) {
            if (m_tags.size() >= kMaxTags || Has(tag)) return false;
            m_tags.emplace_back(tag);
            return true;
        }
        // MC Entity.removeTag: false when it was not there.
        bool Remove(std::string_view tag) {
            const auto it = std::find(m_tags.begin(), m_tags.end(), tag);
            if (it == m_tags.end()) return false;
            m_tags.erase(it);
            return true;
        }
        bool Has(std::string_view tag) const {
            return std::find(m_tags.begin(), m_tags.end(), tag) != m_tags.end();
        }
        bool Empty() const { return m_tags.empty(); }
        size_t Size() const { return m_tags.size(); }
        void Clear() { m_tags.clear(); }
        const std::vector<std::string>& All() const { return m_tags; }

    private:
        // Insertion order (MC's HashSet has none worth matching); a handful
        // of entries at most in practice, so a linear search is the fast one.
        std::vector<std::string> m_tags;
    };

    // MC TagCommand / EntitySelectorOptions "tag": a tag name is an unquoted
    // string — [0-9A-Za-z_\-.+].
    inline bool IsValidEntityTag(std::string_view tag) {
        if (tag.empty()) return false;
        for (const char c : tag) {
            const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            c == '_' || c == '-' || c == '.' || c == '+';
            if (!ok) return false;
        }
        return true;
    }

} // namespace Game
