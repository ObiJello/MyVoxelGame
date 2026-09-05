# Custom skyboxes

Drop each skybox in its own folder here, with six square faces named after
the Minecraft panorama convention:

```
skyboxes/
  my_space_sky/
    panorama_0.png   front (-Z)
    panorama_1.png   right (+X)
    panorama_2.png   back  (+Z)
    panorama_3.png   left  (-X)
    panorama_4.png   up
    panorama_5.png   down
```

Faces should be 1024px+ squares. `.png` is preferred; `.jpg`, `.bmp` and
`.tga` faces are read too (a downloaded "skycube" is often BMP or TGA), as
long as the names are `panorama_0` to `panorama_5`. Sets appear automatically in
Options → World Settings → Skybox (per world), shown as preview cards.

Players do not need this folder: the game also reads
`<game directory>/skyboxes/<name>/panorama_0..5.png` (on macOS
`~/Library/Application Support/obeycraft/skyboxes/`), and the picker's
"Skyboxes Folder..." button opens it. A folder there with the same name as
a shipped set replaces the shipped one.

## Minecraft resource packs (OptiFine custom sky)

A pack enabled in Options → Resource Packs (put it in `<game directory>/resourcepacks`,
folder or .zip) supplies the "Vanilla" sky's layers automatically, the way
OptiFine does. Dropping a pack into the skyboxes folder instead makes it a
selectable sky of its own; both ways read the same files.

A resource pack that ships an OptiFine custom sky works as-is: unzip it and
drop the whole pack folder into the skyboxes folder, without renaming
anything inside it.

```
skyboxes/
  Lads-SkyBox-4/
    pack.mcmeta
    assets/minecraft/optifine/sky/world0/
      sky0.properties      one layer (sky1, sky2... are drawn on top, in order)
      nebula.png           the layer's 3x2 texture named by `source=`
```

Everything OptiFine reads from `sky<n>.properties` is honoured: `source`
(`./file.png`, `assets/...`, a bare name, or a resource location such as
`skybox:stars.png` = `assets/skybox/stars.png`), `startFadeIn` / `endFadeIn` /
`startFadeOut` / `endFadeOut` (`hh:mm`, 6:00 = tick 0), `blend` (add, alpha,
subtract, multiply, dodge, burn, screen, overlay, replace), `rotate`, `speed`,
`axis`, `weather` (clear / rain / thunder strengths summed), `biomes`
(namespace optional, leading `!` excludes), `heights` (ranges like
`0-64 100-`), `transition` (seconds, eases the biome/height change), `days` and
`daysLoop`. Both `world0` (overworld) and `world1` (End) folders are read; the
End's layers draw over the End starfield. Textures sample nearest like
Minecraft unless a `<file>.png.mcmeta` sets `{"texture": {"blur": true}}`.
Weather strengths come from the environment state, which reads 0 until the
engine has weather, so rain-only layers stay hidden and clear layers stay full.

Such a sky is layered over the vanilla sky exactly the way OptiFine does it:
day/night, sun, moon and stars stay, the layers fade in and out on the pack's
schedule, and the "Sky Behavior" option is not used. The texture is the
OptiFine 3x2 grid (bottom, top, south / west, north, east); the game splits
it into the six faces itself.

If the pack also carries a Nuit (`assets/nuit/sky/*.json`) or FabricSkyBoxes
(`assets/fabricskyboxes/sky/*.json`) definition, its `showSun` / `showMoon` /
`showStars` switches are applied the way those mods do (a body shows when any
entry for the world draws it): a pack whose nebula has its own stars can turn
the vanilla ones off, which OptiFine itself cannot do. The older
`mcpatcher/sky` folder is read when `optifine/sky` is absent.

A pack's replacements for the vanilla environment textures are used while it
is the chosen sky: `assets/minecraft/textures/environment/sun.png`,
`moon_phases.png` (the 4x2 phase grid, any size) and `clouds.png` (a 1x1
transparent pixel switches the cloud layer off, which sky packs with painted
clouds ship). Other mod formats in the same pack (Celestial, `assets/celestial`)
are ignored; the OptiFine files describe the same sky.

Packs that use Respackopts (`*.png.rpo` next to the texture) get the default
texture; if you prefer one of the alternatives named in the `.rpo`, rename it
over the default. Respect each pack's licence: many are personal use only and
must not be redistributed with the game.

Good sources: wwwtyro.github.io/space-3d (generates space skyboxes and
downloads exactly these 6 faces), Spacescape, OpenGameArt (CC0 sets), or any
equirectangular panorama converted via jaxry.github.io/panorama-to-cubemap.

## Bundled sets

`space-blue`, `space-lightblue`, `space-red-1/2/3` are from "Space Skyboxes"
by Rawdanitsu (https://opengameart.org/content/space-skyboxes-0), licensed
**CC0 / public domain** — free to redistribute with the game, no attribution
required. Faces are the pack's native 2048×2048, remapped to the panorama
convention above.
