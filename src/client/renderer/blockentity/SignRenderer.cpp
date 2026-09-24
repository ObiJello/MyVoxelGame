// File: src/client/renderer/blockentity/SignRenderer.cpp
#include "SignRenderer.hpp"
#include "BlockEntityShader.hpp"
#include "common/world/lighting/LightCoords.hpp"
#include "../backend/RenderBackend.hpp"
#include "../core/RenderOrigin.hpp"
#include "../gui/GuiRenderState.hpp"
#include "client/world/ClientBlockAccess.hpp"
#include "common/world/block/entity/SignBlockEntity.hpp"
#include "common/world/block/BlockPlacement.hpp"
#include "common/world/block/BlockState.hpp"
#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <functional>
#include <string>
#include <vector>

namespace PlatformMain { std::string GetAssetPath(const std::string& relativePath); }

namespace Render {

    namespace {
        // The shared 24-byte block vertex layout; FontRenderer's glyph
        // emitter brace-initialises exactly this field order.
        struct CubeVert {
            float x, y, z;
            float u, v;
            uint8_t r, g, b, a;
        };
        static_assert(sizeof(CubeVert) == 24, "vertex stride must match block layout");

        int64_t KeyOf(const glm::ivec3& p) {
            return (static_cast<int64_t>(p.x) & 0x3FFFFFF) |
                   ((static_cast<int64_t>(p.z) & 0x3FFFFFF) << 26) |
                   ((static_cast<int64_t>(p.y) & 0xFFF) << 52);
        }

        // MC ARGB.scaleRGB(color, 0.4F): the dark ink of a non-glowing sign.
        uint32_t ScaleRGB(uint32_t argb, float s) {
            const auto ch = [&](int shift) {
                return static_cast<uint32_t>(std::min(255.0f, static_cast<float>((argb >> shift) & 0xFF) * s)) << shift;
            };
            return (argb & 0xFF000000u) | ch(16) | ch(8) | ch(0);
        }

        // MC AbstractSignRenderer.getDarkColor.
        uint32_t DarkColor(const Game::SignText& text) {
            const uint32_t colour = Game::DyeTextColor(text.color);
            if (colour == Game::DyeTextColor(Game::DyeColor::Black) && text.glowing) return 0xFFF0EBCCu;
            return ScaleRGB(colour, 0.4f);
        }

        // MC StandingSignRenderer / HangingSignRenderer.textTransformation:
        // block-local, in blocks; the glyph coordinates are font pixels.
        glm::mat4 TextTransform(bool hanging, bool wall, float yawDeg, bool front) {
            glm::mat4 m(1.0f);
            if (!hanging) {
                m = glm::translate(m, glm::vec3(0.5f, 0.5f, 0.5f));
                m = glm::rotate(m, glm::radians(-yawDeg), glm::vec3(0, 1, 0));
                if (wall) m = glm::translate(m, glm::vec3(0.0f, -0.3125f, -0.4375f));
                if (!front) m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0, 1, 0));
                m = glm::translate(m, glm::vec3(0.0f, 0.33333334f, 0.046666667f));
                const float s = 0.010416667f;
                m = glm::scale(m, glm::vec3(s, -s, s));
            } else {
                m = glm::translate(m, glm::vec3(0.5f, 0.9375f, 0.5f));
                m = glm::rotate(m, glm::radians(-yawDeg), glm::vec3(0, 1, 0));
                m = glm::translate(m, glm::vec3(0.0f, -0.3125f, 0.0f));
                if (!front) m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0, 1, 0));
                m = glm::translate(m, glm::vec3(0.0f, -0.32f, 0.073f));
                const float s = 0.0140625f;
                m = glm::scale(m, glm::vec3(s, -s, s));
            }
            return m;
        }
    } // namespace

    SignRenderer::SignRenderer() = default;
    SignRenderer::~SignRenderer() { Shutdown(); }

    bool SignRenderer::Initialize() {
        if (!g_renderBackend) return false;
        // The shared block-entity shader (BlockEntityShader.hpp): lit and
        // fogged like the terrain, on both backends.
        m_shader = BlockEntityShader::Create();
        if (m_shader == INVALID_SHADER) {
            Log::Error("[SignRenderer] shader compile failed");
            return false;
        }
        if (!m_font.Initialize(PlatformMain::GetAssetPath("assets/textures/font/ascii.png"))) {
            Log::Error("[SignRenderer] font sheet failed to load");
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
            return false;
        }
        m_lastSweep = std::chrono::steady_clock::now();
        m_initialized = true;
        return true;
    }

    bool SignRenderer::BuildLayer(Layer& layer, const void* vertexData, size_t vertexBytes,
                                  const uint32_t* indices, size_t indexCount) {
        DestroyLayer(layer);
        if (indexCount == 0) return true;   // nothing on this layer is not a failure
        layer.vb = g_renderBackend->CreateBuffer(BufferUsage::Vertex, vertexBytes, vertexData);
        layer.ib = g_renderBackend->CreateBuffer(BufferUsage::Index, indexCount * sizeof(uint32_t), indices);
        layer.mesh = g_renderBackend->CreateMesh(layer.vb, layer.ib, GetBlockVertexLayout());
        layer.indexCount = static_cast<uint32_t>(indexCount);
        if (layer.mesh == INVALID_MESH) { DestroyLayer(layer); return false; }
        return true;
    }

    void SignRenderer::DestroyLayer(Layer& l) {
        if (!g_renderBackend) return;
        // Deferred, never immediate: a layer is replaced mid-frame (an edit,
        // or a glowing sign crossing the outline radius) while the previous
        // frame's command stream may still be drawing it. On Vulkan an
        // immediate destroy frees memory the GPU is reading — a device loss
        // (VK_ERROR_DEVICE_LOST from every submit after it). GL's default
        // is an immediate delete; its driver refcounts pending commands.
        if (l.mesh != INVALID_MESH)   { g_renderBackend->DeferredDestroyMesh(l.mesh);   l.mesh = INVALID_MESH; }
        if (l.vb   != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(l.vb);   l.vb = INVALID_BUFFER; }
        if (l.ib   != INVALID_BUFFER) { g_renderBackend->DeferredDestroyBuffer(l.ib);   l.ib = INVALID_BUFFER; }
        l.indexCount = 0;
    }

    void SignRenderer::DestroyCached(Cached& c) {
        DestroyLayer(c.text);
        DestroyLayer(c.glowText);
        DestroyLayer(c.outline);
        c.built = false;
    }

    void SignRenderer::Shutdown() {
        for (auto& [key, cached] : m_cache) DestroyCached(cached);
        m_cache.clear();
        m_font.Shutdown();
        if (g_renderBackend && m_shader != INVALID_SHADER) {
            g_renderBackend->DestroyShader(m_shader);
            m_shader = INVALID_SHADER;
        }
        m_initialized = false;
    }

    void SignRenderer::SweepStale(std::chrono::steady_clock::time_point now) {
        // A sign that has not been drawn for a while (out of view, its
        // chunk gone) gives its buffers back.
        if (now - m_lastSweep < std::chrono::seconds(5)) return;
        m_lastSweep = now;
        for (auto it = m_cache.begin(); it != m_cache.end();) {
            if (now - it->second.lastUsed > std::chrono::seconds(10)) {
                DestroyCached(it->second);
                it = m_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void SignRenderer::Render(const Game::BlockEntity& be, float /*partialTick*/,
                              const glm::mat4& proj, const glm::mat4& view,
                              const glm::vec3& cameraPos) {
        PROFILE_ZONE_N("BE.Sign");
        if (!m_initialized || !g_renderBackend || !Client::g_clientBlockAccess) return;
        const auto* sign = dynamic_cast<const Game::SignBlockEntity*>(&be);
        if (!sign) return;

        const Game::SignText& front = sign->GetText(Game::SignTextSlot::Front);
        const Game::SignText& back  = sign->GetText(Game::SignTextSlot::Back);
        if (!front.HasMessage() && !back.HasMessage()) return;

        const glm::ivec3 pos = be.GetWorldPos();
        const Game::BlockState state = Client::g_clientBlockAccess->GetBlockState(pos.x, pos.y, pos.z);
        const Game::BlockID id = state.Block();
        if (!Game::IsSignBlock(id)) return;

        const auto now = std::chrono::steady_clock::now();
        SweepStale(now);

        // AbstractSignRenderer.isOutlineVisible: the outline is drawn within
        // 16 blocks of the camera (or always while scoping, which this
        // engine has no spyglass for). Black glowing text keeps it at any
        // range — without the outline it would be invisible on the board.
        constexpr float kOutlineRenderDistanceSq = 16.0f * 16.0f;
        const glm::vec3 centre = glm::vec3(pos) + glm::vec3(0.5f);
        const glm::vec3 toCamera = centre - cameraPos;
        const bool outlineInRange = glm::dot(toCamera, toCamera) < kOutlineRenderDistanceSq;

        // The meshes are a function of the text, its styling, whether the
        // outline is drawn and the block's orientation; hash all of it so an
        // edit (or walking into outline range) rebuilds and nothing else does.
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&](uint64_t v) { hash ^= v; hash *= 1099511628211ull; };
        auto drawOutlineFor = [&](const Game::SignText& t) {
            return t.glowing && (t.color == Game::DyeColor::Black || outlineInRange);
        };
        for (const Game::SignText* t : { &front, &back }) {
            for (const std::string& line : t->lines) mix(std::hash<std::string>{}(line));
            mix(static_cast<uint64_t>(t->color) * 31 + (t->glowing ? 1 : 0) + (drawOutlineFor(*t) ? 2 : 0));
        }
        mix(static_cast<uint64_t>(state.Index()) * 65537 + static_cast<uint64_t>(id));

        Cached& cached = m_cache[KeyOf(pos)];
        cached.lastUsed = now;
        if (!cached.built || cached.hash != hash) {
            DestroyCached(cached);
            cached.hash = hash;

            const bool hanging = sign->IsHanging();
            const bool wall    = Game::IsWallSignBlock(id) || Game::IsWallHangingSignBlock(id);
            const float yaw    = Game::SignYawDegrees(state);
            const int lineHeight   = sign->LineHeight();
            const int maxLineWidth = sign->MaxLineWidth();
            const int signMidpoint = 4 * lineHeight / 2;

            // Two layers, because MC draws them in two render types: the
            // glyphs in Font.DisplayMode.POLYGON_OFFSET (pushed towards the
            // viewer), the 8-way outline in NORMAL (TextFeatureRenderer
            // .prepareText: outline first, then the text on top). Sharing a
            // depth mode made the two sets of coplanar quads z-fight.
            // Glowing text is its own layer: MC draws it at full brightness
            // (AbstractSignRenderer: LightCoordsUtil.FULL_BRIGHT), plain text
            // at the sign's light.
            std::vector<CubeVert> textVerts, glowVerts, outlineVerts;
            std::vector<uint32_t> textIdx, glowIdx, outlineIdx;

            for (int face = 0; face < 2; ++face) {
                const bool isFront = face == 0;
                const Game::SignText& text = isFront ? front : back;
                if (!text.HasMessage()) continue;
                const glm::mat4 xform = TextTransform(hanging, wall, yaw, isFront);

                // AbstractSignRenderer.submitSignText: glowing text is drawn in
                // its own colour with a dark 8-way outline, plain text in the
                // darkened colour with no outline.
                const uint32_t dark = DarkColor(text);
                const uint32_t textColor = text.glowing ? Game::DyeTextColor(text.color) : dark;
                const bool outline = drawOutlineFor(text);

                for (int i = 0; i < Game::SignText::kLines; ++i) {
                    // Font.split(input, maxTextLineWidth) → the first piece:
                    // what fits on the board.
                    std::string line = text.lines[i];
                    while (!line.empty() && m_font.GetStringWidth(line) > maxLineWidth) line.pop_back();
                    if (line.empty()) continue;
                    const float x0 = -static_cast<float>(m_font.GetStringWidth(line)) / 2.0f;
                    const float y0 = static_cast<float>(i * lineHeight - signMidpoint);

                    auto emit = [&](std::vector<CubeVert>& verts, std::vector<uint32_t>& idx,
                                    float dx, float dy, uint32_t colour) {
                        TextCommand cmd;
                        cmd.text = line;
                        cmd.x = x0 + dx;
                        cmd.y = y0 + dy;
                        cmd.color = colour;
                        cmd.dropShadow = false;
                        const size_t first = verts.size();
                        m_font.GenerateQuadsTyped<CubeVert>(cmd, verts, idx);
                        for (size_t v = first; v < verts.size(); ++v) {
                            const glm::vec4 p = xform * glm::vec4(verts[v].x, verts[v].y, 0.0f, 1.0f);
                            verts[v].x = p.x; verts[v].y = p.y; verts[v].z = p.z;
                        }
                    };
                    if (outline) {
                        // Font.prepare8xTextOutline: the eight neighbours in
                        // the outline colour, one font pixel apart.
                        for (int ox = -1; ox <= 1; ++ox)
                            for (int oy = -1; oy <= 1; ++oy)
                                if (ox != 0 || oy != 0)
                                    emit(outlineVerts, outlineIdx,
                                         static_cast<float>(ox), static_cast<float>(oy), dark);
                    }
                    if (text.glowing) emit(glowVerts, glowIdx, 0.0f, 0.0f, textColor);
                    else              emit(textVerts, textIdx, 0.0f, 0.0f, textColor);
                }
            }

            if (textVerts.empty() && glowVerts.empty()) return;
            const bool ok =
                BuildLayer(cached.text, textVerts.data(), textVerts.size() * sizeof(CubeVert),
                           textIdx.data(), textIdx.size()) &&
                BuildLayer(cached.glowText, glowVerts.data(), glowVerts.size() * sizeof(CubeVert),
                           glowIdx.data(), glowIdx.size()) &&
                BuildLayer(cached.outline, outlineVerts.data(), outlineVerts.size() * sizeof(CubeVert),
                           outlineIdx.data(), outlineIdx.size());
            if (!ok) { DestroyCached(cached); return; }
            cached.built = true;
        }
        if (cached.text.mesh == INVALID_MESH && cached.glowText.mesh == INVALID_MESH) return;

        // Block-local → render space: the cell's corner minus the view origin
        // (RenderOrigin.hpp).
        const glm::mat4 model = glm::translate(glm::mat4(1.0f), Render::ToRender(glm::dvec3(pos)));
        const glm::mat4 mvp = proj * view * model;

        PipelineState s;
        s.depthTestEnabled  = true;
        s.depthWriteEnabled = true;
        s.blendEnabled      = false;
        s.cullMode          = CullMode::None;
        s.primitiveType     = PrimitiveType::Triangles;

        g_renderBackend->BindShader(m_shader);
        g_renderBackend->BindTexture(m_font.GetFontTexture(), 0);
        g_renderBackend->SetUniformMat4(m_shader, "uMVP", mvp);
        g_renderBackend->SetUniformFloat(m_shader, "uAlphaTest", 0.05f);
        // The frame's fog at this sign's place. Plain text takes the sign's
        // light (its cell's); glowing text and its outline MC draws at
        // LightCoordsUtil.FULL_BRIGHT (SignRenderer.submitSignText).
        const glm::vec3 plainLight = BlockEntityShader::LightAt(pos);
        const glm::vec3 glowLight  = EntityEnvironment::LightColor(
            Game::Lighting::LightCoords::kFullBright);
        BlockEntityShader::ApplyWorld(m_shader, model, cameraPos, pos);

        // Outline first, at the board's own depth (DisplayMode.NORMAL): the
        // text sits a hair in front of the board, so it needs no bias of
        // its own to win against the wood.
        if (cached.outline.mesh != INVALID_MESH) {
            s.depthBiasEnabled = false;
            g_renderBackend->SetPipelineState(s);
            BlockEntityShader::SetLight(m_shader, glowLight);
            g_renderBackend->DrawIndexed(cached.outline.mesh, cached.outline.indexCount);
        }

        // Then the glyphs, pushed towards the viewer (DisplayMode
        // .POLYGON_OFFSET — RenderPipelines.TEXT_POLYGON_OFFSET's depth bias)
        // so they sit over the outline where the two overlap.
        s.depthBiasEnabled  = true;
        s.depthBiasConstant = -1.0f;
        s.depthBiasSlope    = -1.0f;
        g_renderBackend->SetPipelineState(s);
        if (cached.text.mesh != INVALID_MESH) {
            BlockEntityShader::SetLight(m_shader, plainLight);
            g_renderBackend->DrawIndexed(cached.text.mesh, cached.text.indexCount);
        }
        if (cached.glowText.mesh != INVALID_MESH) {
            BlockEntityShader::SetLight(m_shader, glowLight);
            g_renderBackend->DrawIndexed(cached.glowText.mesh, cached.glowText.indexCount);
        }
        g_renderBackend->UnbindMesh();
    }

} // namespace Render
