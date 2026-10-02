// File: src/client/renderer/portal/PortalCrosshair.hpp
//
// Portal's "QuickInfo" HUD overlay — the iconic pair of side brackets
// that flank the crosshair when you're holding the portal gun, with a
// pulse on the side that fires whenever a portal lands. Source's exact
// behaviour lives in `Portal code/sp/src/game/client/portal/hud_quickinfo.cpp`
// (CHUDQuickInfo). The sprite atlas `portal_crosshairs.vtf` is preserved
// 1:1 (we converted it to PNG at build time via srctools); the atlas
// regions match the offsets in `Portal-Root/scripts/mod_textures.txt`.
//
// Drawn ONLY when the player is actively holding the portal gun
// (Items::PortalGun). The standard inverted-XOR crosshair stays under
// it, so removing the gun returns to normal MC-style aiming.
//
// Entirely gated on ENABLE_PORTAL_GUN. When the feature is off the
// translation unit compiles to nothing.

#pragma once

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "../backend/RenderTypes.hpp"
#include <cstdint>
#include <string>

namespace Render {

    class PortalCrosshair {
    public:
        PortalCrosshair() = default;
        ~PortalCrosshair() { Shutdown(); }

        // Load atlas + shader + quad mesh. Idempotent.
        bool Initialize();
        void Shutdown();

        // Render the bracket pair + last-placed indicators at screen
        // centre. `dt` advances the pulse fade. Caller is responsible
        // for only calling this when the player is actually holding
        // the portal gun.
        //
        // `heldGunId` is the held gun's PORTAL_GUN_INSTANCE_ID (0 = never
        // fired). Each bracket is filled while THAT gun's portal of its
        // colour is open, read every frame from the client's copy of the
        // server's pairs (ClientPortalManager) — never from a flag set by
        // whichever portal last landed, which lit the brackets for other
        // players' guns and outlived pairs closed while away.
        void Render(int windowWidth, int windowHeight,
                    int framebufferWidth, int framebufferHeight,
                    float dt, uint64_t heldGunId = 0);

    private:
        bool          m_initialized = false;
        ShaderHandle  m_shader      = INVALID_SHADER;
        TextureHandle m_atlas       = INVALID_TEXTURE;
        BufferHandle  m_vb          = INVALID_BUFFER;
        BufferHandle  m_ib          = INVALID_BUFFER;
        MeshHandle    m_mesh        = INVALID_MESH;
        int           m_atlasW = 0, m_atlasH = 0;  // for UV math
    };

    extern PortalCrosshair g_portalCrosshair;

} // namespace Render

#endif // ENABLE_PORTAL_GUN
