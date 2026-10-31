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

## Several suns (30.10)

The game lights every object with its own fixed-function directional lights: light 0 changes direction from object
to object (e.g. (-0.32 0.57 -0.76), (-0.69 0.41 -0.59), (-0.69 0.71 -0.14)), light 1 is a white horizontal fill
(-1 0 0). Remix turns each distinct directional light into a distant light, so together with our SunStrength sun
the owner saw "three suns" with shadows in different directions. `SingleSun = 1` drops the game's directional lights.

## Sun follows the game clock (30.10)

Owner: shadows must turn during the day. The game clock is a double at `[*0x6F33AC + 0x58]` (hour 0..24), the
same value the game uses for automatic night lights (`0x550562`). `SunFollowsTime = 1` moves our sun: rises in +X at
5:00, 60 degrees over -Y at 13:30, sets in -X at 22:00; at night it stays 8 degrees high (its color is dark then).
Which world axis is north is not known; the path may need turning to match the sky's sun sprite.

## Effect light color (31.10)

Remix colors an effect light (texture in `rtx.lightConverter`, here `lg_head.txr`) by the draw's legacy material
diffuse, times `rtx.effectLightColor` (`rtx_scene_manager.cpp`, `createEffectLight`). The game does not set a
material for glow sprites, so they inherit the previous draw's: the owner saw purple light on a trailer at night.
`GlowLightColor = 1` sets a fixed warm white material for lg_head draws.

## Low sun (31.10)

Owner: sand slopes facing a low sun looked white. With SunFollowsTime the sun near the horizon still had noon
intensity, so surfaces facing it got full light. The sun is now dimmed and warmed by elevation:
intensity x (0.3 + 0.7 f), green x (0.8 + 0.2 f), blue x (0.55 + 0.45 f), f = sin(elevation) / sin(60 degrees).
