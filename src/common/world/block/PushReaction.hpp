// File: src/common/world/block/PushReaction.hpp
//
// MC net.minecraft.world.level.material.PushReaction, plus the enum behind
// BlockBehaviour.Properties.isRedstoneConductor's three possible answers.
//
// Both are pure DATA, transcribed from Blocks.java by
// tools/gen_block_hardness.py into GeneratedBlockHardness.cpp and applied to
// every Block in BlockRegistry::Init. Nothing here is behaviour; the piston
// structure resolver reads PushReaction, and the signal code reads
// RedstoneConductor.
#pragma once

#include <cstdint>

namespace Game {

    // Names are 26.3's (the decompile this engine tracks). The older names —
    // NORMAL, PUSH_ONLY, DESTROY, BLOCK, IGNORE — map one-to-one in order.
    enum class PushReaction : uint8_t {
        PushPull     = 0,   // an ordinary block: pushed AND pulled by a sticky piston
        Push         = 1,   // glazed terracotta: pushed, never pulled
        Popped       = 2,   // plants, torches, dust…: breaks (drops) when pushed
        Immoveable   = 3,   // obsidian, block entities, extended pistons: stops the piston
        IgnoreEntity = 4,   // unused by blocks; MC keeps it for entity push logic
    };

    // MC's `isRedstoneConductor` StatePredicate, which Blocks.java sets to one
    // of three things. `Default` is BlockStateBase::isCollisionShapeFullBlock —
    // the collision shape fills the cell — and is what nearly every block has.
    enum class RedstoneConductor : uint8_t {
        Default = 0,
        Never   = 1,   // glass, leaves, pistons, observers, redstone block, TNT…
        Always  = 2,   // soul sand and mud, whose collision box is not a full cube
    };

} // namespace Game
