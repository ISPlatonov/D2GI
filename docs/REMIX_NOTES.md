# RTX Remix notes

Facts about the Remix runtime that cost time to find. Source: `NVIDIAGameWorks/dxvk-remix` (`src/dxvk/rtx_render/`),
cloned read-only for reading.

## Config

- `rtx.conf` is merged with `dxvk.conf` into one config, so `d3d9.*` options work in `rtx.conf`
  (`rtx_option_layer.cpp`). Priority: dxvk.conf < built-in app config < rtx.conf < mod rtx.conf < user.conf.
- `user.conf` holds what the player changed in the Remix menu and wins over everything.
- `remix-dxvk.log` lists the effective options at start (`info:    rtx.xxx = value`): check there that a setting applied.

## Texture hashes

- Hash = XXH3_64 of mip 0 with tightly packed rows. D2GI's `DebugDraws` log prints it. Changing pixels changes the hash.

## Lights converted from D3D9 lights (`rtx_light_utils.cpp`, `rtx_lights_data.cpp`)

- Point and spot lights become sphere lights of radius `rtx.lightConversionSphereLightFixedRadius` (default 4 world
  units; we use 1, with 4 the headlights lit the hood).
- Intensity comes from the distance where `brightness / (a0 + a1 d + a2 d²)` falls to 1/255, capped by `Range`.
  With only `Attenuation0 = 1` that distance is `Range`, so **intensity ∝ Range²** and the diffuse level is ignored
  (only its color is used). D2GI headlights set brightness through `Range`.
- Spot cone: `Phi/2` = outer angle, `cos(Theta/2) − cos(Phi/2)` = softness, `Falloff` = focus.
- Remix reads the enabled lights at the next draw call (`D3D9RtxFlag::DirtyLights`). Only `d3d9.maxEnabledLights`
  (default 8) can be enabled at once; we set 64.

## Light lifetime (`rtx_light_manager.cpp`)

- A light that moved more than 2 cm is a new light. A light not seen this frame is kept for `rtx.numFramesToKeepLights`
  (default 100) frames.
- A light that stayed in place for `numFramesToKeepLights / 2` frames is put to sleep and **never removed**. With
  `numFramesToKeepLights = 1` the sleep threshold is 0, so static lights never go away (parked headlights stayed on).
- `rtx.suppressLightKeeping = True`: lights the game stops setting disappear the next frame. This is what we use.

## Other

- `rtx.antiCulling.object.enable` / `rtx.antiCulling.light.enable`: keep objects or lights that left the camera
  frustum (only what the game drew before). Not tried yet. Light anti-culling only applies to point lights.
- Auto exposure (`rtx.autoExposure.enabled`) lifted night to day brightness; the owner wants it off.
- Effect lights (`rtx.lightConverter` textures) are point lights at the particle; `rtx.effectLightIntensity`
  10 gives the visible beams the owner liked, 1 is dim.
- Bridge: `.trex/bridge.conf` `sendCreateFunctionServerResponses = False` stops "Failed to create texture" crashes.

## What the owner liked and rejected

- Liked: dark nights with auto exposure off; headlight beams you can see (effect lights at intensity 10, variant C);
  the sun and sky dome (#24, #25).
- Rejected: auto exposure (night as bright as day); variant B (brighter lights and ×3 conversion factor);
  first version of #26 at 10:00 ("night not dark, light not RTX"; retest at night pending).
- Always: check in game before saying something is fixed, at the time of day where it matters (TRUCK.INI `timestart`).
