# RTX Remix mod: backlog

Issues are disabled on this fork, so tickets live here until they are enabled.
Status: `todo` / `in progress` / `done`.

## Ready

### 1. Rain and wet windshield — in progress
**What the game draws in rain** (from `d2gi_remix.log`, `DebugDraws=1` + #13):
- `txr\rain.txr` (Remix hash `88948250F5593569`): ~340 thin world-space triangles around the truck, vertex alpha 255, SRCALPHA/INVSRCALPHA blend, z-write off.
- `txr\drop.txr` (`105A0D629252C4D0`): splashes on the road, 3-vertex world-space triangles. Not drops on the glass.
- A 256x256 blit texture (`1AB38C53610F8621` with `StaticBlitTextures`) on a cab-camera quad; `mod.usda` gives it a wet-glass material.
**Draw order** (frame dump from #17, 8 frames): the world, then splashes (`drop.txr`), traffic-light glows, smoke, then the cab camera last (`glass.txr`, `pribor.txr`, cab parts, the 256x256 rain layer). Remix injects the ray-traced frame at the first UI-tagged texture, `pribor.txr`, so the world, the splashes and the rain streaks (drawn with the world) are ray-traced; only the cab and the wet-glass layer after it are rasterized. So the wet-glass material on the 256x256 layer can never apply while the cab is tagged UI.
**Why rain streaks are still invisible:** open. They are ray-traced and were tried as particles, still not visible. Suspects: Remix takes the material diffuse alpha (0.0 for these draws, `diffSrc1` = vertex color) or the 16x16 texture's alpha, or billboard correction flattens the thin triangles. No rain fell in the 27.10 18:00 run, so this was not tested further.
**Tried:** removing the cab textures from `uiTextures` moves the injection later, but the cab then renders twice (a dark translucent panel and stray lines over the gauges) and rain is still not visible. Reverted.
**Next:** test rain with `rtx.enableBillboardOrientationCorrection = False` and with a D2GI fix that sets material alpha to 1 for `rain.txr`; for the glass, draw the wet layer before the cab or untag the cab properly.
**How to get rain:** not reliable yet. The «Погода» slider (left = rainy) only makes rain more frequent. In three test runs on 27.10 the game drew `rain.txr` once in the second frame and never again (frame dumps from #17): it was not raining, and earlier "rain in the log" was this one-off draw. `wstart`/`wtemp` in TRUCK.INI did not start rain (`wtemp=0` hangs loading). Real rain needs either the owner to report it while playing, or a debug switch that forces the game's weather variable (find it in `king.exe`).
**Acceptance:** rain is visible in the world and the windshield looks wet in rain, with no black areas and a clear view through the glass.

### 4. Audit other cab-camera effects with View Model on — todo
Splashes, mud on glass, headlight glare at night: check that nothing else blows up like the raindrops did.

### 5. Hood material — todo
The hood is not untextured: it is a 20-vertex cab-camera draw with the truck's 256x256 texture atlas (Remix hash `F70CEFD60206880E`, unnamed), drawn before the cab's UI-tagged parts, so it is ray-traced. A material on that hash would also apply to the whole truck seen from outside (tyres, glass and lights are in the same atlas). Needs a roughness/metallic map that follows the atlas layout, made in RTX Remix Toolkit.

### 6. Clean up rtx.conf categories — in progress
Hash → name table in `remix-mod/README.md`. Smoke and dust moved from sky to particles in #18. Still unnamed: entries never seen in the logged runs (other regions, menus) and the 30 `uiTextures`, of which 5 are cab parts (needed as UI for now, see item 1).

### 7. Game froze after toggling cab panels — todo
27.10 18:04: after pressing F3 (info), F5 (radio), F6, F7 in the cab, the game stopped responding; `bridge64.log` ends with `Data Queue overwrite condition triggered`, no crash log. Seen once. Reproduce and check whether a panel or a camera switch causes it.

## In review
- #17: `DebugDraws` frame-order dump (verified in game).
- #18: smoke and dust as particles instead of sky.
- #16 SkipBackdrop (items 2 and 3: sky dome and khaki ground plane hidden in any weather and time of day). Checked in game at the start base in one weather; other weather / time of day still to check.
- #4: HUD text and digits stop flickering (verified in game).
- #5: color-keyed textures in the managed pool, which fixes grass on car wheels (verified in game).

## Done
- #13 `DebugDraws` log shows the Remix hash, vertex alpha, blend state and positions for each draw (hashes match rtx.conf entries).
- #15 Steam Workshop package (`RTX-Workshop.cmd`); published by the owner as item 3828304518 (friends).
- #10 ClearWindshield: no black film over the windshield (verified in game).
- #11 install bundle as a GitHub release + `RTX-Update`/`RTX-Play`/`RTX-Logs` scripts (used to install in this session).
- #12 agent guide (`AGENTS.md`) and project skills.
- #1 KeepDevice (no crash when entering or leaving 3D), #3 PersistentHUD (top panel).
- `UntexturedColors`: solid-color textures for untextured draws.
- `DebugDraws` draw-signature log.
- Local Workshop mod folder `_rtx_remix_mod` so WorkshopModSync stops reverting our DLL/ini.
- Fog disabled (`rtx.enableFog = False`), View Model on for the hood.
