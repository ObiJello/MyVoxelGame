// File: src/client/renderer/entity/ModMobRender.hpp
//
// The render half of the Twilight Forest and Aether creatures
// (docs/mod-ports.md), kept out of MobRenderer.cpp. MobRenderer calls the
// Render::ModMobs functions at fixed points of its per-mob pipeline; each one
// asks the three mod modules in turn and the first that owns the type
// answers:
//
//   TwilightCreatureRender  (TwilightCreatureRender.cpp) — the TF animals and
//                           the TF biome-spawner creatures;
//   AetherCreatureRender    (AetherCreatureRender.cpp)   — every Aether mob;
//   TwilightHostileRender   (TwilightHostileRender.cpp)  — the TF landmark
//                           hostiles;
//   HushCreatureRender      (HushCreatureRender.cpp)     — the deep-Hush
//                           creatures and the Choir Mother (meshes from
//                           tools/hush_creature_meshes.py, not a mod).
//
// Every module implements the same eight functions (the MOD_RENDER_MODULE
// block below); a module answers "not mine" with a null model / an empty
// string / false / 0, and leaves `state` untouched for a type it does not
// own.
//
// The meshes are generated from the mods' own model classes
// (tools/gen_entity_models.py MOD_MODELS); the models here are each mod
// model's setupAnim over the generated parts by name. The render-state inputs
// are the existing EntityRenderState fields — each module documents which MC
// field it carries in which slot.
#pragma once

#include "client/renderer/entity/model/EntityModels.hpp"
#include "common/entity/GeneratedEntityTypes.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace Game { class Mob; }

namespace Render {

    // One extra render layer drawn after the body and its overlay model — MC
    // RenderLayer subclasses (saddles, eyes, wool, wings, glow). `model` null
    // means "the body model again" (the collar/eyes precedent: the same
    // posed geometry re-emitted with another sheet, which wins the depth test
    // as the later equal-depth draw); otherwise the named model, owned by the
    // module and posed from the same render state (AppendMob calls its
    // SetupAnim).
    struct ModLayer {
        EntityModel*     model = nullptr;
        std::string_view texture;
        uint8_t r = 255, g = 255, b = 255, a = 255;   // vertex tint (MC layer colour)
        bool blend = false;       // alpha-blended (MC entityTranslucent)
        bool noOverlay = false;   // MC OverlayTexture.NO_OVERLAY (eyes: no hurt flash)
        // An EMISSIVE render type (MC RenderTypes.eyes / an EyesLayer, the
        // Hush glow sheets): drawn without the light dim — still fogged —
        // and kept on an invisible body, as EyesLayer is. Any other layer
        // is a model copy that an invisible body takes with it.
        bool emissive = false;
    };

    inline constexpr int kMaxModLayers = 4;

#define MOD_RENDER_MODULE(NS)                                                              \
    namespace NS {                                                                         \
        /* The adult model, or null when the type is not this module's. */                 \
        std::unique_ptr<EntityModel> CreateModel(Game::EntityTypeId type);                 \
        /* MC AgeableMobRenderer's babyModel; null = the adult shrunk by kBabyScale. */    \
        std::unique_ptr<EntityModel> CreateBabyModel(Game::EntityTypeId type);             \
        /* The type's default sheet, "" when not this module's. */                         \
        std::string_view TexturePath(Game::EntityTypeId type);                             \
        /* MC <Mob>Renderer.extractRenderState + scale(): per-mob render inputs. */        \
        /* Returns true when the type is this module's. */                                 \
        bool ExtractRenderState(const Game::Mob& mob, Game::EntityTypeId type,             \
                                float partialTick, EntityRenderState& state);              \
        /* getTextureLocation for THIS instance (variants, charging faces); "" = the */    \
        /* default sheet. */                                                               \
        std::string_view InstanceTexturePath(const Game::Mob& mob, Game::EntityTypeId type,\
                                             const EntityRenderState& state);              \
        /* The render layers for this instance this frame; returns how many of `out` */    \
        /* were written (at most kMaxModLayers). */                                        \
        int Layers(const Game::Mob& mob, Game::EntityTypeId type,                          \
                   const EntityRenderState& state, ModLayer* out);                         \
        /* Body drawn alpha-blended (MC entityTranslucent renderType). */                  \
        bool BodyTranslucent(Game::EntityTypeId type);                                     \
        /* getFlipDegrees (90 unless the renderer overrides it); 0 = not mine. */          \
        float FlipDegrees(Game::EntityTypeId type);                                        \
        /* A held item drawn in the model's right hand (item sprite name), or null. */     \
        const char* HeldItem(const Game::Mob& mob, Game::EntityTypeId type,                \
                             const EntityRenderState& state);                              \
    }

    MOD_RENDER_MODULE(TwilightCreatureRender)
    MOD_RENDER_MODULE(AetherCreatureRender)
    MOD_RENDER_MODULE(TwilightHostileRender)
    MOD_RENDER_MODULE(HushCreatureRender)
    // The dispatcher MobRenderer calls — the first module that answers wins.
    MOD_RENDER_MODULE(ModMobs)

#undef MOD_RENDER_MODULE

} // namespace Render
