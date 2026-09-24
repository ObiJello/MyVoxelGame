#!/usr/bin/env python3
"""Bake MC 26.3's game-rule registry into C++.

Reads
  minecraft_code_26.3-pre-2/.../world/level/gamerules/GameRules.java   the registrations
  minecraft_code_26.3-pre-2/.../util/datafix/fixes/GameRuleRegistryFix.java
                                                              the pre-26 (camelCase)
                                                              names, incl. the
                                                              inverted ones
  assets/lang/en_us.json                                      labels + descriptions
and emits src/common/world/level/GeneratedGameRules.inc, one row per rule:

    GAME_RULE(EnumName, "id", "legacyName", legacyInverted, Category, Type,
              defaultValue, minValue, maxValue, "label", "description")

`legacyName` is what an older level.dat (or a player typing the old spelling)
calls the rule; `legacyInverted` marks the disable* rules whose meaning
flipped (disableRaids -> raids). Booleans carry 0/1 for default/min/max.
Integer rules without an upper bound get INT32_MAX (MC's
IntegerArgumentType.integer(min) is min..Integer.MAX_VALUE).

Skipped, and said so: a rule registered behind a feature flag
(max_minecart_speed needs the minecart-improvements experiment) does not
exist in a vanilla world, so it is not registered here either.

The lang lookup is MC's: `gamerule.minecraft.<id>` first (the keys 26.3 added
for its renamed rules), then `gamerule.<legacyName>`; same for
`.description`. A rule with no text at all fails the run rather than
shipping its id as a label.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAMERULES = ROOT / "minecraft_code_26.3-pre-2/decompiled_net/minecraft/world/level/gamerules/GameRules.java"
REGISTRY_FIX = ROOT / "minecraft_code_26.3-pre-2/decompiled_net/minecraft/util/datafix/fixes/GameRuleRegistryFix.java"
LANG = ROOT / "assets/lang/en_us.json"
OUT = ROOT / "src/common/world/level/GeneratedGameRules.inc"

INT32_MAX = 2147483647

# MC GameRuleCategory -> the C++ enumerator (display names come from lang
# gamerule.category.<name> and live in GameRules.cpp).
CATEGORIES = {
    "PLAYER": "Player",
    "MOBS": "Mobs",
    "SPAWNING": "Spawning",
    "DROPS": "Drops",
    "UPDATES": "Updates",
    "CHAT": "Chat",
    "MISC": "Misc",
}

REGISTER_RE = re.compile(
    r'(\w+)\s*=\s*register(Boolean|Integer)\(\s*"([a-z_]+)"\s*,\s*GameRuleCategory\.(\w+)\s*,\s*([^;]*)\);'
)
RENAME_RE = re.compile(
    r'renameAndFixField\("([A-Za-z]+)",\s*"minecraft:([a-z_]+)",\s*'
    # Inverted before plain: the plain alternative is a prefix of it.
    r'(GameRuleRegistryFix::convertBooleanInverted|GameRuleRegistryFix::convertBoolean|'
    r'GameRuleRegistryFix::convertInteger|\(oldValue\) ->)'
)


def enum_name(rule_id: str) -> str:
    return "".join(part.capitalize() for part in rule_id.split("_"))


def parse_rules() -> list[dict]:
    text = GAMERULES.read_text(encoding="utf-8")
    rules = []
    for m in REGISTER_RE.finditer(text):
        _field, kind, rule_id, category, rest = m.groups()
        if category not in CATEGORIES:
            sys.exit(f"unknown category {category} for {rule_id}")
        args = [a.strip() for a in rest.split(",")]
        row = {
            "id": rule_id,
            "enum": enum_name(rule_id),
            "category": CATEGORIES[category],
            "type": "Bool" if kind == "Boolean" else "Int",
            "featureFlag": None,
        }
        if kind == "Boolean":
            # advance_time / advance_weather default to
            # !SharedConstants.DEBUG_WORLD_RECREATE — a debug flag that is off
            # in a shipped build, so the default is true.
            value = {"true": 1, "false": 0,
                     "!SharedConstants.DEBUG_WORLD_RECREATE": 1}.get(args[0]) if len(args) == 1 else None
            if value is None:
                sys.exit(f"unexpected registerBoolean args for {rule_id}: {args}")
            row["default"] = value
            row["min"] = 0
            row["max"] = 1
        else:
            numbers = []
            for a in args:
                if a.startswith("FeatureFlagSet"):
                    row["featureFlag"] = a
                    continue
                numbers.append(int(a))
            if len(numbers) < 2:
                sys.exit(f"registerInteger without a minimum for {rule_id}: {args}")
            row["default"] = numbers[0]
            row["min"] = numbers[1]
            row["max"] = numbers[2] if len(numbers) > 2 else INT32_MAX
        rules.append(row)
    if not rules:
        sys.exit("no registrations found in GameRules.java")
    return rules


def parse_legacy_names() -> dict[str, tuple[str, bool]]:
    """new id -> (legacy name, inverted). The first rename wins where two old
    names map to one rule (enableCommandBlocks / commandBlocksEnabled)."""
    text = REGISTRY_FIX.read_text(encoding="utf-8")
    out: dict[str, tuple[str, bool]] = {}
    for m in RENAME_RE.finditer(text):
        old, new, converter = m.groups()
        inverted = converter.endswith("convertBooleanInverted")
        out.setdefault(new, (old, inverted))
    if not out:
        sys.exit("no renames found in GameRuleRegistryFix.java")
    return out


def lang_lookup(lang: dict, rule_id: str, legacy: str | None, suffix: str) -> str | None:
    for key in (f"gamerule.minecraft.{rule_id}{suffix}",
                f"gamerule.{legacy}{suffix}" if legacy else None):
        if key and key in lang:
            return lang[key]
    return None


def c_string(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> None:
    rules = parse_rules()
    legacy = parse_legacy_names()
    lang = json.loads(LANG.read_text(encoding="utf-8"))

    rows = []
    skipped = []
    for r in rules:
        if r["featureFlag"]:
            skipped.append(f'{r["id"]} ({r["featureFlag"]})')
            continue
        legacy_name, inverted = legacy.get(r["id"], (None, False))
        label = lang_lookup(lang, r["id"], legacy_name, "")
        if label is None:
            sys.exit(f"no lang label for {r['id']} (legacy {legacy_name})")
        description = lang_lookup(lang, r["id"], legacy_name, ".description") or ""
        rows.append(
            f'GAME_RULE({r["enum"]}, "{r["id"]}", '
            f'{c_string(legacy_name) if legacy_name else "nullptr"}, '
            f'{"true" if inverted else "false"}, '
            f'{r["category"]}, {r["type"]}, {r["default"]}, {r["min"]}, {r["max"]}, '
            f'{c_string(label)}, {c_string(description)})'
        )

    header = [
        "// GENERATED by tools/gen_game_rules.py from MC 26.3 GameRules.java,",
        "// GameRuleRegistryFix.java and assets/lang/en_us.json. Do not edit.",
        "//",
        "// GAME_RULE(Enum, id, legacyName, legacyInverted, Category, Type,",
        "//           defaultValue, minValue, maxValue, label, description)",
        "// Registration order is MC's (alphabetical), which is also the order the",
        "// in-world Edit Game Rules screen lists them in within a category.",
    ]
    if skipped:
        header.append("//")
        header.append("// Not registered (feature-flagged in vanilla, absent from a vanilla world):")
        for s in skipped:
            header.append(f"//   {s}")
    OUT.write_text("\n".join(header) + "\n" + "\n".join(rows) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}: {len(rows)} rules, {len(skipped)} skipped")


if __name__ == "__main__":
    main()
