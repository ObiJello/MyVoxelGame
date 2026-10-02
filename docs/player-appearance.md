# Player appearance: stick figures, skins and capes

A player looks like one of three things:

- **The stick figure** in one palette colour (`Game::kPlayerColorTable`). This is the default and is what anyone who chose nothing gets.
- **A painted stick figure.** Each of its 62 cells takes a palette colour (`Game::StickFigurePaint`).
- **MC's player model in a skin.** It uses the classic or slim arms and can wear a cape (`Game::PlayerAppearance` with `mode == Skin`).

`Game::PlayerAppearance` in `src/common/entity/PlayerAppearance.hpp` holds all three. Both the launcher and the game compile it.

## Data flow

```
launcher (Appearance view, skin editor, cape grid, Mojang lookup)
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

- **What travels is the PNG itself, not a URL.** Without a profile server there is nothing to fetch from. The server validates the bytes and relays them as sent. A skin with no PNG means the model's default skin, which is MC `DefaultPlayerSkin`: Steve for classic, Alex for slim. A plain stick figure sends no packet at all.
- **The look is not saved with the player.** Each login sends it again from the launcher's choice. The server forgets a player's look when they disconnect, because player ids are reused (`PlayerAppearances::Forget`). Clients drop it on `PlayerInfo REMOVE` and on disconnect.

## Files

| Piece | Where |
|---|---|
| Data model, PNG decode, legacy 64x32 → 64x64 (`processLegacySkin`), validation, stick-figure text format | `common/entity/PlayerAppearance.*` |
| Every released Java cape (slug, name, `textures.minecraft.net` hash) | `common/entity/PlayerCapes.hpp` |
| Box layout of the classic and slim models and the cape, as data (game model and launcher picking share it) | `common/entity/PlayerModelLayout.hpp` |
| Wire format | `common/network/packets/game/PlayerAppearancePackets.hpp` |
| Server store / relay / join sync | `server/player/PlayerAppearances.*` |
| Client looks, GPU textures, per-body cape physics and swim state | `client/entity/PlayerSkins.*` |
| MC `PlayerModel` / `PlayerCapeModel` | `client/renderer/entity/model/PlayerModel.*` |
| MC `AvatarRenderer`: the body plus armour, held items, cape, head item, elytra (cape texture), riptide | `client/renderer/entity/PlayerSkinRenderer.cpp` (`MobRenderer::RenderPlayerSkins`) |
| Local / remote `SkinnedPlayerPose` builders | `client/renderer/entity/SkinnedPlayerPoses.*` |
| Painted stick-figure geometry (per-cell colours, the cell id in vertex `v`) | `client/renderer/entity/StickFigureGeometry.*` |
| Inventory preview: skin model or painted figure | `client/renderer/gui/items/PlayerInventoryPreview.cpp` |
| First-person bare arm, and the map hands in the skin | `client/renderer/viewmodel/HeldItemRenderer.cpp` (`SetPlayerSkin`) |
| Launcher UI, editor, previews, fetchers | `launcher/appearance/*`, `launcher/ui/LauncherWidgets.*` |

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

## Stick-figure paint format

`stickfigure.txt` is the header line `obeycraft-stickfigure 1`, followed by 62 hex digits, one `PlayerColorId` per cell. The cell order is defined in `PlayerAppearance.hpp`:

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
