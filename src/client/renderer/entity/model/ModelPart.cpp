// File: src/client/renderer/entity/model/ModelPart.cpp
#include "client/renderer/entity/model/ModelPart.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace Render {

    void ModelPart::ResetPose() {
        x = pose.x; y = pose.y; z = pose.z;
        xRot = pose.xRot; yRot = pose.yRot; zRot = pose.zRot;
        xScale = yScale = zScale = 1.0f;
        for (auto& child : children) child->ResetPose();
    }

    ModelPart* ModelPart::AddChild(const std::string& childName, PartPose childPose) {
        auto child = std::make_unique<ModelPart>();
        child->name = childName;
        child->pose = childPose;
        child->ResetPose();
        ModelPart* raw = child.get();
        children.push_back(std::move(child));
        return raw;
    }

    ModelPart* ModelPart::Find(const std::string& childName) {
        for (auto& child : children) {
            if (child->name == childName) return child.get();
            if (ModelPart* found = child->Find(childName)) return found;
        }
        return nullptr;
    }

    glm::mat4 ModelPart::LocalMatrix(const glm::vec3& extraTranslation) const {
        // MC ModelPart.translateAndRotate, in this exact order: translate, then
        // Z, then Y, then X rotation, then scale. Any other rotation order
        // produces subtly wrong limb angles the moment two axes are non-zero,
        // which is every attack and every swimming pose.
        glm::mat4 m = glm::translate(glm::mat4(1.0f),
                                     glm::vec3(x, y, z) + extraTranslation);
        if (zRot != 0.0f) m = glm::rotate(m, zRot, glm::vec3(0.0f, 0.0f, 1.0f));
        if (yRot != 0.0f) m = glm::rotate(m, yRot, glm::vec3(0.0f, 1.0f, 0.0f));
        if (xRot != 0.0f) m = glm::rotate(m, xRot, glm::vec3(1.0f, 0.0f, 0.0f));
        if (xScale != 1.0f || yScale != 1.0f || zScale != 1.0f) {
            m = glm::scale(m, glm::vec3(xScale, yScale, zScale));
        }
        return m;
    }

    namespace { const std::vector<CubeDefinition> kNoCubes; }

    void ModelPart::Build(const glm::mat4& parent, float texWidth, float texHeight,
                          std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx) const {
        if (!visible) return;

        const glm::mat4 world = parent * LocalMatrix();

        for (const CubeDefinition& cube : (skipDraw ? kNoCubes : cubes)) {
            // UVs are normalised here rather than in the shader so one shader
            // serves both 64x64 and 64x32 models — see the header note.
            const size_t firstVert = verts.size();
            BuildCube(cube, world, verts, idx);
            for (size_t i = firstVert; i < verts.size(); ++i) {
                verts[i].u /= texWidth;
                verts[i].v /= texHeight;
            }
        }

        for (const auto& child : children) {
            child->Build(world, texWidth, texHeight, verts, idx);
        }
    }

    void BuildCube(const CubeDefinition& cube, const glm::mat4& transform,
                   std::vector<ModelVertex>& verts, std::vector<uint32_t>& idx) {
        const float g = cube.grow;

        const float minX = cube.originX - g;
        const float minY = cube.originY - g;
        const float minZ = cube.originZ - g;
        const float maxX = cube.originX + cube.sizeX + g;
        const float maxY = cube.originY + cube.sizeY + g;
        const float maxZ = cube.originZ + cube.sizeZ + g;

        // Vertex names mirror ModelPart.Cube.
        const glm::vec3 t0(minX, minY, minZ), t1(maxX, minY, minZ);
        const glm::vec3 t2(maxX, maxY, minZ), t3(minX, maxY, minZ);
        const glm::vec3 l0(minX, minY, maxZ), l1(maxX, minY, maxZ);
        const glm::vec3 l2(maxX, maxY, maxZ), l3(minX, maxY, maxZ);

        // UV columns are laid out from the UNINFLATED size: MC computes the
        // texture layout from w/h/d, not from the grown box, so an inflated hat
        // layer samples the same texels as the head it covers.
        const float w = cube.sizeX, h = cube.sizeY, d = cube.sizeZ;
        const float u0 = cube.texOffsX;
        const float u1 = cube.texOffsX + d;
        const float u2 = cube.texOffsX + d + w;
        const float u22 = cube.texOffsX + d + w + w;
        const float u3 = cube.texOffsX + d + w + d;
        const float u4 = cube.texOffsX + d + w + d + w;
        const float v0 = cube.texOffsY;
        const float v1 = cube.texOffsY + d;
        const float v2 = cube.texOffsY + d + h;

        // MC's standard directional shading, baked into vertex colour.
        const auto shade = [](float s) { return static_cast<uint8_t>(s * 255.0f); };
        const uint8_t S_UP = shade(1.00f);
        const uint8_t S_DOWN = shade(0.50f);
        const uint8_t S_NS = shade(0.80f);
        const uint8_t S_EW = shade(0.60f);

        const auto emit = [&](const glm::vec3 q[4], float U0, float V0, float U1, float V1,
                              uint8_t sh) {
            const uint32_t base = static_cast<uint32_t>(verts.size());

            // Mirroring swaps the u extents, which is exactly what MC's
            // CubeListBuilder.mirror() does — it is a texture-space flip, not a
            // geometry flip, so a mirrored left arm reuses the right arm's
            // texels the right way round.
            const float uA = cube.mirror ? U1 : U0;
            const float uB = cube.mirror ? U0 : U1;

            const glm::vec3 p0 = glm::vec3(transform * glm::vec4(q[0], 1.0f));
            const glm::vec3 p1 = glm::vec3(transform * glm::vec4(q[1], 1.0f));
            const glm::vec3 p2 = glm::vec3(transform * glm::vec4(q[2], 1.0f));
            const glm::vec3 p3 = glm::vec3(transform * glm::vec4(q[3], 1.0f));

            // Vertex 0 takes the HIGH u. See the header note — assigning these
            // the intuitive way round mirrors every face.
            verts.push_back({ p0.x, p0.y, p0.z, uB, V0, sh, sh, sh, 255 });
            verts.push_back({ p1.x, p1.y, p1.z, uA, V0, sh, sh, sh, 255 });
            verts.push_back({ p2.x, p2.y, p2.z, uA, V1, sh, sh, sh, 255 });
            verts.push_back({ p3.x, p3.y, p3.z, uB, V1, sh, sh, sh, 255 });

            idx.push_back(base + 0);
            idx.push_back(base + 1);
            idx.push_back(base + 2);
            idx.push_back(base + 0);
            idx.push_back(base + 2);
            idx.push_back(base + 3);
        };

        { const glm::vec3 q[4] = { l1, l0, t0, t1 }; emit(q, u1, v0, u2,  v1, S_DOWN); }
        { const glm::vec3 q[4] = { t2, t3, l3, l2 }; emit(q, u2, v1, u22, v0, S_UP);   }
        { const glm::vec3 q[4] = { t0, l0, l3, t3 }; emit(q, u0, v1, u1,  v2, S_EW);   }
        { const glm::vec3 q[4] = { t1, t0, t3, t2 }; emit(q, u1, v1, u2,  v2, S_NS);   }
        { const glm::vec3 q[4] = { l1, t1, t2, l2 }; emit(q, u2, v1, u3,  v2, S_EW);   }
        { const glm::vec3 q[4] = { l0, l1, l2, l3 }; emit(q, u3, v1, u4,  v2, S_NS);   }
    }

} // namespace Render
