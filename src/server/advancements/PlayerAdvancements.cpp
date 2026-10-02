// File: src/server/advancements/PlayerAdvancements.cpp
#include "PlayerAdvancements.hpp"
#include "AdvancementText.hpp"

#include "server/IntegratedServer.hpp"
#include "server/network/ServerConnection.hpp"
#include "server/player/ServerPlayer.hpp"
#include "server/session/PlayerSession.hpp"
#include "server/session/PlayerSessionManager.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/core/Log.hpp"
#include "common/core/SaveVersion.hpp"
#include "common/network/PacketTypes.hpp"
#include "common/world/level/DimensionId.hpp"
#include "common/world/level/GameRules.hpp"
#include "common/world/loot/ChestLootTables.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace Server::Advancements {

    namespace {

        int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // Howard Hinnant's days_from_civil / civil_from_days: calendar
        // arithmetic without the platform's timegm.
        int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
            y -= m <= 2;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<int64_t>(doe) - 719468;
        }

        void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
            z += 719468;
            const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const unsigned doe = static_cast<unsigned>(z - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            y = static_cast<int64_t>(yoe) + era * 400;
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const unsigned mp = (5 * doy + 2) / 153;
            d = doy - (153 * mp + 2) / 5 + 1;
            m = mp + (mp < 10 ? 3 : -9);
            y += m <= 2;
        }

        // AdvancementProgress's OBTAINED_TIME_FORMAT, "yyyy-MM-dd HH:mm:ss Z".
        // Written in UTC (+0000), which MC reads back like any other zone.
        std::string FormatObtained(int64_t ms) {
            int64_t secs = ms / 1000;
            if (ms < 0 && ms % 1000) --secs;
            int64_t days = secs / 86400;
            int64_t rem = secs % 86400;
            if (rem < 0) { rem += 86400; --days; }
            int64_t y; unsigned m, d;
            CivilFromDays(days, y, m, d);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%04lld-%02u-%02u %02lld:%02lld:%02lld +0000",
                          static_cast<long long>(y), m, d, static_cast<long long>(rem / 3600),
                          static_cast<long long>((rem / 60) % 60), static_cast<long long>(rem % 60));
            return buf;
        }

        std::optional<int64_t> ParseObtained(const std::string& text) {
            int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
            char sign = '+';
            int zone = 0;
            if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d %c%d", &y, &mo, &d, &h, &mi, &s, &sign, &zone) < 6) {
                return std::nullopt;
            }
            if (mo < 1 || mo > 12 || d < 1 || d > 31) return std::nullopt;
            const int zoneMinutes = (zone / 100) * 60 + zone % 100;
            int64_t secs = DaysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 +
                           h * 3600 + mi * 60 + s;
            secs -= (sign == '-' ? -1 : 1) * static_cast<int64_t>(zoneMinutes) * 60;
            return secs * 1000;
        }

        // MC AdvancementVisibilityEvaluator.
        enum class VisibilityRule { Show, Hide, NoChange };

        VisibilityRule EvaluateVisibilityRule(const Definition& def, bool isDone) {
            if (!def.display) return VisibilityRule::Hide;
            if (isDone) return VisibilityRule::Show;
            return def.display->hidden ? VisibilityRule::Hide : VisibilityRule::NoChange;
        }

        bool EvaluateVisibilityForUnfinishedNode(const std::vector<VisibilityRule>& ascendants) {
            // VISIBILITY_DEPTH = 2: the node and its two nearest ancestors.
            for (int i = 0; i <= 2; ++i) {
                const VisibilityRule rule = ascendants[ascendants.size() - 1 - static_cast<size_t>(i)];
                if (rule == VisibilityRule::Show) return true;
                if (rule == VisibilityRule::Hide) return false;
            }
            return false;
        }

        template <class IsDone, class Output>
        bool EvaluateVisibility(const Game::Advancements::Node& node, std::vector<VisibilityRule>& ascendants,
                                const IsDone& isDone, const Output& output) {
            const bool isSelfDone = isDone(node);
            bool isSelfOrDescendantDone = isSelfDone;
            ascendants.push_back(EvaluateVisibilityRule(*node.def, isSelfDone));
            for (const Game::Advancements::Node* child : node.children) {
                isSelfOrDescendantDone |= EvaluateVisibility(*child, ascendants, isDone, output);
            }
            const bool visible = isSelfOrDescendantDone || EvaluateVisibilityForUnfinishedNode(ascendants);
            ascendants.pop_back();
            output(node, visible);
            return isSelfOrDescendantDone;
        }

        Network::AdvancementEntryData EntryOf(const Definition& def) {
            Network::AdvancementEntryData e;
            e.id = def.id;
            e.parent = def.parent;
            e.display = def.display;
            e.requirements = def.requirements;
            e.sendsTelemetryEvent = def.sendsTelemetryEvent;
            return e;
        }

    } // namespace

    PlayerAdvancements::PlayerAdvancements(std::shared_ptr<const Registry> registry, std::filesystem::path file,
                                           uint32_t playerId)
        : m_registry(std::move(registry)), m_file(std::move(file)), m_playerId(playerId) {}

    // ── Load / save ──────────────────────────────────────────────────────

    void PlayerAdvancements::Load() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        std::error_code ec;
        if (!m_file.empty() && std::filesystem::is_regular_file(m_file, ec)) {
            std::ifstream in(m_file);
            const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
            if (json.is_discarded() || !json.is_object()) {
                Log::Error("[Advancements] Couldn't parse player advancements in %s", m_file.string().c_str());
            } else {
                // Data.forEach: entries applied oldest progress first.
                struct Entry { const Definition* def; AdvancementProgress progress; };
                std::vector<Entry> entries;
                size_t unknown = 0;
                for (auto it = json.begin(); it != json.end(); ++it) {
                    if (it.key() == "DataVersion") continue;
                    const Definition* def = m_registry->Get(it.key());
                    if (!def) {
                        // MC warns and drops it; kept here (see m_preserved).
                        m_preserved.emplace_back(it.key(), it.value().dump());
                        ++unknown;
                        continue;
                    }
                    AdvancementProgress progress;
                    if (auto criteria = it.value().find("criteria");
                        criteria != it.value().end() && criteria->is_object()) {
                        for (auto c = criteria->begin(); c != criteria->end(); ++c) {
                            if (!c.value().is_string()) continue;
                            if (auto ms = ParseObtained(c.value().get<std::string>())) progress.SetCriterion(c.key(), ms);
                        }
                    }
                    entries.push_back(Entry{def, std::move(progress)});
                }
                std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
                    const auto da = a.progress.FirstProgressDate(), db = b.progress.FirstProgressDate();
                    if (!da) return false;
                    if (!db) return true;
                    return *da < *db;
                });
                for (Entry& e : entries) {
                    StartProgress(*e.def, std::move(e.progress));
                    m_progressChanged.insert(e.def);
                    MarkForVisibilityUpdate(*e.def);
                }
                if (unknown > 0) {
                    Log::Info("[Advancements] %zu entries in %s name advancements not loaded here (kept as they are)",
                              unknown, m_file.string().c_str());
                }
            }
        }
        CheckForAutomaticTriggers();
        for (const auto& def : m_registry->definitions) RegisterListeners(*def);
    }

    void PlayerAdvancements::Save() const {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_file.empty()) return;
        nlohmann::ordered_json json = nlohmann::ordered_json::object();
        for (const Definition* def : m_progressOrder) {
            auto it = m_progress.find(def);
            if (it == m_progress.end() || !it->second.HasProgress()) continue;
            nlohmann::ordered_json criteria = nlohmann::ordered_json::object();
            for (const auto& [name, progress] : it->second.Criteria()) {
                if (progress.obtainedMs) criteria[name] = FormatObtained(*progress.obtainedMs);
            }
            nlohmann::ordered_json entry = nlohmann::ordered_json::object();
            entry["criteria"] = std::move(criteria);
            entry["done"] = it->second.IsDone();
            json[def->id] = std::move(entry);
        }
        for (const auto& [id, text] : m_preserved) {
            if (json.contains(id)) continue;
            nlohmann::ordered_json entry = nlohmann::ordered_json::parse(text, nullptr, false);
            if (!entry.is_discarded()) json[id] = std::move(entry);
        }
        json["DataVersion"] = Game::Save::DataVersion();

        std::error_code ec;
        std::filesystem::create_directories(m_file.parent_path(), ec);
        // Written beside, then moved over: a crash mid-write never leaves a
        // truncated progress file.
        const std::filesystem::path tmp = m_file.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                Log::Error("[Advancements] Couldn't save player advancements to %s", m_file.string().c_str());
                return;
            }
            out << json.dump(2);
        }
        std::filesystem::rename(tmp, m_file, ec);
        if (ec) {
            std::filesystem::remove(m_file, ec);
            std::filesystem::rename(tmp, m_file, ec);
            if (ec) Log::Error("[Advancements] Couldn't save player advancements to %s: %s",
                               m_file.string().c_str(), ec.message().c_str());
        }
    }

    // ── Progress ─────────────────────────────────────────────────────────

    void PlayerAdvancements::StartProgress(const Definition& advancement, AdvancementProgress progress) {
        progress.Update(advancement.requirements);
        auto [it, inserted] = m_progress.insert_or_assign(&advancement, std::move(progress));
        (void)it;
        if (inserted) m_progressOrder.push_back(&advancement);
    }

    AdvancementProgress& PlayerAdvancements::GetOrStartProgress(const Definition& advancement) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_progress.find(&advancement);
        if (it == m_progress.end()) {
            StartProgress(advancement, AdvancementProgress{});
            it = m_progress.find(&advancement);
        }
        return it->second;
    }

    void PlayerAdvancements::CheckForAutomaticTriggers() {
        // An advancement with no criteria is granted on load, with its
        // rewards (MC grants them on every load; vanilla has none).
        for (const auto& def : m_registry->definitions) {
            if (!def->criteria.empty()) continue;
            Award(*def, "");
            m_pendingCompletions.push_back(Completion{def.get(), /*announce*/ false});
        }
    }

    bool PlayerAdvancements::Award(const Definition& advancement, const std::string& criterion) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        bool result = false;
        AdvancementProgress& progress = GetOrStartProgress(advancement);
        const bool wasDone = progress.IsDone();
        if (progress.Grant(criterion, NowMs())) {
            UnregisterListeners(advancement);
            m_progressChanged.insert(&advancement);
            result = true;
            if (!wasDone && progress.IsDone()) {
                m_pendingCompletions.push_back(Completion{&advancement, /*announce*/ true});
            }
        }
        if (!wasDone && progress.IsDone()) MarkForVisibilityUpdate(advancement);
        return result;
    }

    bool PlayerAdvancements::Revoke(const Definition& advancement, const std::string& criterion) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        bool result = false;
        AdvancementProgress& progress = GetOrStartProgress(advancement);
        const bool wasDone = progress.IsDone();
        if (progress.Revoke(criterion)) {
            RegisterListeners(advancement);
            m_progressChanged.insert(&advancement);
            result = true;
        }
        if (wasDone && !progress.IsDone()) MarkForVisibilityUpdate(advancement);
        return result;
    }

    void PlayerAdvancements::MarkForVisibilityUpdate(const Definition& advancement) {
        if (const Game::Advancements::Node* node = m_registry->tree.Get(advancement.id)) {
            m_rootsToUpdate.insert(node->Root());
        }
    }

    // ── Listeners ────────────────────────────────────────────────────────

    void PlayerAdvancements::AddListener(const Listener& listener) {
        const auto type = TriggerByName(listener.criterion->trigger);
        if (!type) return;
        auto& list = m_listeners[static_cast<size_t>(*type)];
        if (std::find(list.begin(), list.end(), listener) == list.end()) list.push_back(listener);
    }

    void PlayerAdvancements::RemoveListener(const Listener& listener) {
        const auto type = TriggerByName(listener.criterion->trigger);
        if (!type) return;
        auto& list = m_listeners[static_cast<size_t>(*type)];
        list.erase(std::remove(list.begin(), list.end(), listener), list.end());
    }

    void PlayerAdvancements::RegisterListeners(const Definition& advancement) {
        AdvancementProgress& progress = GetOrStartProgress(advancement);
        if (progress.IsDone()) return;
        for (const CriterionDef& criterion : advancement.criteria) {
            const auto* cp = progress.GetCriterion(criterion.name);
            if (cp && !cp->IsDone()) AddListener(Listener{&advancement, &criterion});
        }
    }

    void PlayerAdvancements::ReRegisterListeners() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        for (const auto& def : m_registry->definitions) RegisterListeners(*def);
    }

    void PlayerAdvancements::UnregisterListeners(const Definition& advancement) {
        AdvancementProgress& progress = GetOrStartProgress(advancement);
        const bool allDone = progress.IsDone();
        for (const CriterionDef& criterion : advancement.criteria) {
            const auto* cp = progress.GetCriterion(criterion.name);
            if (!cp) continue;
            if (cp->IsDone() || allDone) RemoveListener(Listener{&advancement, &criterion});
        }
    }

    // ── Flush ────────────────────────────────────────────────────────────

    void PlayerAdvancements::UpdateTreeVisibility(const Game::Advancements::Node& root,
                                                  std::vector<const Game::Advancements::Node*>& added,
                                                  std::vector<std::string>& removed) {
        std::vector<VisibilityRule> ascendants(3, VisibilityRule::NoChange);
        EvaluateVisibility(
            root, ascendants,
            [this](const Game::Advancements::Node& node) { return GetOrStartProgress(*node.def).IsDone(); },
            [&](const Game::Advancements::Node& node, bool shouldBeVisible) {
                const Definition* def = node.def;
                if (shouldBeVisible) {
                    if (m_visible.insert(def).second) {
                        added.push_back(&node);
                        if (m_progress.count(def)) m_progressChanged.insert(def);
                    }
                } else if (m_visible.erase(def)) {
                    removed.push_back(def->id);
                }
            });
    }

    void PlayerAdvancements::RunCompletions(ServerPlayer& player) {
        if (m_pendingCompletions.empty()) return;
        std::vector<Completion> completions;
        completions.swap(m_pendingCompletions);
        for (const Completion& c : completions) {
            const Definition& def = *c.advancement;
            // AdvancementRewards.grant.
            if (def.rewards.experience != 0) player.getExperience().GivePoints(def.rewards.experience);
            if (!def.rewards.loot.empty()) {
                static Game::JavaRandom s_random(static_cast<int64_t>(NowMs()));
                Game::ChestLoot::LootLevelContext lootLevel;
                lootLevel.dimensionId = player.getDimensionId();
                lootLevel.origin = player.getPosition();
                std::shared_ptr<PlayerSession> session;
                if (g_integratedServer && g_integratedServer->GetSessionManager()) {
                    session = g_integratedServer->GetSessionManager()->GetSession(player.getPlayerId());
                }
                for (const std::string& table : def.rewards.loot) {
                    std::vector<Game::ItemStack> items;
                    if (!Game::ChestLoot::GetRandomItems(table, s_random, player.getLuck(), items, &lootLevel)) continue;
                    for (const Game::ItemStack& stack : items) {
                        if (session) session->GiveItem(stack);
                    }
                }
            }
            // Recipe rewards fill the recipe book, which this engine does
            // not have; function rewards need a function system it does not
            // have either. Both are carried in the data and skipped here.
            if (c.announce && def.display && def.display->announceToChat &&
                Game::Rules::GetBool(Game::Rules::Id::ShowAdvancementMessages) && g_integratedServer) {
                BroadcastAnnouncement(player, def);
            }
        }
    }

    void PlayerAdvancements::FlushDirty(ServerPlayer& player, ServerConnection* connection, bool showAdvancements) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        RunCompletions(player);
        if (m_isFirstPacket || !m_rootsToUpdate.empty() || !m_progressChanged.empty()) {
            std::vector<const Game::Advancements::Node*> added;
            std::vector<std::string> removed;
            for (const Game::Advancements::Node* root : m_rootsToUpdate) UpdateTreeVisibility(*root, added, removed);
            m_rootsToUpdate.clear();

            Network::UpdateAdvancementsS2CPacket packet;
            for (const Definition* def : m_progressOrder) {
                if (!m_progressChanged.count(def) || !m_visible.count(def)) continue;
                packet.progress.emplace_back(def->id, m_progress.at(def));
            }
            m_progressChanged.clear();
            if (!packet.progress.empty() || !added.empty() || !removed.empty()) {
                packet.reset = m_isFirstPacket;
                packet.added.reserve(added.size());
                for (const Game::Advancements::Node* node : added) packet.added.push_back(EntryOf(*node->def));
                packet.removed = std::move(removed);
                packet.showAdvancements = showAdvancements;
                if (connection) {
                    connection->SendPacket(static_cast<uint8_t>(Network::PacketId::UpdateAdvancementsS2C),
                                           Network::Serialization::Serialize(packet));
                }
            }
        }
        m_isFirstPacket = false;
    }

    void PlayerAdvancements::SetSelectedTab(const Definition* tab, ServerConnection* connection) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        const Definition* old = m_lastSelectedTab;
        m_lastSelectedTab = (tab && tab->IsRoot() && tab->display) ? tab : nullptr;
        if (old != m_lastSelectedTab && connection) {
            Network::SelectAdvancementsTabS2CPacket packet;
            if (m_lastSelectedTab) packet.tab = m_lastSelectedTab->id;
            connection->SendPacket(static_cast<uint8_t>(Network::PacketId::SelectAdvancementsTabS2C),
                                   Network::Serialization::Serialize(packet));
        }
    }

} // namespace Server::Advancements
