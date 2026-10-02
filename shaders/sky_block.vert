// File: shaders/sky_block.vert
//
// The sky block's window into the sky (Render::SkyBlockRenderer). Its faces
// are drawn DEPTH-ONLY (colour writes off) right after the view's sky and
// before its terrain: the colour buffer keeps the sky the sky pass just drew,
// and the depth they write keeps everything behind them from drawing over it.
// So this stage only places the face and honours the immersive / gun portal
// clip plane, exactly as the terrain does (terrain.vert).
#version 330 core

// The shared 24-byte block vertex (GetBlockVertexLayout); only the position
// is read. RENDER space (camera-relative, see RenderOrigin.hpp).
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec4 aColor;

uniform mat4 uMVP;
// Render-space clip plane for portal see-through views (ChunkRenderer::
// PortalClipPlane). vec4(0) = no clipping. GL_CLIP_DISTANCE0 is always
// enabled, so the distance must always be written.
uniform vec4 uPortalClipPlane;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    gl_ClipDistance[0] = (any(notEqual(uPortalClipPlane.xyz, vec3(0.0))))
        ? dot(uPortalClipPlane.xyz, aPos) + uPortalClipPlane.w
        : 1.0;
}
