// File: src/client/renderer/viewmodel/GltfLoader.cpp
// See header for scope.

#include "common/core/Features.hpp"
#if ENABLE_PORTAL_GUN

#include "GltfLoader.hpp"
#include "common/core/Log.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>

#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>

namespace Render::Gltf {

    namespace {

        constexpr int kCompByte    = 5120;
        constexpr int kCompUByte   = 5121;
        constexpr int kCompShort   = 5122;
        constexpr int kCompUShort  = 5123;
        constexpr int kCompUInt    = 5125;
        constexpr int kCompFloat   = 5126;

        constexpr uint32_t kGlbMagic  = 0x46546C67u; // "glTF"
        constexpr uint32_t kChunkJson = 0x4E4F534Au; // "JSON"
        constexpr uint32_t kChunkBin  = 0x004E4942u; // "BIN\0"

        // Size of one component, in bytes.
        size_t CompSize(int ct) {
            switch (ct) {
                case kCompByte: case kCompUByte:   return 1;
                case kCompShort: case kCompUShort: return 2;
                case kCompUInt: case kCompFloat:   return 4;
            }
            return 0;
        }
        // Number of components per element by glTF "type" string.
        int CompsPerType(const std::string& t) {
            if (t == "SCALAR") return 1;
            if (t == "VEC2")   return 2;
            if (t == "VEC3")   return 3;
            if (t == "VEC4")   return 4;
            if (t == "MAT4")   return 16;
            return 0;
        }

        struct AccessorView {
            const uint8_t* data    = nullptr;
            size_t         count   = 0;
            size_t         stride  = 0;     // 0 = tightly packed
            int            compTy  = 0;
            std::string    elemTy;
        };

        AccessorView ResolveAccessor(const nlohmann::json& doc,
                                     const std::vector<uint8_t>& bin,
                                     int accessorIdx) {
            AccessorView v;
            if (accessorIdx < 0 ||
                accessorIdx >= (int)doc["accessors"].size()) return v;
            const auto& acc = doc["accessors"][accessorIdx];
            v.count  = acc.value("count", 0);
            v.compTy = acc.value("componentType", 0);
            v.elemTy = acc.value("type", "SCALAR");

            int bvIdx = acc.value("bufferView", -1);
            if (bvIdx < 0 || bvIdx >= (int)doc["bufferViews"].size()) return v;
            const auto& bv = doc["bufferViews"][bvIdx];
            size_t bvOff = bv.value("byteOffset", (size_t)0);
            size_t accOff = acc.value("byteOffset", (size_t)0);
            v.stride = bv.value("byteStride", (size_t)0);

            size_t start = bvOff + accOff;
            if (start >= bin.size()) return v;
            v.data = bin.data() + start;
            return v;
        }

        // Read element at `i`, copy `n` floats into `out`.
        bool ReadFloats(const AccessorView& v, size_t i, int n, float* out) {
            if (!v.data || i >= v.count) return false;
            size_t bytes = sizeof(float) * (size_t)n;
            size_t stride = v.stride ? v.stride : bytes;
            std::memcpy(out, v.data + stride * i, bytes);
            return true;
        }

        // Read one index (8/16/32-bit unsigned), promote to u32.
        bool ReadIndex(const AccessorView& v, size_t i, uint32_t& out) {
            if (!v.data || i >= v.count) return false;
            switch (v.compTy) {
                case kCompUByte: {
                    size_t s = v.stride ? v.stride : 1;
                    out = v.data[s * i]; return true;
                }
                case kCompUShort: {
                    size_t s = v.stride ? v.stride : 2;
                    uint16_t u; std::memcpy(&u, v.data + s * i, 2);
                    out = u; return true;
                }
                case kCompUInt: {
                    size_t s = v.stride ? v.stride : 4;
                    uint32_t u; std::memcpy(&u, v.data + s * i, 4);
                    out = u; return true;
                }
                default: return false;
            }
        }

        // Read VEC4 of UBYTE/USHORT joint indices, clamp into u8 (our
        // vertex layout uses u8 because the backend's VertexAttribute
        // only exposes Float and UByte component types — sufficient
        // for Portal's ≤45-joint skins).
        bool ReadJoints(const AccessorView& v, size_t i, uint8_t* out) {
            if (!v.data || i >= v.count) return false;
            if (v.compTy == kCompUByte) {
                size_t s = v.stride ? v.stride : 4;
                std::memcpy(out, v.data + s * i, 4);
                return true;
            }
            if (v.compTy == kCompUShort) {
                size_t s = v.stride ? v.stride : 8;
                const uint8_t* p = v.data + s * i;
                for (int k = 0; k < 4; ++k) {
                    uint16_t u; std::memcpy(&u, p + k * 2, 2);
                    out[k] = (uint8_t)std::min<uint16_t>(u, 255);
                }
                return true;
            }
            return false;
        }

        // Decompose a rigid+scale column-major mat4 into TRS. Skip the
        // shear/perspective parts that glm::decompose would handle —
        // skinned glTF nodes only ever carry rigid + non-uniform scale.
        // Avoids pulling in glm's experimental matrix_decompose header.
        void MatToTRS(const glm::mat4& m,
                      glm::vec3& t, glm::quat& r, glm::vec3& s) {
            t = glm::vec3(m[3]);
            glm::vec3 c0(m[0]), c1(m[1]), c2(m[2]);
            s = glm::vec3(glm::length(c0), glm::length(c1), glm::length(c2));
            if (s.x == 0.0f) s.x = 1e-6f;
            if (s.y == 0.0f) s.y = 1e-6f;
            if (s.z == 0.0f) s.z = 1e-6f;
            glm::mat3 rot;
            rot[0] = c0 / s.x;
            rot[1] = c1 / s.y;
            rot[2] = c2 / s.z;
            r = glm::quat_cast(rot);
        }

        // Read a full mat4 (16 floats, column-major in glTF/glm).
        bool ReadMat4(const AccessorView& v, size_t i, glm::mat4& out) {
            if (!v.data || i >= v.count || v.compTy != kCompFloat ||
                v.elemTy != "MAT4") return false;
            size_t s = v.stride ? v.stride : 64;
            std::memcpy(glm::value_ptr(out), v.data + s * i, 64);
            return true;
        }

        // Read all floats in an accessor into a flat vector. Honors stride.
        void ReadFloatArray(const AccessorView& v, std::vector<float>& out) {
            if (!v.data || v.compTy != kCompFloat) return;
            int comps = CompsPerType(v.elemTy);
            if (comps <= 0) return;
            out.resize(v.count * (size_t)comps);
            size_t tightBytes = sizeof(float) * (size_t)comps;
            size_t stride = v.stride ? v.stride : tightBytes;
            for (size_t i = 0; i < v.count; ++i) {
                std::memcpy(&out[i * comps], v.data + stride * i, tightBytes);
            }
        }

    } // namespace

    Model LoadGLB(const std::string& path) {
        Model model;

        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            Log::Warning("[GltfLoader] Failed to open %s", path.c_str());
            return model;
        }
        std::streamsize size = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> buf((size_t)size);
        if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
            Log::Warning("[GltfLoader] Failed to read %s", path.c_str());
            return model;
        }

        if (buf.size() < 12) { Log::Warning("[GltfLoader] too small"); return model; }
        uint32_t magic, version, total;
        std::memcpy(&magic,   buf.data() + 0, 4);
        std::memcpy(&version, buf.data() + 4, 4);
        std::memcpy(&total,   buf.data() + 8, 4);
        if (magic != kGlbMagic || version != 2u) {
            Log::Warning("[GltfLoader] %s not glb v2", path.c_str());
            return model;
        }

        size_t cursor = 12;
        nlohmann::json doc;
        std::vector<uint8_t> bin;
        while (cursor + 8 <= buf.size()) {
            uint32_t chunkLen, chunkType;
            std::memcpy(&chunkLen,  buf.data() + cursor + 0, 4);
            std::memcpy(&chunkType, buf.data() + cursor + 4, 4);
            cursor += 8;
            if (cursor + chunkLen > buf.size()) return model;
            if (chunkType == kChunkJson) {
                doc = nlohmann::json::parse(buf.data() + cursor,
                                            buf.data() + cursor + chunkLen,
                                            nullptr, false);
                if (doc.is_discarded()) { Log::Warning("[GltfLoader] JSON parse fail"); return model; }
            } else if (chunkType == kChunkBin) {
                bin.assign(buf.data() + cursor, buf.data() + cursor + chunkLen);
            }
            cursor += chunkLen;
        }
        if (doc.empty() || bin.empty()) return model;

        // -------------------- Nodes --------------------
        // Build TRS-form node list. glTF spec allows either a matrix or
        // TRS components; we decompose the matrix form so the rest of
        // the renderer only handles TRS.
        const auto& jnodes = doc.contains("nodes") ? doc["nodes"] : nlohmann::json::array();
        model.nodes.resize(jnodes.size());
        for (size_t i = 0; i < jnodes.size(); ++i) {
            const auto& jn = jnodes[i];
            Node& n = model.nodes[i];
            n.name      = jn.value("name", std::string{});
            n.meshIndex = jn.value("mesh",  -1);
            n.skinIndex = jn.value("skin",  -1);
            if (jn.contains("matrix")) {
                glm::mat4 m(1.0f);
                for (int k = 0; k < 16; ++k) glm::value_ptr(m)[k] = jn["matrix"][k].get<float>();
                MatToTRS(m, n.translation, n.rotation, n.scale);
            } else {
                if (jn.contains("translation")) {
                    n.translation = glm::vec3(
                        jn["translation"][0].get<float>(),
                        jn["translation"][1].get<float>(),
                        jn["translation"][2].get<float>());
                }
                if (jn.contains("rotation")) {
                    // glTF: [x, y, z, w]; glm::quat: (w, x, y, z)
                    n.rotation = glm::quat(
                        jn["rotation"][3].get<float>(),
                        jn["rotation"][0].get<float>(),
                        jn["rotation"][1].get<float>(),
                        jn["rotation"][2].get<float>());
                }
                if (jn.contains("scale")) {
                    n.scale = glm::vec3(
                        jn["scale"][0].get<float>(),
                        jn["scale"][1].get<float>(),
                        jn["scale"][2].get<float>());
                }
            }
            if (jn.contains("children")) {
                for (const auto& c : jn["children"]) n.children.push_back(c.get<int>());
            }
        }
        // Fill in parent pointers.
        for (size_t i = 0; i < model.nodes.size(); ++i) {
            for (int c : model.nodes[i].children) {
                if (c >= 0 && c < (int)model.nodes.size()) model.nodes[c].parent = (int)i;
            }
        }

        // -------------------- Skins --------------------
        const auto& jskins = doc.contains("skins") ? doc["skins"] : nlohmann::json::array();
        model.skins.resize(jskins.size());
        for (size_t i = 0; i < jskins.size(); ++i) {
            const auto& js = jskins[i];
            Skin& s = model.skins[i];
            for (const auto& j : js["joints"]) s.jointNodeIndices.push_back(j.get<int>());
            int ibmIdx = js.value("inverseBindMatrices", -1);
            AccessorView ibmV = ResolveAccessor(doc, bin, ibmIdx);
            s.inverseBindMatrices.resize(s.jointNodeIndices.size(), glm::mat4(1.0f));
            for (size_t k = 0; k < s.jointNodeIndices.size(); ++k) {
                ReadMat4(ibmV, k, s.inverseBindMatrices[k]);
            }
        }

        // -------------------- Animations --------------------
        const auto& janims = doc.contains("animations") ? doc["animations"] : nlohmann::json::array();
        model.animations.resize(janims.size());
        for (size_t i = 0; i < janims.size(); ++i) {
            const auto& ja = janims[i];
            Animation& a = model.animations[i];
            a.name = ja.value("name", std::string{});

            // Samplers
            const auto& jsmp = ja["samplers"];
            a.samplers.resize(jsmp.size());
            for (size_t k = 0; k < jsmp.size(); ++k) {
                AccessorView iv = ResolveAccessor(doc, bin, jsmp[k].value("input",  -1));
                AccessorView ov = ResolveAccessor(doc, bin, jsmp[k].value("output", -1));
                ReadFloatArray(iv, a.samplers[k].input);
                ReadFloatArray(ov, a.samplers[k].output);
                a.samplers[k].componentsPerKey = CompsPerType(ov.elemTy);
                const std::string interp = jsmp[k].value("interpolation", std::string{"LINEAR"});
                a.samplers[k].isStep = (interp == "STEP");
                if (!a.samplers[k].input.empty()) {
                    a.duration = std::max(a.duration, a.samplers[k].input.back());
                }
            }
            // Channels
            const auto& jch = ja["channels"];
            a.channels.resize(jch.size());
            for (size_t k = 0; k < jch.size(); ++k) {
                a.channels[k].samplerIdx = jch[k].value("sampler", -1);
                const auto& jt = jch[k]["target"];
                a.channels[k].targetNode = jt.value("node", -1);
                const std::string path = jt.value("path", std::string{});
                if      (path == "translation") a.channels[k].path = 0;
                else if (path == "rotation")    a.channels[k].path = 1;
                else if (path == "scale")       a.channels[k].path = 2;
                else                            a.channels[k].path = -1;
            }
        }

        // -------------------- Meshes / Primitives --------------------
        // Build a node→mesh lookup so we can stamp `nodeIndex` on each
        // primitive (carries the skin reference used at render time).
        const auto& meshes    = doc["meshes"];
        const auto& materials = doc.contains("materials")
                                ? doc["materials"]
                                : nlohmann::json::array();
        for (size_t ni = 0; ni < model.nodes.size(); ++ni) {
            const Node& n = model.nodes[ni];
            if (n.meshIndex < 0) continue;
            const auto& mesh = meshes[n.meshIndex];
            if (!mesh.contains("primitives")) continue;
            for (const auto& prim : mesh["primitives"]) {
                Primitive out;
                out.nodeIndex = (int)ni;

                const auto& attrs = prim.value("attributes", nlohmann::json::object());
                int posIdx  = attrs.value("POSITION",    -1);
                int uvIdx   = attrs.value("TEXCOORD_0",  -1);
                int normIdx = attrs.value("NORMAL",      -1);
                int jntIdx  = attrs.value("JOINTS_0",    -1);
                int wgtIdx  = attrs.value("WEIGHTS_0",   -1);
                int idxIdx  = prim.value("indices",      -1);
                int matIdx  = prim.value("material",     -1);

                AccessorView posV = ResolveAccessor(doc, bin, posIdx);
                AccessorView uvV  = ResolveAccessor(doc, bin, uvIdx);
                AccessorView nV   = ResolveAccessor(doc, bin, normIdx);
                AccessorView jV   = ResolveAccessor(doc, bin, jntIdx);
                AccessorView wV   = ResolveAccessor(doc, bin, wgtIdx);
                AccessorView iV   = ResolveAccessor(doc, bin, idxIdx);

                if (!posV.data || posV.compTy != kCompFloat || posV.elemTy != "VEC3") continue;
                out.vertices.reserve(posV.count);
                for (size_t i = 0; i < posV.count; ++i) {
                    Vertex v{};
                    float pos[3] = {0}; ReadFloats(posV, i, 3, pos);
                    v.px = pos[0]; v.py = pos[1]; v.pz = pos[2];

                    if (uvV.data) { float uv[2]={0}; ReadFloats(uvV, i, 2, uv); v.u=uv[0]; v.v=uv[1]; }
                    if (nV.data)  { float n[3]={0,1,0}; ReadFloats(nV, i, 3, n); v.nx=n[0]; v.ny=n[1]; v.nz=n[2]; }
                    else { v.ny = 1.0f; }

                    if (jV.data) ReadJoints(jV, i, v.joints);
                    if (wV.data) {
                        float w[4]={0}; ReadFloats(wV, i, 4, w);
                        for (int k=0;k<4;++k) v.weights[k] = w[k];
                    } else {
                        // Unrigged primitives still need a sensible bone
                        // weighting — bind everything to joint 0 with full
                        // weight so they follow that joint (typically the
                        // skeleton root, which stays at identity).
                        v.weights[0] = 1.0f;
                    }
                    out.vertices.push_back(v);
                }

                if (iV.data) {
                    out.indices.reserve(iV.count);
                    for (size_t i = 0; i < iV.count; ++i) {
                        uint32_t idx = 0;
                        if (ReadIndex(iV, i, idx)) out.indices.push_back(idx);
                    }
                } else {
                    out.indices.reserve(out.vertices.size());
                    for (uint32_t i = 0; i < (uint32_t)out.vertices.size(); ++i) out.indices.push_back(i);
                }

                if (matIdx >= 0 && matIdx < (int)materials.size()) {
                    out.materialName = materials[matIdx].value("name", std::string{});
                }
                model.primitives.push_back(std::move(out));
            }
        }

        Log::Info("[GltfLoader] %s: %zu nodes, %zu skins, %zu anims, %zu prims",
                  path.c_str(), model.nodes.size(), model.skins.size(),
                  model.animations.size(), model.primitives.size());
        for (const auto& a : model.animations) {
            Log::Info("[GltfLoader]   anim %s dur=%.3f ch=%zu",
                      a.name.c_str(), a.duration, a.channels.size());
        }
        return model;
    }

} // namespace Render::Gltf

#endif // ENABLE_PORTAL_GUN
