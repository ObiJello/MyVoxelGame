# Play-test route

The current build supports local survival and creative play and the console
tutorial. It is still an incomplete port: full mob AI, armour, redstone and the
Nether portal trip are not playable yet. Crafting has all 222 original recipes.

## Start

From `/Users/obey/Desktop/MyVoxelGame`, run:

```sh
bash console_port/run.command
```

Choose **Play Game**, then **Classic Tutorial World** for the free-exploration
checks below (terrain, trees, chests); existing saved chunks are deliberately not
rewritten. Keep an older save for the reload and preservation check. The tutorial
itself is **Play Tutorial** — see the tutorial route at the end.

## Test this build

1. Walk around the starting tutorial area with WASD and mouse look. Jump with Space.
   Check nearby trees, grass and other natural terrain around the castle. Press F to
   fly, cross several chunk boundaries in different directions, and return. Note
   any pause longer than about a second or missing terrain, and confirm the starting
   structures and your edits remain intact.
2. Walk on stairs and slabs, climb a ladder, and right-click a wooden door. Confirm
   the door opens, can be walked through, and closes on the next right-click.
3. Find a fence gate in the tutorial structures. Confirm a closed gate blocks you,
   right-click opens it, the center becomes passable, and another right-click closes it.
4. Press E. Select **Fence Gate** from Building Blocks or **Oak Fence** and
   **Nether Brick Fence** from Miscellaneous; the choice
   fills a free hotbar slot, or replaces the selected slot if the bar is full.
   Place fence lines and corners. Confirm rails join
   neighboring fences, solid blocks and gates. Confirm a gate faces your viewing direction.
   Check that Nether Brick Fence is dark red brick, without bright pink faces.
5. Inspect stair runs and corners in the tutorial structures. Confirm both shape
   and texture are correct, without bright pink faces. Note the block type and
   position of any remaining pink stair.
6. Right-click a chest. Use Enter for a whole stack, H for half, and R for one item.
   Right-clicking a slot should also move half. Move matching stacks repeatedly and
   confirm they merge without exceeding 64, 16 or 1 according to the item.
7. Open an adjacent pair of chests and confirm the screen shows 54 slots. Transfer
   items through both halves, close it, and reopen either half. The contents and order
   should match.
8. Pause and choose **Save Game**, then **Save and Exit**. Load that world again and
   check your placed fences/gates, gate open state, chest contents and carried inventory.
9. Continue flying toward the edge of the finite console world. Confirm chunks keep
   streaming during travel and the game prevents crossing beyond the world boundary.
10. Open the creative inventory with E. Select sand from Building Blocks, coal
    from Materials, and a furnace from Miscellaneous. Place the furnace and open it
    with right-click/L2, move sand into the input and press F/L3 to select the fuel
    slot for coal. Confirm the front lights, glass appears after about ten seconds,
    and the cook progress, fuel, and output survive Save Game and reload.
11. Select a cauldron and water bucket from Miscellaneous, then place the cauldron
    and use the bucket on it. Select a glass bottle from Brewing and use it
    once; the water should drop by one level and a water potion should appear in
    the carried inventory. Save and reload to check that the water level remains.
12. Select a brewing stand from Miscellaneous and place it. Brewing supplies
    water potions; Materials supplies nether wart and sugar. Open the stand,
    insert a potion and nether wart, and confirm an awkward potion appears after
    twenty seconds. Add sugar to brew a speed potion. The stand model should show
    a bottle when its slot is occupied, and the brew countdown should survive a
    save and reload. Check that its background, bubbles, and arrow match the
    supplied PS3 scene artwork, and that the speed potion has blue contents.
    Take the brewed speed potion from the stand; it should become the selected
    quickbar item. Hold right mouse or L2 for about 1.6 seconds to drink it,
    then walk and confirm the speed increase. Save and reopen the world; the
    effect should still be active for its remaining duration.
13. Move a tutorial chest item into one of the first nine inventory slots. Close
    the chest and confirm that the same item appears in the quickbar and remains
    there after saving and reopening the world. Select a creative block with E;
    it should fill a free slot without erasing the chest item. Shift-selecting a
    creative stack should fill it to the item's source stack limit.
    In the ninth Inventory category, select a backpack slot and press Enter
    or Cross to swap it with the active quickbar slot.
14. Browse all eight source creative tabs with Tab or L1/R1. Use Page Up/Down,
    [ / ], or the mouse wheel to reach later catalog pages. Check that the
    source sandstone, wood, slab, potion, skull, dye, and spawn-egg variants
    are selectable and have distinct names. Note any missing or
    indistinguishable icons or incorrect names.
15. Select the creative spawn eggs from Miscellaneous and right-click a clear
    block. All 21 catalog creatures should appear and survive Save and Exit.
    Slime and Magma Cube should retain their size; Mooshroom should have its
    red cow skin and three mushrooms. Species AI and attacks are incomplete.

Please report the step number, what happened, keyboard or controller input, and the
on-screen position shown at the upper left. A short description is enough; no save
file is needed unless the failure only appears after reload.

## Survival route

1. **Play Game → Create New World**, leave **Game Mode: Survival**, create it.
   Confirm hearts, food and the XP bar sit above the hotbar and F does not fly.
2. Punch a tree: the crack overlay should grow over about 3 seconds, then an oak
   log pops out and is collected when you walk over it. Grass/dirt drop dirt;
   stone takes about 7.5 s by hand and drops nothing.
3. Press **C**: craft planks, sticks and a crafting table (Tab / L1 R1 switch
   between the seven recipe groups). Place the table and right-click it to see the
   3×3 recipes; make a wooden pickaxe from the Tools group. Stone should now
   break in about a second and drop cobblestone; the pickaxe wears down.
4. Jump off a 6+ block drop (damage), stand under water until the bubbles run out
   (drowning), and step next to lava (burning). Eat by holding right mouse with
   food selected.
5. Press **Q** to throw one item, **Ctrl+Q** for the stack. Die, confirm the
   death screen, respawn, and walk back to collect your dropped items.
6. Save and Exit, reload: you should resume where you stood, still in survival.

## Menus

1. On the title screen move through the four buttons with the arrow keys and with the
   mouse. Minecraft Store should say there are no offers; Leaderboards explains PSN is
   unavailable.
2. Help & Options → How To Play: open several topics, page with Enter / X, scroll long
   pages with Up/Down. Controls: move between the three layouts and check the button
   labels change; toggle Invert Look and Southpaw.
3. Settings: change Autosave, sensitivity, Display HUD and In-Game Tooltips, leave with
   Esc, quit, restart and confirm they stuck (`settings.dat` in the data folder).
   Reset to Defaults: Cancel keeps them, OK resets them.
4. Credits should roll and return with Esc.
5. In a world, Esc opens the pause menu: Save Game asks to overwrite, Exit Game offers
   Exit and save / Exit without saving.

## Tutorial route

Choose **Play Game → Play Tutorial**. Follow the popups from the overview through
moving, looking, jumping, mining wood, crafting planks, sticks and a crafting table,
tools, the furnace, the food bar, the night shelter, and the areas beyond (boats,
farming, redstone, brewing, enchanting, the Nether portal and the music discs). Note
any lesson that does not advance, any popup whose button images are wrong for the
chosen controller layout, and any place the area constraint lets you leave early.
Start a second tutorial afterwards: lessons you completed should be skipped.

## Farming and growth

In a survival world: break tall grass for seeds, till grass or dirt next to water with
a hoe (right click), plant seeds on the farmland, and wait. Wheat should grow through
its eight stages over a few in-game days (faster on wet farmland and in rows), and
bone meal (right click) should ripen it at once. Plant a sapling and wait (or use
bone meal) for a tree. Cut a tree's trunk and watch its leaves decay. Also check that
farmland dries out and turns back to dirt away from water, and that pumpkin and melon
stems bend toward the fruit they grow. Note anything that grows far faster or slower
than you remember.

## Blocks reacting to each other

Pour water (a water bucket) on a slope and watch it run downhill and settle; pour
lava next to water and check that obsidian or cobblestone forms. Stack sand or gravel
in the air and watch it fall and land. Put a torch on a block and break the block
under it. Break the top half of a door (in survival it should drop one door). Light
fire on netherrack or wood with flint and steel and watch it spread and burn wood,
leaves and wool, and burn out on stone. The flames should flicker. Note anything
that flows, falls or burns differently from how you remember it.

## Redstone

Place a lever, a line of redstone dust and a redstone lamp; flip the lever and check
the dust brightens (fainter further along) and the lamp lights, then goes out a moment
after you flip it back. Put a redstone torch on the side of a block and power the block:
the torch should go out. Try a repeater (right click changes its delay, and the torches
on it move), stone and wooden buttons, wooden and stone pressure plates (stand on
them), and doors, trapdoors and fence gates opened by a signal. Note anything that
powers, delays or looks different from how you remember it.

## Pistons

Build a piston and a sticky piston with a lever behind each and a block in front.
Flip the levers: the pistons should push the blocks out one cell and the sticky piston
should pull its block back when switched off. Try pushing a row of blocks, standing in
front of a piston (it should shove you), and pushing obsidian or a chest (neither
moves). Note anything that moves too far, too fast, or looks wrong while moving.

## TNT

Light TNT with flint and steel and step back: it should drop out as a block that
flashes white, swells at the end and explodes after four seconds, leaving a crater
(obsidian and bedrock stay). Try TNT beside TNT (the second goes off soon after),
TNT on a lever or button, and standing a few blocks away (you should be hurt and
thrown). Note how far you are thrown, the size of the crater and anything that drops.
