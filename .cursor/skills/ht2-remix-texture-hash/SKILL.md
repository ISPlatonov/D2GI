---
name: ht2-remix-texture-hash
description: Find the RTX Remix texture hash of something on screen in Hard Truck 2 and put it in rtx.conf (ignore, sky, UI, particle…) or give it a material in mod.usda. Use when an object renders wrong under Remix, needs hiding, or needs a replacement material.
---

# Find a texture hash and use it

## Before you start

- A hash only works if it stays the same across frames, launches, weather and time of day. Textures the game
  rewrites (per-frame blits, solid colors chosen by weather) need a D2GI fix first: `StaticBlitTextures`,
  `UntexturedColors`. If the hash changes between two looks, stop and fix that first (see `ht2-remix-fix`).
- The game must be running, in 3D, with the object on screen. One instance only (see `ht2-test-build`).

## Read the hash

1. Click the game window to focus it (never open_application). Press **Alt+X** → **Developer Settings Menu** →
   **Game Setup** → **Step 1: Categorize Textures**.
2. Hover the object in the scene. Remix highlights it: everything else desaturates, or it gets an outline.
3. **Left-click** it. A popup shows `Texture WxH`, `Hash: …` and the category checkboxes.
4. The popup text is tiny at 4K and screenshots are downscaled, so do not trust a hash read from a screenshot.
   Reliable way: tick a **harmless category that you will undo** (e.g. *Smooth Normals*), then read the exact hash
   from `rtx-remix/logs/remix-dxvk.log`:
   ```
   smoothnormalstextures - added (positive opinion) 1AB38C53610F8621
   ```
   Then untick it, or do not save, so `user.conf`/`rtx.conf` stay clean.
5. Close the menu with **Alt+X**. Never Escape (it goes to the game and can crash the bridge).

Cross-check the size against `d2gi.log` notices (e.g. `Remix: static content for blit texture 256x256`) when the
texture comes from a D2GI fix.

## Use the hash

**Category** (hide, sky, UI, particle, …): edit `remix-mod/package/rtx.conf`, the copy in git. The list format is
`rtx.<category>Textures = 0xAAAA…, 0xBBBB…`, sorted. Main lists: `ignoreTextures`, `skyBoxTextures`, `uiTextures`,
`particleTextures`. A `-0x…` entry removes a hash from a list.

**Material**: add an `over "mat_<HASH>"` block to `remix-mod/package/rtx-remix/mods/HT2_RTX/mod.usda`, following
the existing wet-glass block. Generated textures go through a script in `remix-mod/tools/`, run by
`tools/make_bundle.py`. Do not commit generated DDS files.

Note in `remix-mod/README.md` what the hash is (object, size) and why it is stable.
Ship it as a PR and verify in game (`ht2-test-build`).

## Pitfalls seen so far

- A texture in `ignoreTextures` cannot also get a material: the draw is dropped.
- Marking a full-screen overlay as Particle did not make it transparent. Use a material or a D2GI fix.
- With `rtx.viewModel.enable = True`, cab-camera draws are view models. Check them with the camera turned too
  (Page Down / Delete).
