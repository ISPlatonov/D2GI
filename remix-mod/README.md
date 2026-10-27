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

## Texture names in rtx.conf

From `d2gi_remix.log` (`DebugDraws=1`, which logs each texture's Remix hash). "-" = not seen in the logged runs.

| Category | Hash | Texture |
|----------|------|---------|
| ignore | 3A19562A44B7363F | `common_SizeLight.txr` (car marker lights) |
| ignore | BEFFC37EB6A2C999 | `shadtree0.txr` (tree shadow) |
| ignore | C87C1EE3BD5D7699 | `effect_Shadow.txr` (car shadow) |
| ignore | 093C29F27ECFCF21, 0CF746564CAA68E7, 9F87AA1947B96BA8, D20CFD7A101BEE13 | - |
| particle | 527CD12D15C474E6, 63347F47085C30A8 | `tlglyel.txr`, `tlglgre.txr` (traffic-light glows) |
| particle | 6BE094792FAFF45C | `shadtree1.txr` |
| particle | 71344BA732B735D4 | `lg_stop.txr` (brake-light glow) |
| particle | E6CA919FC18EC23F, 555F8B587FFE7941, 772427658E68F024, B1F73F60EE8A2B56 | `vapor_00..03.txr` (exhaust smoke) |
| particle | 4A14E1A93DD0437B | `dust_02.txr` |
| sky | 4E5BF5AF5C839A10, 6573F3D1B18BEB1C | unnamed 256x256 (sky images) |
| ui | 017AA161E4BFACF9 | `pribor.txr` (cab instruments) |
| ui | 4A2BBB03FC7D39E6 | `RD_body.txr` (cab object) |
| ui | 21236D9F3E9DBF3F, 366BE1ABAC3250D7, DBD29C2D11929143 | unnamed 128x128 cab parts |
| (none) | 88948250F5593569 | `rain.txr` (rain streaks) |
| (none) | 105A0D629252C4D0 | `drop.txr` (rain splashes on the road) |
