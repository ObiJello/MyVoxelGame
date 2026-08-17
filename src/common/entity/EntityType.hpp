// File: src/common/entity/EntityType.hpp
//
// MC net.minecraft.world.entity.EntityType, reduced to the columns this engine
// consumes.
//
// The table itself is GENERATED — `tools/gen_entity_types.py` transcribes it
// from the decompiled EntityType.java, because 86 rows of hand-copied
// `sized(0.6F, 1.95F)` is 86 chances to round a number that must not be
// rounded, and a wrong height is the difference between a zombie fitting under
// a two-block ceiling and not.
//
// This header exists so the ~40 files that include it do not have to know that,
// and so the enum has one obvious home.
//
// ORDERING IS WIRE-VISIBLE: AddEntityS2C sends the type as an index into
// EntityTypeId. Append only. The generator preserves the existing order.
#pragma once

#include "common/entity/GeneratedEntityTypes.hpp"
