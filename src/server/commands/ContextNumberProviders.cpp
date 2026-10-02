// File: src/server/commands/ContextNumberProviders.cpp
#include "ContextNumberProviders.hpp"
#include "CommandStorage.hpp"
#include "NbtPath.hpp"
#include "SnbtParser.hpp"

#include "common/core/JavaRandom.hpp"
#include "common/world/block/BlockRegistry.hpp"
#include "common/world/enchantment/EnchantmentEffects.hpp"
#include "common/world/enchantment/EnchantmentValueEffect.hpp"
#include "common/world/loot/ContextNumberProviderNames.hpp"
#include "common/world/tags/DataTags.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace Server::NumberProviders {

    namespace {

        using json = nlohmann::json;

        // Java's ArithmeticException: getFloat / getInt turn it into 0.
        struct ArithmeticError : std::runtime_error {
            explicit ArithmeticError(const std::string& what) : std::runtime_error(what) {}
        };

        // ── Java number semantics ───────────────────────────────────────────

        int32_t JavaToInt(double v) {   // Java's (int) cast of a float/double
            if (std::isnan(v)) return 0;
            if (v >= 2147483647.0) return std::numeric_limits<int32_t>::max();
            if (v <= -2147483648.0) return std::numeric_limits<int32_t>::min();
            return static_cast<int32_t>(v);
        }
        int64_t JavaToLong(double v) {
            if (std::isnan(v)) return 0;
            if (v >= 9.2233720368547758e18) return std::numeric_limits<int64_t>::max();
            if (v <= -9.2233720368547758e18) return std::numeric_limits<int64_t>::min();
            return static_cast<int64_t>(v);
        }
        int32_t LongToIntSafe(int64_t v) {   // ContextIntProvider.longToIntSafe
            if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) {
                throw ArithmeticError("Value " + std::to_string(v) + " can't be safely converted to int");
            }
            return static_cast<int32_t>(v);
        }
        int32_t FloatToIntSafe(float v) {    // ContextIntProvider.floatToIntSafe
            if (!std::isfinite(v)) throw ArithmeticError("Value can't be safely converted to int");
            return LongToIntSafe(JavaToLong(v));
        }

        // MC Mth.sin / Mth.cos: the 65536-entry table.
        const std::array<float, 65536>& SinTable() {
            static const std::array<float, 65536> table = [] {
                std::array<float, 65536> t{};
                for (int i = 0; i < 65536; ++i) t[static_cast<size_t>(i)] = static_cast<float>(std::sin(i * 3.141592653589793 * 2.0 / 65536.0));
                return t;
            }();
            return table;
        }
        float MthSin(double v) { return SinTable()[static_cast<size_t>(JavaToLong(v * 10430.378350470453) & 65535)]; }
        float MthCos(double v) { return SinTable()[static_cast<size_t>(JavaToLong(v * 10430.378350470453 + 16384.0) & 65535)]; }

        // Mth.nextFloat / Mth.nextInt.
        float NextFloat(Game::JavaRandom& r, float min, float max) { return min >= max ? min : r.NextFloat() * (max - min) + min; }
        int32_t NextInt(Game::JavaRandom& r, int32_t min, int32_t max) {
            return min >= max ? min : r.NextInt(max - min + 1) + min;
        }

        // ── JSON from NBT (the codecs are read over the SNBT's tree) ────────

        json ToJson(const ::World::NBTTag& tag) {
            using T = ::World::NBTTagType;
            switch (tag.type) {
                case T::TAG_Byte: {
                    const int8_t v = static_cast<const ::World::NBTTagByte&>(tag).value;
                    if (v == 0 || v == 1) return json(v == 1);   // SNBT true/false
                    return json(static_cast<int>(v));
                }
                case T::TAG_Short:  return json(static_cast<int>(static_cast<const ::World::NBTTagShort&>(tag).value));
                case T::TAG_Int:    return json(static_cast<const ::World::NBTTagInt&>(tag).value);
                case T::TAG_Long:   return json(static_cast<const ::World::NBTTagLong&>(tag).value);
                case T::TAG_Float:  return json(static_cast<double>(static_cast<const ::World::NBTTagFloat&>(tag).value));
                case T::TAG_Double: return json(static_cast<const ::World::NBTTagDouble&>(tag).value);
                case T::TAG_String: return json(static_cast<const ::World::NBTTagString&>(tag).value);
                case T::TAG_Byte_Array: { json a = json::array(); for (int8_t v : static_cast<const ::World::NBTTagByteArray&>(tag).value) a.push_back(v); return a; }
                case T::TAG_Int_Array:  { json a = json::array(); for (int32_t v : static_cast<const ::World::NBTTagIntArray&>(tag).value) a.push_back(v); return a; }
                case T::TAG_Long_Array: { json a = json::array(); for (int64_t v : static_cast<const ::World::NBTTagLongArray&>(tag).value) a.push_back(v); return a; }
                case T::TAG_List: {
                    json a = json::array();
                    for (const auto& e : static_cast<const ::World::NBTTagList&>(tag).value) if (e) a.push_back(ToJson(*e));
                    return a;
                }
                case T::TAG_Compound: {
                    json o = json::object();
                    for (const auto& [k, v] : static_cast<const ::World::NBTTagCompound&>(tag).value) if (v) o[k] = ToJson(*v);
                    return o;
                }
                case T::TAG_End: break;
            }
            return json();
        }

        bool NumberOf(const json& j, double& out) {
            if (j.is_number()) { out = j.get<double>(); return true; }
            if (j.is_boolean()) { out = j.get<bool>() ? 1.0 : 0.0; return true; }
            return false;
        }

        std::string Bare(std::string id) {
            if (id.rfind("minecraft:", 0) == 0) id.erase(0, 10);
            return id;
        }

        struct ParseError : std::runtime_error {
            explicit ParseError(const std::string& what) : std::runtime_error(what) {}
        };

    } // namespace

    // ── Conditions (MC LootItemCondition) ───────────────────────────────────

    struct Condition {
        enum class Kind : uint8_t {
            Delegate, Inverted, AllOf, AnyOf, MatchBlock, IntCheck, FloatCheck, TableBonus,
            SurvivesExplosion, KilledByPlayer, EntityScores, False
        };
        Kind kind = Kind::False;
        Game::EnchantmentCondition delegate;
        std::vector<std::shared_ptr<Condition>> terms;
        // match_block
        std::vector<std::string> blocks;            // ids (bare) and "#tag"s; empty = any
        bool anyBlock = true;
        struct StateMatch { std::string name; bool ranged = false; std::string exact, min, max; bool hasMin = false, hasMax = false; };
        std::vector<StateMatch> state;
        bool needsUnsupported = false;              // nbt / components (deviation: fails)
        // value checks: a point, or a line
        Provider value, point, rangeMin, rangeMax;
        // table_bonus
        std::vector<float> chances;
    };

    struct Node {
        enum class Kind : uint8_t {
            Constant, Abs, Avg, Ceil, Conditional, Cos, Sub, EnchantmentLevel, Floor, FromInt, FromFloat,
            Length, Max, Min, Mod, FloorMod, FloorDiv, Negate, Dispatcher, Pow, Mul, Div, Round, Sin, Sqrt,
            Storage, Add, Truncate, Uniform, Weighted, Binomial, Score
        };
        bool isInt = false;
        Kind kind = Kind::Constant;
        float fconst = 0.0f;
        int32_t iconst = 0;
        Provider a, b, c;                                     // input / left,right / base,exponent / min,max / on_true,on_false
        std::vector<Provider> inputs;                         // aggregates
        std::vector<std::pair<Provider, int>> weighted;       // weighted_list
        std::vector<std::pair<std::shared_ptr<Condition>, Provider>> cases;   // number_dispatcher
        std::shared_ptr<Condition> condition;                 // conditional
        Game::LevelBasedValue level;                          // enchantment_level
        std::string storageId;                                // storage
        Nbt::Path path;
    };

    namespace {

        float EvalF(const Node& n, Context& ctx);
        int32_t EvalI(const Node& n, Context& ctx);
        bool Test(const Condition& c, Context& ctx);

        Game::JavaRandom& RandomOf(Context& ctx) {
            if (!ctx.random) throw ArithmeticError("no random source");
            return *ctx.random;
        }

        float FloatOrThrow(const Node& n, Context& ctx) {   // getFloatOrThrow
            const float v = EvalF(n, ctx);
            if (!std::isfinite(v)) throw ArithmeticError("Invalid value");
            return v;
        }

        const Provider& PickWeighted(const Node& n, Context& ctx) {
            int total = 0;
            for (const auto& [p, w] : n.weighted) total += w;
            if (total <= 0) throw ArithmeticError("empty weighted list");
            int selection = RandomOf(ctx).NextInt(total);
            for (const auto& [p, w] : n.weighted) {
                selection -= w;
                if (selection < 0) return p;
            }
            return n.weighted.back().first;
        }

        const Provider& Dispatch(const Node& n, Context& ctx) {
            for (const auto& [cond, value] : n.cases) if (Test(*cond, ctx)) return value;
            return n.c;   // default
        }

        // StoredNumberAccess.getNumericTag.
        bool StoredNumber(const Node& n, double& out) {
            auto data = CommandStorage::Get(n.storageId);
            std::vector<Nbt::TagPtr> tags;
            std::string error;
            if (!n.path.Get(data, tags, error) || tags.size() != 1 || !tags[0] || !Nbt::IsNumeric(*tags[0])) return false;
            out = Nbt::NumericValue(*tags[0]);
            return true;
        }

        float EvalF(const Node& n, Context& ctx) {
            using K = Node::Kind;
            if (n.isInt) return static_cast<float>(EvalI(n, ctx));
            switch (n.kind) {
                case K::Constant: return n.fconst;
                case K::Abs:      return std::fabs(EvalF(*n.a, ctx));
                case K::Avg: {
                    float sum = 0.0f;
                    int count = 0;
                    for (const auto& p : n.inputs) { sum += EvalF(*p, ctx); ++count; }
                    return sum / static_cast<float>(count);
                }
                case K::Ceil: {
                    const float v = EvalF(*n.a, ctx);
                    const int32_t i = JavaToInt(v);
                    return static_cast<float>(v > static_cast<float>(i) ? i + 1 : i);
                }
                case K::Conditional: return EvalF(Test(*n.condition, ctx) ? *n.a : *n.b, ctx);
                case K::Cos:      return MthCos(static_cast<double>(EvalF(*n.a, ctx)));
                case K::Sub:      return EvalF(*n.a, ctx) - EvalF(*n.b, ctx);
                // ENCHANTMENT_LEVEL is never set by the compute sources: level 0.
                case K::EnchantmentLevel: return n.level.Calculate(0);
                case K::Floor:    return static_cast<float>(std::floor(static_cast<double>(EvalF(*n.a, ctx))));
                case K::FromInt:  return static_cast<float>(EvalI(*n.a, ctx));
                case K::Length: {
                    float sum = 0.0f;
                    for (const auto& p : n.inputs) { const float v = EvalF(*p, ctx); sum += v * v; }
                    return static_cast<float>(std::sqrt(static_cast<double>(sum)));
                }
                case K::Max: {
                    float v = -3.4028235E38f;
                    for (const auto& p : n.inputs) v = std::max(v, EvalF(*p, ctx));
                    return v;
                }
                case K::Min: {
                    float v = std::numeric_limits<float>::max();
                    for (const auto& p : n.inputs) v = std::min(v, EvalF(*p, ctx));
                    return v;
                }
                case K::Mod: {
                    const float right = EvalF(*n.b, ctx);
                    if (right == 0.0f) return std::numeric_limits<float>::quiet_NaN();
                    const float left = EvalF(*n.a, ctx);
                    return std::fmod(std::fmod(left, right) + right, right);   // Mth.positiveModulo
                }
                case K::Negate:   return -EvalF(*n.a, ctx);
                case K::Dispatcher: return EvalF(*Dispatch(n, ctx), ctx);
                case K::Pow:      return static_cast<float>(std::pow(static_cast<double>(EvalF(*n.a, ctx)), static_cast<double>(EvalF(*n.b, ctx))));
                case K::Mul: {
                    float v = 1.0f;
                    for (const auto& p : n.inputs) v *= EvalF(*p, ctx);
                    return v;
                }
                case K::Div:      return EvalF(*n.a, ctx) / EvalF(*n.b, ctx);
                case K::Round:    return static_cast<float>(JavaToInt(std::floor(static_cast<double>(EvalF(*n.a, ctx)) + 0.5)));
                case K::Sin:      return MthSin(static_cast<double>(EvalF(*n.a, ctx)));
                case K::Sqrt:     return static_cast<float>(std::sqrt(static_cast<double>(EvalF(*n.a, ctx))));
                case K::Storage: {
                    double v = 0.0;
                    return StoredNumber(n, v) ? static_cast<float>(v) : EvalF(*n.c, ctx);
                }
                case K::Add: {
                    float v = 0.0f;
                    for (const auto& p : n.inputs) v += EvalF(*p, ctx);
                    return v;
                }
                case K::Truncate: {
                    const float v = EvalF(*n.a, ctx);
                    return v > 0.0f ? static_cast<float>(std::floor(v)) : static_cast<float>(std::ceil(v));
                }
                case K::Uniform:  return NextFloat(RandomOf(ctx), EvalF(*n.a, ctx), EvalF(*n.b, ctx));
                case K::Weighted: return EvalF(*PickWeighted(n, ctx), ctx);
                default: break;
            }
            throw ArithmeticError("not a float provider");
        }

        int32_t EvalI(const Node& n, Context& ctx) {
            using K = Node::Kind;
            if (!n.isInt) return FloatToIntSafe(EvalF(n, ctx));
            switch (n.kind) {
                case K::Constant: return n.iconst;
                case K::Abs: {
                    const int32_t v = EvalI(*n.a, ctx);
                    if (v == std::numeric_limits<int32_t>::min()) throw ArithmeticError("Overflow to represent absolute value");
                    return v < 0 ? -v : v;
                }
                case K::Avg: {
                    int64_t sum = 0, count = 0;
                    for (const auto& p : n.inputs) { sum += EvalI(*p, ctx); ++count; }
                    if (count == 0) throw ArithmeticError("/ by zero");
                    return LongToIntSafe(sum / count);
                }
                case K::Binomial: {
                    const int32_t trials = EvalI(*n.a, ctx);
                    const float p = FloatOrThrow(*n.b, ctx);
                    Game::JavaRandom& r = RandomOf(ctx);
                    int32_t result = 0;
                    for (int32_t i = 0; i < trials; ++i) if (r.NextFloat() < p) ++result;
                    return result;
                }
                case K::Conditional: return EvalI(Test(*n.condition, ctx) ? *n.a : *n.b, ctx);
                case K::Sub: return LongToIntSafe(static_cast<int64_t>(EvalI(*n.a, ctx)) - EvalI(*n.b, ctx));
                case K::FromFloat: return FloatToIntSafe(EvalF(*n.a, ctx));
                case K::Max: {
                    int32_t v = std::numeric_limits<int32_t>::min();
                    for (const auto& p : n.inputs) v = std::max(v, EvalI(*p, ctx));
                    return v;
                }
                case K::Min: {
                    int32_t v = std::numeric_limits<int32_t>::max();
                    for (const auto& p : n.inputs) v = std::min(v, EvalI(*p, ctx));
                    return v;
                }
                case K::FloorMod: {
                    const int32_t x = EvalI(*n.a, ctx), y = EvalI(*n.b, ctx);
                    if (y == 0) throw ArithmeticError("/ by zero");
                    const int64_t m = static_cast<int64_t>(x) % y;
                    return static_cast<int32_t>((m != 0 && ((m < 0) != (y < 0))) ? m + y : m);
                }
                case K::FloorDiv: {
                    const int32_t x = EvalI(*n.a, ctx), y = EvalI(*n.b, ctx);
                    if (y == 0) throw ArithmeticError("/ by zero");
                    if (x == std::numeric_limits<int32_t>::min() && y == -1) throw ArithmeticError("integer overflow");
                    int32_t q = x / y;
                    if ((x % y != 0) && ((x < 0) != (y < 0))) --q;
                    return q;
                }
                case K::Mod: {
                    const int32_t x = EvalI(*n.a, ctx), y = EvalI(*n.b, ctx);
                    if (y == 0) throw ArithmeticError("/ by zero");
                    if (y == -1) return 0;
                    return x % y;
                }
                case K::Div: {
                    const int32_t x = EvalI(*n.a, ctx), y = EvalI(*n.b, ctx);
                    if (y == 0) throw ArithmeticError("/ by zero");
                    if (x == std::numeric_limits<int32_t>::min() && y == -1) return x;   // Java wraps
                    return x / y;
                }
                case K::Negate: {
                    const int32_t v = EvalI(*n.a, ctx);
                    if (v == std::numeric_limits<int32_t>::min()) throw ArithmeticError("integer overflow");
                    return -v;
                }
                case K::Dispatcher: return EvalI(*Dispatch(n, ctx), ctx);
                case K::Pow: {
                    // Math.powExact: a negative exponent and overflow throw.
                    const int32_t base = EvalI(*n.a, ctx), exponent = EvalI(*n.b, ctx);
                    if (base == 0 && exponent == 0) throw ArithmeticError("Result of 0 to the power of 0 is undefined");
                    if (exponent < 0) throw ArithmeticError("negative exponent");
                    if (base == 0) return 0;
                    if (base == 1) return 1;
                    if (base == -1) return (exponent % 2 == 0) ? 1 : -1;
                    int64_t result = 1;   // |base| >= 2: overflows within 32 steps
                    for (int32_t i = 0; i < exponent; ++i) {
                        result *= base;
                        if (result > std::numeric_limits<int32_t>::max() || result < std::numeric_limits<int32_t>::min()) {
                            throw ArithmeticError("integer overflow");
                        }
                    }
                    return static_cast<int32_t>(result);
                }
                case K::Mul: {
                    // A Java long product (wrapping), then longToIntSafe.
                    uint64_t v = 1;
                    for (const auto& p : n.inputs) v *= static_cast<uint64_t>(static_cast<int64_t>(EvalI(*p, ctx)));
                    return LongToIntSafe(static_cast<int64_t>(v));
                }
                case K::Score:
                    // DEVIATION: no scoreboard in this engine — every score is
                    // absent, which MC answers with the fallback.
                    return EvalI(*n.c, ctx);
                case K::Storage: {
                    double v = 0.0;
                    return StoredNumber(n, v) ? JavaToInt(v) : EvalI(*n.c, ctx);
                }
                case K::Add: {
                    int64_t v = 0;
                    for (const auto& p : n.inputs) v += EvalI(*p, ctx);
                    return LongToIntSafe(v);
                }
                case K::Uniform:  return NextInt(RandomOf(ctx), EvalI(*n.a, ctx), EvalI(*n.b, ctx));
                case K::Weighted: return EvalI(*PickWeighted(n, ctx), ctx);
                default: break;
            }
            throw ArithmeticError("not an int provider");
        }

        // ── Condition tests ─────────────────────────────────────────────────

        bool RangeTest(const Condition& c, Context& ctx, double input, bool isInt) {
            // FloatRangePredicate / IntRangePredicate: Point = equal; Line =
            // min <= input <= max with either bound optional.
            const auto value = [&](const Provider& p) -> double {
                return isInt ? static_cast<double>(GetInt(p, ctx)) : static_cast<double>(GetFloat(p, ctx));
            };
            if (c.point) return input == value(c.point);
            if (c.rangeMin && input < value(c.rangeMin)) return false;
            if (c.rangeMax && input > value(c.rangeMax)) return false;
            return true;
        }

        int ValueIndex(Game::PropertyId prop, const std::string& name) {
            for (uint16_t i = 0; i < Game::BlockStates::PropertyValueCount(prop); ++i) {
                if (Game::BlockStates::PropertyValueName(prop, i) == name) return i;
            }
            return -1;
        }

        bool MatchBlock(const Condition& c, Context& ctx) {
            if (!ctx.hasBlockState) return false;
            const Game::BlockState state = ctx.blockState;
            const Game::BlockID block = state.Block();
            const std::string slug = Game::BlockRegistry::Get(block).registrySlug;
            if (!c.anyBlock) {
                bool in = false;
                for (const std::string& b : c.blocks) {
                    if (!b.empty() && b[0] == '#') {
                        if (Game::DataTags::HasTag(Game::DataTags::Registry::Block, slug, b.substr(1))) { in = true; break; }
                    } else if (b == slug) {
                        in = true;
                        break;
                    }
                }
                if (!in) return false;
            }
            for (const Condition::StateMatch& m : c.state) {
                Game::PropertyId prop{};
                bool found = false;
                for (uint16_t s = 0; s < Game::BlockStates::PropertyCount(block); ++s) {
                    const Game::PropertyId p = Game::BlockStates::PropertyAt(block, s);
                    if (Game::BlockStates::PropertyName(p) == m.name) { prop = p; found = true; break; }
                }
                if (!found) return false;
                const std::string have(state.GetName(prop));
                if (!m.ranged) {
                    if (have != m.exact) return false;
                    continue;
                }
                // RangedMatcher: compared as the property's values (ints
                // numerically, others by declaration order).
                const int at = ValueIndex(prop, have);
                char* end = nullptr;
                const long haveNum = std::strtol(have.c_str(), &end, 10);
                const bool numeric = end && *end == '\0' && !have.empty();
                const auto compare = [&](const std::string& bound) -> int {
                    if (numeric) {
                        char* e = nullptr;
                        const long b = std::strtol(bound.c_str(), &e, 10);
                        if (e && *e == '\0') return haveNum < b ? -1 : haveNum > b ? 1 : 0;
                    }
                    const int bi = ValueIndex(prop, bound);
                    return at < bi ? -1 : at > bi ? 1 : 0;
                };
                if (m.hasMin && compare(m.min) < 0) return false;
                if (m.hasMax && compare(m.max) > 0) return false;
            }
            // DEVIATION: nbt / components block sub-predicates are not modelled.
            return !c.needsUnsupported;
        }

        bool Test(const Condition& c, Context& ctx) {
            using K = Condition::Kind;
            switch (c.kind) {
                case K::Inverted: return !Test(*c.terms.front(), ctx);
                case K::AllOf:
                    for (const auto& t : c.terms) if (!Test(*t, ctx)) return false;
                    return true;
                case K::AnyOf:
                    for (const auto& t : c.terms) if (Test(*t, ctx)) return true;
                    return false;
                case K::MatchBlock: return MatchBlock(c, ctx);
                case K::IntCheck:   return RangeTest(c, ctx, static_cast<double>(GetInt(c.value, ctx)), true);
                case K::FloatCheck: return RangeTest(c, ctx, static_cast<double>(GetFloat(c.value, ctx)), false);
                case K::TableBonus: {
                    // No TOOL in the compute contexts: level 0.
                    const float chance = c.chances.empty() ? 0.0f : c.chances.front();
                    return ctx.random && ctx.random->NextFloat() < chance;
                }
                case K::SurvivesExplosion: return true;    // no EXPLOSION_RADIUS
                case K::KilledByPlayer:    return false;   // no LAST_DAMAGE_PLAYER
                case K::EntityScores:      return false;   // DEVIATION: no scoreboard
                case K::Delegate: {
                    Game::EnchantmentContext e;
                    e.level = ctx.level;
                    e.blocks = ctx.blocks;
                    e.random = ctx.random;
                    e.thisEntity = ctx.thisEntity;
                    e.origin = ctx.origin;
                    e.hasBlockState = ctx.hasBlockState;
                    e.blockState = ctx.blockState;
                    return c.delegate.Test(e);
                }
                case K::False: return false;
            }
            return false;
        }

        // ── Parsing (the providers' codecs over the SNBT tree) ──────────────

        Provider ParseHolder(const json& j, bool isInt);
        std::shared_ptr<Condition> ParseCondition(const json& j);

        // The built-in registries, defined in the same SNBT form the codecs
        // read (MC's bootstraps), parsed once on first use.
        const json& PredicateDefinitions() {
            static const json defs = {
                {"block/fast_cooking", {{"condition", "minecraft:match_block"}, {"blocks", {"smoker", "blast_furnace"}}}},
                {"tool/can_shear", {{"condition", "minecraft:match_tool"}, {"predicate", {{"items", "shears"}}}}},
                {"tool/can_silk_touch", {{"condition", "minecraft:match_tool"},
                                         {"predicate", {{"predicates", {{"enchantments", json::array({{{"enchantments", "silk_touch"}, {"levels", {{"min", 1}}}}})}}}}}}},
            };
            return defs;
        }

        const std::unordered_map<std::string, Provider>& Registry(bool isInt) {
            static std::unordered_map<std::string, Provider> floats, ints;
            static bool built = false;
            if (!built) {
                built = true;
                const auto fast = [](const char* onTrue, const char* onFalse) {
                    return json{{"type", "conditional"}, {"condition", "minecraft:block/fast_cooking"},
                                {"on_true", onTrue}, {"on_false", onFalse}};
                };
                floats["cooking/normal_speed_multiplier"] = ParseHolder(1.0, false);
                floats["cooking/fast_speed_multiplier"] = ParseHolder(2.0, false);
                floats["brewing/speed_default"] = ParseHolder(1.0, false);
                floats["cooking/speed_default"] = ParseHolder(fast("cooking/fast_speed_multiplier", "cooking/normal_speed_multiplier"), false);

                ints["cooking/normal_burn_time_reduction_factor"] = ParseHolder(1, true);
                ints["cooking/fast_burn_time_reduction_factor"] = ParseHolder(2, true);
                ints["brewing/uses_default"] = ParseHolder(20, true);
                const auto compostable = [](int chance) -> json {
                    if (chance >= 100) return 1;
                    return {{"type", "number_dispatcher"},
                            {"cases", json::array({{{"condition", {{"condition", "minecraft:match_block"}, {"blocks", "composter"},
                                                                    {"state", {{"level", "0"}}}}},
                                                    {"value", 1}}})},
                            {"default", {{"type", "weighted_list"},
                                         {"distribution", json::array({{{"data", 1}, {"weight", chance}},
                                                                       {{"data", 0}, {"weight", 100 - chance}}})}}}};
                };
                ints["compostable/low"] = ParseHolder(compostable(30), true);
                ints["compostable/low_medium"] = ParseHolder(compostable(50), true);
                ints["compostable/medium"] = ParseHolder(compostable(65), true);
                ints["compostable/medium_high"] = ParseHolder(compostable(85), true);
                ints["compostable/always_add_one"] = ParseHolder(compostable(100), true);
                const std::pair<const char*, int> cooking[] = {
                    {"time_bamboo", 50}, {"time_wool_slabs", 50}, {"time_wool_carpets", 67}, {"time_dry_plants", 100},
                    {"time_wood_items_extra_small", 100}, {"time_wool", 100}, {"time_wood_slabs", 150},
                    {"time_wood_items_large", 200}, {"time_roots", 300}, {"time_wood_blocks", 300},
                    {"time_wood_items_small", 300}, {"time_hanging_signs", 800}, {"time_boats", 1200},
                    {"time_coal", 1600}, {"time_blaze_rod", 2400}, {"time_dried_kelp_block", 4001},
                    {"time_coal_block", 16000}, {"time_lava_bucket", 20000}};
                for (const auto& [name, seconds] : cooking) {
                    ints[std::string("cooking/") + name] = ParseHolder(
                        json{{"type", "div"}, {"left", seconds},
                             {"right", fast("cooking/fast_burn_time_reduction_factor", "cooking/normal_burn_time_reduction_factor")}},
                        true);
                }
            }
            return isInt ? ints : floats;
        }

        Provider Reference(const std::string& rawId, bool isInt) {
            std::string id = rawId;
            if (id.rfind("minecraft:", 0) != 0 && id.find(':') == std::string::npos) id = "minecraft:" + id;
            const auto& registry = Registry(isInt);
            auto it = id.rfind("minecraft:", 0) == 0 ? registry.find(id.substr(10)) : registry.end();
            if (it == registry.end() || !it->second) {
                throw ParseError("Can't find element '" + id + "' of type '" +
                                 (isInt ? "minecraft:context_int_provider" : "minecraft:context_float_provider") + "'");
            }
            return it->second;
        }

        const json& Field(const json& o, const char* name) {
            auto it = o.find(name);
            if (it == o.end()) throw ParseError(std::string("No key ") + name + " in MapLike");
            return *it;
        }

        std::vector<Provider> ParseList(const json& j, bool isInt) {
            // HolderSet: a list (non-empty for the aggregates), or one holder.
            std::vector<Provider> out;
            if (j.is_array()) for (const json& e : j) out.push_back(ParseHolder(e, isInt));
            else out.push_back(ParseHolder(j, isInt));
            if (out.empty()) throw ParseError("inputs must not be empty");
            return out;
        }

        Provider ParseTyped(const json& o, bool isInt) {
            auto node = std::make_shared<Node>();
            node->isInt = isInt;
            const std::string type = Bare(Field(o, "type").get<std::string>());
            using K = Node::Kind;
            const auto unary = [&](K k) { node->kind = k; node->a = ParseHolder(Field(o, "input"), isInt); };
            const auto binary = [&](K k) {
                node->kind = k;
                node->a = ParseHolder(Field(o, "left"), isInt);
                node->b = ParseHolder(Field(o, "right"), isInt);
            };
            const auto aggregate = [&](K k) { node->kind = k; node->inputs = ParseList(Field(o, "inputs"), isInt); };
            if (type == "constant") {
                node->kind = K::Constant;
                double v = 0.0;
                if (!NumberOf(Field(o, "value"), v)) throw ParseError("Not a number: value");
                if (isInt) node->iconst = JavaToInt(v); else node->fconst = static_cast<float>(v);
            } else if (type == "abs") unary(K::Abs);
            else if (type == "avg") aggregate(K::Avg);
            else if (type == "add") aggregate(K::Add);
            else if (type == "mul") aggregate(K::Mul);
            else if (type == "max") aggregate(K::Max);
            else if (type == "min") aggregate(K::Min);
            else if (type == "negate") unary(K::Negate);
            else if (type == "sub") binary(K::Sub);
            else if (type == "div") binary(K::Div);
            else if (type == "mod") binary(K::Mod);
            else if (type == "pow") {
                node->kind = K::Pow;
                node->a = ParseHolder(Field(o, "base"), isInt);
                node->b = ParseHolder(Field(o, "exponent"), isInt);
            } else if (type == "uniform") {
                node->kind = K::Uniform;
                node->a = ParseHolder(Field(o, "min"), isInt);
                node->b = ParseHolder(Field(o, "max"), isInt);
            } else if (type == "conditional") {
                node->kind = K::Conditional;
                node->condition = ParseCondition(Field(o, "condition"));
                node->a = ParseHolder(Field(o, "on_true"), isInt);
                node->b = o.contains("on_false") ? ParseHolder(o["on_false"], isInt) : ParseHolder(0, isInt);
            } else if (type == "number_dispatcher") {
                node->kind = K::Dispatcher;
                const json& cases = Field(o, "cases");
                if (!cases.is_array()) throw ParseError("cases must be a list");
                for (const json& c : cases) {
                    node->cases.emplace_back(ParseCondition(Field(c, "condition")), ParseHolder(Field(c, "value"), isInt));
                }
                node->c = o.contains("default") ? ParseHolder(o["default"], isInt) : ParseHolder(0, isInt);
            } else if (type == "weighted_list") {
                node->kind = K::Weighted;
                const json& dist = Field(o, "distribution");
                if (!dist.is_array() || dist.empty()) throw ParseError("distribution must be a non-empty list");
                for (const json& w : dist) {
                    double weight = 0.0;
                    if (!NumberOf(Field(w, "weight"), weight) || weight < 0) throw ParseError("Invalid weight");
                    node->weighted.emplace_back(ParseHolder(Field(w, "data"), isInt), static_cast<int>(weight));
                }
            } else if (type == "storage") {
                node->kind = K::Storage;
                if (!CommandStorage::NormalizeId(Field(o, "storage").get<std::string>(), node->storageId)) {
                    throw ParseError("Invalid storage id");
                }
                std::string error;
                if (!Nbt::Path::Parse(Field(o, "path").get<std::string>(), node->path, error)) {
                    throw ParseError("Failed to parse path: " + error);
                }
                node->c = o.contains("fallback") ? ParseHolder(o["fallback"], isInt) : ParseHolder(0, isInt);
            } else if (type == "environment_attribute") {
                // DEVIATION: this engine has no environment attributes.
                throw ParseError("environment_attribute providers are not supported by this game (no environment attributes)");
            } else if (!isInt && (type == "ceil" || type == "cos" || type == "floor" || type == "round" ||
                                  type == "sin" || type == "sqrt" || type == "truncate")) {
                unary(type == "ceil" ? K::Ceil : type == "cos" ? K::Cos : type == "floor" ? K::Floor :
                      type == "round" ? K::Round : type == "sin" ? K::Sin : type == "sqrt" ? K::Sqrt : K::Truncate);
            } else if (!isInt && type == "length") {
                aggregate(K::Length);
            } else if (!isInt && type == "from_int") {
                node->kind = K::FromInt;
                node->a = ParseHolder(Field(o, "input"), true);
            } else if (!isInt && type == "enchantment_level") {
                node->kind = K::EnchantmentLevel;
                if (!Game::LevelBasedValue::Parse(Field(o, "amount"), node->level)) throw ParseError("Invalid amount");
            } else if (isInt && type == "from_float") {
                node->kind = K::FromFloat;
                node->a = ParseHolder(Field(o, "input"), false);
            } else if (isInt && type == "binomial") {
                node->kind = K::Binomial;
                node->a = ParseHolder(Field(o, "n"), true);
                node->b = ParseHolder(Field(o, "p"), false);
            } else if (isInt && type == "floor_mod") {
                binary(K::FloorMod);
            } else if (isInt && type == "floor_div") {
                binary(K::FloorDiv);
            } else if (isInt && type == "score") {
                node->kind = K::Score;
                (void)Field(o, "target");
                (void)Field(o, "score").get<std::string>();
                node->c = o.contains("fallback") ? ParseHolder(o["fallback"], true) : ParseHolder(0, true);
            } else {
                throw ParseError("Unknown registry key in ResourceKey[minecraft:root / minecraft:" +
                                 std::string(isInt ? "context_int_provider_type" : "context_float_provider_type") +
                                 "]: minecraft:" + type);
            }
            return node;
        }

        // RegistryCodecs.holder: an id (a reference), a bare number
        // (ConstantValue.INLINE_CODEC), or a typed compound.
        Provider ParseHolder(const json& j, bool isInt) {
            if (j.is_string()) return Reference(j.get<std::string>(), isInt);
            double v = 0.0;
            if (NumberOf(j, v)) {
                auto node = std::make_shared<Node>();
                node->isInt = isInt;
                node->kind = Node::Kind::Constant;
                if (isInt) node->iconst = JavaToInt(v); else node->fconst = static_cast<float>(v);
                return node;
            }
            if (j.is_object()) return ParseTyped(j, isInt);
            throw ParseError("Not a provider: " + j.dump());
        }

        std::shared_ptr<Condition> ParseCondition(const json& j) {
            auto c = std::make_shared<Condition>();
            using K = Condition::Kind;
            if (j.is_string()) {
                const std::string id = Bare(j.get<std::string>());
                const json& defs = PredicateDefinitions();
                auto it = defs.find(id);
                if (it == defs.end()) throw ParseError("Can't find element 'minecraft:" + id + "' of type 'minecraft:predicate'");
                return ParseCondition(*it);
            }
            if (j.is_array()) {   // the list form = all_of
                c->kind = K::AllOf;
                for (const json& t : j) c->terms.push_back(ParseCondition(t));
                return c;
            }
            if (!j.is_object()) throw ParseError("Not a condition: " + j.dump());
            const std::string type = Bare(Field(j, "condition").get<std::string>());
            if (type == "inverted") {
                c->kind = K::Inverted;
                c->terms.push_back(ParseCondition(Field(j, "term")));
            } else if (type == "all_of" || type == "any_of") {
                c->kind = type == "all_of" ? K::AllOf : K::AnyOf;
                const json& terms = Field(j, "terms");
                if (!terms.is_array()) throw ParseError("terms must be a list");
                for (const json& t : terms) c->terms.push_back(ParseCondition(t));
            } else if (type == "match_block") {
                c->kind = K::MatchBlock;
                if (j.contains("blocks")) {
                    c->anyBlock = false;
                    const json& b = j["blocks"];
                    if (b.is_array()) for (const json& e : b) c->blocks.push_back(Bare(e.get<std::string>()));
                    else {
                        std::string s = b.get<std::string>();
                        if (!s.empty() && s[0] == '#') c->blocks.push_back("#" + Bare(s.substr(1)));
                        else c->blocks.push_back(Bare(s));
                    }
                }
                if (j.contains("state")) {
                    for (const auto& [name, value] : j["state"].items()) {
                        Condition::StateMatch m;
                        m.name = name;
                        if (value.is_object()) {
                            m.ranged = true;
                            if (value.contains("min")) { m.hasMin = true; m.min = value["min"].is_string() ? value["min"].get<std::string>() : value["min"].dump(); }
                            if (value.contains("max")) { m.hasMax = true; m.max = value["max"].is_string() ? value["max"].get<std::string>() : value["max"].dump(); }
                        } else {
                            m.exact = value.is_string() ? value.get<std::string>() : value.is_boolean() ? (value.get<bool>() ? "true" : "false") : value.dump();
                        }
                        c->state.push_back(std::move(m));
                    }
                }
                c->needsUnsupported = j.contains("nbt") || j.contains("components") || j.contains("predicates");
            } else if (type == "int_value_check" || type == "float_value_check") {
                const bool isInt = type == "int_value_check";
                c->kind = isInt ? K::IntCheck : K::FloatCheck;
                c->value = ParseHolder(Field(j, "value"), isInt);
                const json& test = Field(j, "test");
                if (test.is_object() && !test.contains("type") && (test.contains("min") || test.contains("max"))) {
                    if (test.contains("min")) c->rangeMin = ParseHolder(test["min"], isInt);
                    if (test.contains("max")) c->rangeMax = ParseHolder(test["max"], isInt);
                } else {
                    c->point = ParseHolder(test, isInt);
                }
            } else if (type == "table_bonus") {
                c->kind = K::TableBonus;
                (void)Field(j, "enchantment");
                const json& chances = Field(j, "chances");
                if (!chances.is_array() || chances.empty()) throw ParseError("chances must be a non-empty list");
                for (const json& v : chances) { double d = 0.0; NumberOf(v, d); c->chances.push_back(static_cast<float>(d)); }
            } else if (type == "survives_explosion") {
                c->kind = K::SurvivesExplosion;
            } else if (type == "killed_by_player") {
                c->kind = K::KilledByPlayer;
            } else if (type == "entity_scores") {
                c->kind = K::EntityScores;
            } else if (type == "environment_attribute_check") {
                // DEVIATION: no environment attributes — the check fails.
                c->kind = K::False;
            } else {
                c->kind = K::Delegate;
                c->delegate = Game::EnchantmentCondition::Parse(j);
            }
            return c;
        }

        // ResourceOrIdArgument: an identifier names a registry entry; anything
        // else is the inline SNBT value.
        bool IsIdentifier(const std::string& s) {
            if (s.empty()) return false;
            int colons = 0;
            for (char ch : s) {
                if (ch == ':') { if (++colons > 1) return false; continue; }
                if (!(std::islower(static_cast<unsigned char>(ch)) || std::isdigit(static_cast<unsigned char>(ch)) ||
                      ch == '_' || ch == '-' || ch == '.' || ch == '/')) {
                    return false;
                }
            }
            return true;
        }

        Provider ParseArgument(const std::string& text, bool isInt, std::string& error) {
            try {
                if (IsIdentifier(text)) return Reference(text, isInt);
                std::string snbtError;
                ::World::NBTTagPtr tag = Snbt::ParseValue(text, snbtError);
                if (!tag) { error = snbtError; return nullptr; }
                return ParseHolder(ToJson(*tag), isInt);
            } catch (const ParseError& e) {
                error = std::string("Failed to parse: ") + e.what();
            } catch (const std::exception& e) {
                error = std::string("Failed to parse: ") + e.what();
            }
            return nullptr;
        }

    } // namespace

    Provider ParseFloatArgument(const std::string& text, std::string& error) { return ParseArgument(text, false, error); }
    Provider ParseIntArgument(const std::string& text, std::string& error)   { return ParseArgument(text, true, error); }

    float GetFloat(const Provider& provider, Context& context) {
        if (!provider) return 0.0f;
        try {
            const float v = EvalF(*provider, context);
            return std::isfinite(v) ? v : 0.0f;
        } catch (const ArithmeticError&) {
            return 0.0f;
        }
    }

    int GetInt(const Provider& provider, Context& context) {
        if (!provider) return 0;
        try {
            return EvalI(*provider, context);
        } catch (const ArithmeticError&) {
            return 0;
        }
    }

} // namespace Server::NumberProviders
