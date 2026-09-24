#!/usr/bin/env python3
"""Extract the original player's complete food permission/exhaustion predicates."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root/'original/reference-only/Player.cpp').read_text()
methods = []
for signature in ['void Player::causeFoodExhaustion(', 'bool Player::canEat(',
                  'bool Player::isAllowedToFly(', 'bool Player::isAllowedToIgnoreExhaustion(',
                  'bool Player::hasInvulnerablePrivilege(']:
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        if source[end] == '{': depth += 1
        if source[end] == '}': depth -= 1
        end += 1
    methods.append(source[start:end])
body = '\n\n'.join(methods)
if '--reference' in sys.argv:
    body = body.replace('Player::', 'ReferenceFoodPolicy::')
    text = '#include "PlayerFoodReference.h"\n'+body+'\n'
else:
    body = body.replace('Player::', 'PlayerFoodRules::')
    for original, field in [('HostCanFly','hostCanFly'), ('HostCanChangeHunger','hostCanChangeHunger'),
                            ('HostCanBeInvisible','hostCanBeInvisible'), ('TrustPlayers','trustPlayers')]:
        body = body.replace('app.GetGameHostOption(eGameHostOption_'+original+')', 'settings.'+field)
    for original, field in [('CanFly','canFly'), ('ClassicHunger','classicHunger'),
                            ('Invulnerable','invulnerablePrivilege'), ('CannotBuild','cannotBuild')]:
        body = body.replace('getPlayerGamePrivilege(PlayerFoodRules::ePlayerGamePrivilege_'+original+')', 'settings.'+field)
    body = body.replace('abilities.flying','settings.flying').replace('abilities.invulnerable','settings.invulnerable').replace('level->isClientSide','settings.clientSide')
    text = '#include "PlayerFoodRules.h"\nnamespace console {\n'+body+'\n}\n'
destination = Path(sys.argv[1])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text(text)
