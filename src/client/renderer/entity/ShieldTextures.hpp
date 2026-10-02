// File: src/client/renderer/entity/ShieldTextures.hpp
//
// The sheet a shield stack's ShieldModel draws with (MC ShieldSpecialRenderer):
// shield_base_nopattern for a plain shield; with BANNER_PATTERNS or BASE_COLOR,
// shield_base under BannerRenderer.submitPatterns' layers — the base pattern
// (entity/shield/base) in the base colour (white when absent), then up to 16
// pattern layers, each its entity/shield/<asset> sheet tinted with the
// layer's DyeColor.getTextureDiffuseColor. MC draws each layer as another
// pass of the whole model with the same UVs, alpha-blended; composing the
// layers into one sheet in texture space is the same picture. Cached per
// distinct look; dropped on a resource pack change.
#pragma once

#include "client/renderer/backend/RenderTypes.hpp"

namespace Game { struct ItemStack; }

namespace Render::ShieldTextures {

    TextureHandle ForStack(const Game::ItemStack& stack);

} // namespace Render::ShieldTextures
