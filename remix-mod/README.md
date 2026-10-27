# Hard Truck 2 RTX Remix mod

`package/` is the content of the game's local Workshop mod folder `_rtx_remix_mod`.
WorkshopModSync copies that folder into the game root on every Steam launch, so this is the only
place to install into (files copied straight into the game root get reverted).

| File | Purpose |
|------|---------|
| `package/rtx.conf` | Remix texture categories (ignore, sky, UI, particles), fog off, view model on |
| `package/rtx-remix/mods/HT2_RTX/mod.usda` | Material overrides keyed by game texture hash |
| `package/mod.cfg`, `package/preview.png` | Workshop metadata; priority -10 so it wins over D2GI and Modern Patch |
| `tools/gen_rain_normal.py` | Generates the raindrop normal-map sprite sheet (not committed, built by CI) |

`ddraw.dll` and `d2gi.ini` are added from the D2GI build. `tools/make_bundle.py` assembles everything
into `HT2-RTX.zip`; CI publishes it as the release `latest` (master) or `pr-N` (pull requests).

## Materials

- `mat_1AB38C53610F8621`: windshield rain layer (256x256) → wet thin glass (translucent, animated
  raindrop normal map, 4x2 sprite sheet). The hash is stable only because `StaticBlitTextures` fills
  that texture with constant content. It must stay out of `rtx.ignoreTextures`.
