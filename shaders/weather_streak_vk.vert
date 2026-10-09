// File: shaders/weather_streak_vk.vert
// Rain and snow columns as their STREAKS: one instanced quad per opaque
// rectangle of the weather texture, per column, per texture repeat
// (WeatherEffectRenderer's streak path). MC draws each column as one quad
// a block wide over the whole texture, of which ~2.5 % has any alpha —
// 97 % of the fragments it launches fail the 0.1 cutout. A tile GPU is
// bound by fragment launches in that pass, so the launches are moved to
// where the texels are: the renderer greedily rectangles the texture's
// alpha > 0 texels at load (~110 rects for rain.png), and this shader
// places each rect exactly where the column quad would have sampled it.
// The output is the column quad's, pixel for pixel: a pixel is inside a
// rect's quad iff the texel it samples (nearest) lies in that rect, and
// the uv it gets is the same uv.
//
// Per vertex (the streak template, the 24-byte block layout):
//   aPos   = (u0, v0, copy)   the rect's left/top in texture space [0, 1),
//                             copy 1 = the part wrapped past the column's
//                             right edge (snow's u offset is fractional)
//   aUV    = (u1, v1)         the rect's right/bottom (exclusive)
//   aColor = (corner / 255, 0, 0, 0)   0 top-left, 1 top-right,
//                                      2 bottom-right, 3 bottom-left
// Per instance (one column x one texture repeat):
//   aColumn = (cx, cz, hx, hz)   render-space centre and half-extent of the
//                                column quad (MC's columnSizeX/Z / 2)
//   aSpan   = (bottomY, topY, uOffset, vOffset)   render-space y range; the
//             texture offsets (vOffset carries the render origin's y, see
//             the renderer)
//   aTint   = the column's RGBA (the vertex colour MC gives the quad)
//   aRepeat = (R, 0, 0, 0)   the texture repeat: v in [R, R + 1)
//
// MC's quad: u = uOffset + fx across the column (fx 0 at the -half corner,
// 1 at +half); v = (bottomY + topY - y) * 0.25 + vOffset, i.e. the texture
// repeats every 4 blocks and runs DOWN the column. Fragment shaders:
// mob_particle_vk.frag (full resolution) / weather_half_vk.frag.
#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aColumn;
layout(location = 4) in vec4 aSpan;
layout(location = 5) in vec4 aTint;
layout(location = 6) in vec4 aRepeat;

layout(push_constant) uniform PC {
    mat4  uMVP;          // 0
    vec2  uScreenSize;   // 64
    float uLineWidth;    // 72
    float uAlphaTest;    // 76
    vec4  uColor;        // 80
    vec4  uUVRange;      // 96 — the portal clip plane (uEntityClipPlane)
    vec4  uScalars;      // 112
} pc;

out gl_PerVertex {
    vec4  gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec3 vRenderPos;

void main() {
    const int corner = int(aColor.r * 255.0 + 0.5);
    const float u0 = aPos.x, v0 = aPos.y, u1 = aUV.x, v1 = aUV.y;
    const float uOffset = aSpan.z, vOffset = aSpan.w;

    // Across the column: the rect starts at fraction w = fract(u0 - uOffset)
    // and is dw wide; the part past 1 wraps to the left edge (copy 1).
    const float w = fract(u0 - uOffset);
    const float dw = u1 - u0;
    float fx0, fx1, tu0;
    if (aPos.z < 0.5) {
        fx0 = w;
        fx1 = min(w + dw, 1.0);
        tu0 = u0;
    } else {
        fx0 = 0.0;
        fx1 = max(w + dw - 1.0, 0.0);   // zero width when nothing wraps
        tu0 = u0 + (1.0 - w);
    }

    // Down the column: this repeat's rect spans texture v [R + v0, R + v1),
    // which is y from (bottom + top) - 4 (v - vOffset); clamped to the
    // column, and the clamped edges re-derive their v so the texels stay
    // where the column quad had them.
    const float sum = aSpan.x + aSpan.y;
    const float yHi = clamp(sum - 4.0 * (aRepeat.x + v0 - vOffset), aSpan.x, aSpan.y);
    const float yLo = clamp(sum - 4.0 * (aRepeat.x + v1 - vOffset), aSpan.x, aSpan.y);

    const bool top = corner == 0 || corner == 1;
    const bool right = corner == 1 || corner == 2;
    const float fx = right ? fx1 : fx0;
    const float y = top ? yHi : yLo;
    const float across = 2.0 * fx - 1.0;
    const vec3 pos = vec3(aColumn.x + across * aColumn.z, y, aColumn.y + across * aColumn.w);

    gl_Position = pc.uMVP * vec4(pos, 1.0);
    vUV = vec2(tu0 + (fx - fx0), (sum - y) * 0.25 + vOffset);
    vColor = aTint;
    vRenderPos = pos;
    gl_ClipDistance[0] = (any(notEqual(pc.uUVRange.xyz, vec3(0.0))))
        ? dot(pc.uUVRange.xyz, pos) + pc.uUVRange.w
        : 1.0;
}
