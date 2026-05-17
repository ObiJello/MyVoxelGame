// File: src/client/renderer/viewmodel/PortalGunViewmodel.hpp
//
// First-person portal-gun viewmodel — renders the real Portal v_portalgun
// mesh (extracted via SourceIO from Portal-Root/) skinned to its
// armature and plays Source's keyframe animations 1:1 (no procedural
// shortcuts): `@idle` continuously, `@fire1` triggered on each shot.
//
// Drawn after the world pass, before bloom, only when the player is
// holding Items::PortalGun.
//
// Per-primitive material → texture mapping uses the material names
// extracted by the glTF loader, matching the PNGs converted from VTF.
// Hand primitives are filtered out at load time — Source plays their
// rig from the player's body animations which we don't drive here.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "../backend/RenderTypes.hpp"
#include "GltfLoader.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace Render {

    class PortalGunViewmodel {
    public:
        PortalGunViewmodel() = default;
        ~PortalGunViewmodel() { Shutdown(); }

        bool Initialize();
        void Shutdown();

        // Per-frame: advance animation clock, evaluate bones, render.
        void Render(float aspect, float dt);

        // Trigger Source's `@fire1` animation (15-frame, 0.625 s clip
        // from v_portalgun.mdl). Called from PlayerController on each
        // shot. The clip plays once then `@idle` resumes.
        void OnFire();

    private:
        struct DrawCall {
            MeshHandle    mesh        = INVALID_MESH;
            BufferHandle  vbo         = INVALID_BUFFER;
            BufferHandle  ibo         = INVALID_BUFFER;
            uint32_t      indexCount  = 0;
            TextureHandle texture     = INVALID_TEXTURE;
            int           skinIndex   = -1;     // -1 = unskinned static draw
            bool          isGlass     = false;
            bool          skip        = false;  // hidden (e.g. detached hands)
        };

        bool          m_initialized = false;
        ShaderHandle  m_shader      = INVALID_SHADER;
        std::vector<DrawCall> m_drawCalls;
        std::unordered_map<std::string, TextureHandle> m_textureCache;

        // CPU-side model: hierarchy + skins + animations + a working
        // copy of node TRS that animations mutate each frame.
        Gltf::Model               m_model;
        std::vector<Gltf::Node>   m_workingNodes;  // mutable copy of m_model.nodes

        // Animation clock — seconds into the currently-playing clip.
        float       m_animTime    = 0.0f;
        int         m_currentClip = -1;     // index into m_model.animations
        int         m_idleClip    = -1;
        int         m_fireClip    = -1;
        bool        m_loopCurrent = true;

        float       m_time        = 0.0f;   // total elapsed for sway/bob

        TextureHandle LoadTextureCached(const std::string& assetBasename);
        // Sample the current animation at m_animTime, mutating m_workingNodes.
        void SampleAnimation();
        // Recompute bone matrices for `skinIndex` into `out`.
        void ComputeBoneMatrices(int skinIndex, std::vector<glm::mat4>& out) const;
        // Compute a node's world matrix from working TRS, walking parents.
        glm::mat4 WorldMatrix(int nodeIndex) const;
        // Switch to a named animation clip.
        void PlayClip(int clipIndex, bool loop);
    };

    extern PortalGunViewmodel g_portalGunViewmodel;

} // namespace Render

#endif // ENABLE_PORTAL_GUN
