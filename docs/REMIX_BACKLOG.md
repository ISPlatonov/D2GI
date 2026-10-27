# RTX Remix mod: backlog

Issues are disabled on this fork, so tickets live here until they are enabled.
Status: `todo` / `in progress` / `done`.

## Ready

### 1. Rain and wet windshield — in progress
**What the game draws in rain** (from `d2gi_remix.log`, `DebugDraws=1` + #13):
- `txr\rain.txr` (Remix hash `88948250F5593569`): ~340 thin world-space triangles around the truck, vertex alpha 255, SRCALPHA/INVSRCALPHA blend, z-write off.
- `txr\drop.txr` (`105A0D629252C4D0`): splashes on the road, 3-vertex world-space triangles. Not drops on the glass.
- A 256x256 blit texture (`1AB38C53610F8621` with `StaticBlitTextures`) on a cab-camera quad; `mod.usda` gives it a wet-glass material.
**Why none of it shows from the cab:** cab frames and exterior frames are different frames. From the driver's seat the game draws `drop.txr`, then `glass.txr`, then `pribor.txr`, and never submits `rain.txr` at all. `rain.txr` appears only in frames with no cab. Splashes are already before the gauges, so moving the UI injection later cannot create rain draws. Removing the cab hashes from `uiTextures` rendered the cab twice and left the rain invisible. Reverted.
**Now:** `[REMIX] EmitCabRain=1` remembers `rain.txr` and the world camera (the view matrix used for the splashes). On the first `pribor.txr` of a frame that had splashes and no rain sheet, D2GI draws six world-space `rain.txr` sheets around that camera, before the gauges. `88948250F5593569` and `105A0D629252C4D0` are in `rtx.particleTextures`. Not confirmed in game yet. The wet-glass layer is still the next step after the streaks are visible.
**How to get rain:** see the `ht2-test-build` skill. The «Погода» slider all the way left gives rain, even when Remix does not show it; confirm with `txr\rain.txr` in `d2gi_remix.log`.
**Acceptance:** rain is visible in the world and the windshield looks wet in rain, with no black areas and a clear view through the glass.

### 4. Audit other cab-camera effects with View Model on — todo
Splashes, mud on glass, headlight glare at night: check that nothing else blows up like the raindrops did.

### 5. Hood material — todo
The hood is a flat untextured surface. Give it a painted-metal material in RTX Remix Toolkit.

### 6. Clean up rtx.conf categories — todo
With #13 every hash can be matched to a texture name. Found so far: exhaust smoke and dust (`vapor_00..03`, `dust_02`) are in `rtx.skyBoxTextures`; tree shadows and traffic-light glows are in `particleTextures`/`ignoreTextures` as expected. Recheck every entry by name and move wrong ones.

## In review
- #16 SkipBackdrop (items 2 and 3: sky dome and khaki ground plane hidden in any weather and time of day): the game draws them untextured with z-write off before the world, the only untextured 3D draws like that (checked in `d2gi_remix.log`). D2GI skips them.
- #13: `DebugDraws` log shows the Remix hash, vertex alpha, blend state and positions for each draw.
- #4: HUD text and digits stop flickering (verified in game).
- #5: color-keyed textures in the managed pool, which fixes grass on car wheels (verified in game).

## Done
- Workshop item 3828304518 updated to `pr-18 cc5d082` by Claude (27.10 18:22), change note in Russian.
- #10 ClearWindshield: no black film over the windshield (verified in game).
- #11 install bundle as a GitHub release + `RTX-Update`/`RTX-Play`/`RTX-Logs` scripts (used to install in this session).
- #12 agent guide (`AGENTS.md`) and project skills.
- #1 KeepDevice (no crash when entering or leaving 3D), #3 PersistentHUD (top panel).
- `UntexturedColors`: solid-color textures for untextured draws.
- `DebugDraws` draw-signature log.
- Local Workshop mod folder `_rtx_remix_mod` so WorkshopModSync stops reverting our DLL/ini.
- Fog disabled (`rtx.enableFog = False`), View Model on for the hood.
