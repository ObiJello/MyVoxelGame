// File: src/client/renderer/shader/ShaderPackGlsl.cpp
#include "ShaderPackGlsl.hpp"
#include "client/shader/PackCompiler.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace Render::PackGlsl {

    namespace {

        // ── #include ─────────────────────────────────────────────────────
        // OptiFine: an absolute include ("/lib/x.glsl") is under shaders/,
        // a relative one is next to the including file.
        std::string DirOf(const std::string& relPath) {
            const size_t slash = relPath.find_last_of('/');
            return slash == std::string::npos ? std::string() : relPath.substr(0, slash + 1);
        }

        std::string Normalize(const std::string& path) {
            std::vector<std::string> parts;
            std::stringstream ss(path);
            std::string part;
            while (std::getline(ss, part, '/')) {
                if (part.empty() || part == ".") continue;
                if (part == "..") { if (!parts.empty()) parts.pop_back(); continue; }
                parts.push_back(part);
            }
            std::string out;
            for (size_t i = 0; i < parts.size(); ++i) { if (i) out += '/'; out += parts[i]; }
            return out;
        }

        bool ResolveIncludes(const std::string& source, const std::string& relPath,
                             const FileReader& read, int depth, std::string& out, std::string& error) {
            if (depth > 16) { error = "include nesting too deep at " + relPath; return false; }
            static const std::regex kInclude(R"re(^[ \t]*#[ \t]*include[ \t]+"([^"]+)"[ \t]*(?://.*)?$)re");
            std::istringstream in(source);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::smatch m;
                if (std::regex_match(line, m, kInclude)) {
                    const std::string target = m[1].str();
                    const std::string path = Normalize(target[0] == '/' ? target.substr(1) : DirOf(relPath) + target);
                    std::string body;
                    if (!read(path, body)) { error = "missing include \"" + target + "\" from " + relPath; return false; }
                    std::string expanded;
                    if (!ResolveIncludes(body, path, read, depth + 1, expanded, error)) return false;
                    out += expanded;
                    out += '\n';
                } else {
                    out += line;
                    out += '\n';
                }
            }
            return true;
        }

        // ── version / extensions ─────────────────────────────────────────
        // The engine's context is core profile; 330 is the floor and a pack
        // asking for more keeps its number.
        // Line-based rather than a multiline regex: libc++'s std::regex
        // support for the multiline flag is uneven across Xcode versions.
        template <typename Pred>
        std::string FilterLines(const std::string& source, Pred keep) {
            std::string out;
            std::istringstream in(source);
            std::string line;
            while (std::getline(in, line)) {
                if (!keep(line)) continue;
                out += line;
                out += '\n';
            }
            return out;
        }

        int ExtractVersion(std::string& source) {
            static const std::regex kVersion(R"(^[ \t]*#[ \t]*version[ \t]+(\d+))");
            int version = 120;
            bool found = false;
            source = FilterLines(source, [&](const std::string& line) {
                std::smatch m;
                if (!found && std::regex_search(line, m, kVersion)) {
                    version = std::stoi(m[1].str());
                    found = true;
                    return false;
                }
                return true;
            });
            return version < 330 ? 330 : version;
        }

        // Every format name OptiFine accepts in a `const int colortexNFormat`
        // line, defined so the line compiles (the value is read by
        // FindFormatDecls, never by the shader).
        const char* kFormatNames[] = {
            "R8", "RG8", "RGB8", "RGBA8", "R8_SNORM", "RG8_SNORM", "RGB8_SNORM", "RGBA8_SNORM",
            "R16", "RG16", "RGB16", "RGBA16", "R16_SNORM", "RG16_SNORM", "RGB16_SNORM", "RGBA16_SNORM",
            "R16F", "RG16F", "RGB16F", "RGBA16F", "R32F", "RG32F", "RGB32F", "RGBA32F",
            "R32I", "RG32I", "RGB32I", "RGBA32I", "R32UI", "RG32UI", "RGB32UI", "RGBA32UI",
            "R8I", "RG8I", "RGB8I", "RGBA8I", "R8UI", "RG8UI", "RGB8UI", "RGBA8UI",
            "R16I", "RG16I", "RGB16I", "RGBA16I", "R16UI", "RG16UI", "RGB16UI", "RGBA16UI",
            "R11F_G11F_B10F", "RGB10_A2", "RGB9_E5", "RGB565", "RGB5_A1", "RGBA4", "RGB4", "RGBA2", "R3_G3_B2",
            "SRGB8", "SRGB8_ALPHA8",
        };

        std::string CommonPrelude(int version) {
            std::string p = "#version " + std::to_string(version) + " core\n";
            p += "// --- ShaderPipeline prelude: compatibility-profile names onto core-profile declarations ---\n";
            int i = 0;
            for (const char* f : kFormatNames) { p += "#define " + std::string(f) + " " + std::to_string(i++) + "\n"; }
            // The standard macros OptiFine and Iris define for packs, which
            // test them in #if (an undefined name there is a GLSL error).
            p += "#define MC_VERSION 12104\n"
                 "#define MC_GL_VERSION 330\n"
                 "#define MC_GLSL_VERSION 330\n"
#if defined(__APPLE__)
                 "#define MC_OS_MAC\n"
#elif defined(_WIN32)
                 "#define MC_OS_WINDOWS\n"
#else
                 "#define MC_OS_LINUX\n"
#endif
                 "#define MC_GL_VENDOR_OTHER\n"
                 "#define MC_GL_RENDERER_OTHER\n"
                 "#define MC_RENDER_QUALITY 1.0\n"
                 "#define MC_SHADOW_QUALITY 1.0\n"
                 "#define MC_HAND_DEPTH 0.125\n"
                 // No MC_ANISOTROPIC_FILTERING: OptiFine defines it only while
                 // its own AF is on and Iris never does; Complementary takes
                 // it as that OptiFine setting and draws an error screen.
                 "#define MC_RENDER_STAGE_NONE 0\n"
                 "#define MC_RENDER_STAGE_SKY 1\n"
                 "#define MC_RENDER_STAGE_SUNSET 2\n"
                 "#define MC_RENDER_STAGE_CUSTOM_SKY 3\n"
                 "#define MC_RENDER_STAGE_SUN 4\n"
                 "#define MC_RENDER_STAGE_MOON 5\n"
                 "#define MC_RENDER_STAGE_STARS 6\n"
                 "#define MC_RENDER_STAGE_VOID 7\n"
                 "#define MC_RENDER_STAGE_TERRAIN_SOLID 8\n"
                 "#define MC_RENDER_STAGE_TERRAIN_CUTOUT_MIPPED 9\n"
                 "#define MC_RENDER_STAGE_TERRAIN_CUTOUT 10\n"
                 "#define MC_RENDER_STAGE_ENTITIES 11\n"
                 "#define MC_RENDER_STAGE_BLOCK_ENTITIES 12\n"
                 "#define MC_RENDER_STAGE_DESTROY 13\n"
                 "#define MC_RENDER_STAGE_OUTLINE 14\n"
                 "#define MC_RENDER_STAGE_DEBUG 15\n"
                 "#define MC_RENDER_STAGE_HAND_SOLID 16\n"
                 "#define MC_RENDER_STAGE_TERRAIN_TRANSLUCENT 17\n"
                 "#define MC_RENDER_STAGE_TRIPWIRE 18\n"
                 "#define MC_RENDER_STAGE_PARTICLES 19\n"
                 "#define MC_RENDER_STAGE_CLOUDS 20\n"
                 "#define MC_RENDER_STAGE_RAIN_SNOW 21\n"
                 "#define MC_RENDER_STAGE_WORLD_BORDER 22\n"
                 "#define MC_RENDER_STAGE_HAND_TRANSLUCENT 23\n";
            p += "uniform mat4 sp_ModelViewMatrix;\n"
                 "uniform mat4 sp_ProjectionMatrix;\n"
                 "uniform mat4 sp_ModelViewProjectionMatrix;\n"
                 "uniform mat4 sp_ModelViewMatrixInverse;\n"
                 "uniform mat4 sp_ProjectionMatrixInverse;\n"
                 "uniform mat4 sp_NormalMatrix4;\n"
                 "#define sp_NormalMatrix mat3(sp_NormalMatrix4)\n"
                 "uniform mat4 sp_TextureMatrix[2];\n"
                 "#define gl_ModelViewMatrix sp_ModelViewMatrix\n"
                 "#define gl_ProjectionMatrix sp_ProjectionMatrix\n"
                 "#define gl_ModelViewProjectionMatrix sp_ModelViewProjectionMatrix\n"
                 "#define gl_ModelViewMatrixInverse sp_ModelViewMatrixInverse\n"
                 "#define gl_ProjectionMatrixInverse sp_ProjectionMatrixInverse\n"
                 "#define gl_NormalMatrix sp_NormalMatrix\n"
                 "#define gl_TextureMatrix sp_TextureMatrix\n"
                 "#define texture2D texture\n"
                 "#define texture3D texture\n"
                 "#define textureCube texture\n"
                 "#define texture2DLod textureLod\n"
                 "#define texture3DLod textureLod\n"
                 "#define textureCubeLod textureLod\n"
                 "#define texture2DProj textureProj\n"
                 "#define texture2DProjLod textureProjLod\n"
                 "#define texture2DGrad textureGrad\n"
                 "#define texture2DGradARB textureGrad\n"
                 "#define texture2DLodOffset textureLodOffset\n"
                 "#define texture2DOffset textureOffset\n"
                 // shadowtex is bound as a plain depth texture holding 1.0:
                 // a shadow sample reads "not occluded", as the vec4 the
                 // compatibility function returned.
                 "#define sampler2DShadow sampler2D\n"
                 "#define sampler1DShadow sampler1D\n"
                 "#define shadow2D(s, c) vec4(float(texture(s, (c).xy).r >= (c).z - 0.0005))\n"
                 "#define shadow2DLod(s, c, l) vec4(float(textureLod(s, (c).xy, l).r >= (c).z - 0.0005))\n"
                 "#define shadow2DProj(s, c) vec4(float(textureProj(s, (c).xyw).r >= (c).z / (c).w - 0.0005))\n";
            // gl_Fog, the compatibility built-in struct (Complementary reads
            // gl_Fog.start / .scale in its fog and End checks): MC's linear
            // fog from the pipeline's sp_Fog* uniforms (ShaderPipeline::SetUniforms).
            p += "uniform vec4 sp_FogColor;\n"
                 "uniform float sp_FogStart;\n"
                 "uniform float sp_FogEnd;\n"
                 "struct sp_FogParameters { vec4 color; float density; float start; float end; float scale; };\n"
                 "sp_FogParameters sp_Fog() {\n"
                 "    return sp_FogParameters(sp_FogColor, 0.0, sp_FogStart, sp_FogEnd, 1.0 / max(sp_FogEnd - sp_FogStart, 1e-4));\n"
                 "}\n"
                 "#define gl_Fog sp_Fog()\n";
            return p;
        }

        // The engine's terrain inputs and uniforms (shaders/terrain.vert),
        // decoded for the pack. Position, slot table and render origin are
        // exactly the engine's; the facing rides in the slot's bits 10..12
        // (TerrainVertex::kNormalMask); gl_MultiTexCoord1/2 are the vertex's
        // MC light coords (block, sky; 0..240 — aLight, the light word the
        // mesher bakes, Vertex.hpp) exactly as OptiFine/Iris feed lmcoord;
        // mc_Entity is 8 for
        // water and 10 for lava, told from the sprite rectangles, 0 for
        // everything else until the mesher carries block ids.
        std::string TerrainVertexPrelude(int version) {
            std::string p = CommonPrelude(version);
            p += "layout(location = 0) in vec4 aPosSlot;\n"
                 "layout(location = 1) in vec2 aTexCoord;\n"
                 "layout(location = 2) in vec4 aColor;\n"
                 "layout(location = 3) in vec4 aLight;\n"
                 "uniform mat4 uMVP;\n"
                 "uniform vec4 uPortalClipPlane;\n"
                 "uniform ivec3 uRenderOrigin;\n"
                 "layout(std140) uniform SectionOrigins { ivec4 uOrigins[1024]; };\n"
                 "uniform sampler2D sp_entityMap;\n"
                 "uniform sampler2D sp_spriteTable;\n"
                 "uniform vec2 sp_entityMapSize;\n"
                 "uniform float sp_unmappedEntity;\n"
                 "vec3 sp_worldPos; vec3 sp_normal; vec4 sp_color; vec4 sp_mcEntity; vec4 sp_midTexCoord; vec4 sp_tangent; vec2 sp_texCoord;\n"
                 "#define attribute in\n"
                 "#define varying out\n"
                 "#define gl_Vertex vec4(sp_worldPos, 1.0)\n"
                 "#define gl_MultiTexCoord0 vec4(sp_texCoord, 0.0, 1.0)\n"
                 "#define gl_MultiTexCoord1 vec4(aLight.xy * 255.0, 0.0, 1.0)\n"
                 "#define gl_MultiTexCoord2 vec4(aLight.xy * 255.0, 0.0, 1.0)\n"
                 "#define gl_Color sp_color\n"
                 "#define gl_Normal sp_normal\n"
                 "#define mc_Entity sp_mcEntity\n"
                 "#define mc_midTexCoord sp_midTexCoord\n"
                 "#define at_tangent sp_tangent\n"
                 "#define at_midBlock vec3(0.0)\n"
                 "#define at_velocity vec3(0.0)\n"
                 "#undef gl_ModelViewProjectionMatrix\n"
                 "#define gl_ModelViewProjectionMatrix uMVP\n"
                 "#define ftransform() (uMVP * vec4(sp_worldPos, 1.0))\n"
                 "out vec4 sp_TexCoord[4];\n"
                 "out vec4 sp_FrontColor;\n"
                 "out float sp_FogFragCoord;\n"
                 "#define gl_TexCoord sp_TexCoord\n"
                 "#define gl_FrontColor sp_FrontColor\n"
                 "#define gl_BackColor sp_FrontColor\n"
                 "#define gl_FogFragCoord sp_FogFragCoord\n"
                 "void sp_terrainSetup() {\n"
                 "    int slotRaw = int(aPosSlot.w * 65535.0 + 0.5);\n"
                 "    int slot = slotRaw & 0x3FF;\n"
                 "    vec3 rel = aPosSlot.xyz * (65535.0 / 2048.0) - 2.0;\n"
                 "    sp_worldPos = vec3(uOrigins[slot].xyz - uRenderOrigin) + rel;\n"
                 "    int f = (slotRaw >> 10) & 7;\n"
                 "    sp_normal = f == 0 ? vec3(-1.0, 0.0, 0.0) : f == 1 ? vec3(1.0, 0.0, 0.0) : f == 2 ? vec3(0.0, -1.0, 0.0)\n"
                 "              : f == 3 ? vec3(0.0, 1.0, 0.0) : f == 4 ? vec3(0.0, 0.0, -1.0) : f == 5 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);\n"
                 "    vec3 t = abs(sp_normal.y) > 0.5 ? vec3(1.0, 0.0, 0.0) : normalize(cross(sp_normal, vec3(0.0, 1.0, 0.0)));\n"
                 "    sp_tangent = vec4(t, 1.0);\n"
                 "    sp_color = vec4(aColor.rgb, 1.0);\n"
                 "    // The atlas uv. An untiled vertex carries it; a tiled one (a fluid plate,\n"
                 "    // or a two-sided plant quad — TerrainVertex::Tiled / TwoSided) carries a\n"
                 "    // tile-space position and its sprite id, and the uv is the sprite's rect\n"
                 "    // (sp_spriteTable) at that position: a plant drawn with the packed word as\n"
                 "    // its uv was a solid green square (2026-10-09).\n"
                 "    sp_texCoord = aTexCoord;\n"
                 "    if ((slotRaw & 0x8000) != 0) {\n"
                 "        int packedTile = int(aTexCoord.x * 65535.0 + 0.5);\n"
                 "        int spriteV = int(aTexCoord.y * 65535.0 + 0.5);\n"
                 "        vec2 tile; int sprite;\n"
                 "        if ((slotRaw & 0x2000) != 0) { tile = vec2(float(packedTile & 0x1F), float((packedTile >> 5) & 0x1F)) / 16.0; sprite = spriteV & 0x7FFF; }\n"
                 "        else { tile = vec2(float(packedTile & 0xFF), float((packedTile >> 8) & 0xFF)); sprite = spriteV; }\n"
                 "        vec4 rect = texelFetch(sp_spriteTable, ivec2(sprite & 255, sprite >> 8), 0);\n"
                 "        sp_texCoord = rect.xy + tile * rect.zw;\n"
                 "    }\n"
                 "    sp_midTexCoord = vec4(sp_texCoord, 0.0, 1.0);\n"
                 "    // block.properties id and the sprite's centre, from the atlas-space map (ShaderPipeline::EnsureEntityMap).\n"
                 "    float id = sp_unmappedEntity;\n"
                 "    if (sp_entityMapSize.x > 0.0) {\n"
                 "        ivec2 cell = ivec2(clamp(floor(sp_texCoord * sp_entityMapSize), vec2(0.0), sp_entityMapSize - 1.0));\n"
                 "        vec4 e = texelFetch(sp_entityMap, cell, 0);\n"
                 "        int idRaw = int(e.r * 255.0 + 0.5) + int(e.g * 255.0 + 0.5) * 256;\n"
                 "        int spriteRaw = int(e.b * 255.0 + 0.5) + int(e.a * 255.0 + 0.5) * 256;\n"
                 "        if (idRaw != 0) id = float(idRaw);\n"
                 "        if (spriteRaw != 0) {\n"
                 "            int sid = spriteRaw - 1;\n"
                 "            vec4 rect = texelFetch(sp_spriteTable, ivec2(sid & 255, sid >> 8), 0);\n"
                 "            sp_midTexCoord = vec4(rect.xy + rect.zw * 0.5, 0.0, 1.0);\n"
                 "        }\n"
                 "    }\n"
                 "    sp_mcEntity = vec4(id, 0.0, 0.0, 1.0);\n"
                 "}\n"
                 "#line 1\n";
            return p;
        }

        // The engine's block-layout inputs (shaders/block.vert and the
        // inline sky, cloud, block-entity and particle shaders): a world
        // (render-space) position, atlas uv, colour; uMVP, and for the
        // block family a portal or entity clip plane.
        std::string EntityVertexPrelude(int version) {
            std::string p = CommonPrelude(version);
            p += "layout(location = 0) in vec3 aPos;\n"
                 "layout(location = 1) in vec2 aUV;\n"
                 "layout(location = 2) in vec4 aColor;\n"
                 "uniform mat4 uMVP;\n"
                 "uniform mat4 uModel;\n"
                 "uniform vec4 uPortalClipPlane;\n"
                 "uniform vec4 uEntityClipPlane;\n"
                 "vec3 sp_normal; vec4 sp_color; vec4 sp_mcEntity; vec4 sp_midTexCoord; vec4 sp_tangent; vec4 sp_viewPos;\n"
                 "#define attribute in\n"
                 "#define varying out\n"
                 // These renderers' uMVP may carry a per-object model matrix
                 // (a sign, a chest lid, an item) that aPos knows nothing
                 // about, so the vertex is handed over in VIEW space,
                 // recovered from its clip position through the frame's
                 // inverse projection, and the model-view is the identity.
                 "#define gl_Vertex sp_viewPos\n"
                 "#undef gl_ModelViewMatrix\n"
                 "#define gl_ModelViewMatrix mat4(1.0)\n"
                 "#undef gl_ModelViewMatrixInverse\n"
                 "#define gl_ModelViewMatrixInverse mat4(1.0)\n"
                 "#undef gl_NormalMatrix\n"
                 "#define gl_NormalMatrix mat3(1.0)\n"
                 "#define gl_MultiTexCoord0 vec4(aUV, 0.0, 1.0)\n"
                 "#define gl_MultiTexCoord1 vec4(0.0, 240.0, 0.0, 1.0)\n"
                 "#define gl_MultiTexCoord2 vec4(0.0, 240.0, 0.0, 1.0)\n"
                 "#define gl_Color sp_color\n"
                 "#define gl_Normal sp_normal\n"
                 "#define mc_Entity sp_mcEntity\n"
                 "#define mc_midTexCoord sp_midTexCoord\n"
                 "#define at_tangent sp_tangent\n"
                 "#define at_midBlock vec3(0.0)\n"
                 "#define at_velocity vec3(0.0)\n"
                 "#undef gl_ModelViewProjectionMatrix\n"
                 "#define gl_ModelViewProjectionMatrix uMVP\n"
                 "#define ftransform() (uMVP * vec4(aPos, 1.0))\n"
                 "out vec4 sp_TexCoord[4];\n"
                 "out vec4 sp_FrontColor;\n"
                 "out float sp_FogFragCoord;\n"
                 "#define gl_TexCoord sp_TexCoord\n"
                 "#define gl_FrontColor sp_FrontColor\n"
                 "#define gl_BackColor sp_FrontColor\n"
                 "#define gl_FogFragCoord sp_FogFragCoord\n"
                 "void sp_entitySetup() {\n"
                 "    vec4 clip = uMVP * vec4(aPos, 1.0);\n"
                 "    sp_viewPos = sp_ProjectionMatrixInverse * clip;\n"
                 "    sp_viewPos /= sp_viewPos.w;\n"
                 "    sp_color = aColor;\n"
                 "    sp_normal = normalize(mat3(sp_ModelViewMatrix) * vec3(0.0, 1.0, 0.0));\n"
                 "    sp_tangent = vec4(normalize(mat3(sp_ModelViewMatrix) * vec3(1.0, 0.0, 0.0)), 1.0);\n"
                 "    sp_midTexCoord = vec4(aUV, 0.0, 1.0);\n"
                 "    sp_mcEntity = vec4(0.0, 0.0, 0.0, 1.0);\n"
                 "}\n"
                 "#line 1\n";
            return p;
        }

        std::string EntityVertexEpilogue() {
            return "\nvoid main() {\n"
                   "    sp_entitySetup();\n"
                   "    sp_packMain();\n"
                   "    vec4 cp = any(notEqual(uPortalClipPlane.xyz, vec3(0.0))) ? uPortalClipPlane : uEntityClipPlane;\n"
                   "    gl_ClipDistance[0] = any(notEqual(cp.xyz, vec3(0.0))) ? dot(cp.xyz, aPos) + cp.w : 1.0;\n"
                   "}\n";
        }

        std::string EntityFragmentEpilogue() {
            // sp_alphaRef: shaders.properties' alphaTest for the program, 0.1
            // by default as Iris.
            return "\nvoid main() {\n"
                   "    sp_packMain();\n"
                   "    if (sp_FragData[0].a < sp_alphaRef) discard;\n"
                   "}\n";
        }

        std::string TerrainVertexEpilogue() {
            return "\nvoid main() {\n"
                   "    sp_terrainSetup();\n"
                   "    sp_packMain();\n"
                   "    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))\n"
                   "        ? dot(uPortalClipPlane.xyz, sp_worldPos) + uPortalClipPlane.w : 1.0;\n"
                   "}\n";
        }

        std::string TerrainFragmentEpilogue() {
            // The engine's per-pass alpha test (ChunkRenderer::RenderLayerPass
            // sets uAlphaTest: 0.1 opaque, 0.5 cutout, 0.01 translucent).
            return "\nvoid main() {\n"
                   "    sp_packMain();\n"
                   "    if (sp_FragData[0].a < uAlphaTest) discard;\n"
                   "}\n";
        }

        // The pack's own attribute declarations for the inputs we synthesise
        // would collide with the prelude's names: drop them.
        bool IsSynthesisedAttributeDecl(const std::string& line) {
            static const std::regex kDecl(R"(^[ \t]*(attribute|in)[ \t]+(vec[234]|float)[ \t]+(mc_Entity|mc_midTexCoord|at_tangent|at_midBlock|at_velocity)[ \t]*;)");
            return std::regex_search(line, kDecl);
        }

        // MC's block atlas sampler is called `texture`, a reserved function
        // name in core GLSL. Iris calls it gtexture; so do we.
        std::string RenameTextureSampler(std::string s) {
            s = std::regex_replace(s, std::regex(R"(sampler2D[ \t]+texture[ \t]*;)"), "sampler2D gtexture;");
            s = std::regex_replace(s, std::regex(R"(\([ \t]*texture[ \t]*,)"), "(gtexture,");
            return s;
        }

        std::string RenameMain(std::string s) {
            return std::regex_replace(s, std::regex(R"(\bvoid[ \t]+main[ \t]*\([ \t]*(void)?[ \t]*\))"), "void sp_packMain()");
        }

        // The engine keeps GL_CLIP_DISTANCE0 enabled for its portal clip,
        // so a vertex shader that leaves gl_ClipDistance[0] unwritten is
        // clipped by whatever the driver finds there: every full-screen
        // pass drew nothing until this wrote 1.0.
        std::string CompositeVertexEpilogue() {
            return "\nvoid main() {\n"
                   "    sp_packMain();\n"
                   "    gl_ClipDistance[0] = 1.0;\n"
                   "}\n";
        }

        std::string VertexPrelude(int version) {
            std::string p = CommonPrelude(version);
            p += "#define attribute in\n"
                 "#define varying out\n"
                 "layout(location = 0) in vec3 sp_aPosition;\n"
                 "layout(location = 1) in vec2 sp_aTexCoord;\n"
                 "layout(location = 2) in vec4 sp_aColor;\n"
                 "#define gl_Vertex vec4(sp_aPosition, 1.0)\n"
                 "#define gl_MultiTexCoord0 vec4(sp_aTexCoord, 0.0, 1.0)\n"
                 "#define gl_MultiTexCoord1 vec4(sp_aTexCoord, 0.0, 1.0)\n"
                 "#define gl_MultiTexCoord2 vec4(sp_aTexCoord, 0.0, 1.0)\n"
                 "#define gl_Color sp_aColor\n"
                 "#define gl_Normal vec3(0.0, 0.0, 1.0)\n"
                 "#define ftransform() (sp_ModelViewProjectionMatrix * vec4(sp_aPosition, 1.0))\n"
                 "out vec4 sp_TexCoord[4];\n"
                 "out vec4 sp_FrontColor;\n"
                 "out float sp_FogFragCoord;\n"
                 "#define gl_TexCoord sp_TexCoord\n"
                 "#define gl_FrontColor sp_FrontColor\n"
                 "#define gl_BackColor sp_FrontColor\n"
                 "#define gl_FogFragCoord sp_FogFragCoord\n"
                 "#line 1\n";
            return p;
        }

        std::string FragmentPrelude(int version, bool terrain) {
            std::string p = CommonPrelude(version);
            if (terrain) p += "uniform float uAlphaTest;\nuniform float sp_alphaRef;\n";
            p += "#define varying in\n"
                 "in vec4 sp_TexCoord[4];\n"
                 "in vec4 sp_FrontColor;\n"
                 "in float sp_FogFragCoord;\n"
                 "#define gl_TexCoord sp_TexCoord\n"
                 "#define gl_Color sp_FrontColor\n"
                 "#define gl_FogFragCoord sp_FogFragCoord\n"
                 "layout(location = 0) out vec4 sp_FragData[8];\n"
                 "#define gl_FragData sp_FragData\n"
                 "#define gl_FragColor sp_FragData[0]\n"
                 "#line 1\n";
            return p;
        }

        // `/* DRAWBUFFERS:013 */` or `/* RENDERTARGETS: 0,1,3 */`.
        std::vector<int> ParseDrawBuffers(const std::string& source) {
            std::vector<int> out;
            std::smatch m;
            static const std::regex kRT(R"(/\*[ \t]*RENDERTARGETS[ \t]*:[ \t]*([0-9, \t]+)\*/)");
            static const std::regex kDB(R"(/\*[ \t]*DRAWBUFFERS[ \t]*:[ \t]*([0-9A-Za-z]+)[ \t]*\*/)");
            if (std::regex_search(source, m, kRT)) {
                std::stringstream ss(m[1].str());
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    std::string digits;
                    for (char c : tok) if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
                    if (!digits.empty()) out.push_back(std::stoi(digits));
                }
            } else if (std::regex_search(source, m, kDB)) {
                for (char c : m[1].str()) {
                    if (std::isdigit(static_cast<unsigned char>(c))) out.push_back(c - '0');
                    else if (std::isalpha(static_cast<unsigned char>(c))) out.push_back(10 + std::tolower(static_cast<unsigned char>(c)) - 'a');
                }
            }
            if (out.empty()) out.push_back(0);
            return out;
        }

        // The pack source with only its #if branches decided (glslang's
        // preprocessor), for questions the raw text cannot answer: which
        // draw-buffer directive and which sampler declarations are live.
        // Callers mark what they ask about with tokens the preprocessor
        // keeps. False without glslang or when the source does not
        // preprocess — the raw-text answer then stands.
        bool PreprocessBranches(std::string src, bool fragment, std::string& pre) {
            // glslang will not #define a reserved gl_ name (the prelude maps
            // gl_ModelViewMatrix and friends that way); the copy only has to
            // decide the #if branches, which never test those names.
            for (size_t p = src.find("gl_"); p != std::string::npos; p = src.find("gl_", p + 6)) {
                if (p > 0 && (std::isalnum(static_cast<unsigned char>(src[p - 1])) || src[p - 1] == '_')) continue;
                src.insert(p, "sp_");
            }
            // Line continuations (Complementary) need GLSL 4.20; the version
            // only changes what the preprocessor accepts here.
            src = std::regex_replace(src, std::regex(R"(^#version[^\n]*)"), "#version 450 core",
                                     std::regex_constants::format_first_only);
            return Shaders::PackCompiler::Preprocess(src, fragment, pre);
        }

        // The directive that survives the program's own #if branches — the
        // last one left after preprocessing, as Iris and OptiFine read it.
        // A program can name its buffers twice (Complementary's composite:
        // DRAWBUFFERS:7, then :71 when PBR reflections are on; Sildur's water:
        // :41, then :412 only for OptiFine before 1.16.4), and the raw text
        // cannot tell which applies. False: the raw parse stands.
        bool ActiveDrawBuffers(const std::string& source, std::vector<int>& out) {
            static const std::regex kDirective(R"(/\*[ \t]*(RENDERTARGETS|DRAWBUFFERS)[ \t]*:[ \t]*([0-9A-Za-z, \t]+?)[ \t]*\*/)");
            std::string marked;
            std::vector<std::string> directives;
            size_t last = 0;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), kDirective); it != std::sregex_iterator(); ++it) {
                const std::smatch& m = *it;
                marked.append(source, last, static_cast<size_t>(m.position(0)) - last);
                marked += " sp_directive_" + std::to_string(directives.size()) + "_ ";
                directives.push_back(m.str(0));
                last = static_cast<size_t>(m.position(0) + m.length(0));
            }
            if (directives.size() < 2) return false;   // one or none: the raw parse is exact
            marked.append(source, last, std::string::npos);
            std::string pre;
            if (!PreprocessBranches(std::move(marked), true, pre)) return false;
            static const std::regex kMarker(R"(\bsp_directive_([0-9]+)_\b)");
            int lastRt = -1, lastDb = -1;
            for (auto mt = std::sregex_iterator(pre.begin(), pre.end(), kMarker); mt != std::sregex_iterator(); ++mt) {
                const int i = std::atoi((*mt)[1].str().c_str());
                if (i < 0 || i >= static_cast<int>(directives.size())) continue;
                (directives[i].find("RENDERTARGETS") != std::string::npos ? lastRt : lastDb) = i;
            }
            const int pick = lastRt >= 0 ? lastRt : lastDb;   // RENDERTARGETS wins, as in ParseDrawBuffers
            if (pick < 0) return false;
            out = ParseDrawBuffers(directives[pick]);
            return true;
        }

        // Which of `lines` (0-based line numbers in `source`) the
        // preprocessor keeps. False when it cannot run.
        bool SurvivingLines(const std::string& source, bool fragment, const std::set<size_t>& lines,
                            std::set<size_t>& surviving) {
            std::string marked;
            std::istringstream in(source);
            std::string line;
            for (size_t n = 0; std::getline(in, line); ++n) {
                if (lines.count(n)) marked += " sp_line_" + std::to_string(n) + "_ ";
                marked += line;
                marked += '\n';
            }
            std::string pre;
            if (!PreprocessBranches(std::move(marked), fragment, pre)) return false;
            static const std::regex kMarker(R"(\bsp_line_([0-9]+)_\b)");
            surviving.clear();
            for (auto mt = std::sregex_iterator(pre.begin(), pre.end(), kMarker); mt != std::sregex_iterator(); ++mt) {
                surviving.insert(static_cast<size_t>(std::stoull((*mt)[1].str())));
            }
            return true;
        }

    } // namespace

    std::vector<FormatDecl> FindFormatDecls(const std::string& source) {
        std::vector<FormatDecl> out;
        static const std::regex kDecl(R"(const[ \t]+int[ \t]+colortex(\d+)Format[ \t]*=[ \t]*([A-Za-z0-9_]+)[ \t]*;)");
        for (auto it = std::sregex_iterator(source.begin(), source.end(), kDecl); it != std::sregex_iterator(); ++it) {
            out.push_back({std::stoi((*it)[1].str()), (*it)[2].str()});
        }
        // The legacy names: gcolor .. gaux4 = colortex0 .. colortex7.
        static const char* kLegacy[] = { "gcolor", "gdepth", "gnormal", "composite", "gaux1", "gaux2", "gaux3", "gaux4" };
        for (int i = 0; i < 8; ++i) {
            const std::regex kL(std::string("const[ \\t]+int[ \\t]+") + kLegacy[i] + "Format[ \\t]*=[ \\t]*([A-Za-z0-9_]+)[ \\t]*;");
            std::smatch m;
            if (std::regex_search(source, m, kL)) out.push_back({i, m[1].str()});
        }
        return out;
    }

    std::vector<ClearDecl> FindClearDecls(const std::string& source) {
        std::vector<ClearDecl> out;
        static const char* kLegacy[] = { "gcolor", "gdepth", "gnormal", "composite", "gaux1", "gaux2", "gaux3", "gaux4" };
        auto indexOf = [&](const std::string& name) {
            if (name.rfind("colortex", 0) == 0) return std::atoi(name.c_str() + 8);
            for (int i = 0; i < 8; ++i) if (name == kLegacy[i]) return i;
            return -1;
        };
        static const std::regex kClear(
            R"(const[ \t]+bool[ \t]+(colortex\d+|gcolor|gdepth|gnormal|composite|gaux[1-4])Clear[ \t]*=[ \t]*(true|false)[ \t]*;)");
        for (auto it = std::sregex_iterator(source.begin(), source.end(), kClear); it != std::sregex_iterator(); ++it) {
            ClearDecl d{ indexOf((*it)[1].str()), true, (*it)[2].str() == "true", false, {0.0f, 0.0f, 0.0f, 0.0f} };
            if (d.index >= 0) out.push_back(d);
        }
        static const std::regex kColor(
            R"(const[ \t]+vec4[ \t]+(colortex\d+|gcolor|gdepth|gnormal|composite|gaux[1-4])ClearColor[ \t]*=[ \t]*vec4[ \t]*\(([^)]*)\)[ \t]*;)");
        for (auto it = std::sregex_iterator(source.begin(), source.end(), kColor); it != std::sregex_iterator(); ++it) {
            ClearDecl d{ indexOf((*it)[1].str()), false, true, true, {0.0f, 0.0f, 0.0f, 0.0f} };
            if (d.index < 0) continue;
            std::stringstream ss((*it)[2].str());
            std::string part;
            int n = 0;
            while (n < 4 && std::getline(ss, part, ',')) d.color[n++] = static_cast<float>(std::atof(part.c_str()));
            if (n == 1) for (int i = 1; i < 4; ++i) d.color[i] = d.color[0];   // vec4(x)
            out.push_back(d);
        }
        return out;
    }

    Translated Translate(const std::string& source, Stage stage,
                         const std::string& relPath, const FileReader& read) {
        Translated t;
        std::string expanded;
        if (!ResolveIncludes(source, relPath, read, 0, expanded, t.error)) return t;

        const bool fragment = stage == Stage::Fragment || stage == Stage::TerrainFragment || stage == Stage::EntityFragment;
        // "terrain" here: any gbuffers program (the engine's inputs, the
        // pack's main wrapped, the atlas sampler renamed).
        const bool terrain  = stage != Stage::Vertex && stage != Stage::Fragment;
        if (fragment) t.drawBuffers = ParseDrawBuffers(expanded);

        std::string body = expanded;
        const int version = ExtractVersion(body);

        // A missing extension is a warning under `enable`, an error under
        // `require`; none of the ones packs ask for change the meaning of
        // what compiles here.
        body = std::regex_replace(body, std::regex(R"(:[ \t]*require\b)"), ": enable");
        // Legacy extensions that core 330 either has or rejects by name,
        // and a pack's own precision statements (from mobile-minded authors).
        static const std::regex kLegacyExt(R"(^[ \t]*#[ \t]*extension[ \t]+GL_(EXT_gpu_shader4|ARB_shader_texture_lod|ARB_texture_rectangle|EXT_texture_array|ARB_explicit_attrib_location|ARB_shading_language_420pack)\b)");
        static const std::regex kPrecision(R"(^[ \t]*precision[ \t]+\w+[ \t]+\w+[ \t]*;)");
        body = FilterLines(body, [&](const std::string& line) {
            if (std::regex_search(line, kLegacyExt) || std::regex_search(line, kPrecision)) return false;
            if (terrain && !fragment && IsSynthesisedAttributeDecl(line)) return false;
            return true;
        });
        if (terrain) body = RenameTextureSampler(body);
        if (terrain || stage == Stage::Vertex) body = RenameMain(body);

        switch (stage) {
            case Stage::Vertex:          t.source = VertexPrelude(version) + body + CompositeVertexEpilogue(); break;
            case Stage::Fragment:        t.source = FragmentPrelude(version, false) + body; break;
            case Stage::TerrainVertex:   t.source = TerrainVertexPrelude(version) + body + TerrainVertexEpilogue(); break;
            case Stage::TerrainFragment: t.source = FragmentPrelude(version, true) + body + TerrainFragmentEpilogue(); break;
            case Stage::EntityVertex:    t.source = EntityVertexPrelude(version) + body + EntityVertexEpilogue(); break;
            case Stage::EntityFragment:  t.source = FragmentPrelude(version, true) + body + EntityFragmentEpilogue(); break;
        }
        // With the prelude's defines in place (MC_VERSION, MC_OS_*), which
        // of several draw-buffer directives the program really ends on.
        if (fragment) {
            std::vector<int> active;
            if (ActiveDrawBuffers(t.source, active)) t.drawBuffers = std::move(active);
        }
        return t;
    }

    // ── the Vulkan target ────────────────────────────────────────────────

    namespace {
        struct UniformDecl { std::string type, name, array; };   // array: "[N]" or ""

        bool IsSamplerType(const std::string& type) {
            return type.find("sampler") != std::string::npos;
        }
        bool StartsWithUniform(const std::string& line, size_t& pos) {
            pos = line.find_first_not_of(" \t");
            if (pos == std::string::npos) return false;
            return line.compare(pos, 7, "uniform") == 0 &&
                   (pos + 7 >= line.size() || std::isspace(static_cast<unsigned char>(line[pos + 7])));
        }
        // `type a = x, b[2], c;` → the declarators, commas inside parentheses
        // or brackets left alone.
        std::vector<std::string> SplitDeclarators(const std::string& list) {
            std::vector<std::string> out;
            int depth = 0;
            std::string cur;
            for (char c : list) {
                if (c == '(' || c == '[') ++depth;
                else if (c == ')' || c == ']') --depth;
                if (c == ',' && depth == 0) { out.push_back(cur); cur.clear(); continue; }
                cur += c;
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        }
        std::string TrimWs(const std::string& t) {
            const size_t a = t.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return "";
            const size_t b = t.find_last_not_of(" \t\r\n");
            return t.substr(a, b - a + 1);
        }
        // `uniform [precision] type declarators ;` on one line. Returns the
        // type and the declarators (initializers stripped) or false.
        bool ParseUniformLine(const std::string& line, std::string& type, std::vector<UniformDecl>& decls,
                              std::string* rest = nullptr) {
            static const std::regex kLine(R"(^[ \t]*uniform[ \t]+(?:(?:lowp|mediump|highp)[ \t]+)?([A-Za-z_][A-Za-z0-9_]*)[ \t]+([^;]+);)");
            std::smatch m;
            if (!std::regex_search(line, m, kLine)) return false;
            if (rest) *rest = m.suffix().str();   // whatever follows on the line
            type = m[1].str();
            for (std::string d : SplitDeclarators(m[2].str())) {
                const size_t eq = d.find('=');
                if (eq != std::string::npos) d = d.substr(0, eq);
                d = TrimWs(d);
                UniformDecl u;
                const size_t br = d.find('[');
                if (br != std::string::npos) { u.name = TrimWs(d.substr(0, br)); u.array = TrimWs(d.substr(br)); }
                else u.name = d;
                u.type = type;
                if (!u.name.empty()) decls.push_back(u);
            }
            return !decls.empty();
        }
        // Locations a varying of `type[array]` takes (Vulkan: a column or a
        // scalar/vector per location, arrays by element).
        int LocationsOf(const std::string& type, const std::string& array) {
            int per = 1;
            if (type == "mat4" || type == "mat4x4") per = 4;
            else if (type == "mat3" || type == "mat3x3") per = 3;
            else if (type == "mat2" || type == "mat2x2") per = 2;
            else if (type == "dvec3" || type == "dvec4") per = 2;
            int count = 1;
            if (!array.empty()) count = std::max(1, std::atoi(array.c_str() + 1));
            return per * count;
        }
        // `[flat|smooth|noperspective|centroid]* (in|out|varying) type
        // name[array];` with no layout of its own — `varying` is the
        // compatibility spelling the prelude #defines to the stage's
        // direction, so it counts as that direction here.
        // Returns the length of the declaration matched at the start of
        // `line` (0: none), so a caller can walk a line of several.
        size_t ParseVarying(const std::string& line, const char* direction, std::string& qualifiers,
                            std::string& type, std::string& name, std::string& array) {
            if (line.find("layout") != std::string::npos) return 0;
            static const std::regex kOut(R"(^[ \t]*((?:(?:flat|smooth|noperspective|centroid)[ \t]+)*)(?:out|varying)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*(\[[0-9]+\])?[ \t]*;)");
            static const std::regex kIn(R"(^[ \t]*((?:(?:flat|smooth|noperspective|centroid)[ \t]+)*)(?:in|varying)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*(\[[0-9]+\])?[ \t]*;)");
            std::smatch m;
            if (!std::regex_search(line, m, direction[0] == 'o' ? kOut : kIn)) return 0;
            qualifiers = m[1].str();
            type = m[2].str();
            name = m[3].str();
            array = m[4].matched ? m[4].str() : "";
            return static_cast<size_t>(m[0].length());
        }
        // `flat out vec3 upVec, sunVec;` → one declaration per name, which
        // ParseVarying then takes one at a time (Complementary's composite1).
        // Only plain names (an optional [N]) count: a parameter list of a
        // function signature wrapped onto its own line is left alone.
        std::string ExpandVaryingList(const std::string& line) {
            if (line.find(',') == std::string::npos || line.find("layout") != std::string::npos) return line;
            static const std::regex kList(R"(^([ \t]*(?:(?:flat|smooth|noperspective|centroid)[ \t]+)*(?:out|in|varying)[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+)([^;(){}]*,[^;(){}]*);)");
            static const std::regex kName(R"(^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*(\[[0-9]+\])?[ \t]*$)");
            std::smatch m;
            if (!std::regex_search(line, m, kList)) return line;
            std::string expanded;
            for (const std::string& d : SplitDeclarators(m[2].str())) {
                if (!std::regex_match(d, kName)) return line;
                expanded += m[1].str() + TrimWs(d) + "; ";
            }
            return expanded + m.suffix().str();
        }
    } // namespace

    bool Vulkanize(const std::string& vertexCore, const std::string& fragmentCore,
                   const std::map<std::string, int>& samplerUnits, int fragmentOutputs,
                   Vulkanized& out) {
        out = Vulkanized{};
        // The block members, in first-seen order, and the sampler slots.
        std::vector<UniformDecl> members;
        std::set<std::string> memberNames;
        // The samplers both stages declare (name → type), then their slots:
        // the pipeline's units for the ones it binds; a free slot for the
        // rest (they read whatever the slot holds, as on OpenGL); and when
        // the layout is full, an ALIAS of a same-type sampler declared in
        // the same stage — such samplers (Distant Horizons' dhDepthTex*, an
        // optional feature's input) are read only on paths that are off.
        std::map<std::string, std::string> declared[2];   // [0] vertex, [1] fragment
        // Declared without a binding: samplers whose every declaration sits
        // in an #if branch the preprocessor drops (Complementary's colored
        // lighting and world-space reflection inputs), so they take no slot
        // and are never an alias target; and, when the layout is full with
        // no same-type sampler to alias, the rest — likely behind an option
        // too (used for real, glslang rejects it: the same failure as
        // running out of slots).
        std::set<std::string> samplerUnslotted;
        std::set<std::string> inactiveSamplers;
        auto collect = [&](const std::string& src, int stage) {
            struct Decl { size_t line; std::string type, name; };
            std::vector<Decl> found;
            std::set<size_t> lineNumbers;
            std::istringstream in(src);
            std::string line;
            for (size_t n = 0; std::getline(in, line); ++n) {
                size_t pos;
                std::string type;
                std::vector<UniformDecl> decls;
                if (!StartsWithUniform(line, pos) || !ParseUniformLine(line, type, decls) || !IsSamplerType(type)) continue;
                for (const UniformDecl& d : decls) found.push_back({n, type, d.name});
                lineNumbers.insert(n);
            }
            // The raw text holds every #if branch; the preprocessor says
            // which declarations are live (all of them when it cannot run).
            std::set<size_t> live;
            const bool known = SurvivingLines(src, stage == 1, lineNumbers, live);
            for (const Decl& d : found) {
                if (!known || live.count(d.line)) declared[stage].emplace(d.name, d.type);
                else inactiveSamplers.insert(d.name);
            }
        };
        collect(vertexCore, 0);
        collect(fragmentCore, 1);
        for (const std::string& name : inactiveSamplers) {
            if (!declared[0].count(name) && !declared[1].count(name)) samplerUnslotted.insert(name);
        }
        std::map<std::string, int> samplerSlot;            // name → slot, both stages
        std::map<std::string, std::string> samplerAlias;   // name → the sampler it stands for
        {
            std::set<int> usedSlots;
            std::vector<std::string> unbound;
            for (const auto& decls : declared) {
                for (const auto& [name, type] : decls) {
                    if (samplerSlot.count(name)) continue;
                    auto known = samplerUnits.find(name);
                    if (known != samplerUnits.end()) { samplerSlot[name] = known->second; usedSlots.insert(known->second); }
                    else if (std::find(unbound.begin(), unbound.end(), name) == unbound.end()) unbound.push_back(name);
                }
            }
            for (const std::string& name : unbound) {
                int slot = -1;
                for (int s = 15; s >= 0; --s) if (!usedSlots.count(s)) { slot = s; break; }
                if (slot >= 0) { samplerSlot[name] = slot; usedSlots.insert(slot); continue; }
                // Full: an alias in every stage that declares it.
                std::string type;
                for (const auto& decls : declared) if (auto it = decls.find(name); it != decls.end()) type = it->second;
                std::string alias;
                for (const auto& decls : declared) {
                    if (!decls.count(name)) continue;
                    std::string candidate;
                    for (const auto& [other, otherType] : decls) {
                        if (other != name && otherType == type && samplerSlot.count(other)) { candidate = other; break; }
                    }
                    if (candidate.empty()) { alias.clear(); break; }
                    if (alias.empty()) alias = candidate;
                    else if (alias != candidate && !decls.count(alias)) alias = candidate;   // must exist in this stage too
                }
                if (alias.empty()) samplerUnslotted.insert(name);
                else samplerAlias[name] = alias;
            }
        }
        auto slotFor = [&](const std::string& name, std::string& error) -> int {
            auto it = samplerSlot.find(name);
            if (it != samplerSlot.end()) return it->second;
            error = "sampler " + name + " has no slot";
            return -1;
        };
        // Varying locations from the vertex stage, by name.
        std::map<std::string, int> varyingLocation;
        int nextLocation = 0;
        int nextAttribute = 8;   // the engine's inputs are 0..3
        // What each stage declared, by name: a fragment input the vertex
        // stage never writes gets a vertex output (Vulkan pairs them; an
        // unwritten one read undefined on OpenGL too).
        struct VaryingDecl { std::string qualifiers, type, array; int location = 0; };
        std::map<std::string, VaryingDecl> vertexOuts, fragmentIns;

        auto process = [&](const std::string& src, bool vertex, std::string& result) -> bool {
            std::istringstream in(src);
            std::string line;
            std::vector<std::string> lines;
            std::deque<std::string> pending;
            bool hasTextureSampler = false;
            for (;;) {
                if (!pending.empty()) { line = pending.front(); pending.pop_front(); }
                else if (!std::getline(in, line)) break;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t pos;
                if (line.rfind("#version", line.find_first_not_of(" \t")) != std::string::npos &&
                    line.find("#version") == line.find_first_not_of(" \t")) {
                    lines.push_back("#version 450");
                    lines.push_back("// PackUniforms (ShaderPackGlsl::Vulkanize) is declared below the preprocessor lines.");
                    // Fast math (the backends' usual, and what the pack's own
                    // GL driver gives it) computes pow as exp2(y * log2(x)),
                    // NaN at x = 0; packs raise black to 2.2 everywhere
                    // (Sildur's water fog, 2026-10-09). A base clamped just
                    // above zero: the same result to the eighth decimal, no NaN.
                    lines.push_back("#define pow(x, y) pow(max((x), 1e-7), (y))");
                    continue;
                }
                if (StartsWithUniform(line, pos)) {
                    std::string type, rest;
                    std::vector<UniformDecl> decls;
                    if (ParseUniformLine(line, type, decls, &rest)) {
                        // The line may go on (`uniform sampler2D a; in vec2 v;`):
                        // the remainder is the next line to translate.
                        if (rest.find_first_not_of(" \t\r") != std::string::npos) pending.push_front(rest);
                        if (IsSamplerType(type)) {
                            std::string rebuilt;
                            for (const UniformDecl& d : decls) {
                                std::string name = d.name;
                                if (name == "texture") { hasTextureSampler = true; name = "sp_texture"; }
                                auto alias = samplerAlias.find(d.name);
                                if (alias != samplerAlias.end()) {
                                    const std::string target = alias->second == "texture" ? "sp_texture" : alias->second;
                                    rebuilt += "#define " + name + " " + target + "\n";
                                    continue;
                                }
                                if (samplerUnslotted.count(d.name)) {
                                    rebuilt += "uniform " + type + " " + name + d.array + "; ";
                                    continue;
                                }
                                std::string err;
                                const int slot = slotFor(d.name, err);
                                if (slot < 0) { out.error = err; return false; }
                                rebuilt += "layout(set = 0, binding = " + std::to_string(slot) + ") uniform " + type + " " + name + d.array + "; ";
                            }
                            if (!rebuilt.empty() && rebuilt.back() == '\n') rebuilt.pop_back();
                            lines.push_back(rebuilt);
                        } else if (type.find("image") != std::string::npos || type == "atomic_uint" ||
                                   type == "writeonly" || type == "readonly" || type == "coherent" ||
                                   type == "volatile" || type == "restrict") {   // a memory qualifier: an image follows
                            // Opaque but no sampler (Iris custom images,
                            // `uniform writeonly image2D x;`): never a block
                            // member. Left as written — behind an option
                            // that is off, as in Complementary, the
                            // preprocessor drops it; used, glslang says so.
                            lines.push_back(line.substr(0, line.size() - rest.size()));
                        } else {
                            for (const UniformDecl& d : decls) {
                                if (memberNames.insert(d.name).second) members.push_back(d);
                            }
                            lines.push_back("// uniform " + type + " -> PackUniforms");
                        }
                        continue;
                    }
                }
                // The engine's own std140 block (the terrain prelude's
                // SectionOrigins): the user uniform set, as the engine's
                // Vulkan shaders bind it.
                if (line.find("layout(std140) uniform ") != std::string::npos) {
                    lines.push_back(std::regex_replace(line, std::regex(R"(layout\(std140\)[ \t]+uniform)"),
                                                       "layout(std140, set = 3, binding = 0) uniform"));
                    continue;
                }
                // A pack's own vertex attribute (`attribute type name;`, one
                // the engine does not feed): an input location past the
                // engine's own, so it compiles and reads zero.
                static const std::regex kAttribute(R"(^[ \t]*attribute[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*;)");
                std::smatch am;
                if (vertex && std::regex_search(line, am, kAttribute)) {
                    lines.push_back("layout(location = " + std::to_string(nextAttribute) + ") in " + am[1].str() + " " + am[2].str() + ";");
                    nextAttribute += LocationsOf(am[1].str(), "");
                    continue;
                }
                // Varyings: locations. Several may share a line, and the line
                // may go on after them (`out vec2 vUV; void main() {`).
                {
                    std::string rest = ExpandVaryingList(line), rebuilt;
                    bool any = false;
                    for (;;) {
                        std::string q, type, name, array;
                        const size_t len = ParseVarying(rest, vertex ? "out" : "in", q, type, name, array);
                        if (len == 0) break;
                        int loc;
                        auto known = varyingLocation.find(name);
                        if (known != varyingLocation.end()) loc = known->second;
                        else {
                            loc = nextLocation;
                            nextLocation += LocationsOf(type, array);
                            varyingLocation[name] = loc;
                        }
                        rebuilt += "layout(location = " + std::to_string(loc) + ") " + q + (vertex ? "out " : "in ") + type + " " + name + array + "; ";
                        (vertex ? vertexOuts : fragmentIns)[name] = VaryingDecl{q, type, array, loc};
                        rest = rest.substr(len);
                        any = true;
                    }
                    if (any) { lines.push_back(rebuilt + rest); continue; }
                }
                lines.push_back(line);
            }
            // Second pass: the renames, and the fragment output count.
            std::string joined;
            for (const std::string& l : lines) { joined += l; joined += '\n'; }
            joined = std::regex_replace(joined, std::regex(R"(\bgl_VertexID\b)"), "gl_VertexIndex");
            joined = std::regex_replace(joined, std::regex(R"(\bgl_InstanceID\b)"), "gl_InstanceIndex");
            if (hasTextureSampler) {
                // The identifier, never the function: `texture(` stays.
                joined = std::regex_replace(joined, std::regex(R"(\btexture\b(?![ \t]*\())"), "sp_texture");
            }
            if (!vertex) {
                const std::string outputs = std::to_string(std::max(1, std::min(fragmentOutputs, 8)));
                joined = std::regex_replace(joined, std::regex(R"(out[ \t]+vec4[ \t]+sp_FragData\[8\])"), "out vec4 sp_FragData[" + outputs + "]");
            }
            result = std::move(joined);
            return true;
        };

        std::string vert, frag;
        if (!process(vertexCore, true, vert)) return false;
        if (!process(fragmentCore, false, frag)) return false;
        for (const auto& [name, d] : fragmentIns) {
            if (vertexOuts.count(name)) continue;
            vert += "\nlayout(location = " + std::to_string(d.location) + ") " + d.qualifiers + "out " + d.type + " " + name + d.array + ";\n";
        }
        // Clip space: the pack's matrices stay GL-style everywhere (its
        // own depth arithmetic — gbufferProjectionInverse on depth*2-1 —
        // needs them so), and the vertex stage remaps clip z from GL's
        // [-w, w] to Vulkan's [0, w] after the pack's main: the depth the
        // backends store then equals OpenGL's (z_ndc + 1) / 2.
        {
            static const std::regex kMain(R"(\bvoid[ \t]+main[ \t]*\([ \t]*(void)?[ \t]*\))");
            std::smatch m;
            if (std::regex_search(vert, m, kMain)) {
                // (The core translation's own main — the clip-distance wrapper
                // around the pack's sp_packMain — is what gets wrapped here.)
                vert = std::regex_replace(vert, kMain, "void sp_vkMain()", std::regex_constants::format_first_only);
                vert += "\nvoid main() { sp_vkMain(); gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; }\n";
            } else {
                out.error = "vertex stage has no main()";
                return false;
            }
        }

        // The block, the same text in both stages, placed after the last
        // preprocessor line at the top (version, extensions, the prelude's
        // defines) — before any declaration that could use a member.
        std::string block = "layout(std140, set = 1, binding = 0) uniform PackUniforms {\n";
        for (const UniformDecl& m : members) block += "    " + m.type + " " + m.name + m.array + ";\n";
        block += "};\n";
        auto insertBlock = [&](std::string& src) {
            if (members.empty()) return;
            // After the version, the extensions and the prelude's defines —
            // and never inside a conditional block, whose other branch would
            // then lack it: the insertion point is the end of the last
            // directive line at #if depth 0 before the first declaration.
            size_t p = 0, candidate = 0;
            int depth = 0;
            size_t ifStart = std::string::npos;
            while (p < src.size()) {
                const size_t eol = src.find('\n', p);
                const size_t next = eol == std::string::npos ? src.size() : eol + 1;
                const std::string l = src.substr(p, next - p);
                const size_t first = l.find_first_not_of(" \t\r\n");
                const bool blank = first == std::string::npos;
                const bool directive = !blank && (l[first] == '#' || l.compare(first, 2, "//") == 0);
                if (!blank && !directive) {
                    if (depth > 0 && ifStart != std::string::npos) candidate = ifStart;
                    break;
                }
                if (directive && l[first] == '#') {
                    const std::string d = l.substr(first + 1);
                    const size_t w = d.find_first_not_of(" \t");
                    const std::string word = w == std::string::npos ? "" : d.substr(w, d.find_first_of(" \t\r\n", w) - w);
                    if (word == "if" || word == "ifdef" || word == "ifndef") { if (depth == 0) ifStart = p; ++depth; }
                    else if (word == "endif") { if (depth > 0) --depth; if (depth == 0) { ifStart = std::string::npos; candidate = next; } }
                    else if (depth == 0) candidate = next;
                } else if (depth == 0) {
                    candidate = next;
                }
                p = next;
            }
            src.insert(candidate, block);
        };
        insertBlock(vert);
        insertBlock(frag);
        out.vertex = std::move(vert);
        out.fragment = std::move(frag);
        for (const auto& [name, slot] : samplerSlot) out.samplers.emplace_back(name, slot);
        return true;
    }

} // namespace Render::PackGlsl
