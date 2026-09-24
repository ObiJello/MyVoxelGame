#!/usr/bin/env python3
"""Extract original player XP formulas and NBT fields, with checked native state."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root/'original/reference-only/Player.cpp').read_text()
methods=[]
for signature in ['void Player::increaseXp(', 'void Player::withdrawExperienceLevels(',
                  'int Player::getXpNeededForNextLevel(', 'void Player::levelUp(',
                  'int Player::getExperienceReward(', 'bool Player::isAlwaysExperienceDropper(']:
    start=source.index(signature)
    end=source.index('{', start)+1
    depth=1
    while depth:
        if source[end]=='{': depth+=1
        if source[end]=='}': depth-=1
        end+=1
    methods.append(source[start:end])
body='\n\n'.join(methods).replace('Player::', 'PlayerExperience::').replace('shared_ptr<Player> killedBy','')
read_start=source.index('experienceProgress = entityTag->getFloat(L"XpP");')
read_end=source.index('\n',source.index('totalExperience = entityTag->getInt(L"XpTotal");',read_start))
read=source[read_start:read_end]
write_start=source.index('entityTag->putFloat(L"XpP", experienceProgress);')
write_end=source.index('\n',source.index('entityTag->putInt(L"XpTotal", totalExperience);',write_start))
write=source[write_start:write_end]
if '--reference' not in sys.argv:
    body=body.replace('void PlayerExperience::increaseXp(', 'void PlayerExperience::applyXp(')
    body=body.replace('score += i;', 'score = std::bit_cast<int>(std::uint32_t(score) + std::uint32_t(i));')
    body=body.replace('experienceLevel -= amount;', '''if (amount < 0) throw std::invalid_argument("Negative experience withdrawal");
    experienceLevel -= amount;''')
    body=body.replace('experienceLevel++;', '''if (experienceLevel == maxLevel) throw std::overflow_error("Experience level cost exceeds console integer range");
    experienceLevel++;''')
    body=body.replace('int reward = experienceLevel * 7;', 'std::int64_t reward = std::int64_t(experienceLevel) * 7;')
    body+='''
void PlayerExperience::increaseXp(int amount)
{
    if (amount < 0) throw std::invalid_argument("Negative experience award");
    PlayerExperience next = *this;
    next.applyXp(amount);
    *this = next;
}
'''
    read='''if (!entityTag) throw IoError("Missing experience save tag");
    PlayerExperience next = *this;
    next.experienceProgress = entityTag->getFloat(L"XpP");
    next.experienceLevel = entityTag->getInt(L"XpLevel");
    next.totalExperience = entityTag->getInt(L"XpTotal");
    if (!std::isfinite(next.experienceProgress) || next.experienceProgress < 0 || next.experienceProgress >= 1 ||
        next.experienceLevel < 0 || next.experienceLevel > maxLevel || next.totalExperience < 0)
        throw IoError("Invalid experience save state");
    *this = next;'''
    write='if (!entityTag) throw IoError("Missing experience save tag");\n'+write
body+='\nvoid PlayerExperience::readAdditionalSaveData(CompoundTag* entityTag) {\n'+read+'\n}\n'
body+='\nvoid PlayerExperience::addAdditionalSaveData(CompoundTag* entityTag) {\n'+write+'\n}\n'
destination=Path(sys.argv[1]);destination.parent.mkdir(parents=True,exist_ok=True)
destination.write_text('#include "PlayerExperience.h"\n#include "NbtIo.h"\n#include <bit>\n#include <cmath>\n#include <stdexcept>\nnamespace console {\n'+body+'\n}\n')
