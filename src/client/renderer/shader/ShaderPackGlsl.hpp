// File: src/client/renderer/shader/ShaderPackGlsl.hpp
//
// Turns a shader pack's GLSL — written for Minecraft's compatibility-profile
// GL, `#version 120` with gl_Vertex, gl_TexCoord, gl_FragData and the
// built-in matrices — into core-profile GLSL the engine's context compiles.
// The same job Iris's transformer does, by textual rewriting: a prelude that
// declares the replacements (sp_* uniforms and varyings) and #defines the
// legacy names onto them, the legacy storage qualifiers mapped to in/out,
// the deprecated texture functions renamed, and `#include` resolved against
// the pack. Also reads the directives a pass carries as comments:
// `/* DRAWBUFFERS:013 */`, `/* RENDERTARGETS: 0,1,3 */`, and the
// `const int colortexNFormat = RGBA16F;` buffer formats.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Render::PackGlsl {

    // Vertex/Fragment: a full-screen pass (composite, deferred, final).
    // TerrainVertex/TerrainFragment: a gbuffers program drawn over the
    // engine's packed terrain vertices — the prelude decodes them into
    // gl_Vertex, gl_Normal, gl_Color, gl_MultiTexCoord0/1, mc_Entity and
    // at_tangent, wraps the pack's main() so the decode runs first, and
    // applies the engine's alpha test after it.
    // EntityVertex/EntityFragment: a gbuffers program over the engine's
    // 24-byte block-layout renderers (entities, block entities, sky,
    // clouds, particles): position, uv and colour are per vertex already.
    enum class Stage { Vertex, Fragment, TerrainVertex, TerrainFragment, EntityVertex, EntityFragment };

    // Reads a pack file by path relative to the pack's `shaders/` folder
    // (no leading slash). Returns false when it does not exist.
    using FileReader = std::function<bool(const std::string& relPath, std::string& out)>;

    struct Translated {
        std::string      source;
        std::vector<int> drawBuffers;     // fragment: from DRAWBUFFERS / RENDERTARGETS, default {0}
        std::string      error;           // set when translation itself failed (missing include)
    };

    // `relPath` is the file's own path under shaders/ (for relative includes).
    Translated Translate(const std::string& source, Stage stage,
                         const std::string& relPath, const FileReader& read);

    // `const int colortexNFormat = X;` across a source: (index, format name).
    struct FormatDecl { int index; std::string format; };
    std::vector<FormatDecl> FindFormatDecls(const std::string& source);

    // `const bool colortexNClear = false;` and
    // `const vec4 colortexNClearColor = vec4(r, g, b, a);` across a source.
    // A buffer that is not cleared keeps its contents from frame to frame
    // (a pack's TAA or exposure history).
    struct ClearDecl { int index; bool hasClear; bool clear; bool hasColor; float color[4]; };
    std::vector<ClearDecl> FindClearDecls(const std::string& source);

} // namespace Render::PackGlsl
