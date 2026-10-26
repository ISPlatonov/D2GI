# RTX Remix mod: backlog

Issues are disabled on this fork, so tickets live here until they are enabled.
Status: `todo` / `in progress` / `done`.

## Ready

### 1. Wet windshield effect (raindrops on glass) — todo
**Problem:** with `rtx.viewModel.enable = True` (needed for the hood), the game's windshield raindrop layer is treated as a view model and covers the whole screen with white dots on black. As a workaround the drop texture is in `rtx.ignoreTextures`, so there are no drops on the glass at all.
**Acceptance:** drops on the windshield are visible in rain and do not block the view (Remix material replacement with a translucent wet-glass material, or a D2GI-side fix).

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
- PR #1: keep one D3D9 device across DirectDraw re-creation (Remix crash fix). Needs a few more clean launches before leaving draft.

## Done
- `UntexturedColors`: solid-color textures for untextured draws.
- `DebugDraws` draw-signature log.
- Local Workshop mod folder `_rtx_remix_mod` so WorkshopModSync stops reverting our DLL/ini.
- Fog disabled (`rtx.enableFog = False`), View Model on for the hood.
