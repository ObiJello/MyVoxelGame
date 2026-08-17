// File: src/client/renderer/entity/model/SetupAnimRunner.hpp
//
// Executes a compiled MC setupAnim (see tools/gen_setup_anim.py).
//
// The program is a flat list of statements over a postfix expression tape.
// Baking resolves each statement's part NAME to a live ModelPart once, so a
// frame is a walk over pointers and a small float stack — no string compares,
// no allocation.
#pragma once

#include "client/renderer/entity/model/GeneratedSetupAnim.hpp"

#include <vector>

namespace Render {

    class ModelPart;
    struct EntityRenderState;

    class SetupAnimProgram {
    public:
        SetupAnimProgram() = default;

        // Statements naming a part the mesh does not have are dropped, not
        // fatal: the mesh and the model class can legitimately disagree when
        // MC builds the layer from a different class than it animates.
        static SetupAnimProgram Bake(ModelPart& root, const AnimProgram& prog);

        bool Valid() const { return m_prog != nullptr; }

        void Run(const EntityRenderState& state) const;

    private:
        // A `Part` node READS a part's current value — MC's
        // `Mth.clamp(this.rightArm.xRot, -0.4F, 0.4F)` and every
        // `Mth.rotLerpRad(t, this.head.xRot, …)`. Resolving those at bake time
        // matters: evaluating them as zero silently turns a clamp into a
        // constant and a rotLerp into a snap.
        struct PartRef { ModelPart* part; PartField field; };

        const AnimProgram* m_prog = nullptr;
        std::vector<ModelPart*> m_parts;      // one per statement, null = skip
        std::vector<PartRef>    m_nodeParts;  // one per node in [m_nodeLo, m_nodeHi)
        int m_nodeLo = 0;
        int m_localCount = 0;
    };

} // namespace Render
