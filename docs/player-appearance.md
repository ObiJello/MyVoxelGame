# Player appearance: stick figures, skins and capes

A player looks like one of four things:

- **The stick figure** in one palette colour (`Game::kPlayerColorTable`). This is the default and is what anyone who chose nothing gets.
- **A painted stick figure.** Each of its 62 cells takes a palette colour (`Game::StickFigurePaint`).
- **A drawn figure.** A picture the player draws with a round brush in palette colours, worn *instead of* the stick figure: upright, turning with the body, every stroke drawn the way the stick figure draws its own limbs (`Game::StickFigureDrawing`).
- **MC's player model in a skin.** It uses the classic or slim arms and can wear a cape (`Game::PlayerAppearance` with `mode == Skin`).

`Game::PlayerAppearance` in `src/common/entity/PlayerAppearance.hpp` holds all of them. Both the launcher and the game compile it.

## Data flow

```
launcher (Appearance view, skin editor, drawing editor, cape grid, Mojang lookup)
  │  launcher.json "appearance" block  (persisted choice)
  │  on Play / Join: {obeycraft}/appearance/skin.png, cape.png, stickfigure.txt
  ▼
game CLI  --skin-mode stick|skin  --skin <png>  --skin-model classic|slim
          --cape <png>  --stick-figure <txt>      (plus the old --color)
  │  PlatformMain: Game::LoadAppearanceFromArgs → Client::PlayerSkins::SetLocal
  ▼
ClientConnection::HandleLoginSuccess ── PlayerAppearanceC2S (0xC8) ──▶ server
                                        Server::PlayerAppearances::HandleC2S
                                          Sanitize (PNG, 64x64/64x32 skin, 64x32 cape, 64 KB caps)
                                          store by player id
  every other client ◀── PlayerAppearanceS2C (0xC9, playerId + look) ──┘
  newcomer           ◀── PlayerAppearanceS2C for every present player  (SyncOnJoin)
  │  ClientPacketHandler::onPlayerAppearanceS2C → Sanitize again → PlayerSkins::SetRemote
  ▼
renderers (textures upload lazily on the render thread)
```

- **A drawing travels as its compact encoding** (below). The server checks it like a skin (`Sanitize`): one that is malformed or over a cap is dropped and the rest of the look relayed.
- **What travels is the PNG itself, not a URL.** Without a profile server there is nothing to fetch from. The server validates the bytes and relays them as sent. A skin with no PNG means the model's default skin, which is MC `DefaultPlayerSkin`: Steve for classic, Alex for slim. A plain stick figure sends no packet at all.
- **The look is not saved with the player.** Each login sends it again from the launcher's choice. The server forgets a player's look when they disconnect, because player ids are reused (`PlayerAppearances::Forget`). Clients drop it on `PlayerInfo REMOVE` and on disconnect.

## Files

| Piece | Where |
|---|---|
| Data model, PNG decode, legacy 64x32 → 64x64 (`processLegacySkin`), validation, stick-figure file format (`StickFigureFile`) | `common/entity/PlayerAppearance.*` |
| The drawn figure: canvas, strokes, caps, encoding (and the old pixel canvas's conversion) | `common/entity/StickFigureDrawing.*` |
| Every released Java cape (slug, name, `textures.minecraft.net` hash) | `common/entity/PlayerCapes.hpp` |
| Box layout of the classic and slim models and the cape, as data (game model and launcher picking share it) | `common/entity/PlayerModelLayout.hpp` |
| Wire format | `common/network/packets/game/PlayerAppearancePackets.hpp` |
| Server store / relay / join sync | `server/player/PlayerAppearances.*` |
| Client looks, GPU textures, per-body cape physics and swim state | `client/entity/PlayerSkins.*` |
| MC `PlayerModel` / `PlayerCapeModel` | `client/renderer/entity/model/PlayerModel.*` |
| MC `AvatarRenderer`: the body plus armour, held items, cape, head item, elytra (cape texture), riptide | `client/renderer/entity/PlayerSkinRenderer.cpp` (`MobRenderer::RenderPlayerSkins`) |
| Local / remote `SkinnedPlayerPose` builders | `client/renderer/entity/SkinnedPlayerPoses.*` |
| Painted stick-figure geometry (per-cell colours, the cell id in vertex `v`); the drawing's figure-space segments and layers (`BuildDrawingMesh`), their placement at a body as line pairs (`AppendDrawingLines`), their widening into ribbons and round joints (`AppendDrawingStrokeTriangles`), and the stick figure as strokes (`StickFigureStrokes`) | `client/renderer/entity/StickFigureGeometry.*` |
| Inventory preview: skin model, painted figure or drawing | `client/renderer/gui/items/PlayerInventoryPreview.cpp` |
| First-person bare arm, and the map hands in the skin | `client/renderer/viewmodel/HeldItemRenderer.cpp` (`SetPlayerSkin`) |
| Launcher UI, editor, previews, fetchers | `launcher/appearance/*`, `launcher/ui/LauncherWidgets.*` |
| The drawing editor (full window) | `launcher/appearance/FigureDrawingEditor.*`; its 3D preview, and the Appearance view's, through `PlayerMesh.*` (`AppendDrawingPreview`) |

## Rendering rules

- **Stick-figure players** stay with `PlayerRenderer`. A painted figure is the same geometry split per cell. Every palette colour goes through the same hurt flash, light and translucency as the plain figure. `RenderSingle` takes a `subjectId` (the local player, or a remote id for the portal ghosts) to choose whose paint to use. If the subject's look is a skin, it draws nothing.
- **Skinned players** go through `MobRenderer::RenderPlayerSkins`, from three places:
  - `drawLocalBody` for your own body in third person and in portal views;
  - `drawRemotePlayerLayers` for everyone else in every view;
  - the portal-ghost passes, which use the same layer pass.

  `PlayerRenderer::Render` and the stick-figure elytra and riptide passes skip skinned players.
- **Pose stack.** This is `LivingEntityRenderer.submit` plus `AvatarRenderer.setupRotations`: crouch offset, bed, scale, body yaw, death topple / riptide spin / sleep, glide tip with flying yaw, swim tilt, then flip, 0.9375, −1.501.
- **`setupAnim`.** This is `HumanoidModel`'s, plus the glide head and `setupSwimAnimation`, which only the player model uses.
- **Cape.** MC `ClientAvatarState`: the cloak point chases the body at ¼ of the gap per tick, plus the walk bob. `AvatarRenderer.extractCapeState` turns it into `capeFlap` / `capeLean` / `capeLean2`, and `PlayerCapeModel` applies those with `rotateBy`.
  - It is hidden on an invisible body and under an elytra. The elytra then takes the cape's texture (MC `WingsLayer`).
  - It is nudged out over a chestplate.
- **Remote state.** For remote players, swimming is inferred as sprinting with the eye under water. On-ground is inferred as no vertical travel this tick. The wire carries neither.
- **Spectators** are the translucent floating head. An INVISIBLE body is hidden but keeps its layers, unless the viewer is a spectator, who sees it translucent.

- **Drawn figures** (`PlayerRenderer::AppendDrawing`) replace the stick figure entirely; nothing of the figure is built. The rules:
  - **Look.** Each stroke is drawn the way the stick figure draws a limb. Its segments ride the figure's line list as line pairs, and `SubmitFigures` widens them toward the camera: a camera-facing ribbon the stroke's width, plus a camera-facing disc at every point for the round joints and ends. Together they are the silhouette of a round tube along the stroke. From the front the drawing is the picture on the canvas, from the side a figure of tubes, and it never vanishes edge-on. The colour is flat like the limbs': no face shading.
  - **Telling strokes from limbs.** A line pair's `v` is a limb's paint cell (≥ 0) or a stroke's negative tag (`DrawingStrokeTag`: its layer and whether its end is capped). A stroke's `u` is its radius in blocks, multiplied by the body scale and the portal model's scale (`ScaleLineWidth`); a limb's `u` is the scale itself.
  - **Cache.** Each look is flattened once (`BuildDrawingMesh`) into figure-space segments and cached per player, keyed by `PlayerSkins`' revision, which changes whenever that player's look is replaced. Only the placement and the camera-facing widening run each frame.
  - **Layers.** Strokes are coplanar, so two overlapping strokes of different colours would sit at the same depth. Each stroke gets a layer one above every earlier stroke of another colour whose ink it touches (capped at 31). The widening pulls a stroke toward the eye by 1/4096 of the distance per layer. It moves along the eye ray, so the picture on screen does not move and only its depth changes; a later stroke wins as it did on the canvas, from the front and from behind.
  - **Placement.** Every frame the segments are placed at the body: upright at the feet, turned with the body yaw (not a billboard), in render space (`Render::ToRender`) like the rest of the figure.
  - **Shading.** The palette goes through the figure's own hurt flash, light (the lightmap colour at the eye, `EntityEnvironment::LitAt`) and translucency. GL and Vulkan share the stick figure's strip draw; there is no texture.
  - **Poses.** The glide, riptide spin, death topple, body scale and portal-ghost model reach the strokes as they reach the limbs. Sneaking squashes the drawing toward the feet by the stick figure's crouched eye height over its standing one (`kDrawingCrouchScaleY`, 0.84), keeping the stroke width, so it lowers without sinking into the ground. In bed it lies on its back, head on the pillow (`LieDrawing`); the stick figure's sideways roll would stand a flat picture on its edge. Swimming tilts it like the player model (`SwimDrawing`: AvatarRenderer's swim rotation and the visually-swimming shift), from `PlayerSkins`' per-tick swim state, which is ticked for drawn figures too.
  - **Spectators, invisibility, outline.** A spectator's floating head keeps the strokes from the stick figure's neck (1.44 blocks) up; a stroke crossing that height is cut there and ends round. INVISIBLE, translucent and GLOWING follow the stick figure's rules, since the strokes ride its line batches. Translucent strokes darken slightly where a ribbon and its joint disc overlap, as the limbs' strips do.
  - **Culling.** A drawing taller than 1.8 blocks grows the culling box to its height.
  - **Inventory preview.** The preview widens the strokes toward a viewer far out along +Z (the box's orthographic view) and draws them in stroke order, so a later stroke lies over an earlier one; no stroke is thinner than a pixel. A drawing taller than 1.8 is shrunk to fit the box.
  - **First person.** Nothing is drawn: the figure has no first-person body.
  - **Budget.** A stroke costs about 27 vertices a point at the limbs' width and 60 at the largest brush. The strip buffer holds 262,144 vertices a frame, so even a drawing at the point cap in the largest brush fits on its own; a frame that overflows draws the strips that fit. The ring buffer (head rings, back-of-head discs) holds 65,536.

## Stick-figure file format

`stickfigure.txt` (`Game::StickFigureFile`, read by `--stick-figure`, capped at 64 KB) is version 4:

```
obeycraft-stickfigure 4
paint <62 hex digits>      optional: the painted cells
drawing <hex>              optional: StickFigureDrawing::Encode, worn instead of the figure
```

Unknown lines are skipped. The launcher writes the file when the figure is painted or drawn; the drawing line appears only when the Drawn style is chosen. A drawing line that fails to decode drops the drawing alone. Older files still load:

- **Version 3** has the same lines, its drawing in the old pixel encoding. `Decode` converts it to strokes (below).
- **Version 2** has the paint line plus `sculpt <hex>`, the retired 3D voxels. The sculpt line is skipped, so the figure loads plain or painted.
- **Version 1** is the header line `obeycraft-stickfigure 1` followed by the 62 digits, giving a painted figure.

`launcher.json` keeps the paint in `stick_paint`, the style in `stick_painted` / `stick_drawn`, and the drawing as the same hex in `stick_drawing`, kept even while another style is worn. A pixel drawing saved there by the previous launcher is converted on load and saved back as strokes. An older launcher's `stick_sculpt` is ignored and gone after the next save.

### Paint cells

Each hex digit is one `PlayerColorId`. The cell order is defined in `PlayerAppearance.hpp`:

| Part | Cells | Runs from |
|---|---|---|
| Torso | 8 | neck to hip |
| Left arm, right arm | 6 each | shoulder to hand |
| Left leg, right leg | 6 each | hip to foot |
| Head ring | 16 | starts at the player's right and runs up over the crown |
| Eyes | 2 | left, then right |
| Smile | 4 | from the player's left to their right |
| Back of head | 8 | |

On the wire the paint is the raw cell bytes, prefixed by the cell count. A newer peer with more cells is read safely.

### The drawing (`Game::StickFigureDrawing`)

A list of strokes, painted in order: a later stroke lies over an earlier one. A stroke is one palette colour, one brush radius and a polyline of one or more points; one point is a dot. It is round, the brush's circle swept along the polyline, so its ends and joints are round.

**The canvas** is 0.6 × 3.0 blocks:

- **Width:** 0.6 blocks, MC's player hitbox width.
- **Height:** 3.0 blocks above the feet and nothing below them, so a tall hat or raised arms fit over the 1.8-block player.

Points sit on a 0.5 mm grid (1/2000 block): x from 0 to 1200, y from 0 to 6000. x counts from the canvas's left *as seen from the front* (the player's right), y up from the feet. In figure space (blocks, feet at the origin, +X the player's right, +Y up, +Z the way the body faces) point (x, y) is at X = 0.3 − x/2000, Y = y/2000, Z = 0. The grid bounds the centre line; the editor also keeps each stroke's whole width on the canvas.

**Radius** is whole millimetres (1/1000 block), 4 to 150. The stick figure's limbs are 18 mm (`kLineRadius`, PlayerRenderer's 1.8 cm strip half-width) and its head outline and smile 25 mm (`kRingRadius`). The editor's brush starts at 18.

Colours come from the stick-figure palette only, the painter's rule.

**Encoding.** The file and `launcher.json` carry it as hex; `PlayerAppearanceC2S/S2C` carry it as raw bytes.

```
byte  version (4)
strokes, until the bytes end:
  byte    colour       (PlayerColorId)
  byte    radius       (mm, 4 .. 150)
  VarInt  points - 1   (LEB128)
  u16le   x, u16le y   the first point
  (points - 1) x { zigzag VarInt dx, zigzag VarInt dy }   from the previous point
```

A typical point costs 2 to 4 bytes. The version number follows the appearance version that brought strokes (4); 2 and 3 were never drawings.

**Version 1, the old pixel canvas.** The first Drawn style was 32 × 160 pixels over the same canvas, run-length encoded: a header `1, 32, 160`, then `(value, length − 1)` runs in reading order, value 0 for transparent or `PlayerColorId + 1`, capped at 2,048 runs. `Decode` still reads it and converts it: each row's run of one colour (or each column's, whichever takes fewer strokes) becomes a stroke through the pixel centres with a 10 mm radius, just over half a pixel, so neighbouring rows close up and the drawing survives as the same picture. A pathological drawing past the caps keeps the strokes that fit.

**Caps.** These are hard limits: `Decode` refuses a drawing over them, `Validate` fails, the server's `Sanitize` drops one, and the editor will not make one.

- 2,048 strokes.
- 4,096 points in all.
- 24,577 encoded bytes (every stroke header at most 8 bytes, every further point at most 4), which is about 49 KB of hex in the stick-figure file.

They bound the in-game geometry and the packet. An ordinary drawing is a few dozen strokes and a few hundred points. The editor simplifies what the mouse draws (`Simplify`, Ramer–Douglas–Peucker), so a stroke costs its shape and not the mouse's sample rate.

**Wire.** `PlayerAppearance` version 4 has the same layout as version 3, ending with two fields after the paint:

- `VarInt sculptLength, bytes`: version 2's voxel slot, now always 0. A version-2 sender's bytes there are skipped.
- `VarInt drawingLength, bytes`: 0 means no drawing. Since version 4 it holds strokes; a version-3 sender's pixel canvas still decodes, converted.

Older senders stop before these fields. A drawing over the byte cap, or one that does not decode, is skipped, and the look arrives without it. The server relays the look to everyone, and to late joiners through `SyncOnJoin`. A drawn figure sends a packet even in the player's plain colour.

### The drawing editor

Open it from Appearance → Stick figure → Style **Drawn** → *Start drawing* / *Edit drawing*. It takes the whole window and edits a copy. **DONE** writes the copy back and wears it; the back button asks before discarding changes.

The window has three parts:

- **Tools (left).**
  - **Brush (B).** A round brush. Each drag is one stroke, drawn live as the mouse moves: a point is taken every screen pixel of movement, the path is lightly smoothed (two passes of a 1-2-1 average, ends kept; the *Smooth strokes* toggle), then simplified to within half a screen pixel.
  - **Eraser (E).** A round brush that cuts away the parts of strokes it covers. A stroke is cut where its centre line comes within the eraser's radius plus its own, so what is left ends, round, at the eraser's edge. A stroke cut in the middle becomes two, and a closed loop cut once stays one stroke.
  - **Line (L), Rectangle (R), Circle (O).** Outlines as strokes. Drag from end to end, or across the box. Shift snaps a line's angle to 15° and makes the box square.
  - **Eyedropper (I).** Takes the topmost stroke's colour and size.
  - **Brush size.** A continuous slider, logarithmic from 4 to 150 mm radius, with a mark at the stick figure's own line; also `[` / `]` and the scroll wheel over it. The label shows the stroke's width. Strokes take it rounded to the millimetre.
  - **Mirror (M)** repeats every stroke, and every erase, on the other half. A stroke on the centre line is not doubled.
  - **The palette.**
  - *From stick figure* turns your plain or painted figure's front view into real strokes (`StickFigureStrokes`): the torso, legs and arms at the limbs' width, the head outline and smile at the rings', the eyes as dots, each part split where its paint changes colour. The hands are cut at the hitbox's sides and the feet lifted by their radius, so everything stays on the canvas.
  - *Clear* empties the canvas so you can draw from scratch. It asks first, and undo brings the drawing back.
- **Canvas (middle).** Strokes are drawn antialiased, a thick line per segment and a disc per point. A faint guide shows the stick figure and a dashed line at the player's 1.8 blocks, with the heights labelled; it can be switched off. The brush's circle follows the cursor at its true size, with its mirror image while mirroring. Scroll zooms about the cursor. Right-drag, middle-drag or Space-drag pans. *All* / *Player* / − / + set the view.
- **3D preview (right).** The game's own strokes, widened toward the preview's camera as the game widens them, standing on the hitbox's footprint, turning and zooming, updated live.

Undo / redo is Ctrl+Z / Ctrl+Shift+Z (or Ctrl+Y). The top bar counts the strokes and the share of the nearest cap used. A brush stroke that would pass a cap stops where it last fitted, and a shape or an erase that would pass one does not happen, each with a note.
