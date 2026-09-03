// File: src/client/portal/ImmersivePortalTraveler.cpp
#include "common/core/Features.hpp"
#if ENABLE_IMMERSIVE_PORTALS

#include "ImmersivePortalTraveler.hpp"

#include "ClientImmersivePortals.hpp"
#include "client/world/ClientLevel.hpp"
#include "client/network/NetworkClient.hpp"
#include "client/network/ClientConnection.hpp"
#include "client/renderer/environment/SkyRenderer.hpp"
#include "common/network/packets/game/PortalTeleportC2SPacket.hpp"
#include "common/core/Log.hpp"
#include "common/core/Mth.hpp"

#include <algorithm>
#include <cmath>

namespace Client {

    ImmersivePortalTraveler g_immersivePortalTraveler;

    namespace {
        using Game::Immersive::Portal;
        namespace PortalFlag = Game::Immersive::PortalFlag;

        // Guards against the same crossing being read twice while the
        // position settles; NOT against a quick reversal. It used to be
        // 120 ms, and a player who stepped in and immediately backed out
        // re-crossed the reverse surface inside that window unseen — and
        // walked on through it, into the space behind the far portal. The
        // nudge past the far surface (kNudge) is what really prevents the
        // double read, so this can be a couple of frames.
        constexpr auto   kCooldown          = std::chrono::milliseconds(25);
        // A segment longer than this is a teleport of some other kind, not
        // movement; the mod refuses to interpret it (|Δ|² > 1600).
        constexpr double kMaxSegmentLength  = 40.0;
        // How far past the far surface next frame's segment starts.
        constexpr double kNudge             = 0.01;
        // An eye skimming the frame's edge still counts (mod leniency).
        constexpr double kEdgeLeniency      = 0.05;
        // The crossing fires this far BEFORE the eye reaches the surface,
        // and the arrival lands this far beyond the far one. The camera's
        // near clip is 0.05: an eye closer than that to a wall-mounted
        // surface has its near plane behind the wall face, and the wall
        // around the portal is clipped away for a frame — the flash on the
        // way through, and a see-through wall when standing in it.
        constexpr double kEyeMargin         = 0.07;
    }

    bool ImmersivePortalTraveler::OnCooldown() const {
        return std::chrono::steady_clock::now() < m_cooldownUntil;
    }

    std::optional<ImmersivePortalTraveler::Crossing> ImmersivePortalTraveler::Check(
            const glm::dvec3& lastEye, const glm::dvec3& eye, const glm::dvec3& feet,
            const glm::vec3& velocity, float yaw, float pitch, float scale) {
        if (OnCooldown()) return std::nullopt;
        if (!ClientLevels::HasSession()) return std::nullopt;
        if (glm::length(eye - lastEye) > kMaxSegmentLength) return std::nullopt;

        // Nearest crossing along the segment wins (two overlapping portals).
        const Portal* best = nullptr;
        double bestT = 2.0;
        glm::dvec3 hitPoint{0.0};
        GetClientImmersivePortals().ForEach([&](const Portal& p) {
            if (!p.Has(PortalFlag::Teleportable)) return;
            // The surface, moved kEyeMargin toward the viewer, is what the
            // eye's segment is tested against.
            Portal early = p;
            early.origin += p.Normal() * kEyeMargin;
            const auto hit = early.RaytraceSegment(lastEye, eye, std::max(kEdgeLeniency, p.CrossingLeniency()));
            if (!hit || hit->t >= bestT) return;
            best = &p;
            bestT = hit->t;
            hitPoint = hit->point;
        });
        if (!best) return std::nullopt;
        const Portal& portal = *best;

        Crossing c;
        c.portalId        = portal.id;
        c.dimensionBefore = ClientLevels::ActiveDimension();
        c.dimensionAfter  = portal.IsMirror() ? portal.dimension : portal.destDimension;
        c.eyeBefore       = eye;
        c.newEye          = portal.TransformPoint(eye);
        // Land at least the margin beyond the far surface (the mapped eye
        // sits on the near side of it when the crossing fired early).
        {
            const glm::dvec3 content  = portal.ContentDirection();
            const glm::dvec3 farPoint = portal.IsMirror() ? portal.origin : portal.destination;
            const double depth = glm::dot(c.newEye - farPoint, content);
            if (depth < kEyeMargin) c.newEye += content * (kEyeMargin - depth);
            c.arrivalDirection = content;
        }
        // The body hangs below the mapped eye. The eye offset is not rotated:
        // physics has no notion of a tilted player, and a rotating portal
        // that changes "up" is the mod's gravity-changer territory.
        // The body scales with the portal: through a portal whose far side
        // is twice as large the player arrives twice as tall, so the eye
        // offset maps through the scale too and the feet land on the far
        // ground rather than a body's worth above or below it.
        const double portalScale = portal.IsMirror() ? 1.0 : portal.scale;
        c.newScale        = static_cast<float>(scale * portalScale);
        c.newFeet         = c.newEye - (eye - feet) * portalScale;
        // The gun's arrival rules (its pre-immersive teleport had them, the
        // generic margin above does not):
        //   • wall exit: the eye lands at the oval's centre, which puts
        //     the feet 1.62 m down — inside the floor under a wall portal
        //     whose opening is two blocks tall. Feet no lower than the
        //     opening's bottom edge, standing on the surface below it.
        //   • ceiling exit: the eye a decimetre below the ceiling so the
        //     head is not inside the ceiling block; the body hangs down.
        if (portal.kind == Game::Immersive::PortalKind::PortalGun) {
            const glm::dvec3 content  = portal.ContentDirection();
            const glm::dvec3 farPoint = portal.IsMirror() ? portal.origin : portal.destination;
            if (std::abs(content.y) < 0.3) {
                const double bottom = farPoint.y - 1.0;
                if (c.newFeet.y < bottom) {
                    const double lift = bottom - c.newFeet.y;
                    c.newFeet.y += lift;
                    c.newEye.y  += lift;
                }
            } else if (content.y < -0.7) {
                constexpr double kCeilingEyeOffset = 0.1;
                const double drop = c.newEye.y - (farPoint.y - kCeilingEyeOffset);
                if (drop > 0.0) {
                    c.newEye.y  -= drop;
                    c.newFeet.y -= drop;
                }
            }
        }
        c.newVelocity     = glm::vec3(portal.TransformLocalVec(glm::dvec3(velocity)));
        // The gun's floor exit pops the player out (Portal's fling): a
        // walk into a wall portal maps to a few metres per second straight
        // up out of a floor portal, which is not enough to clear the floor
        // plane — the body (1.62 m below the eye) has to rise clear of it
        // at gravity 32 m/s², v ≥ √(2·32·1.62) ≈ 10.2 m/s. The gun's old
        // teleport used 12 for margin; the immersive crossing kept the
        // mapped velocity only, and lost the pop.
        if (portal.kind == Game::Immersive::PortalKind::PortalGun) {
            const glm::dvec3 content = portal.ContentDirection();
            if (content.y > 0.7) {
                constexpr float kMinFloorExitVelocity = 12.0f;
                if (c.newVelocity.y < kMinFloorExitVelocity) c.newVelocity.y = kMinFloorExitVelocity;
            }
        }

        const glm::vec3 forward = Game::Mth::ViewVector(pitch, yaw);
        const glm::dvec3 newForward = glm::normalize(portal.TransformLocalVecNonScale(glm::dvec3(forward)));
        c.newYaw   = Game::Mth::YRotFromVector(glm::vec3(newForward));
        c.newPitch = Game::Mth::XRotFromVector(glm::vec3(newForward));

        // Continue the motion a hair on the far side so next frame's segment
        // starts beyond the reverse portal's surface.
        const glm::dvec3 motion = glm::length(eye - lastEye) > 1e-9
            ? glm::normalize(portal.TransformLocalVecNonScale(eye - lastEye))
            : portal.ContentDirection();
        c.nextLastEye = c.newEye + motion * kNudge;
        (void)hitPoint;
        return c;
    }

    void ImmersivePortalTraveler::Commit(const Crossing& c) {
        m_cooldownUntil = std::chrono::steady_clock::now() + kCooldown;

        if (c.dimensionAfter != c.dimensionBefore) {
            // The far level already exists: it was streamed in to be seen
            // through the portal. Keep the one being left — the portal
            // behind the player still shows it.
            ClientLevels::SetActive(c.dimensionAfter, /*keepPrevious=*/true);
            ClientLevels::SetPacketDimension(c.dimensionAfter);
            ::Render::g_skyRenderer.SetDimension(Game::DimensionToRaw(c.dimensionAfter));
        }

        if (g_networkClient && g_networkClient->IsConnected()) {
            if (auto connection = g_networkClient->GetConnection()) {
                Network::PortalTeleportC2SPacket packet;
                packet.dimensionBefore = static_cast<int8_t>(Game::DimensionToRaw(c.dimensionBefore));
                packet.eyeX = c.eyeBefore.x; packet.eyeY = c.eyeBefore.y; packet.eyeZ = c.eyeBefore.z;
                packet.portalId = c.portalId;
                packet.yaw   = c.newYaw;
                packet.pitch = c.newPitch;
                connection->SendPacket(static_cast<uint8_t>(Network::PacketId::PortalTeleportC2S),
                                       Network::Serialization::Serialize(packet));
            }
        }

        Log::Info("[ImmersivePortals] Crossed #%u: %s (%.1f, %.1f, %.1f) -> %s (%.1f, %.1f, %.1f)",
                  c.portalId,
                  std::string(Game::DimensionName(c.dimensionBefore)).c_str(),
                  c.eyeBefore.x, c.eyeBefore.y, c.eyeBefore.z,
                  std::string(Game::DimensionName(c.dimensionAfter)).c_str(),
                  c.newEye.x, c.newEye.y, c.newEye.z);
    }

} // namespace Client

#endif // ENABLE_IMMERSIVE_PORTALS
