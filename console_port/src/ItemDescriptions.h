#pragma once

namespace console {
// Item::items[id]->getDescriptionId(data) and ->getUseDescriptionId() as string
// IDs (see ConsoleStrings.h); -1 for unregistered IDs. Generated into
// ported/ItemDescriptions.cpp by tools/extract_descriptions.py.
int consoleDescriptionId(int id, int data = -1);
int consoleUseDescriptionId(int id);
}
