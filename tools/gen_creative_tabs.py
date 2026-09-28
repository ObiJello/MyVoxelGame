#!/usr/bin/env python3
# tools/gen_creative_tabs.py
#
# Mirrors the creative inventory's tab contents into C++ data:
#
#   * MC 26.3's world/item/CreativeModeTabs.java (minecraft_code_26.3-pre-2):
#     every tab's builder (row, column, title, icon, type, flags, background)
#     and its displayItems generator, statement by statement, in the order the
#     generator emits — colour collections in gameplay or registry order, the
#     weathering-copper families, and the generate* helpers (potions, books,
#     paintings, fireworks, goat horns, stews, ominous bottles, test blocks,
#     light levels, the ominous banner) as special rows the engine expands at
#     tab-build time.
#   * The Aether's AetherCreativeTabs.java and Twilight Forest's
#     TFCreativeTabs.java (mods_reference/, git-ignored): every mod tab's
#     displayItems in order, plus the insertAfter() rows each mod adds to the
#     vanilla tabs.
#   * Every item id each source registers (vanilla Items.java, AetherItems +
#     AetherBlocks, TFItems + TFBlocks), so the engine can tell a vanilla item
#     from a mod item from an engine-only one.
#
# Rows carry REGISTRY IDS, never engine ItemIDs: the screen resolves each id
# against the item registry when it builds the tabs and skips the ones the
# engine does not register, so an item another change registers later shows
# up in its MC position without re-running this script.
#
# Output (overwritten): src/client/renderer/gui/GeneratedCreativeModeTabs.inc
#
# Run: python3 tools/gen_creative_tabs.py

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MC = ROOT / "minecraft_code_26.3-pre-2" / "decompiled_net" / "minecraft"
TABS_JAVA = MC / "world" / "item" / "CreativeModeTabs.java"
ITEMS_JAVA = MC / "world" / "item" / "Items.java"
BLOCK_ITEM_IDS = MC / "references" / "BlockItemIds.java"
ITEM_IDS = MC / "references" / "ItemIds.java"
BLOCK_IDS = MC / "references" / "BlockIds.java"

AETHER = ROOT / "mods_reference" / "aether" / "src" / "main" / "java" / "com" / "aetherteam" / "aether"
AETHER_TABS = AETHER / "item" / "AetherCreativeTabs.java"
AETHER_ITEMS = AETHER / "item" / "AetherItems.java"
AETHER_BLOCKS = AETHER / "block" / "AetherBlocks.java"

TF = ROOT / "mods_reference" / "twilightforest" / "src" / "main" / "java" / "twilightforest"
TF_TABS = TF / "init" / "TFCreativeTabs.java"
TF_ITEMS = TF / "init" / "TFItems.java"
TF_BLOCKS = TF / "init" / "TFBlocks.java"
TF_ENTITIES = TF / "init" / "TFEntities.java"

OUT = ROOT / "src" / "client" / "renderer" / "gui" / "GeneratedCreativeModeTabs.inc"

# ColorCollection field order (the record's component order = forEach order).
COLOR_ORDER = ["white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
               "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black"]
COLOR_ACCESSORS = {"white": "white", "orange": "orange", "magenta": "magenta", "lightBlue": "light_blue",
                   "yellow": "yellow", "lime": "lime", "pink": "pink", "gray": "gray",
                   "lightGray": "light_gray", "cyan": "cyan", "purple": "purple", "blue": "blue",
                   "brown": "brown", "green": "green", "red": "red", "black": "black"}
COPPER_WEATHERING = ["", "exposed_", "weathered_", "oxidized_"]
COPPER_WAXED = ["waxed_", "waxed_exposed_", "waxed_weathered_", "waxed_oxidized_"]
COPPER_STATES = {"unaffected": 0, "exposed": 1, "weathered": 2, "oxidized": 3}

# TF registry names the engine registers under another id (the vanilla
# mangrove set and furnace own the plain names) — levelgen/TwilightBlocks.cpp.
TF_RENAMES = {
    "mangrove_log": "tf_mangrove_log", "mangrove_wood": "tf_mangrove_wood",
    "mangrove_leaves": "tf_mangrove_leaves", "mangrove_sapling": "tf_mangrove_sapling",
    "mangrove_planks": "tf_mangrove_planks", "stripped_mangrove_log": "stripped_tf_mangrove_log",
    "stripped_mangrove_wood": "stripped_tf_mangrove_wood", "mangrove_stairs": "tf_mangrove_stairs",
    "mangrove_slab": "tf_mangrove_slab", "mangrove_fence": "tf_mangrove_fence",
    "mangrove_fence_gate": "tf_mangrove_fence_gate", "mangrove_door": "tf_mangrove_door",
    "mangrove_trapdoor": "tf_mangrove_trapdoor", "mangrove_button": "tf_mangrove_button",
    "mangrove_pressure_plate": "tf_mangrove_pressure_plate", "mangrove_root": "tf_mangrove_root",
    "mangrove_banister": "tf_mangrove_banister", "smoker": "tf_smoker",
}


def die(msg: str) -> None:
    print(f"gen_creative_tabs: {msg}", file=sys.stderr)
    sys.exit(1)


# ── Vanilla item ids ─────────────────────────────────────────────────────────

def load_ids(path: Path) -> dict[str, str]:
    """FIELD -> item id for BlockItemIds / ItemIds (create("x") or create("block", "item"))."""
    text = path.read_text()
    out: dict[str, str] = {}
    for m in re.finditer(r'\b([A-Z0-9_]+)\s*=\s*(?:BlockItemId\.)?create\("([a-z0-9_/]+)"(?:,\s*"([a-z0-9_/]+)")?\)', text):
        out[m.group(1)] = m.group(3) or m.group(2)
    return out


def load_vanilla_items() -> tuple[dict[str, str], list[str]]:
    """Items.FIELD -> id for plain items, and every vanilla item id in Items.java order."""
    block_ids = load_ids(BLOCK_ITEM_IDS)
    item_ids = load_ids(ITEM_IDS)
    text = ITEMS_JAVA.read_text()
    field_to_id: dict[str, str] = {}
    all_ids: list[str] = []
    collections: dict[str, str] = {}
    for m in re.finditer(r'^\s{6}([A-Z0-9_]+) = ([\w.]+)\((BlockItemIds|ItemIds)\.([A-Z0-9_]+)', text, re.M):
        field, call, table, key = m.groups()
        if "Collection" in call:
            collections[field] = call
            continue
        ids = block_ids if table == "BlockItemIds" else item_ids
        field_to_id[field] = ids.get(key, key.lower())
    # Collections expand to their 16 / 8 members (ids from the builders).
    for field, call in collections.items():
        for item_id in expand_collection(field, call):
            all_ids.append(item_id)
    all_ids = list(field_to_id.values()) + all_ids
    return field_to_id, all_ids


def colored_id(field: str, color: str) -> str:
    base = {
        "WOOL": "wool", "WOOL_STAIRS": "wool_stairs", "WOOL_SLAB": "wool_slab",
        "DYED_TERRACOTTA": "terracotta", "CARPET": "carpet", "STAINED_GLASS": "stained_glass",
        "STAINED_GLASS_PANE": "stained_glass_pane", "DYED_SHULKER_BOX": "shulker_box",
        "GLAZED_TERRACOTTA": "glazed_terracotta", "CONCRETE": "concrete",
        "CONCRETE_STAIRS": "concrete_stairs", "CONCRETE_SLAB": "concrete_slab",
        "CONCRETE_POWDER": "concrete_powder", "HARNESS": "harness", "DYED_BUNDLE": "bundle",
        "CUSHION": "cushion", "DYE": "dye", "BED": "bed", "BANNER": "banner", "DYED_CANDLE": "candle",
    }.get(field)
    if base is None:
        die(f"unknown ColorCollection Items.{field}")
    return f"{color}_{base}"


def copper_id(field: str, waxed: bool, state: int) -> str:
    if field == "COPPER_BLOCK":
        names = ["copper_block", "copper", "copper", "copper"]
        return (COPPER_WAXED if waxed else COPPER_WEATHERING)[state] + names[state]
    return (COPPER_WAXED if waxed else COPPER_WEATHERING)[state] + field.lower()


def expand_collection(field: str, call: str) -> list[str]:
    if call.startswith("ColorCollection"):
        return [colored_id(field, c) for c in COLOR_ORDER]
    return ([copper_id(field, False, s) for s in range(4)] +
            [copper_id(field, True, s) for s in range(4)])


def load_vanilla_block_only_ids(item_ids: set[str]) -> list[str]:
    """Every vanilla BLOCK id that is not also an item id (wall signs, crops,
    potted plants, candle cakes, fluids...): BlockIds.java plus the block half
    of BlockItemIds' two-name rows. The engine registers these as blocks with
    no item form, and the tab builder must not mistake them for engine items."""
    ids = set(re.findall(r'create\("([a-z0-9_]+)"', BLOCK_IDS.read_text()))
    for base in re.findall(r'createSimpleColored\("([a-z0-9_]+)"\)', BLOCK_IDS.read_text()):
        ids |= {f"{c}_{base}" for c in COLOR_ORDER}
    ids |= set(re.findall(r'BlockItemId\.create\("([a-z0-9_]+)"', BLOCK_ITEM_IDS.read_text()))
    return sorted(ids - item_ids)


# ── Vanilla tabs ─────────────────────────────────────────────────────────────

class Tab:
    def __init__(self, key: str):
        self.key = key
        self.row = "Top"
        self.column = 0
        self.title = ""
        self.icon = ""
        self.type = "Category"
        self.aligned_right = False
        self.show_title = True
        self.can_scroll = True
        self.background = "items"
        self.requires_permissions = False
        self.entries: list[tuple[str, str, str, str]] = []   # (kind, id, visibility, extra)


def parse_vanilla_tabs(field_to_id: dict[str, str]) -> list[Tab]:
    text = TABS_JAVA.read_text()
    m = re.search(r'List<DyeColor> gameplayColorOrder = List\.of\(([^)]*)\)', text)
    if not m:
        die("gameplayColorOrder not found")
    gameplay = [c.strip().replace("DyeColor.", "").lower() for c in m.group(1).split(",")]

    def item(field: str) -> str:
        if field not in field_to_id:
            die(f"Items.{field} has no id")
        return field_to_id[field]

    tabs: list[Tab] = []
    starts = [mm for mm in re.finditer(r'Registry\.register\(registry, \(ResourceKey\)([A-Z_]+), CreativeModeTab\.builder\(CreativeModeTab\.Row\.(TOP|BOTTOM), (\d+)\)', text)]
    for i, st in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else text.find("private static void registerColoredItems")
        body = text[st.start():end]
        tab = Tab(st.group(1).lower())
        tab.row = "Top" if st.group(2) == "TOP" else "Bottom"
        tab.column = int(st.group(3))
        tm = re.search(r'Component\.translatable\("([^"]+)"\)', body)
        tab.title = tm.group(1) if tm else ""
        im = re.search(r'return new ItemStack\(([^;]*)\);', body)
        icon_expr = im.group(1) if im else ""
        cm = re.search(r'\(ItemLike\)Blocks\.([A-Z_]+)\.([a-zA-Z]+)\(\)', icon_expr)
        if cm:
            tab.icon = colored_id(cm.group(1), COLOR_ACCESSORS[cm.group(2)])
        else:
            fm = re.search(r'(?:Items|Blocks)\.([A-Z0-9_]+)', icon_expr)
            tab.icon = item(fm.group(1)) if fm else ""
        if ".alignedRight()" in body:
            tab.aligned_right = True
        if ".hideTitle()" in body:
            tab.show_title = False
        if ".noScrollBar()" in body:
            tab.can_scroll = False
        tym = re.search(r'\.type\(CreativeModeTab\.Type\.([A-Z]+)\)', body)
        if tym:
            tab.type = tym.group(1).capitalize()
        bgm = re.search(r'\.backgroundTexture\(([A-Z_]+)\)', body)
        if bgm:
            tab.background = {"INVENTORY_BACKGROUND": "inventory", "SEARCH_BACKGROUND": "item_search"}[bgm.group(1)]
        if "parameters.hasPermissions()" in body:
            tab.requires_permissions = True
        if tab.type == "Category":
            parse_display_items(tab, body, item, gameplay)
        tabs.append(tab)
    return tabs


def parse_display_items(tab: Tab, body: str, item, gameplay: list[str]) -> None:
    lines = body.splitlines()
    pending_collection: str | None = None
    i = 0
    add = tab.entries.append
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        m = re.match(r'\w+\.accept\(\(ItemLike\)Items\.([A-Z0-9_]+)\);$', line)
        if m:
            add(("Item", item(m.group(1)), "ParentAndSearch", ""))
            continue
        m = re.match(r'\w+\.accept\(\(ItemLike\)Items\.([A-Z0-9_]+)\.([a-zA-Z]+)\(\)\);$', line)
        if m:   # Items.WOOL.white()
            add(("Item", colored_id(m.group(1), COLOR_ACCESSORS[m.group(2)]), "ParentAndSearch", ""))
            continue
        m = re.match(r'\w+\.accept\(\(ItemLike\)Items\.([A-Z0-9_]+)\.(waxed|weathering)\(\)\.([a-z]+)\(\)\);$', line)
        if m:   # Items.LIGHTNING_ROD.waxed().unaffected()
            add(("Item", copper_id(m.group(1), m.group(2) == "waxed", COPPER_STATES[m.group(3)]), "ParentAndSearch", ""))
            continue
        m = re.match(r'registerColoredItems\(\w+, gameplayColorOrder, Items\.([A-Z0-9_]+)\);$', line)
        if m:
            for c in gameplay:
                add(("Item", colored_id(m.group(1), c), "ParentAndSearch", ""))
            continue
        m = re.match(r'(?:(?:WeatheringCopperCollection(?:\.ByState)?|ColorCollection) )?var10000 = Items\.([A-Z0-9_]+)(?:\.(waxed|weathering)\(\))?;$', line)
        if m:
            pending_collection = m.group(1) + ("." + m.group(2) if m.group(2) else "")
            continue
        m = re.match(r'var10000\.forEach\(\w+::accept\);$', line)
        if m:
            if pending_collection is None:
                die(f"{tab.key}: forEach without a collection")
            field, _, part = pending_collection.partition(".")
            if field in ("DYED_BUNDLE", "HARNESS", "DYE", "WOOL", "BED", "BANNER", "CUSHION", "DYED_CANDLE",
                         "DYED_SHULKER_BOX", "CARPET"):
                for c in COLOR_ORDER:
                    add(("Item", colored_id(field, c), "ParentAndSearch", ""))
            else:
                states = []
                if part in ("", "weathering"):
                    states += [(False, s) for s in range(4)]
                if part in ("", "waxed"):
                    states += [(True, s) for s in range(4)]
                for waxed, s in states:
                    add(("Item", copper_id(field, waxed, s), "ParentAndSearch", ""))
            pending_collection = None
            continue
        m = re.match(r'Items\.DYE\.forEach\(\(dye\) -> \{$', line)
        if m:
            for c in COLOR_ORDER:
                add(("Item", colored_id("DYE", c), "ParentAndSearch", ""))
            i += 2   # the accept line and the closing brace
            continue
        m = re.match(r'copperBlockFamilies\(\(family\) -> \{$', line)
        if m:
            # WeatheringCopperCollection.ByState var10000 = family.weathering(); / waxed()
            which = lines[i].strip()
            waxed = "family.waxed()" in which
            for fam in COPPER_FAMILIES:
                for s in range(4):
                    add(("Item", copper_id(fam, waxed, s), "ParentAndSearch", ""))
            i += 4
            continue
        m = re.search(r'generatePresetPaintings\(\w+, parameters\.holders\(\), paintings, \(variant\) -> \{', line)
        if m:
            neg = "!variant.is(PaintingVariantTags.PLACEABLE)" in lines[i]
            add(("OpPaintings" if neg else "PresetPaintings", "painting", "ParentAndSearch", ""))
            continue
        m = re.match(r'generateFireworksAllDurations\(\w+, CreativeModeTab\.TabVisibility\.([A-Z_]+)\);$', line)
        if m:
            add(("Fireworks", "firework_rocket", vis(m.group(1)), ""))
            continue
        m = re.search(r'generateInstrumentTypes\(\w+, instruments, Items\.([A-Z_]+), InstrumentTags\.([A-Z_]+), CreativeModeTab\.TabVisibility\.([A-Z_]+)\)', line)
        if m:
            add(("Instruments", item(m.group(1)), vis(m.group(3)), m.group(2).lower()))
            continue
        m = re.match(r'generateSuspiciousStews\(\w+, CreativeModeTab\.TabVisibility\.([A-Z_]+)\);$', line)
        if m:
            add(("SuspiciousStews", "suspicious_stew", vis(m.group(1)), ""))
            continue
        m = re.match(r'generateOminousBottles\(\w+, CreativeModeTab\.TabVisibility\.([A-Z_]+)\);$', line)
        if m:
            add(("OminousBottles", "ominous_bottle", vis(m.group(1)), ""))
            continue
        m = re.search(r'generatePotionEffectTypes\(\w+, potions, Items\.([A-Z_]+), CreativeModeTab\.TabVisibility\.([A-Z_]+)', line)
        if m:
            add(("Potions", item(m.group(1)), vis(m.group(2)), ""))
            continue
        m = re.search(r'generateEnchantmentBookTypes(OnlyMaxLevel|AllLevels)\(\w+, enchantments, CreativeModeTab\.TabVisibility\.([A-Z_]+)\)', line)
        if m:
            kind = "EnchantedBooksMax" if m.group(1) == "OnlyMaxLevel" else "EnchantedBooksAll"
            add((kind, "enchanted_book", vis(m.group(2)), ""))
            continue
        if "Raid.getOminousBannerInstance" in line:
            add(("OminousBanner", "white_banner", "ParentAndSearch", ""))
            continue
        if "TestBlock.setModeOnStack(new ItemStack(Items.TEST_BLOCK), mode)" in line:
            add(("TestBlockModes", "test_block", "ParentAndSearch", ""))
            continue
        if "LightBlock.setLightOnStack(new ItemStack(Items.LIGHT), lightLevel)" in line:
            add(("LightLevels", "light", "ParentAndSearch", ""))
            continue
        # Everything else must be scaffolding; anything that looks like an
        # accept() we did not understand is a hard error, so a new MC
        # construct can never be dropped silently.
        if ".accept(" in line or "generate" in line:
            die(f"{tab.key}: unhandled statement: {line}")


def vis(java: str) -> str:
    return {"PARENT_AND_SEARCH_TABS": "ParentAndSearch", "PARENT_TAB_ONLY": "ParentOnly",
            "SEARCH_TAB_ONLY": "SearchOnly"}[java]


def load_copper_families() -> list[str]:
    text = TABS_JAVA.read_text()
    m = re.search(r'private static void copperBlockFamilies\([^)]*\) \{(.*?)\n   \}', text, re.S)
    if not m:
        die("copperBlockFamilies not found")
    return re.findall(r'output\.accept\(Items\.([A-Z_]+)\);', m.group(1))


COPPER_FAMILIES: list[str] = []


# ── Mod registries and tabs ──────────────────────────────────────────────────

def load_mod_items(*paths: Path) -> dict[str, str]:
    """(Class.FIELD) -> registry name, e.g. 'AetherBlocks.SKYROOT_LOG' -> 'skyroot_log'."""
    out: dict[str, str] = {}
    for p in paths:
        cls = p.stem
        text = p.read_text()
        for m in re.finditer(r'\b([A-Z0-9_]+)\s*=\s*[\w.]*?register\w*\(\s*"([a-z0-9_/]+)"', text):
            out.setdefault(f"{cls}.{m.group(1)}", m.group(2))
    return out


def parse_aether(names: dict[str, str]) -> tuple[list[tuple], list[tuple]]:
    text = AETHER_TABS.read_text()
    entries: list[tuple] = []
    seen: set[str] = set()
    for tm in re.finditer(r'CREATIVE_MODE_TABS\.register\("([a-z_]+)".*?\}\)\.build\(\)\);', text, re.S):
        for line in tm.group(0).splitlines():
            m = re.search(r'output\.accept\((Aether(?:Blocks|Items))\.([A-Z0-9_]+)\.get\(\)\)', line)
            if m:
                key = f"{m.group(1)}.{m.group(2)}"
                if key not in names:
                    die(f"aether: {key} has no registry name")
                name = names[key]
                if name not in seen:
                    seen.add(name)
                    entries.append(("Item", name, "ParentAndSearch", ""))
            elif "createSwetBannerItemStack" in line:
                # A banner with the swet pattern — needs BANNER_PATTERNS.
                if "swet_banner" not in seen:
                    seen.add("swet_banner")
                    entries.append(("SwetBanner", "white_banner", "ParentAndSearch", ""))
            elif "output.accept(" in line:
                die(f"aether: unhandled statement: {line.strip()}")
    inserts: list[tuple] = []
    em = re.search(r'public static void buildCreativeModeTabs\(.*', text, re.S)
    if em:
        tab = None
        for line in em.group(0).splitlines():
            tmm = re.search(r'tab == CreativeModeTabs\.([A-Z_]+)', line)
            if tmm:
                tab = tmm.group(1).lower()
            m = re.search(r'insertAfter\(new ItemStack\(Items\.([A-Z0-9_]+)\), new ItemStack\((Aether(?:Blocks|Items))\.([A-Z0-9_]+)\.get\(\)\)', line)
            if m:
                inserts.append((tab, m.group(1), names[f"{m.group(2)}.{m.group(3)}"]))
    return entries, inserts


def parse_tf(names: dict[str, str]) -> tuple[list[tuple], list[tuple]]:
    text = TF_TABS.read_text()
    entries: list[tuple] = []
    seen: set[str] = set()

    def tf_name(cls_field: str) -> str:
        if cls_field not in names:
            die(f"twilightforest: {cls_field} has no registry name")
        n = names[cls_field]
        return TF_RENAMES.get(n, n)

    def add(kind: str, name: str, extra: str = "") -> None:
        key = name + "|" + extra + "|" + kind
        if key in seen:
            return
        seen.add(key)
        entries.append((kind, name, "ParentAndSearch", extra))

    body_end = text.find("private static void generateGearWithEnchants")
    tabs_text = text[:body_end]
    for line in tabs_text.splitlines():
        s = line.strip()
        m = re.match(r'output\.accept\((TF(?:Blocks|Items))\.([A-Z0-9_]+)(?:\.get\(\)\.getDefaultInstance\(\))?\);$', s)
        if m:
            add("Item", tf_name(f"{m.group(1)}.{m.group(2)}"))
            continue
        m = re.match(r'generateGearWithEnchants\(output, (TF(?:Blocks|Items))\.([A-Z0-9_]+), (.*)\);$', s)
        if m:
            ench = re.findall(r'Enchantments\.([A-Z_]+)\), (\d+)\)', m.group(3))
            extra = ",".join(f"{e.lower()}:{lvl}" for e, lvl in ench)
            add("EnchantedItem", tf_name(f"{m.group(1)}.{m.group(2)}"), extra)
            continue
        m = re.match(r'createDefaultSkullCandle\(output, (TF(?:Blocks|Items))\.([A-Z0-9_]+)\);$', s)
        if m:
            add("Item", tf_name(f"{m.group(1)}.{m.group(2)}"))
            continue
        if s == "createCaskets(output);":
            add("Item", tf_name("TFItems.KEEPSAKE_CASKET"))
            continue
        if s == "createSpawnEggsAlphabetical(output);":
            for egg in tf_spawn_eggs():
                add("Item", egg)
            continue
        if s == "createGlassSwordAndLoreVer(output);":
            add("Item", tf_name("TFItems.GLASS_SWORD"))
            continue
        if "output.accept(" in s or s.startswith("generate") or s.startswith("create"):
            die(f"twilightforest: unhandled statement: {s}")
    inserts: list[tuple] = []
    em = re.search(r'public static void addToTabs\(.*', text, re.S)
    if em:
        tab = None
        for line in em.group(0).splitlines():
            tmm = re.search(r'getTabKey\(\) == CreativeModeTabs\.([A-Z_]+)', line)
            if tmm:
                tab = tmm.group(1).lower()
            m = re.search(r'insertAfter\(new ItemStack\(Items\.([A-Z0-9_]+)\), (TF(?:Blocks|Items))\.([A-Z0-9_]+)\.toStack\(\)', line)
            if m:
                inserts.append((tab, m.group(1), tf_name(f"{m.group(2)}.{m.group(3)}")))
    return entries, inserts


def tf_spawn_eggs() -> list[str]:
    """TFEntities.registerWithEgg("name", ...) rows in declaration order — the
    SPAWN_EGGS DeferredRegister's entry order, which createSpawnEggsAlphabetical
    streams (the declarations are kept alphabetical)."""
    text = TF_ENTITIES.read_text()
    live = "\n".join(l for l in text.splitlines() if not l.lstrip().startswith("//"))
    return [f"{n}_spawn_egg" for n in re.findall(r'registerWithEgg\(\s*"([a-z0-9_]+)"', live)]


# ── Emit ─────────────────────────────────────────────────────────────────────

def cstr(s: str) -> str:
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def emit(tabs: list[Tab], vanilla_items: list[str], vanilla_blocks: list[str], aether_tab: list[tuple], aether_ins: list[tuple],
         aether_all: list[str], tf_tab: list[tuple], tf_ins: list[tuple], tf_all: list[str],
         field_to_id: dict[str, str]) -> str:
    out: list[str] = []
    w = out.append
    w("// File: src/client/renderer/gui/GeneratedCreativeModeTabs.inc")
    w("// AUTO-GENERATED by tools/gen_creative_tabs.py — DO NOT EDIT BY HAND.")
    w("// Sources: minecraft_code_26.3-pre-2 CreativeModeTabs.java / Items.java,")
    w("// mods_reference AetherCreativeTabs.java + TFCreativeTabs.java.")
    w("// Included by CreativeModeTabs.cpp only (inside namespace Render::CreativeTabData).")
    w("")
    w("// ── Vanilla tabs, registry order (MC CreativeModeTabs.bootstrap) ──")
    for tab in tabs:
        name = "k" + "".join(p.capitalize() for p in tab.key.split("_"))
        w(f"static const CreativeEntry {name}Entries[] = {{")
        if not tab.entries:
            w("    {EntryKind::Item, nullptr, Visibility::ParentAndSearch, nullptr},")
        for kind, item_id, v, extra in tab.entries:
            w(f"    {{EntryKind::{kind}, {cstr(item_id)}, Visibility::{v}, {cstr(extra) if extra else 'nullptr'}}},")
        w("};")
    w("")
    w("static const VanillaTabDef kVanillaTabs[] = {")
    for tab in tabs:
        name = "k" + "".join(p.capitalize() for p in tab.key.split("_"))
        count = len(tab.entries)
        w(f"    {{{cstr(tab.key)}, Row::{tab.row}, {tab.column}, {cstr(tab.title)}, {cstr(tab.icon)}, "
          f"TabType::{tab.type}, {str(tab.aligned_right).lower()}, {str(tab.show_title).lower()}, "
          f"{str(tab.can_scroll).lower()}, {cstr(tab.background)}, {str(tab.requires_permissions).lower()}, "
          f"{name}Entries, {count}}},")
    w("};")
    w("")
    w("// ── Every vanilla item id (Items.java declaration order) ──")
    w("static const char* const kVanillaItemIds[] = {")
    for i in range(0, len(vanilla_items), 6):
        w("    " + " ".join(cstr(x) + "," for x in vanilla_items[i:i + 6]))
    w("};")
    w("")
    w("// ── Every vanilla block id with no item form (BlockIds.java + the block")
    w("//    half of BlockItemIds' two-name rows) ──")
    w("static const char* const kVanillaBlockOnlyIds[] = {")
    for i in range(0, len(vanilla_blocks), 6):
        w("    " + " ".join(cstr(x) + "," for x in vanilla_blocks[i:i + 6]))
    w("};")
    w("")

    def mod_block(prefix: str, tab_entries: list[tuple], inserts: list[tuple], all_ids: list[str]) -> None:
        w(f"static const CreativeEntry k{prefix}TabEntries[] = {{")
        for kind, item_id, v, extra in tab_entries:
            w(f"    {{EntryKind::{kind}, {cstr(item_id)}, Visibility::{v}, {cstr(extra) if extra else 'nullptr'}}},")
        w("};")
        w(f"// The mod's own insertAfter() rows into the vanilla tabs (tab, after, item).")
        w(f"static const ModInsert k{prefix}Inserts[] = {{")
        if not inserts:
            w("    {nullptr, nullptr, nullptr},")
        for tab, after_field, item_id in inserts:
            w(f"    {{{cstr(tab)}, {cstr(field_to_id.get(after_field, after_field.lower()))}, {cstr(item_id)}}},")
        w("};")
        w(f"static const size_t k{prefix}InsertCount = {len(inserts)};")
        w(f"// Every item id the mod registers (to tell its items from engine ones).")
        w(f"static const char* const k{prefix}ItemIds[] = {{")
        for i in range(0, len(all_ids), 6):
            w("    " + " ".join(cstr(x) + "," for x in all_ids[i:i + 6]))
        w("};")
        w("")

    w("// ── The Aether (AetherCreativeTabs: building, dungeon, natural, functional,")
    w("//    redstone, equipment & utilities, armour & accessories, food & drinks,")
    w("//    ingredients, spawn eggs — concatenated, first occurrence kept) ──")
    mod_block("Aether", aether_tab, aether_ins, aether_all)
    w("// ── Twilight Forest (TFCreativeTabs: blocks, items, equipment, food —")
    w("//    concatenated, first occurrence kept) ──")
    mod_block("Twilight", tf_tab, tf_ins, tf_all)
    return "\n".join(out) + "\n"


def main() -> None:
    global COPPER_FAMILIES
    for p in (TABS_JAVA, ITEMS_JAVA, BLOCK_ITEM_IDS, ITEM_IDS, BLOCK_IDS, AETHER_TABS, TF_TABS):
        if not p.exists():
            die(f"missing {p}")
    COPPER_FAMILIES = load_copper_families()
    field_to_id, vanilla_items = load_vanilla_items()
    tabs = parse_vanilla_tabs(field_to_id)

    # A mod item whose registry path is also a vanilla item's (TF's own
    # mangrove sign and boats) cannot be told apart in the engine's single
    # namespace unless the engine registers it under another id (TF_RENAMES);
    # such rows would put the VANILLA item in the mod's tab, so they go.
    vanilla = set(vanilla_items)

    def mod_only(rows: list[tuple]) -> list[tuple]:
        return [r for r in rows if r[1] not in vanilla]

    aether_names = load_mod_items(AETHER_ITEMS, AETHER_BLOCKS)
    aether_tab, aether_ins = parse_aether(aether_names)
    aether_tab = [r for r in aether_tab if r[0] == "SwetBanner" or r[1] not in vanilla]
    aether_ins = [r for r in aether_ins if r[2] not in vanilla]
    aether_all = sorted(set(aether_names.values()) - vanilla)

    tf_names = load_mod_items(TF_ITEMS, TF_BLOCKS)
    tf_tab, tf_ins = parse_tf(tf_names)
    tf_tab = mod_only(tf_tab)
    tf_ins = [r for r in tf_ins if r[2] not in vanilla]
    tf_all = sorted(({TF_RENAMES.get(n, n) for n in tf_names.values()} | {e[1] for e in tf_tab} |
                     set(tf_spawn_eggs())) - vanilla)

    vanilla_blocks = load_vanilla_block_only_ids(set(vanilla_items))
    text = emit(tabs, vanilla_items, vanilla_blocks, aether_tab, aether_ins, aether_all, tf_tab, tf_ins, tf_all, field_to_id)
    OUT.write_text(text)
    total = sum(len(t.entries) for t in tabs)
    print(f"wrote {OUT.relative_to(ROOT)}: {len(tabs)} vanilla tabs, {total} rows, "
          f"{len(vanilla_items)} vanilla ids, aether {len(aether_tab)} rows, tf {len(tf_tab)} rows")


if __name__ == "__main__":
    main()
