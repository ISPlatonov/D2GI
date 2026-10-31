# RTX Remix mod: backlog

Issues are disabled on this fork, so tickets live here until they are enabled.
Status: `todo` / `in progress` / `done`.

## Ready

### 8. Lighting — in progress
**What the game gives Remix** (#21 light log): no sun. Light slot 0 is re-set per object with nearby point lights (range 80) and a spot from 50 units above (cone 0.72/1.69) whose color changes per object, plus street/station lamps (Remix shows ~13 sphere lights at the start base at night). Two horizontal directional lights (white and dim blue) appear once. Ambient alternates per object (0x1D1D1D … 0x979797). Daylight in the original game is mostly ambient and vertex color, so under Remix the world is lit by the sky only: flat, "light from nowhere".
**Done in #22:** auto exposure off (`rtx.autoExposure.enabled = False`): eye adaptation lifted night to day brightness; with it off night is dark and the station is lit by its own lamps, day (overcast) looks natural. Headlight glow `lg_head.txr` was in `ignoreTextures`; now a particle and an effect light (sphere, radius 0.3, intensity 10). Checked in game 27.10 21:3x, night and day at the start base.
**Done in #24/#25 (merged):** a sun from the fog color (`SunStrength`), the sky dome back as Remix sky and dimmed at night, effect light intensity 1 (10 washed the station out to white at night). Checked in game 27.10 at 10:00 and 23:00.
**Done in #26 (merged 28.10):** real headlights for every vehicle: two spot lights at each vehicle's lamps, following the game's light flags (player: L / Ctrl+L), in every camera view. `rtx.suppressLightKeeping`, sphere radius 1, `d3d9.maxEnabledLights = 64`. Details: `docs/KING_EXE_NOTES.md`, `docs/REMIX_NOTES.md`.
**Owner feedback 28.10:** first reaction was "night not dark, light not RTX" (seen at 10:00 start); asked to retest at night. Still to check: high beam, a car driving away lighting the road, street lamps with radius 1.
**Earlier plan:** a sun. D2GI could add a directional light whose color/intensity follows the game's time of day (WEATHER.INI `sunenv_*` colors); the player's own headlights as two spot lights from the cab camera; check other vehicles' headlights while driving at night.


### 1. Rain and wet windshield — in progress
**What the game draws in rain** (from `d2gi_remix.log`, `DebugDraws=1` + #13):
- `txr\rain.txr` (Remix hash `88948250F5593569`): ~340 thin world-space triangles around the truck, vertex alpha 255, SRCALPHA/INVSRCALPHA blend, z-write off.
- `txr\drop.txr` (`105A0D629252C4D0`): splashes on the road, 3-vertex world-space triangles. Not drops on the glass.
- A 256x256 blit texture (`1AB38C53610F8621` with `StaticBlitTextures`) on a cab-camera quad; `mod.usda` gives it a wet-glass material.
**Why none of it shows from the cab:** cab frames and exterior frames are different frames. From the driver's seat the game draws `drop.txr`, then `glass.txr`, then `pribor.txr`, and never submits `rain.txr` at all. `rain.txr` appears only in frames with no cab. Splashes are already before the gauges, so moving the UI injection later cannot create rain draws. Removing the cab hashes from `uiTextures` rendered the cab twice and left the rain invisible. Reverted.
**Now:** `[REMIX] EmitCabRain=1` remembers `rain.txr` and the world camera (the view matrix used for the splashes). On the first `pribor.txr` of a frame that had splashes and no rain sheet, D2GI draws six world-space `rain.txr` sheets around that camera, before the gauges. `88948250F5593569` and `105A0D629252C4D0` are in `rtx.particleTextures`. Not confirmed in game yet. The wet-glass layer is still the next step after the streaks are visible.
**Rain works outside the cab** (27.10 20:05, owner's hint): with the «Погода» slider left it rains from the start; press **V** for the third-person camera and rain streaks are visible and ray-traced under Remix, with no rtx.conf entry needed. From the cab the game draws no rain streaks at all (frame dumps: `rain.txr` only in third-person frames), so "no rain in the cab" is the original game's behaviour. What is left is the wet windshield: the 256x256 layer is drawn after the cab's UI-tagged parts (rasterized, material ignored) and did not appear in the cab frames dumped while standing still; check it while driving in rain.
**How to get rain:** «Погода» slider all the way left, new game, V to check. Do not edit `wstart`/`wtemp` in TRUCK.INI (`wtemp=0` hangs loading).
**Acceptance:** rain is visible in the world and the windshield looks wet in rain, with no black areas and a clear view through the glass.

### 4. Audit other cab-camera effects with View Model on — todo
Splashes, mud on glass, headlight glare at night: check that nothing else blows up like the raindrops did.

### 5. Hood material — todo
The hood is not untextured: it is a 20-vertex cab-camera draw with the truck's 256x256 texture atlas (Remix hash `F70CEFD60206880E`, unnamed), drawn before the cab's UI-tagged parts, so it is ray-traced. A material on that hash would also apply to the whole truck seen from outside (tyres, glass and lights are in the same atlas). Needs a roughness/metallic map that follows the atlas layout, made in RTX Remix Toolkit.

### 6. Clean up rtx.conf categories — in progress
Hash → name table in `remix-mod/README.md`. Smoke and dust moved from sky to particles in #18. Still unnamed: entries never seen in the logged runs (other regions, menus) and the 30 `uiTextures`, of which 5 are cab parts (needed as UI for now, see item 1).

### 7. Game froze after toggling cab panels — todo
27.10 18:04: after pressing F3 (info), F5 (radio), F6, F7 in the cab, the game stopped responding; `bridge64.log` ends with `Data Queue overwrite condition triggered`, no crash log. Seen once. Reproduce and check whether a panel or a camera switch causes it.

### 9. Draw distance (map visible only nearby) — in progress
The owner wants the world drawn much farther (also needed so off-screen geometry exists for light and shadows).
**Cause found (28.10):** the far clip plane and fog come from WEATHER.INI every frame (far plane = 0.6 × fog far 900 ≈ 540 units on a clear day, 100 on a bad night). LOD/switch/portal culling were not the limit. See `docs/KING_EXE_NOTES.md` → "World and draw distance".
**#29:** `DrawDistance = 300` multiplies the weather fog far/near (and so the far plane) in memory. Earlier switch-distance patch removed.
**Checked 29.10 (Claude, start base, rain, 10:00):** with 100 the view from the cab ends in grey right behind the first trees; with 300 the mountains and the far forest are drawn. Owner check pending: clear day, night, highway, FPS.

### 11. Vehicle headlights drop out — todo
29.10, owner: our headlight light sometimes "leaks away" (moves off / disappears for a moment), and other vehicles' light sometimes suddenly drops off. Suspects: the per-frame vehicle scan (`UpdateVehicleList`, 4 MB window) losing a vehicle; the 16-nearest / 600-unit cut; lamp positions learned from glow quads (`LearnVehicleLamp`) being re-learned wrong; the game's light flag toggling. Reproduce with DebugDraws and log when a tracked vehicle is dropped or its lamp position jumps.

### 12. Surface materials (grass, asphalt, metal, glass) — in progress
29.10, owner: make surfaces look like grass or metal. First pass: roughness/metallic per texture (names → hashes from the DebugDraws log) and procedural tileable normal maps for grass and asphalt (`remix-mod/tools/gen_material_maps.py`, generated at bundle time). Truck bodies and other regions' textures are not in the log yet (unnamed or never seen).
**30.10:** owner: materials look unrealistic, vehicle bodies do not look real. 27 vehicle atlases (body, glass and trim in one 256x256 RGB565 texture; unnamed in the DebugDraws log) taken from a Remix capture (`rtx-remix/captures/textures/<HASH>.dds`, GLI1 = 565 with a 148-byte header). Per-atlas roughness masks by color class: paint 0.18, glass 0.04, dark trim 0.55 (`gen_vehicle_masks.py`). Truck steel parts metallic 0.7 → 0.3.
**31.10:** highway roadside, gas station, meadow, trees, sports car (#33); asphalt normal softer (looked like crumpled foil near the truck). River water: dark glossy opaque with our own color (the game's turquoise looked like a pool) and a ripple normal map. Flowing ripples (8 s loop, owner: 4x slower than the first try). Dam spillway waterfall: own animated foam albedo + normal (1 s loop, falling along V), dam concrete.

### 13. Mountain backdrops stay bright at night — todo
31.10 (owner's screenshots): at night the sky is dark but the mountain skybox images (`skyBoxTextures`, e.g. 172E71FD33DEDF0E, 409C83E1829A1815, FEBCB5E0E999B867) glow at full brightness. Dim them like the sky dome (stage-1 TFACTOR by `SkyDomeScale()`), matched by Remix hash.

### 10. Pause menu «Главное меню» freezes the picture — todo
28.10: in game, Esc → «Главное меню» → «Не сохранять» leaves a frozen frame (sky and ground) and the menu never shows; only Alt+F4 helps. «Выход» → «Не сохранять» works. Probably the 3D → menu switch under KeepDevice.

## In review
- SingleSun (30.10): owner saw "three suns" (capture 23-40-56); the game's per-object directional lights became extra distant lights. Drop them, keep only our sun. Check: one shadow direction. Plus SunFollowsTime: the sun moves with the game clock (owner: shadows did not turn during the day).
- Nothing open (28.10). #27 (effect light intensity back to 10) was closed by the owner.

## Done
- #4 HUD flicker, #5 managed-pool textures, #16 SkipBackdrop, #17 frame-order dump, #18 smoke/dust as particles, #22–#26 lighting (all merged).
- Workshop item 3828304518 updated to `pr-37 3300866` by Claude (31.10 11:17; water, waterfall, moving sun, vehicle paint, glow light color, new preview: the same start-view frame re-shot on this build; 11:48 replaced by the night version at 23:00, which the owner preferred). The first two submits failed with Steam code 2; the third went through.
- Workshop item 3828304518 updated to `pr-30 bba1e0b` by Claude (29.10 00:49, headlights, draw distance x3, materials).
- Workshop item 3828304518 updated to `pr-18 cc5d082` by Claude (27.10 18:22), change note in Russian.
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
