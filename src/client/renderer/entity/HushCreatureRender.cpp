// File: src/client/renderer/entity/HushCreatureRender.cpp
//
// Render::HushCreatureRender (ModMobRender.hpp) — the deep-Hush creatures
// and the Choir Mother (common/entity/mobs/HushCreatures.hpp). Their meshes
// are the Hush rows of GeneratedEntityModels (tools/hush_creature_meshes.py);
// the animations below are this engine's own, written against those part
// names. Sheets are in-house (tools/gen_hush_entity_textures.py).
//
// The bodies take the world's light like every mob (the Hush's dim night);
// the glow sheets are an eyes-style second pass (NO_OVERLAY, no hurt flash,
// EMISSIVE — ModLayer::emissive) over the luminous parts, so those parts
// stay bright in the dark, and the golem's angry state swaps in a brighter
// sheet.
//
// Render-state slots carrying these mobs' values (each noted at its model):
//   isAngry     — golem angry / moth circling a light / mimic revealed
//   isCharging  — the Choir Mother conducting
//   squish      — the Choir Mother's phase (1..3); the Unsung's (0..3)
//   attackTicksRemaining — the golem's swing clock
//   isAggressive — the mimic striking (arms up); isCrouching — its echo of
//                  a player's crouch (the synced pose)
// and for the Unsung (TheUnsung.hpp): isCharging = arms raised (the baton's
// wind-up, conducting, a phase's tell), tendrilAnimation = channelling,
// spikesAnimation = holding a stolen voice, tailAnimation = which voice,
// stunnedTicksRemaining = staggered, standScale = its rise out of the Heart
// (0..1), playingDeadFactor = its dissolve (0..1).
#include "client/renderer/entity/ModMobRender.hpp"
#include "client/renderer/entity/ModModelHelpers.hpp"

#include "common/entity/mobs/HushCreatures.hpp"
#include "common/entity/mobs/TheUnsung.hpp"
#include "common/world/level/AurelithQuest.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <memory>
#include <string>

namespace Render::HushCreatureRender {

    namespace {

        template <typename T>
        const T* MobAs(const Game::Mob& mob, Game::EntityTypeId type, Game::EntityTypeId id) {
            if (type != id) return nullptr;
            assert(dynamic_cast<const T*>(&mob) != nullptr);
            return static_cast<const T*>(&mob);
        }

        // MC Mth.triangleWave(t, period) — IronGolemModel's swing curve.
        float TriangleWave(float t, float period) {
            return (std::fabs(std::fmod(t, period) - period * 0.5f) - period * 0.25f) /
                   (period * 0.25f);
        }

        ModelPart* Need(ModelPart& root, const char* name) { return root.Find(name); }

        // ── Lumen moth: wings beat fast, the body bobs, the abdomen sways.
        class LumenMothModel : public GeneratedModel {
        public:
            LumenMothModel() : GeneratedModel("lumen_moth") {
                m_body = Need(m_root, "body");
                m_head = Need(m_root, "head");
                m_abdomen = Need(m_root, "abdomen");
                m_lw = Need(m_root, "left_wing");
                m_rw = Need(m_root, "right_wing");
                m_lhw = Need(m_root, "left_hind_wing");
                m_rhw = Need(m_root, "right_hind_wing");
            }
            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                const float t = s.ageInTicks + s.entityId * 3.7f;
                const float beat = std::sin(t * 1.6f) * 0.95f + 0.15f;
                if (m_lw)  m_lw->zRot  += beat;
                if (m_rw)  m_rw->zRot  -= beat;
                if (m_lhw) m_lhw->zRot += beat * 0.7f;
                if (m_rhw) m_rhw->zRot -= beat * 0.7f;
                if (m_body) m_body->y += std::sin(t * 0.25f) * 0.8f;
                if (m_abdomen) m_abdomen->xRot += std::sin(t * 0.18f) * 0.15f;
                if (m_head) {
                    m_head->xRot += s.xRot * kModDegToRad * 0.5f;
                    m_head->yRot += s.yRot * kModDegToRad * 0.5f;
                }
            }
        private:
            ModelPart *m_body, *m_head, *m_abdomen, *m_lw, *m_rw, *m_lhw, *m_rhw;
        };

        // ── Crystal golem: IronGolemModel.setupAnim's walk and two-arm
        // swing (the arms rise on attackTicksRemaining), the head looks.
        class CrystalGolemModel : public GeneratedModel {
        public:
            CrystalGolemModel() : GeneratedModel("crystal_golem") {
                m_head = Need(m_root, "head");
                m_ra = Need(m_root, "right_arm");
                m_la = Need(m_root, "left_arm");
                m_rl = Need(m_root, "right_leg");
                m_ll = Need(m_root, "left_leg");
            }
            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                if (m_head) {
                    m_head->yRot = s.yRot * kModDegToRad;
                    m_head->xRot = s.xRot * kModDegToRad;
                }
                const float pos = s.walkAnimationPos;
                const float spd = s.walkAnimationSpeed;
                if (m_rl) m_rl->xRot = -1.5f * TriangleWave(pos, 13.0f) * spd;
                if (m_ll) m_ll->xRot = 1.5f * TriangleWave(pos, 13.0f) * spd;
                if (!m_ra || !m_la) return;
                if (s.attackTicksRemaining > 0.0f) {
                    m_ra->xRot = -2.0f + 1.5f * TriangleWave(s.attackTicksRemaining, 10.0f);
                    m_la->xRot = -2.0f + 1.5f * TriangleWave(s.attackTicksRemaining, 10.0f);
                } else {
                    m_ra->xRot = (-0.2f + 1.5f * TriangleWave(pos, 13.0f)) * spd;
                    m_la->xRot = (-0.2f - 1.5f * TriangleWave(pos, 13.0f)) * spd;
                }
                // An angry golem hunches its arms outward.
                if (s.isAngry) {
                    m_ra->zRot += 0.12f;
                    m_la->zRot -= 0.12f;
                }
            }
        private:
            ModelPart *m_head, *m_ra, *m_la, *m_rl, *m_ll;
        };

        // ── Hush leviathan: a travelling wave down the tail (pitch), the
        // fluke and fins beating slowly, the jaw-less head nodding.
        class HushLeviathanModel : public GeneratedModel {
        public:
            HushLeviathanModel() : GeneratedModel("hush_leviathan") {
                m_t1 = Need(m_root, "tail1");
                m_t2 = Need(m_root, "tail2");
                m_t3 = Need(m_root, "tail3");
                m_fluke = Need(m_root, "fluke");
                m_lf = Need(m_root, "left_fin");
                m_rf = Need(m_root, "right_fin");
                m_head = Need(m_root, "head");
            }
            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                const float t = s.ageInTicks * 0.06f + s.entityId;
                // A slow undulation: pitch waves down the tail (a whale's
                // stroke is up-and-down), each segment a step behind.
                if (m_t1) m_t1->xRot += std::sin(t) * 0.10f;
                if (m_t2) m_t2->xRot += std::sin(t - 0.8f) * 0.16f;
                if (m_t3) m_t3->xRot += std::sin(t - 1.6f) * 0.22f;
                if (m_fluke) m_fluke->xRot += std::sin(t - 2.4f) * 0.30f;
                const float fin = std::sin(t * 0.8f) * 0.25f;
                if (m_lf) m_lf->zRot += fin;
                if (m_rf) m_rf->zRot -= fin;
                if (m_head) m_head->xRot += std::sin(t + 1.0f) * 0.03f;
            }
        private:
            ModelPart *m_t1, *m_t2, *m_t3, *m_fluke, *m_lf, *m_rf, *m_head;
        };

        // ── Echo mimic: a player's body, two seconds late. It replays the
        // player's look, walk, swings and crouch, so it is posed by MC
        // HumanoidModel.setupAnim in full — ModHumanoidModel's look, walk and
        // arm bob, plus setupAttackAnimation (the right-handed WHACK) and the
        // crouch — with the arms reaching forward only while it strikes, and
        // the wisp trailing.
        class EchoMimicModel : public ModHumanoidModel {
        public:
            EchoMimicModel() : ModHumanoidModel("echo_mimic", false) {
                m_headPart = Need(m_root, "head");
                m_bodyPart = Need(m_root, "body");
                m_ra = Need(m_root, "right_arm");
                m_la = Need(m_root, "left_arm");
                m_rl = Need(m_root, "right_leg");
                m_ll = Need(m_root, "left_leg");
                m_wisp = Need(m_root, "wisp");
            }
            void SetupAnim(const EntityRenderState& s) override {
                ModHumanoidModel::SetupAnim(s);
                if (s.isAggressive && m_ra && m_la) {
                    // Striking: both arms reach for you, like a zombie's
                    // (AnimationUtils.animateZombieArms, without the swing).
                    m_ra->xRot = -1.4f + std::sin(s.ageInTicks * 0.2f) * 0.1f;
                    m_la->xRot = -1.4f - std::sin(s.ageInTicks * 0.2f) * 0.1f;
                    if (s.attackTime > 0.0f) {
                        const float a = std::sin(s.attackTime * kModPi);
                        m_ra->xRot -= a * 0.8f;
                        m_la->xRot -= a * 0.8f;
                    }
                } else if (s.attackTime > 0.0f && m_ra && m_la && m_bodyPart) {
                    SetupAttackAnimation(s);
                }
                if (s.isCrouching && m_ra && m_la && m_rl && m_ll && m_bodyPart && m_headPart) {
                    // MC HumanoidModel.setupAnim's crouch, after the swing.
                    m_bodyPart->xRot = 0.5f;
                    m_ra->xRot += 0.4f;
                    m_la->xRot += 0.4f;
                    m_rl->z += 4.0f;
                    m_ll->z += 4.0f;
                    m_headPart->y += 4.2f;
                    m_bodyPart->y += 3.2f;
                    m_la->y += 3.2f;
                    m_ra->y += 3.2f;
                }
                if (m_wisp) {
                    m_wisp->xRot += s.walkAnimationSpeed * 0.6f +
                                    std::sin(s.ageInTicks * 0.15f) * 0.12f;
                }
            }
        private:
            // MC HumanoidModel.setupAttackAnimation for a right-handed WHACK
            // (the empty hand's SwingAnimationType): the body twists on a
            // sine of the square root of progress and the arms slide round
            // with it, then the right arm chops (Ease.outQuart) down past the
            // look pitch. The arm bob ModHumanoidModel already added is
            // additive, so the order against it does not matter.
            void SetupAttackAnimation(const EntityRenderState& s) {
                const float attack = s.attackTime;
                m_bodyPart->yRot = std::sin(std::sqrt(attack) * kModPi * 2.0f) * 0.2f;
                m_ra->z =  std::sin(m_bodyPart->yRot) * 5.0f * s.ageScale;
                m_ra->x = -std::cos(m_bodyPart->yRot) * 5.0f * s.ageScale;
                m_la->z = -std::sin(m_bodyPart->yRot) * 5.0f * s.ageScale;
                m_la->x =  std::cos(m_bodyPart->yRot) * 5.0f * s.ageScale;
                m_ra->yRot += m_bodyPart->yRot;
                m_la->yRot += m_bodyPart->yRot;
                m_la->xRot += m_bodyPart->yRot;
                const float inv = 1.0f - attack;
                const float eased = 1.0f - inv * inv * inv * inv;
                const float aa = std::sin(eased * kModPi);
                const float headXRot = s.xRot * kModDegToRad;
                const float bb = std::sin(attack * kModPi) * -(headXRot - 0.7f) * 0.75f;
                m_ra->xRot -= aa * 1.2f + bb;
                m_ra->yRot += m_bodyPart->yRot * 2.0f;
                m_ra->zRot += std::sin(attack * kModPi) * -0.4f;
            }

            ModelPart *m_headPart, *m_bodyPart, *m_ra, *m_la, *m_rl, *m_ll, *m_wisp;
        };

        // ── The Choir Mother: tendrils and veil ripple, the horns sway, the
        // arms conduct (raised and beating time) while summoning; the head
        // looks.
        class ChoirMotherModel : public GeneratedModel {
        public:
            ChoirMotherModel() : GeneratedModel("choir_mother") {
                m_head = Need(m_root, "head");
                m_la = Need(m_root, "left_arm");
                m_ra = Need(m_root, "right_arm");
                m_lfa = Need(m_root, "left_forearm");
                m_rfa = Need(m_root, "right_forearm");
                m_lh = Need(m_root, "left_horn");
                m_rh = Need(m_root, "right_horn");
                for (const char* n : { "veil_front", "veil_back", "veil_left", "veil_right" }) {
                    m_veil.push_back(Need(m_root, n));
                }
                for (int i = 0; i < 6; ++i) {
                    m_tendrils.push_back(Need(m_root, ("tendril_" + std::to_string(i)).c_str()));
                    m_tips.push_back(Need(m_root, ("tendril_" + std::to_string(i) + "_tip").c_str()));
                }
            }
            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                const float t = s.ageInTicks;
                // Faster, wilder motion as the fight goes on (phase in squish).
                const float agitation = 1.0f + 0.35f * (s.squish - 1.0f);
                if (m_head) {
                    m_head->yRot = s.yRot * kModDegToRad;
                    m_head->xRot = s.xRot * kModDegToRad;
                }
                const float horn = std::sin(t * 0.08f) * 0.08f;
                if (m_lh) m_lh->zRot -= horn;
                if (m_rh) m_rh->zRot += horn;
                for (size_t i = 0; i < m_veil.size(); ++i) {
                    ModelPart* v = m_veil[i];
                    if (!v) continue;
                    const float w = std::sin(t * 0.12f * agitation + i * 1.7f) * 0.07f;
                    if (i < 2) v->xRot += (i == 0 ? -w : w);
                    else       v->zRot += (i == 2 ? -w : w);
                }
                for (size_t i = 0; i < m_tendrils.size(); ++i) {
                    const float ph = t * 0.1f * agitation + i * 1.05f;
                    if (ModelPart* p = m_tendrils[i]) {
                        p->xRot += std::sin(ph) * 0.25f;
                        p->zRot += std::cos(ph * 0.8f) * 0.2f;
                    }
                    if (ModelPart* p = m_tips[i]) p->xRot += std::sin(ph - 0.9f) * 0.35f;
                }
                if (!m_la || !m_ra) return;
                if (s.isCharging) {
                    // Conducting: arms up and out, beating time.
                    const float beat = std::sin(t * 0.5f) * 0.35f;
                    m_la->xRot = -2.3f + beat;
                    m_ra->xRot = -2.3f - beat;
                    m_la->zRot -= 0.4f;
                    m_ra->zRot += 0.4f;
                    if (m_lfa) m_lfa->xRot = -0.5f - beat;
                    if (m_rfa) m_rfa->xRot = -0.5f + beat;
                } else {
                    const float drift = std::sin(t * 0.06f) * 0.15f;
                    m_la->xRot = -0.2f + drift;
                    m_ra->xRot = -0.2f - drift;
                    if (m_lfa) m_lfa->xRot = -0.3f;
                    if (m_rfa) m_rfa->xRot = -0.3f;
                }
            }
        private:
            ModelPart *m_head, *m_la, *m_ra, *m_lfa, *m_rfa, *m_lh, *m_rh;
            std::vector<ModelPart*> m_veil, m_tendrils, m_tips;
        };

        // ── The Unsung: the conductor it learned to be. Idle, it hangs with
        // the baton held forward and its robes breathing; raised (the
        // wind-up, conducting, a phase's tell) both arms go up and beat
        // time; channelling, it spreads its arms to the rings with its head
        // thrown back; staggered, it slumps. The four voice shards orbit
        // its shoulders — a stolen one swings wide and blazes, all four
        // draw in tight while it drinks the Chord.
        class TheUnsungModel : public GeneratedModel {
        public:
            TheUnsungModel() : GeneratedModel("the_unsung") {
                m_head = Need(m_root, "head");
                m_body = Need(m_root, "body");
                m_la = Need(m_root, "left_arm");
                m_ra = Need(m_root, "right_arm");
                m_lfa = Need(m_root, "left_forearm");
                m_rfa = Need(m_root, "right_forearm");
                m_baton = Need(m_root, "baton");
                m_cl = Need(m_root, "crest_left");
                m_cr = Need(m_root, "crest_right");
                for (const char* n : { "robe_front", "robe_back", "robe_left", "robe_right",
                                       "mantle_front", "mantle_back", "mantle_left", "mantle_right" }) {
                    m_robe.push_back(Need(m_root, n));
                }
                for (int i = 0; i < 5; ++i) {
                    m_tendrils.push_back(Need(m_root, ("tendril_" + std::to_string(i)).c_str()));
                    m_tips.push_back(Need(m_root, ("tendril_" + std::to_string(i) + "_tip").c_str()));
                }
                for (int i = 0; i < 4; ++i) m_shards[i] = Need(m_root, ("shard_" + std::to_string(i)).c_str());
            }
            void SetupAnim(const EntityRenderState& s) override {
                m_root.ResetPose();
                const float t = s.ageInTicks;
                const float phase = s.squish;
                const bool raised = s.isCharging;
                const bool channel = s.tendrilAnimation > 0.5f;
                const bool stagger = s.stunnedTicksRemaining > 0.5f;
                const float rise = std::clamp(s.standScale, 0.0f, 1.0f);
                const float agitation = 1.0f + 0.3f * std::max(0.0f, phase - 1.0f);

                if (m_head) {
                    m_head->yRot = s.yRot * kModDegToRad;
                    m_head->xRot = s.xRot * kModDegToRad;
                    if (channel) m_head->xRot = -0.55f;
                    if (stagger) m_head->xRot = 0.45f + std::sin(t * 0.9f) * 0.08f;
                }
                if (m_body && stagger) m_body->xRot += 0.22f;
                const float crest = std::sin(t * 0.07f * agitation) * 0.1f;
                if (m_cl) m_cl->xRot += crest;
                if (m_cr) m_cr->xRot -= crest;

                for (size_t i = 0; i < m_robe.size(); ++i) {
                    ModelPart* p = m_robe[i];
                    if (!p) continue;
                    const float w = std::sin(t * 0.09f * agitation + i * 1.3f) * (i < 4 ? 0.05f : 0.08f);
                    if (i % 4 < 2) p->xRot += (i % 4 == 0 ? -w : w);
                    else           p->zRot += (i % 4 == 2 ? -w : w);
                }
                for (size_t i = 0; i < m_tendrils.size(); ++i) {
                    const float ph = t * 0.11f * agitation + i * 1.25f;
                    if (ModelPart* p = m_tendrils[i]) {
                        p->xRot += std::sin(ph) * 0.3f;
                        p->zRot += std::cos(ph * 0.7f) * 0.25f;
                    }
                    if (ModelPart* p = m_tips[i]) p->xRot += std::sin(ph - 0.8f) * 0.4f;
                }

                if (m_la && m_ra) {
                    if (rise < 1.0f) {
                        // Rising out of the Heart: hauling itself up, arms high.
                        m_la->xRot = -2.9f * (1.0f - rise * 0.4f);
                        m_ra->xRot = -2.9f * (1.0f - rise * 0.4f);
                        m_la->zRot -= 0.5f;
                        m_ra->zRot += 0.5f;
                    } else if (channel) {
                        const float w = std::sin(t * 0.3f) * 0.12f;
                        m_la->xRot = -2.6f + w;
                        m_ra->xRot = -2.6f - w;
                        m_la->zRot -= 1.0f;
                        m_ra->zRot += 1.0f;
                        if (m_lfa) m_lfa->xRot = -0.3f;
                        if (m_rfa) m_rfa->xRot = -0.3f;
                    } else if (stagger) {
                        m_la->xRot = 0.3f;
                        m_ra->xRot = 0.3f;
                        m_la->zRot -= 0.05f;
                        m_ra->zRot += 0.05f;
                    } else if (raised) {
                        // Conducting: the baton hand high and beating time,
                        // the other hand held up and open.
                        const float beat = std::sin(t * 0.55f * agitation);
                        m_ra->xRot = -2.5f + beat * 0.45f;
                        m_ra->zRot += 0.25f;
                        m_la->xRot = -1.9f - beat * 0.2f;
                        m_la->zRot -= 0.55f;
                        if (m_rfa) m_rfa->xRot = -0.6f - beat * 0.3f;
                        if (m_lfa) m_lfa->xRot = -0.4f;
                    } else {
                        const float drift = std::sin(t * 0.05f) * 0.12f;
                        m_la->xRot = -0.25f + drift;
                        m_ra->xRot = -0.55f - drift;       // the baton held out
                        if (m_rfa) m_rfa->xRot = -0.5f;
                        if (m_lfa) m_lfa->xRot = -0.2f;
                    }
                }
                if (m_baton && raised && !channel) m_baton->xRot += std::sin(t * 0.55f * agitation) * 0.4f;

                // The voices it learned.
                const int stolen = s.spikesAnimation > 0.5f ? static_cast<int>(s.tailAnimation + 0.5f) : -1;
                const float speed = (channel ? 0.22f : 0.035f + 0.01f * phase) * (stagger ? 0.4f : 1.0f);
                for (int i = 0; i < 4; ++i) {
                    ModelPart* p = m_shards[i];
                    if (!p) continue;
                    float radius = channel ? 5.0f : 10.5f;
                    if (i == stolen) radius = 14.0f + std::sin(t * 0.2f) * 1.0f;
                    radius *= rise;
                    const float a = t * speed + i * 1.5707964f;
                    p->x = std::cos(a) * radius;
                    p->z = std::sin(a) * radius;
                    p->y = -4.0f + std::sin(t * 0.08f + i * 1.7f) * 2.0f + (stagger ? 5.0f : 0.0f);
                    p->yRot += t * (i == stolen ? 0.25f : 0.06f);
                    const float scale = i == stolen ? 1.5f : 1.0f;
                    p->xScale = p->yScale = p->zScale = scale;
                }
            }
        private:
            ModelPart *m_head, *m_body, *m_la, *m_ra, *m_lfa, *m_rfa, *m_baton, *m_cl, *m_cr;
            std::vector<ModelPart*> m_robe, m_tendrils, m_tips;
            ModelPart* m_shards[4] = {};
        };

        bool IsHushCreature(Game::EntityTypeId type) {
            switch (type) {
                case Game::EntityTypeId::LumenMoth:
                case Game::EntityTypeId::CrystalGolem:
                case Game::EntityTypeId::HushLeviathan:
                case Game::EntityTypeId::EchoMimic:
                case Game::EntityTypeId::ChoirMother:
                case Game::EntityTypeId::TheUnsung:
                    return true;
                default:
                    return false;
            }
        }

    } // namespace

    std::unique_ptr<EntityModel> CreateModel(Game::EntityTypeId type) {
        switch (type) {
            case Game::EntityTypeId::LumenMoth:
                if (FindGenModel("lumen_moth")) return std::make_unique<LumenMothModel>();
                return nullptr;
            case Game::EntityTypeId::CrystalGolem:
                if (FindGenModel("crystal_golem")) return std::make_unique<CrystalGolemModel>();
                return nullptr;
            case Game::EntityTypeId::HushLeviathan:
                if (FindGenModel("hush_leviathan")) return std::make_unique<HushLeviathanModel>();
                return nullptr;
            case Game::EntityTypeId::EchoMimic:
                if (FindGenModel("echo_mimic")) return std::make_unique<EchoMimicModel>();
                return nullptr;
            case Game::EntityTypeId::ChoirMother:
                if (FindGenModel("choir_mother")) return std::make_unique<ChoirMotherModel>();
                return nullptr;
            case Game::EntityTypeId::TheUnsung:
                if (FindGenModel("the_unsung")) return std::make_unique<TheUnsungModel>();
                return nullptr;
            default:
                return nullptr;
        }
    }

    std::unique_ptr<EntityModel> CreateBabyModel(Game::EntityTypeId type) {
        (void)type;   // none of them has a baby
        return nullptr;
    }

    std::string_view TexturePath(Game::EntityTypeId type) {
        switch (type) {
            case Game::EntityTypeId::LumenMoth:     return "assets/textures/entity/hush/lumen_moth.png";
            case Game::EntityTypeId::CrystalGolem:  return "assets/textures/entity/hush/crystal_golem.png";
            case Game::EntityTypeId::HushLeviathan: return "assets/textures/entity/hush/hush_leviathan.png";
            case Game::EntityTypeId::EchoMimic:     return "assets/textures/entity/hush/echo_mimic_shimmer.png";
            case Game::EntityTypeId::ChoirMother:   return "assets/textures/entity/hush/choir_mother.png";
            case Game::EntityTypeId::TheUnsung:     return "assets/textures/entity/hush/the_unsung.png";
            default:                                return {};
        }
    }

    bool ExtractRenderState(const Game::Mob& mob, Game::EntityTypeId type,
                            float partialTick, EntityRenderState& state) {
        if (const auto* moth = MobAs<Game::LumenMoth>(mob, type, Game::EntityTypeId::LumenMoth)) {
            state.isAngry = moth->IsCirclingLight();
            return true;
        }
        if (const auto* golem = MobAs<Game::CrystalGolem>(mob, type, Game::EntityTypeId::CrystalGolem)) {
            state.isAngry = golem->IsAngryVisible();
            const int tick = golem->GetAttackAnimationTick();
            state.attackTicksRemaining = tick > 0 ? static_cast<float>(tick) - partialTick : 0.0f;
            return true;
        }
        if (type == Game::EntityTypeId::HushLeviathan) {
            // Drawn at twice the mesh (6.4 x 3 blocks), pitched by its
            // climb about the middle of the body.
            state.modelScale = glm::vec3(2.0f);
            state.swimPitchDeg = state.xRot;
            state.swimPivotY = 1.5f;
            return true;
        }
        if (const auto* mimic = MobAs<Game::EchoMimic>(mob, type, Game::EntityTypeId::EchoMimic)) {
            state.isAngry = mimic->IsRevealed();
            // The echo of a player's crouch (the synced pose): MC
            // HumanoidMobRenderer's isCrouching, and AvatarRenderer's
            // getRenderOffset — a crouching player is drawn 2 px lower (+Y is
            // down in the flipped model frame the offset applies in).
            state.isCrouching = mob.GetPose() == Game::Pose::Crouching;
            if (state.isCrouching) state.modelOffset.y += 2.0f / 16.0f;
            return true;
        }
        if (const auto* mother = MobAs<Game::ChoirMother>(mob, type, Game::EntityTypeId::ChoirMother)) {
            state.modelScale = glm::vec3(1.5f);
            state.isCharging = mother->IsConducting();
            state.squish = static_cast<float>(std::max(1, mother->SyncedPhase()));
            return true;
        }
        if (const auto* unsung = MobAs<Game::TheUnsung>(mob, type, Game::EntityTypeId::TheUnsung)) {
            state.modelScale = glm::vec3(1.6f);
            state.squish = static_cast<float>(unsung->SyncedPhase());
            state.isCharging = unsung->IsWindingUp();
            state.tendrilAnimation = unsung->IsChannelling() ? 1.0f : 0.0f;
            state.spikesAnimation = unsung->IsShielded() ? 1.0f : 0.0f;
            state.tailAnimation = static_cast<float>(unsung->StolenVoice());
            state.stunnedTicksRemaining = unsung->IsStaggered() ? 1.0f : 0.0f;
            // Its rise out of the Heart: the arms haul and the shards spiral
            // out over the first two seconds this client saw it.
            state.standScale = unsung->IsEmerging()
                ? std::min(1.0f, (static_cast<float>(unsung->ClientEmergeAge()) + partialTick) / 60.0f)
                : std::min(1.0f, 0.5f + (static_cast<float>(unsung->ClientEmergeAge()) + partialTick) / 40.0f);
            state.playingDeadFactor = std::clamp(
                (static_cast<float>(mob.deathTime) + partialTick) / static_cast<float>(Game::TheUnsung::kDeathTicks),
                0.0f, 1.0f) * (mob.deathTime > 0 ? 1.0f : 0.0f);
            return true;
        }
        return false;
    }

    std::string_view InstanceTexturePath(const Game::Mob& mob, Game::EntityTypeId type,
                                         const EntityRenderState& state) {
        (void)mob;
        if (type == Game::EntityTypeId::CrystalGolem && state.isAngry) {
            return "assets/textures/entity/hush/crystal_golem_angry.png";
        }
        // Revealed, the mimic is a near-solid figure; otherwise a shimmer.
        if (type == Game::EntityTypeId::EchoMimic && state.isAngry) {
            return "assets/textures/entity/hush/echo_mimic.png";
        }
        return {};
    }

    int Layers(const Game::Mob& mob, Game::EntityTypeId type,
               const EntityRenderState& state, ModLayer* out) {
        (void)mob;
        switch (type) {
            case Game::EntityTypeId::LumenMoth:
                // The luminous abdomen, eyes and eyespots; brighter (a second,
                // blended pass) while it circles a light.
                out[0].texture = "assets/textures/entity/hush/lumen_moth_glow.png";
                out[0].noOverlay = true;
                out[0].emissive = true;   // a glow sheet: no light dim
                if (!state.isAngry) return 1;
                out[1].texture = "assets/textures/entity/hush/lumen_moth_glow.png";
                out[1].noOverlay = true;
                out[1].emissive = true;   // a glow sheet: no light dim
                out[1].blend = true;
                out[1].a = static_cast<uint8_t>(150 + 90 * std::sin(state.ageInTicks * 0.3f));
                return 2;
            case Game::EntityTypeId::CrystalGolem:
                // Crystals, eyes and core — drawn only while angry, over the
                // brighter sheet: the "lit up" golem.
                if (!state.isAngry) return 0;
                out[0].texture = "assets/textures/entity/hush/crystal_golem_glow.png";
                out[0].noOverlay = true;
                out[0].emissive = true;   // a glow sheet: no light dim
                out[0].r = 230; out[0].g = 220; out[0].b = 255;
                return 1;
            case Game::EntityTypeId::HushLeviathan:
                out[0].texture = "assets/textures/entity/hush/hush_leviathan_glow.png";
                out[0].noOverlay = true;
                out[0].emissive = true;   // a glow sheet: no light dim
                return 1;
            case Game::EntityTypeId::EchoMimic:
                // The shimmer: the faint sheet again, its alpha pulsing.
                out[0].texture = "assets/textures/entity/hush/echo_mimic_shimmer.png";
                out[0].blend = true;
                out[0].noOverlay = true;
                out[0].a = static_cast<uint8_t>(120 + 100 * std::sin(state.ageInTicks * 0.25f));
                return 1;
            case Game::EntityTypeId::ChoirMother:
                out[0].texture = "assets/textures/entity/hush/choir_mother_glow.png";
                out[0].noOverlay = true;
                out[0].emissive = true;   // a glow sheet: no light dim
                return 1;
            case Game::EntityTypeId::TheUnsung: {
                // The throat, the crest, the fingers, the shards — always lit.
                out[0].texture = "assets/textures/entity/hush/the_unsung_glow.png";
                out[0].noOverlay = true;
                out[0].emissive = true;   // a glow sheet: no light dim
                // The aura: a film over the whole figure, coloured by what it
                // is doing — the stolen voice's colour while it hides behind
                // it, white flickers while it reels, dark violet while it
                // drinks the Chord, a brightening white as it is sung to rest.
                const auto* unsung = MobAs<Game::TheUnsung>(mob, type, Game::EntityTypeId::TheUnsung);
                if (!unsung) return 1;
                const float t = state.ageInTicks;
                ModLayer& aura = out[1];
                aura.texture = "assets/textures/entity/hush/the_unsung_aura.png";
                aura.noOverlay = true;
                aura.emissive = true;
                aura.blend = true;
                if (state.playingDeadFactor > 0.0f) {
                    aura.r = aura.g = aura.b = 255;
                    aura.a = static_cast<uint8_t>(std::clamp(40.0f + 215.0f * state.playingDeadFactor, 0.0f, 255.0f));
                    return 2;
                }
                if (unsung->IsChannelling()) {
                    aura.r = 90; aura.g = 40; aura.b = 170;
                    aura.a = static_cast<uint8_t>(150 + 60 * std::sin(t * 0.5f));
                    return 2;
                }
                if (unsung->IsStaggered()) {
                    aura.r = aura.g = aura.b = 240;
                    aura.a = static_cast<uint8_t>(60 + 60 * std::fabs(std::sin(t * 1.3f)));
                    return 2;
                }
                if (unsung->IsShielded()) {
                    const uint32_t c = Game::Aurelith::VoiceColour(
                        static_cast<Game::Aurelith::Voice>(unsung->StolenVoice() & 3));
                    aura.r = static_cast<uint8_t>((c >> 16) & 0xFF);
                    aura.g = static_cast<uint8_t>((c >> 8) & 0xFF);
                    aura.b = static_cast<uint8_t>(c & 0xFF);
                    aura.a = static_cast<uint8_t>(90 + 50 * std::sin(t * 0.25f));
                    return 2;
                }
                if (unsung->IsEmerging()) {
                    aura.r = 60; aura.g = 30; aura.b = 110;
                    aura.a = 170;
                    return 2;
                }
                return 1;
            }
            default:
                return 0;
        }
    }

    bool BodyTranslucent(Game::EntityTypeId type) {
        // The mimic is an after-image: its body is blended.
        return type == Game::EntityTypeId::EchoMimic;
    }

    float FlipDegrees(Game::EntityTypeId type) {
        (void)type;
        // The Unsung does not topple (MobRenderer keeps it upright); every
        // other Hush creature falls onto its side.
        if (type == Game::EntityTypeId::TheUnsung) return 0.0f;
        return IsHushCreature(type) ? 90.0f : 0.0f;
    }

    const char* HeldItem(const Game::Mob& mob, Game::EntityTypeId type,
                         const EntityRenderState& state) {
        (void)mob; (void)type; (void)state;
        return nullptr;
    }

} // namespace Render::HushCreatureRender
