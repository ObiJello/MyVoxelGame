// File: src/client/renderer/shader/ShaderExpr.hpp
//
// The expression language of a shader pack's custom uniforms
// (shaders.properties `uniform.<type>.<name> = <expr>` and
// `variable.<type>.<name> = <expr>`, OptiFine's and Iris's):
//
//   uniform.int.framemod8 = fmod(frameCounter, 8)
//   uniform.float.sunGlow = smooth(1, clamp(sunPosition.y / 100.0, 0, 1), 5, 5)
//   variable.vec3.dir     = normalize(shadowLightPosition)
//
// Values are floats or vec2/3/4, operated on component-wise with scalar
// broadcast. Supported: + - * / %, comparisons, && || !, ?:, swizzles,
// vecN() constructors, the GLSL scalar functions, if(c, a, b), and
// smooth(id, value, fadeUp, fadeDown) (seconds), whose state the caller
// keeps between frames.
#pragma once

#include <map>
#include <string>

namespace Render::ShaderExpr {

    struct Value {
        float v[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        int   n = 1;
        static Value Scalar(float x) { Value r; r.v[0] = x; r.n = 1; return r; }
        static Value Vec(float x, float y, float z = 0.0f, float w = 0.0f, int n = 3) {
            Value r; r.v[0] = x; r.v[1] = y; r.v[2] = z; r.v[3] = w; r.n = n; return r;
        }
    };

    using Vars = std::map<std::string, Value>;
    using SmoothState = std::map<int, Value>;

    struct Context {
        const Vars*  vars = nullptr;
        SmoothState* smooth = nullptr;    // smooth(): previous values by id
        float        deltaSeconds = 0.0f;
    };

    // False with `error` set when the expression does not parse or names an
    // unknown variable or function.
    bool Evaluate(const std::string& expr, const Context& ctx, Value& out, std::string& error);

} // namespace Render::ShaderExpr
