// File: src/client/renderer/entity/ModMobRender.cpp
//
// Render::ModMobs — the dispatcher MobRenderer calls (ModMobRender.hpp): each
// function asks the four modules in turn; the first that owns the type
// answers.
#include "client/renderer/entity/ModMobRender.hpp"

namespace Render::ModMobs {

    std::unique_ptr<EntityModel> CreateModel(Game::EntityTypeId type) {
        if (auto m = TwilightCreatureRender::CreateModel(type)) return m;
        if (auto m = AetherCreatureRender::CreateModel(type)) return m;
        if (auto m = TwilightHostileRender::CreateModel(type)) return m;
        return HushCreatureRender::CreateModel(type);
    }

    std::unique_ptr<EntityModel> CreateBabyModel(Game::EntityTypeId type) {
        if (auto m = TwilightCreatureRender::CreateBabyModel(type)) return m;
        if (auto m = AetherCreatureRender::CreateBabyModel(type)) return m;
        if (auto m = TwilightHostileRender::CreateBabyModel(type)) return m;
        return HushCreatureRender::CreateBabyModel(type);
    }

    std::string_view TexturePath(Game::EntityTypeId type) {
        std::string_view p = TwilightCreatureRender::TexturePath(type);
        if (p.empty()) p = AetherCreatureRender::TexturePath(type);
        if (p.empty()) p = TwilightHostileRender::TexturePath(type);
        if (p.empty()) p = HushCreatureRender::TexturePath(type);
        return p;
    }

    bool ExtractRenderState(const Game::Mob& mob, Game::EntityTypeId type,
                            float partialTick, EntityRenderState& state) {
        return TwilightCreatureRender::ExtractRenderState(mob, type, partialTick, state) ||
               AetherCreatureRender::ExtractRenderState(mob, type, partialTick, state) ||
               TwilightHostileRender::ExtractRenderState(mob, type, partialTick, state) ||
               HushCreatureRender::ExtractRenderState(mob, type, partialTick, state);
    }

    std::string_view InstanceTexturePath(const Game::Mob& mob, Game::EntityTypeId type,
                                         const EntityRenderState& state) {
        std::string_view p = TwilightCreatureRender::InstanceTexturePath(mob, type, state);
        if (p.empty()) p = AetherCreatureRender::InstanceTexturePath(mob, type, state);
        if (p.empty()) p = TwilightHostileRender::InstanceTexturePath(mob, type, state);
        if (p.empty()) p = HushCreatureRender::InstanceTexturePath(mob, type, state);
        return p;
    }

    int Layers(const Game::Mob& mob, Game::EntityTypeId type,
               const EntityRenderState& state, ModLayer* out) {
        if (int n = TwilightCreatureRender::Layers(mob, type, state, out)) return n;
        if (int n = AetherCreatureRender::Layers(mob, type, state, out)) return n;
        if (int n = TwilightHostileRender::Layers(mob, type, state, out)) return n;
        return HushCreatureRender::Layers(mob, type, state, out);
    }

    bool BodyTranslucent(Game::EntityTypeId type) {
        return TwilightCreatureRender::BodyTranslucent(type) ||
               AetherCreatureRender::BodyTranslucent(type) ||
               TwilightHostileRender::BodyTranslucent(type) ||
               HushCreatureRender::BodyTranslucent(type);
    }

    float FlipDegrees(Game::EntityTypeId type) {
        if (float f = TwilightCreatureRender::FlipDegrees(type); f != 0.0f) return f;
        if (float f = AetherCreatureRender::FlipDegrees(type); f != 0.0f) return f;
        if (float f = TwilightHostileRender::FlipDegrees(type); f != 0.0f) return f;
        return HushCreatureRender::FlipDegrees(type);
    }

    const char* HeldItem(const Game::Mob& mob, Game::EntityTypeId type,
                         const EntityRenderState& state) {
        if (const char* s = TwilightCreatureRender::HeldItem(mob, type, state)) return s;
        if (const char* s = AetherCreatureRender::HeldItem(mob, type, state)) return s;
        if (const char* s = TwilightHostileRender::HeldItem(mob, type, state)) return s;
        return HushCreatureRender::HeldItem(mob, type, state);
    }

} // namespace Render::ModMobs
