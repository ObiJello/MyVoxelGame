#!/usr/bin/env python3
"""Adapt original FoodData through explicit player/item hooks, or emit a reference.

The reference retains original signatures and numeric bodies with fixture-only
entity declarations. Native input validation and overflow fixes are not applied
to that separate reference build.
"""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
destination = Path(sys.argv[1])
destination.mkdir(parents=True, exist_ok=True)
reference = '--reference' in sys.argv
header = (root/'original/Minecraft.World/FoodData.h').read_text()
source = (root/'original/Minecraft.World/FoodData.cpp').read_text()
start = source.index('FoodData::FoodData()')
source = source[start:]
if reference:
    prefix = '#include "FoodReferenceRuntime.h"\n#include "NbtIo.h"\n'
else:
    header = header.replace('class FoodItem;\nclass Player;', '#include "FoodAccess.h"')
    header = header.replace('FoodItem *item', 'const console::FoodItemAccess& item')
    header = header.replace('shared_ptr<Player> player', 'console::FoodPlayerAccess& player')
    source = source.replace('FoodItem *item', 'const console::FoodItemAccess& item')
    source = source.replace('shared_ptr<Player> player', 'console::FoodPlayerAccess& player')
    source = source.replace('item->', 'item.').replace('player->level->difficulty', 'player.difficulty()').replace('player->', 'player.')
    source = source.replace('player.hurt(DamageSource::starve, 1)', 'player.starve(1)')
    source = source.replace('int difficulty = player.difficulty();', '''int difficulty = player.difficulty();
    if (difficulty < Difficulty::PEACEFUL || difficulty > Difficulty::HARD)
        throw std::invalid_argument("Invalid food difficulty");''')
    source = source.replace('foodLevel = min(food + foodLevel, FoodConstants::MAX_FOOD);', '''if (food < 0 || !std::isfinite(saturationModifier) || saturationModifier < 0)
        throw std::invalid_argument("Invalid food nutrition or saturation modifier");
    foodLevel = static_cast<int>(std::min<std::int64_t>(std::int64_t(food) + foodLevel, FoodConstants::MAX_FOOD));''')
    source = source.replace('exhaustionLevel = min(exhaustionLevel + amount,', '''if (!std::isfinite(amount) || amount < 0)
        throw std::invalid_argument("Invalid exhaustion amount");
    exhaustionLevel = min(exhaustionLevel + amount,''')
    source = source.replace('this->foodLevel = food;', '''if (food < 0 || food > FoodConstants::MAX_FOOD) throw std::invalid_argument("Invalid food level");
    this->foodLevel = food;''')
    source = source.replace('this->saturationLevel = saturation;', '''if (!std::isfinite(saturation) || saturation < 0 || saturation > FoodConstants::MAX_SATURATION)
        throw std::invalid_argument("Invalid saturation level");
    this->saturationLevel = saturation;''')
    source = source.replace('this->exhaustionLevel = exhaustion;', '''if (!std::isfinite(exhaustion) || exhaustion < 0 || exhaustion > FoodConstants::MAX_SATURATION * 2)
        throw std::invalid_argument("Invalid exhaustion level");
    this->exhaustionLevel = exhaustion;''')
    a = source.index('void FoodData::readAdditionalSaveData(')
    b = source.index('void FoodData::addAdditonalSaveData(', a)
    source = source[:a] + '''void FoodData::readAdditionalSaveData(CompoundTag *entityTag)
{
    if (!entityTag) throw IoError("Missing food save tag");
    if (!entityTag->contains(L"foodLevel")) return;
    for (const auto* key : {L"foodLevel", L"foodTickTimer"})
        if (entityTag->contains(key) && entityTag->get(key)->getId() != Tag::TAG_Int)
            throw IoError("Invalid food integer tag");
    for (const auto* key : {L"foodSaturationLevel", L"foodExhaustionLevel"})
        if (entityTag->contains(key) && entityTag->get(key)->getId() != Tag::TAG_Float)
            throw IoError("Invalid food float tag");
    FoodData next = *this;
    next.setFoodLevel(entityTag->getInt(L"foodLevel"));
    next.tickTimer = entityTag->getInt(L"foodTickTimer");
    if (next.tickTimer < 0 || next.tickTimer >= FoodConstants::HEALTH_TICK_COUNT)
        throw IoError("Invalid food tick timer");
    next.setSaturation(entityTag->getFloat(L"foodSaturationLevel"));
    next.setExhaustion(entityTag->getFloat(L"foodExhaustionLevel"));
    *this = next;
}

''' + source[b:]
    source = source.replace('entityTag->putInt(L"foodLevel", foodLevel);', '''if (!entityTag) throw IoError("Missing food save tag");
    entityTag->putInt(L"foodLevel", foodLevel);''')
    prefix = '#include "FoodAccess.h"\n#include "NbtIo.h"\n#include <algorithm>\n#include <cmath>\n#include <cstdint>\n#include <stdexcept>\n#include "Difficulty.h"\n'
(destination/'FoodData.h').write_text(header+'\n')
(destination/'FoodData.cpp').write_text(prefix+'#include "FoodConstants.h"\n#include "FoodData.h"\n'+source+'\n')
