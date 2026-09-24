// File: src/client/renderer/shader/ShaderPackGlsl.cpp
#include "ShaderPackGlsl.hpp"

#include <cctype>
#include <cstdlib>
#include <regex>
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
                 "#define MC_ANISOTROPIC_FILTERING 16\n"
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
                 "vec3 sp_worldPos; vec3 sp_normal; vec4 sp_color; vec4 sp_mcEntity; vec4 sp_midTexCoord; vec4 sp_tangent;\n"
                 "#define attribute in\n"
                 "#define varying out\n"
                 "#define gl_Vertex vec4(sp_worldPos, 1.0)\n"
                 "#define gl_MultiTexCoord0 vec4(aTexCoord, 0.0, 1.0)\n"
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
                 "    sp_midTexCoord = vec4(aTexCoord, 0.0, 1.0);\n"
                 "    // block.properties id and the sprite's centre, from the atlas-space map (ShaderPipeline::EnsureEntityMap).\n"
                 "    float id = sp_unmappedEntity;\n"
                 "    if (sp_entityMapSize.x > 0.0) {\n"
                 "        ivec2 cell = ivec2(clamp(floor(aTexCoord * sp_entityMapSize), vec2(0.0), sp_entityMapSize - 1.0));\n"
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
        return t;
    }

} // namespace Render::PackGlsl
