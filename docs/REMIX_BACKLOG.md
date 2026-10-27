# RTX Remix mod: backlog

Issues are disabled on this fork, so tickets live here until they are enabled.
Status: `todo` / `in progress` / `done`.

## Ready

### 1. Wet windshield effect (raindrops on glass) — in progress
**How the game does it:** each frame the game reads the back buffer back, draws a blurred and refracted copy of it plus droplets into a texture (a sysmem blit into a texture via `UpdateSurface`), and puts that texture on a full-screen quad drawn by the cab camera. The wipers clear arcs in it.
**Why it breaks under Remix:** back-buffer readback has no ray-traced image, so the layer is black with droplets on top. As a view model it covers the whole windshield. Marking it Particle does not help: it is still opaque black.
**Current state:** `StaticBlitTextures` replaces that upload with constant content, so the hash no longer changes every frame. In game that hash is `1AB38C53610F8621`. The wet-glass material targets `mat_1AB38C53610F8621`. The old key `9CB3B053B225F1AC` does not match this DLL. The texture must stay out of `rtx.ignoreTextures`.
**Acceptance:** the windshield looks wet in rain, with no black areas, and the view through the glass stays clear. Drop size and speed still need a look in game.

### 2. Stable hash for the sky dome — todo
**Problem:** the untextured sky dome gets a 1x1 texture whose hash depends on its color (`UntexturedColors`). The color changes with time of day and weather, so the `rtx.ignoreTextures` entry only covers one shade.
**Acceptance:** D2GI recognizes the sky dome and skips the solid-color texture for it (or uses one constant texture), so it is hidden in every weather and time of day.

### 3. Stable hash for the ground fallback plane — todo
**Problem:** same as the sky dome. The untextured khaki plane under the map covers roads and grass under Remix. It is ignored by a color-dependent hash.
**Acceptance:** the plane never appears in any region, weather or time of day.

### 4. Audit other cab-camera effects with View Model on — todo
Splashes, mud on glass, headlight glare at night: check that nothing else blows up like the raindrops did.

### 5. Hood material — todo
The hood is a flat untextured surface. Give it a painted-metal material in RTX Remix Toolkit.

## In review
- #11: install bundle as a GitHub release + `RTX-Update`/`RTX-Play`/`RTX-Logs` scripts.
- #12: agent guide (`AGENTS.md`) and project skills.
- #4: HUD text and digits stop flickering (verified in game).
- #5: color-keyed textures in the managed pool, which fixes grass on car wheels (verified in game).

## Done
- #1 KeepDevice (no crash when entering or leaving 3D), #3 PersistentHUD (top panel).
- `UntexturedColors`: solid-color textures for untextured draws.
- `DebugDraws` draw-signature log.
- Local Workshop mod folder `_rtx_remix_mod` so WorkshopModSync stops reverting our DLL/ini.
- Fog disabled (`rtx.enableFog = False`), View Model on for the hood.
