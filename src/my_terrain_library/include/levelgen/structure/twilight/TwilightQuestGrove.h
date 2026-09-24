#pragma once

#include "levelgen/structure/TwilightStructures.h"

// Twilight Forest 4.9 — the Quest Grove ("twilightforest:quest_grove"):
// type/QuestGroveStructure.java + QuestGrove.java (piece type
// "twilightforest:tfquest1", template "twilightforest:quest_grove").
//
// twilight_pieces::buildQuestGrove (declared in TwilightStructures.h) is
// defined in TwilightQuestGrove.cpp. The "quest_ram" data marker is not
// spawned (no quest ram entity); the "dispenser" marker becomes a dropper
// with the "twilightforest:quest_grove_dropper" loot table, as in the mod.
