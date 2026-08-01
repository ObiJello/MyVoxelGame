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

Faces should be 1024px+ squares. Sets appear automatically in
Options → World Settings → Skybox (per world).

Good sources: wwwtyro.github.io/space-3d (generates space skyboxes and
downloads exactly these 6 faces), Spacescape, OpenGameArt (CC0 sets), or any
equirectangular panorama converted via jaxry.github.io/panorama-to-cubemap.

## Bundled sets

`space-blue`, `space-lightblue`, `space-red-1/2/3` are from "Space Skyboxes"
by Rawdanitsu (https://opengameart.org/content/space-skyboxes-0), licensed
**CC0 / public domain** — free to redistribute with the game, no attribution
required. Faces are the pack's native 2048×2048, remapped to the panorama
convention above.
