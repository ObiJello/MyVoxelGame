// File: src/client/renderer/entity/StickFigureGeometry.cpp
#include "StickFigureGeometry.hpp"
#include <cmath>

namespace Render {

    namespace {

        void PushLine(std::vector<StickVertex>& out,
                      const glm::vec3& a, const glm::vec3& b,
                      uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
            out.push_back({a.x, a.y, a.z, 0.0f, 0.0f, r, g, bl, al});
            out.push_back({b.x, b.y, b.z, 0.0f, 0.0f, r, g, bl, al});
        }

        void PushCircle(std::vector<StickVertex>& out,
                        const glm::vec3& center, const glm::vec3& right,
                        const glm::vec3& up, float radius, int segments,
                        float startAngle, float endAngle,
                        uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
            float step = (endAngle - startAngle) / static_cast<float>(segments);
            for (int i = 0; i < segments; ++i) {
                float a0 = startAngle + step * static_cast<float>(i);
                float a1 = startAngle + step * static_cast<float>(i + 1);
                glm::vec3 p0 = center + right * (std::cos(a0) * radius) + up * (std::sin(a0) * radius);
                glm::vec3 p1 = center + right * (std::cos(a1) * radius) + up * (std::sin(a1) * radius);
                PushLine(out, p0, p1, r, g, bl, al);
            }
        }

        // Push a filled disc as a triangle fan. The disc normal faces along `normal`.
        // With CullMode::Back, the disc is only visible from the side `normal` points at.
        void PushDisc(std::vector<StickVertex>& out,
                      const glm::vec3& center, const glm::vec3& right,
                      const glm::vec3& up, const glm::vec3& /*normal*/,
                      float radius, int segments,
                      uint8_t r, uint8_t g, uint8_t bl, uint8_t al) {
            constexpr float TWO_PI = 2.0f * 3.14159265f;
            float step = TWO_PI / static_cast<float>(segments);
            for (int i = 0; i < segments; ++i) {
                float a0 = step * static_cast<float>(i);
                float a1 = step * static_cast<float>(i + 1);
                glm::vec3 p0 = center + right * (std::cos(a0) * radius) + up * (std::sin(a0) * radius);
                glm::vec3 p1 = center + right * (std::cos(a1) * radius) + up * (std::sin(a1) * radius);
                // Triangle: center, p0, p1 (CCW when viewed from normal direction)
                out.push_back({center.x, center.y, center.z, 0.0f, 0.0f, r, g, bl, al});
                out.push_back({p0.x, p0.y, p0.z, 0.0f, 0.0f, r, g, bl, al});
                out.push_back({p1.x, p1.y, p1.z, 0.0f, 0.0f, r, g, bl, al});
            }
        }

    } // namespace

    void BuildStickFigure(std::vector<StickVertex>& lineVerts,
                          std::vector<StickVertex>& triVerts,
                          const glm::vec3& feetPos,
                          float headYawDeg, float bodyYawDeg,
                          float /*pitchDeg*/, bool isCrouching) {
        const uint8_t cr = 0, cg = 255, cb = 60, ca = 255;
        constexpr float PI = 3.14159265f;
        const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

        // Body orientation
        float bodyRad = glm::radians(bodyYawDeg);
        glm::vec3 bodyFwd{std::cos(bodyRad), 0.0f, std::sin(bodyRad)};
        glm::vec3 bodyRight = glm::normalize(glm::cross(bodyFwd, worldUp));

        // Head orientation
        float headRad = glm::radians(headYawDeg);
        glm::vec3 lookDir{std::cos(headRad), 0.0f, std::sin(headRad)};
        glm::vec3 faceRight = glm::normalize(glm::cross(lookDir, worldUp));

        // Crouching (Minecraft's HumanoidModel.java)
        float crouchTilt     = isCrouching ? 0.5f : 0.0f;
        float crouchHeadDrop = isCrouching ? (4.2f / 16.0f) : 0.0f;
        float crouchBodyDrop = isCrouching ? (3.2f / 16.0f) : 0.0f;
        float crouchLegBack  = isCrouching ? (4.0f / 16.0f) : 0.0f;

        float neckY  = 1.44f - crouchBodyDrop;
        float hipY   = 0.90f;
        float headCY = 1.62f - crouchHeadDrop;
        float handY  = 1.10f - crouchBodyDrop;

        glm::vec3 neck  = feetPos + worldUp * neckY  + bodyFwd * std::sin(crouchTilt) * 0.3f;
        glm::vec3 hip   = feetPos + worldUp * hipY;
        glm::vec3 headC = feetPos + worldUp * headCY + bodyFwd * std::sin(crouchTilt) * 0.35f;

        glm::vec3 footL = feetPos + bodyRight * (-0.20f) - bodyFwd * crouchLegBack;
        glm::vec3 footR = feetPos + bodyRight * ( 0.20f) - bodyFwd * crouchLegBack;
        glm::vec3 shoulderPos = neck - worldUp * 0.14f; // slightly below neck
        glm::vec3 shoulderL = shoulderPos + bodyRight * (-0.05f);
        glm::vec3 shoulderR = shoulderPos + bodyRight * ( 0.05f);
        glm::vec3 handL = feetPos + worldUp * handY + bodyRight * (-0.35f) + bodyFwd * std::sin(crouchTilt) * 0.2f;
        glm::vec3 handR = feetPos + worldUp * handY + bodyRight * ( 0.35f) + bodyFwd * std::sin(crouchTilt) * 0.2f;

        // --- LINES: Body, legs, arms ---
        PushLine(lineVerts, neck, hip, cr, cg, cb, ca);
        PushLine(lineVerts, hip, footL, cr, cg, cb, ca);
        PushLine(lineVerts, hip, footR, cr, cg, cb, ca);
        PushLine(lineVerts, shoulderL, handL, cr, cg, cb, ca);
        PushLine(lineVerts, shoulderR, handR, cr, cg, cb, ca);

        // --- LINES: Front head outline + face features ---
        float headRadius = 0.18f;
        glm::vec3 frontC = headC;

        PushCircle(lineVerts, frontC, faceRight, worldUp, headRadius, 16,
                   0.0f, 2.0f * PI, cr, cg, cb, ca);

        // Eyes
        float eyeOffY = 0.04f, eyeOffX = 0.06f, eyeLen = 0.03f;
        glm::vec3 eyeL = frontC + worldUp * eyeOffY + faceRight * (-eyeOffX);
        glm::vec3 eyeR = frontC + worldUp * eyeOffY + faceRight * ( eyeOffX);
        PushLine(lineVerts, eyeL - faceRight * eyeLen, eyeL + faceRight * eyeLen, cr, cg, cb, ca);
        PushLine(lineVerts, eyeR - faceRight * eyeLen, eyeR + faceRight * eyeLen, cr, cg, cb, ca);

        // Smile
        glm::vec3 mouthC = frontC - worldUp * 0.04f;
        PushCircle(lineVerts, mouthC, faceRight, worldUp, 0.07f, 8, PI, 2.0f * PI, cr, cg, cb, ca);

        // --- TRIANGLES: Back-of-head filled disc (GPU face-culled) ---
        // Placed at headC (no offset) so it lines up with the neck/body connection.
        // Front features are offset forward, so they still render in front of this disc.
        PushDisc(triVerts, headC, faceRight, worldUp, -lookDir,
                 headRadius, 16, cr, cg, cb, ca);
    }

} // namespace Render
