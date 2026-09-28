// File: src/client/renderer/gui/MountInventoryScreen.cpp
//
// See MountInventoryScreen.hpp. MC sources: AbstractMountInventoryScreen,
// HorseInventoryScreen, NautilusInventoryScreen and
// ClientPacketListener.handleMountScreenOpen.
#include "MountInventoryScreen.hpp"

#include "GuiGraphics.hpp"
#include "GuiRenderState.hpp"
#include "screens/Screen.hpp"          // LoadStandaloneGuiTexture
#include "client/entity/ClientMobManager.hpp"
#include "client/renderer/entity/MobRenderer.hpp"
#include "common/entity/Mob.hpp"
#include "common/world/damagesource/CombatTracker.hpp"   // EntityDisplayName

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace Render {

    namespace {

        // The mount the last MountScreenOpenS2C named (MC keeps the entity
        // on the screen; this side re-finds it by id).
        struct RequestedMount {
            bool    valid = false;
            int     inventoryColumns = 0;
            int32_t entityId = 0;
        };
        RequestedMount s_requested;

        // The client's copy of the mount, or null once it is gone.
        Game::Mob* FindClientMount(int32_t entityId) {
            if (!Client::g_clientMobManager) return nullptr;
            const Client::ClientMob* entry = Client::g_clientMobManager->GetMob(entityId);
            return entry && entry->mob ? entry->mob.get() : nullptr;
        }

        // MC InventoryScreen.extractEntityInInventoryFollowsMouse + the
        // GuiEntityRenderer picture-in-picture it hands the state to: the
        // entity turned toward the cursor (body 20°, head a further 20° per
        // radian of atan(offset / 40)), centred in the box on half its
        // height plus offsetY, scaled `size` GUI pixels a block, lit by
        // ENTITY_IN_UI and clipped to the box. MC draws into an off-screen
        // texture it blits; here the posed triangles go straight into the
        // GUI with depth — the same picture.
        void RenderEntityInInventoryFollowsMouse(GuiGraphics& g, int x0, int y0, int x1, int y1, int size,
                                                 float offsetY, float mouseX, float mouseY,
                                                 const Game::Mob& entity) {
            MobRenderer* renderer = MobRenderer::Instance();
            GuiRenderState* rs = g.GetRenderState();
            if (!renderer || !rs || !Client::g_clientMobManager) return;

            const float centerX = static_cast<float>(x0 + x1) / 2.0f;
            const float centerY = static_cast<float>(y0 + y1) / 2.0f;
            const float xAngle = std::atan((centerX - mouseX) / 40.0f);
            const float yAngle = std::atan((centerY - mouseY) / 40.0f);

            // rotation = rotateZ(PI) · rotateX(yAngle · 20°).
            glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), 3.1415927f, glm::vec3(0.0f, 0.0f, 1.0f));
            rotation = glm::rotate(rotation, yAngle * 20.0f * 0.017453292f, glm::vec3(1.0f, 0.0f, 0.0f));

            // Lighting.ENTITY_IN_UI, in the box's space (GUI y down, +z
            // toward the viewer); a render-space normal reaches that space
            // through scale(1, 1, -1) · rotation (PoseStack's normal matrix
            // for scale(s, s, -s)), so the lights go back through its
            // transpose.
            static const glm::vec3 kInventoryLight0 = glm::normalize(glm::vec3( 0.2f, -1.0f, 1.0f));
            static const glm::vec3 kInventoryLight1 = glm::normalize(glm::vec3(-0.2f, -1.0f, 0.0f));
            const glm::mat3 normalToGui = glm::mat3(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 1.0f, -1.0f)) * rotation);

            MobRenderer::GuiEntityPose pose;
            pose.bodyRot    = 180.0f + xAngle * 20.0f;
            pose.headYawRel = xAngle * 20.0f;
            pose.xRot       = -yAngle * 20.0f;
            pose.light0 = glm::normalize(glm::transpose(normalToGui) * kInventoryLight0);
            pose.light1 = glm::normalize(glm::transpose(normalToGui) * kInventoryLight1);

            static std::vector<MobRenderer::GuiEntityBatch> s_batches;
            if (!renderer->CaptureForGui(*Client::g_clientMobManager, entity.GetId(), pose, s_batches)) return;

            // boundingBoxHeight / scale, with the scale then 1.
            const float scale = entity.scale > 0.0f ? entity.scale : 1.0f;
            const float boundingBoxHeight = entity.GetBbHeight() / scale;

            // PictureInPictureRenderer.prepare: translate(centre),
            // scale(size, size, -size); GuiEntityRenderer.renderToTexture:
            // translate(translation), rotate(rotation).
            glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(centerX, centerY, 0.0f));
            m = glm::scale(m, glm::vec3(static_cast<float>(size), static_cast<float>(size),
                                        -static_cast<float>(size)));
            m = glm::translate(m, glm::vec3(0.0f, boundingBoxHeight / 2.0f + offsetY, 0.0f));
            m = m * rotation;

            g.EnableScissor(x0, y0, x1, y1);
            for (const MobRenderer::GuiEntityBatch& batch : s_batches) {
                for (size_t i = 0; i + 2 < batch.triangles.size(); i += 3) {
                    const ModelVertex* v[3] = { &batch.triangles[i], &batch.triangles[i + 1],
                                                &batch.triangles[i + 2] };
                    QuadCommand q;
                    q.texture = batch.texture;
                    q.useDepth = true;
                    // The model bakes its shade (and any dye) into the
                    // vertex colour, one per face.
                    q.color = (static_cast<uint32_t>(v[0]->a) << 24) | (static_cast<uint32_t>(v[0]->r) << 16) |
                              (static_cast<uint32_t>(v[0]->g) << 8) | static_cast<uint32_t>(v[0]->b);
                    // A triangle as a quad whose last corner repeats.
                    for (int c = 0; c < 4; ++c) {
                        const ModelVertex& mv = *v[std::min(c, 2)];
                        const glm::vec4 p = m * glm::vec4(mv.x, mv.y, mv.z, 1.0f);
                        q.px[c] = p.x;
                        q.py[c] = p.y;
                        q.pz[c] = p.z;
                        q.u[c]  = mv.u;
                        q.v[c]  = mv.v;
                    }
                    rs->SubmitQuad(q);
                }
            }
            g.DisableScissor();
        }

        constexpr const char* kHorseTexture    = "assets/textures/gui/container/horse.png";
        constexpr const char* kNautilusTexture = "assets/textures/gui/container/nautilus.png";
        constexpr const char* kSlotSprite       = "container/slot";
        constexpr const char* kChestSlotsSprite = "container/horse/chest_slots";

    } // namespace

    MountInventoryScreen& GetMountInventoryScreen() {
        static MountInventoryScreen s;
        return s;
    }

    void MountInventoryScreen::Configure(Game::MountInventoryMenu::Kind kind, int inventoryColumns,
                                         const std::string& title) {
        m_kind = kind;
        m_inventoryColumns = inventoryColumns;
        m_title = title;
        // The nautilus draws another sheet; drop a cached one of the other kind.
        if (m_backgroundTried && m_backgroundKind != kind) {
            m_background = INVALID_TEXTURE;
            m_backgroundTried = false;
        }
    }

    TextureHandle MountInventoryScreen::EnsureBackground() {
        if (m_backgroundTried) return m_background;
        m_backgroundTried = true;
        m_backgroundKind = m_kind;
        int w = 0, h = 0;
        m_background = LoadStandaloneGuiTexture(
            m_kind == Game::MountInventoryMenu::Kind::Nautilus ? kNautilusTexture : kHorseTexture, w, h);
        return m_background;
    }

    bool MountInventoryScreen::SlotActive(int menuIndex) const {
        Game::AbstractContainerMenu* menu = Menu();
        return menu && menu->IsValidSlotIndex(menuIndex) && menu->GetSlot(menuIndex).IsActive();
    }

    void MountInventoryScreen::RenderBg(GuiGraphics& g, int leftPos, int topPos) {
        // AbstractMountInventoryScreen.extractBackground: the panel from its
        // 256 x 256 sheet.
        const TextureHandle bg = EnsureBackground();
        if (bg == INVALID_TEXTURE) {
            g.Fill(leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H, 0xC0202020);
        } else {
            g.Blit(bg, leftPos, topPos, leftPos + IMAGE_W, topPos + IMAGE_H,
                   0.0f, 0.0f, static_cast<float>(IMAGE_W) / 256.0f, static_cast<float>(IMAGE_H) / 256.0f);
        }
        // The chest grid: blitSprite(chest_slots, 90, 54, 0, 0, xo + 79,
        // yo + 17, columns * 18, 54) — the horse screen only.
        if (m_inventoryColumns > 0 && m_kind == Game::MountInventoryMenu::Kind::Horse) {
            g.BlitSprite(kChestSlotsSprite, 90, 54, 0, 0, leftPos + 79, topPos + 17,
                         m_inventoryColumns * 18, 54);
        }
        // The saddle and armour slot frames, only while the slot is active.
        if (SlotActive(Game::MountInventoryMenu::SLOT_SADDLE)) {
            g.BlitSprite(kSlotSprite, leftPos + 7, topPos + 35 - 18, 18, 18);
        }
        if (SlotActive(Game::MountInventoryMenu::SLOT_BODY_ARMOR)) {
            g.BlitSprite(kSlotSprite, leftPos + 7, topPos + 35, 18, 18);
        }
        // The mount itself in the box at (26, 18)-(78, 70):
        // InventoryScreen.extractEntityInInventoryFollowsMouse(graphics,
        // xo + 26, yo + 18, xo + 78, yo + 70, 17, 0.25F, xMouse, yMouse,
        // mount), above the panel.
        if (s_requested.valid) {
            if (const Game::Mob* mount = FindClientMount(s_requested.entityId)) {
                g.NextStratum();
                RenderEntityInInventoryFollowsMouse(g, leftPos + 26, topPos + 18, leftPos + 78, topPos + 70, 17,
                                                    0.25f, MouseGui().x, MouseGui().y, *mount);
            }
        }
    }

    void MountInventoryScreen::RenderLabels(GuiGraphics& g, int leftPos, int topPos) {
        // AbstractContainerScreen's labels: the title (the mount's display
        // name) at (8, 6) and "Inventory" at (8, imageHeight - 94).
        g.DrawString(m_title, leftPos + 8, topPos + 6, LABEL_COLOR, false);
        g.DrawString("Inventory", leftPos + 8, topPos + IMAGE_H - 94, LABEL_COLOR, false);
    }

    void OpenClientMountScreen(uint32_t containerId, int inventoryColumns, int32_t entityId) {
        // handleMountScreenOpen: an AbstractHorse gets the horse screen, an
        // AbstractNautilus the nautilus one, anything else nothing.
        Game::Mob* mount = FindClientMount(entityId);
        if (!mount || !mount->HasCustomInventoryScreen()) return;
        s_requested = RequestedMount{true, inventoryColumns < 0 ? 0 : inventoryColumns, entityId};
        OpenClientContainerScreen(Game::MenuType::MountInventory, containerId,
                                  Game::EntityDisplayName(*mount, mount->Level()));
    }

    bool BuildClientMountScreen(Game::Inventory* inventory, bool creative, uint32_t containerId,
                                const std::string& title) {
        if (!inventory || !s_requested.valid) return false;
        const int32_t entityId = s_requested.entityId;
        Game::Mob* mount = FindClientMount(entityId);
        if (!mount) return false;

        // new HorseInventoryMenu / NautilusInventoryMenu(containerId,
        // inventory, new SimpleContainer(size), mount, columns).
        const Game::MountInventoryMenu::Kind kind = Game::MountInventoryMenu::KindFor(mount->GetType());
        auto menu = std::make_unique<Game::MountInventoryMenu>(
            inventory, [entityId]() { return FindClientMount(entityId); }, kind, s_requested.inventoryColumns,
            /*serverSide=*/false);
        menu->containerId = containerId;
        menu->creative = creative;
        SetClientContainerMenu(std::move(menu), Game::MenuType::MountInventory);

        MountInventoryScreen& screen = GetMountInventoryScreen();
        screen.Configure(kind, s_requested.inventoryColumns,
                         title.empty() ? Game::EntityDisplayName(*mount, mount->Level()) : title);
        screen.Open();
        return true;
    }

} // namespace Render
