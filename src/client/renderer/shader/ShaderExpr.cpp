// File: src/client/renderer/shader/ShaderExpr.cpp
#include "ShaderExpr.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

namespace Render::ShaderExpr {

    namespace {

        struct Parser {
            const std::string& s;
            const Context&     ctx;
            size_t             i = 0;
            std::string        error;

            Parser(const std::string& src, const Context& c) : s(src), ctx(c) {}

            bool fail(const std::string& msg) {
                if (error.empty()) error = msg + " at " + std::to_string(i) + " in '" + s + "'";
                return false;
            }
            void ws() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
            bool peek(const char* tok) {
                ws();
                const size_t n = std::char_traits<char>::length(tok);
                return s.compare(i, n, tok) == 0;
            }
            bool accept(const char* tok) {
                if (!peek(tok)) return false;
                i += std::char_traits<char>::length(tok);
                return true;
            }

            // Component-wise binary op with scalar broadcast.
            static Value Apply(const Value& a, const Value& b, float (*f)(float, float)) {
                Value r;
                r.n = a.n == 1 ? b.n : a.n;
                for (int k = 0; k < r.n; ++k) r.v[k] = f(a.n == 1 ? a.v[0] : a.v[k], b.n == 1 ? b.v[0] : b.v[k]);
                return r;
            }
            static Value Map(const Value& a, float (*f)(float)) {
                Value r = a;
                for (int k = 0; k < r.n; ++k) r.v[k] = f(a.v[k]);
                return r;
            }
            static bool Truthy(const Value& a) { return a.v[0] != 0.0f; }

            // ternary := logicalOr ('?' expr ':' expr)?
            bool ternary(Value& out) {
                if (!logicalOr(out)) return false;
                if (accept("?")) {
                    Value a, b;
                    if (!ternary(a)) return false;
                    if (!accept(":")) return fail("expected ':'");
                    if (!ternary(b)) return false;
                    out = Truthy(out) ? a : b;
                }
                return true;
            }
            bool logicalOr(Value& out) {
                if (!logicalAnd(out)) return false;
                while (accept("||")) {
                    Value r;
                    if (!logicalAnd(r)) return false;
                    out = Value::Scalar(Truthy(out) || Truthy(r) ? 1.0f : 0.0f);
                }
                return true;
            }
            bool logicalAnd(Value& out) {
                if (!equality(out)) return false;
                while (accept("&&")) {
                    Value r;
                    if (!equality(r)) return false;
                    out = Value::Scalar(Truthy(out) && Truthy(r) ? 1.0f : 0.0f);
                }
                return true;
            }
            bool equality(Value& out) {
                if (!relational(out)) return false;
                for (;;) {
                    if (accept("==")) { Value r; if (!relational(r)) return false; out = Value::Scalar(out.v[0] == r.v[0] ? 1.0f : 0.0f); }
                    else if (accept("!=")) { Value r; if (!relational(r)) return false; out = Value::Scalar(out.v[0] != r.v[0] ? 1.0f : 0.0f); }
                    else return true;
                }
            }
            bool relational(Value& out) {
                if (!additive(out)) return false;
                for (;;) {
                    if (accept("<=")) { Value r; if (!additive(r)) return false; out = Value::Scalar(out.v[0] <= r.v[0] ? 1.0f : 0.0f); }
                    else if (accept(">=")) { Value r; if (!additive(r)) return false; out = Value::Scalar(out.v[0] >= r.v[0] ? 1.0f : 0.0f); }
                    else if (accept("<"))  { Value r; if (!additive(r)) return false; out = Value::Scalar(out.v[0] <  r.v[0] ? 1.0f : 0.0f); }
                    else if (accept(">"))  { Value r; if (!additive(r)) return false; out = Value::Scalar(out.v[0] >  r.v[0] ? 1.0f : 0.0f); }
                    else return true;
                }
            }
            bool additive(Value& out) {
                if (!multiplicative(out)) return false;
                for (;;) {
                    if (accept("+"))      { Value r; if (!multiplicative(r)) return false; out = Apply(out, r, [](float a, float b) { return a + b; }); }
                    else if (accept("-")) { Value r; if (!multiplicative(r)) return false; out = Apply(out, r, [](float a, float b) { return a - b; }); }
                    else return true;
                }
            }
            bool multiplicative(Value& out) {
                if (!unary(out)) return false;
                for (;;) {
                    if (accept("*"))      { Value r; if (!unary(r)) return false; out = Apply(out, r, [](float a, float b) { return a * b; }); }
                    else if (accept("/")) { Value r; if (!unary(r)) return false; out = Apply(out, r, [](float a, float b) { return b != 0.0f ? a / b : 0.0f; }); }
                    else if (accept("%")) { Value r; if (!unary(r)) return false; out = Apply(out, r, [](float a, float b) { return b != 0.0f ? std::fmod(a, b) : 0.0f; }); }
                    else return true;
                }
            }
            bool unary(Value& out) {
                if (accept("-")) { if (!unary(out)) return false; out = Map(out, [](float a) { return -a; }); return true; }
                if (accept("+")) return unary(out);
                if (accept("!")) { if (!unary(out)) return false; out = Value::Scalar(Truthy(out) ? 0.0f : 1.0f); return true; }
                return postfix(out);
            }
            // postfix := primary ('.' swizzle)*
            bool postfix(Value& out) {
                if (!primary(out)) return false;
                while (i < s.size() && s[i] == '.' && i + 1 < s.size() && std::isalpha(static_cast<unsigned char>(s[i + 1]))) {
                    ++i;
                    Value r;
                    int n = 0;
                    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && n < 4) {
                        const char c = s[i++];
                        int idx = c == 'x' || c == 'r' || c == 's' ? 0 : c == 'y' || c == 'g' || c == 't' ? 1
                                : c == 'z' || c == 'b' || c == 'p' ? 2 : c == 'w' || c == 'a' || c == 'q' ? 3 : -1;
                        if (idx < 0 || idx >= out.n) return fail("bad swizzle");
                        r.v[n++] = out.v[idx];
                    }
                    r.n = n;
                    out = r;
                }
                return true;
            }
            bool number(Value& out) {
                const char* start = s.c_str() + i;
                char* end = nullptr;
                const float v = std::strtof(start, &end);
                if (end == start) return false;
                i += static_cast<size_t>(end - start);
                if (i < s.size() && (s[i] == 'f' || s[i] == 'F')) ++i;
                out = Value::Scalar(v);
                return true;
            }
            bool args(std::vector<Value>& out) {
                if (!accept("(")) return fail("expected '('");
                if (accept(")")) return true;
                for (;;) {
                    Value v;
                    if (!ternary(v)) return false;
                    out.push_back(v);
                    if (accept(",")) continue;
                    if (accept(")")) return true;
                    return fail("expected ',' or ')'");
                }
            }
            bool primary(Value& out) {
                ws();
                if (i >= s.size()) return fail("unexpected end");
                if (accept("(")) {
                    if (!ternary(out)) return false;
                    if (!accept(")")) return fail("expected ')'");
                    return true;
                }
                if (std::isdigit(static_cast<unsigned char>(s[i])) || (s[i] == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
                    return number(out);
                }
                if (!std::isalpha(static_cast<unsigned char>(s[i])) && s[i] != '_') return fail("unexpected character");
                const size_t start = i;
                while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
                const std::string name = s.substr(start, i - start);
                if (name == "true")  { out = Value::Scalar(1.0f); return true; }
                if (name == "false") { out = Value::Scalar(0.0f); return true; }
                if (peek("(")) return call(name, out);
                if (ctx.vars) {
                    auto it = ctx.vars->find(name);
                    if (it != ctx.vars->end()) { out = it->second; return true; }
                }
                return fail("unknown name '" + name + "'");
            }

            bool call(const std::string& fn, Value& out) {
                std::vector<Value> a;
                if (!args(a)) return false;
                auto need = [&](size_t n) { if (a.size() < n) { fail(fn + " needs " + std::to_string(n) + " argument(s)"); return false; } return true; };
                auto unaryFn = [&](float (*f)(float)) { if (!need(1)) return false; out = Map(a[0], f); return true; };
                auto binaryFn = [&](float (*f)(float, float)) { if (!need(2)) return false; out = Apply(a[0], a[1], f); return true; };

                if (fn == "sin")   return unaryFn([](float x) { return std::sin(x); });
                if (fn == "cos")   return unaryFn([](float x) { return std::cos(x); });
                if (fn == "tan")   return unaryFn([](float x) { return std::tan(x); });
                if (fn == "asin")  return unaryFn([](float x) { return std::asin(x); });
                if (fn == "acos")  return unaryFn([](float x) { return std::acos(x); });
                if (fn == "atan") {
                    if (a.size() >= 2) return binaryFn([](float y, float x) { return std::atan2(y, x); });
                    return unaryFn([](float x) { return std::atan(x); });
                }
                if (fn == "sinh")  return unaryFn([](float x) { return std::sinh(x); });
                if (fn == "cosh")  return unaryFn([](float x) { return std::cosh(x); });
                if (fn == "tanh")  return unaryFn([](float x) { return std::tanh(x); });
                if (fn == "exp")   return unaryFn([](float x) { return std::exp(x); });
                if (fn == "exp2")  return unaryFn([](float x) { return std::exp2(x); });
                if (fn == "log")   return unaryFn([](float x) { return x > 0.0f ? std::log(x) : 0.0f; });
                if (fn == "log2")  return unaryFn([](float x) { return x > 0.0f ? std::log2(x) : 0.0f; });
                if (fn == "sqrt")  return unaryFn([](float x) { return x > 0.0f ? std::sqrt(x) : 0.0f; });
                if (fn == "inversesqrt") return unaryFn([](float x) { return x > 0.0f ? 1.0f / std::sqrt(x) : 0.0f; });
                if (fn == "abs")   return unaryFn([](float x) { return std::fabs(x); });
                if (fn == "sign")  return unaryFn([](float x) { return x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : 0.0f; });
                if (fn == "floor") return unaryFn([](float x) { return std::floor(x); });
                if (fn == "ceil")  return unaryFn([](float x) { return std::ceil(x); });
                if (fn == "round") return unaryFn([](float x) { return std::round(x); });
                if (fn == "fract") return unaryFn([](float x) { return x - std::floor(x); });
                if (fn == "radians") return unaryFn([](float x) { return x * 0.017453292f; });
                if (fn == "degrees") return unaryFn([](float x) { return x * 57.29578f; });
                if (fn == "float" || fn == "int" || fn == "bool") {
                    if (!need(1)) return false;
                    out = Value::Scalar(fn == "int" ? std::trunc(a[0].v[0]) : fn == "bool" ? (a[0].v[0] != 0.0f ? 1.0f : 0.0f) : a[0].v[0]);
                    return true;
                }
                if (fn == "fmod" || fn == "mod") return binaryFn([](float x, float y) { return y != 0.0f ? (x - y * std::floor(x / y)) : 0.0f; });
                if (fn == "min")  return binaryFn([](float x, float y) { return x < y ? x : y; });
                if (fn == "max")  return binaryFn([](float x, float y) { return x > y ? x : y; });
                if (fn == "pow")  return binaryFn([](float x, float y) { return std::pow(x, y); });
                if (fn == "step") return binaryFn([](float edge, float x) { return x < edge ? 0.0f : 1.0f; });
                if (fn == "clamp") {
                    if (!need(3)) return false;
                    out = Apply(Apply(a[0], a[1], [](float x, float lo) { return x < lo ? lo : x; }), a[2], [](float x, float hi) { return x > hi ? hi : x; });
                    return true;
                }
                if (fn == "mix") {
                    if (!need(3)) return false;
                    const Value d = Apply(a[1], a[0], [](float y, float x) { return y - x; });
                    out = Apply(a[0], Apply(d, a[2], [](float x, float t) { return x * t; }), [](float x, float y) { return x + y; });
                    return true;
                }
                if (fn == "smoothstep") {
                    if (!need(3)) return false;
                    Value t = Apply(Apply(a[2], a[0], [](float x, float e0) { return x - e0; }),
                                    Apply(a[1], a[0], [](float e1, float e0) { return e1 - e0; }),
                                    [](float num, float den) { return den != 0.0f ? num / den : 0.0f; });
                    t = Map(t, [](float x) { x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; return x * x * (3.0f - 2.0f * x); });
                    out = t;
                    return true;
                }
                if (fn == "if") {
                    if (!need(3)) return false;
                    out = Truthy(a[0]) ? a[1] : a[2];
                    return true;
                }
                if (fn == "vec2" || fn == "vec3" || fn == "vec4") {
                    const int n = fn[3] - '0';
                    Value r;
                    r.n = n;
                    int k = 0;
                    for (const Value& v : a) for (int c = 0; c < v.n && k < n; ++c) r.v[k++] = v.v[c];
                    if (a.size() == 1 && a[0].n == 1) for (int c = 1; c < n; ++c) r.v[c] = a[0].v[0];
                    else if (k != n) return fail(fn + " needs " + std::to_string(n) + " components");
                    out = r;
                    return true;
                }
                if (fn == "length" || fn == "normalize") {
                    if (!need(1)) return false;
                    float sum = 0.0f;
                    for (int c = 0; c < a[0].n; ++c) sum += a[0].v[c] * a[0].v[c];
                    const float len = std::sqrt(sum);
                    if (fn == "length") { out = Value::Scalar(len); return true; }
                    out = a[0];
                    for (int c = 0; c < out.n; ++c) out.v[c] = len > 0.0f ? a[0].v[c] / len : 0.0f;
                    return true;
                }
                if (fn == "dot" || fn == "distance") {
                    if (!need(2)) return false;
                    float sum = 0.0f;
                    for (int c = 0; c < a[0].n; ++c) {
                        const float x = a[0].v[c], y = a[1].n == 1 ? a[1].v[0] : a[1].v[c];
                        sum += fn == "dot" ? x * y : (x - y) * (x - y);
                    }
                    out = Value::Scalar(fn == "dot" ? sum : std::sqrt(sum));
                    return true;
                }
                if (fn == "cross") {
                    if (!need(2)) return false;
                    const float* x = a[0].v; const float* y = a[1].v;
                    out = Value::Vec(x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]);
                    return true;
                }
                if (fn == "random") { out = Value::Scalar(static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)); return true; }
                if (fn == "randomInt") {
                    if (!need(2)) return false;
                    const int lo = static_cast<int>(a[0].v[0]), hi = static_cast<int>(a[1].v[0]);
                    out = Value::Scalar(static_cast<float>(hi > lo ? lo + std::rand() % (hi - lo + 1) : lo));
                    return true;
                }
                if (fn == "smooth") {
                    // smooth(id, value, fadeUp, fadeDown) or smooth(value, fadeUp, fadeDown) (an implicit id).
                    if (!need(3)) return false;
                    const bool hasId = a.size() >= 4;
                    const int id = hasId ? static_cast<int>(a[0].v[0]) : -1 - static_cast<int>(i);
                    const Value target = a[hasId ? 1 : 0];
                    const float up = a[hasId ? 2 : 1].v[0], down = a[hasId ? 3 : 2].v[0];
                    if (!ctx.smooth) { out = target; return true; }
                    auto it = ctx.smooth->find(id);
                    if (it == ctx.smooth->end()) { (*ctx.smooth)[id] = target; out = target; return true; }
                    Value cur = it->second;
                    cur.n = target.n;
                    for (int c = 0; c < target.n; ++c) {
                        const float t = target.v[c] - cur.v[c] >= 0.0f ? up : down;
                        const float k = t > 0.0f ? 1.0f - std::exp(-ctx.deltaSeconds / t) : 1.0f;
                        cur.v[c] += (target.v[c] - cur.v[c]) * k;
                    }
                    it->second = cur;
                    out = cur;
                    return true;
                }
                return fail("unknown function '" + fn + "'");
            }
        };

    } // namespace

    bool Evaluate(const std::string& expr, const Context& ctx, Value& out, std::string& error) {
        Parser p(expr, ctx);
        if (!p.ternary(out)) { error = p.error; return false; }
        p.ws();
        if (p.i != expr.size()) { error = "trailing input at " + std::to_string(p.i) + " in '" + expr + "'"; return false; }
        return true;
    }

} // namespace Render::ShaderExpr
