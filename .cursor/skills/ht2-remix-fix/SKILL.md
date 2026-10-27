---
name: ht2-remix-fix
description: Add or change a D2GI [REMIX] fix (C++ in ddraw.dll) for an RTX Remix rendering problem in Hard Truck 2 — from finding the draw to an in-game check. Use when rtx.conf or a material cannot solve it because the game feeds Remix something wrong.
---

# Add a D2GI [REMIX] fix

## 1. Understand the draw first

- Turn on `DebugDraws = 1` in the `d2gi.ini` of a test build. It logs one line per distinct draw signature to
  `d2gi_remix.log`: primitive, FVF, vertex count, stage states, texture names and sizes. Find the draw by texture
  name. Turn it off again.
- Write down how the game does the effect (backlog item) before changing it. Guessing has cost us builds.

## 2. Implement

- One switch per fix in `[REMIX]`:
  - `d2gi_config.h`: `static bool s_bRemixX;` + `static bool RemixXEnabled()`.
  - `d2gi_config.cpp`: `s_bRemixX = !!GetPrivateProfileInt(TEXT("REMIX"), TEXT("X"), FALSE, …);` Default **FALSE**.
    Append `X=%d` to the `Remix options:` log line.
  - `D2GI/d2gi.ini`: `X = 1` with a 2–3 line comment: what Remix gets wrong, and what `1` does.
- Match narrowly: texture name (`GetDebugName()`, needs name injection in `d2gi_hooks.cpp`) or exact
  size/format, plus the draw shape. Never a broad shape alone.
- Log the first time the fix triggers: `Logger::Log(TEXT("Remix: …"))`, once (static flag), not per frame.
- Code is MSVC v143, Win32, C++ of the D2GI codebase style (tabs, `m_` members, `TEXT()`).
  You cannot compile on Linux. Read the code carefully and let CI compile it.

## 3. Ship

Branch `fix/remix-<slug>`, conventional commit, PR with: what was wrong, what the switch does, a test plan with
in-game steps. Update `docs/REMIX_BACKLOG.md`. CI publishes `pr-N`; then run `ht2-test-build`.

## Existing switches (keep them working)

| Switch | Why |
|--------|-----|
| `DebugDraws` | Draw-signature log for investigation |
| `UntexturedColors` | Untextured geometry gets a 1x1 texture of its color (Remix draws untextured as white) |
| `KeepDevice` | One D3D9 device across DirectDraw re-creation (bridge crashes otherwise) |
| `PersistentHUD` | Replays 2D HUD blits each frame (game redraws only on change → flicker) |
| `ManagedTextures` | Color-keyed textures in managed pool (fixes grass on wheels) |
| `StaticBlitTextures` | Per-frame sysmem→texture blits get constant content (stable hash for materials) |
