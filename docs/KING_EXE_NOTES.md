# king.exe notes (reverse engineering)

Everything here is for the Steam `king.exe` 8.1 (PE timestamp `0x400502EA`, image base `0x400000`, no ASLR).
D2GI reads and patches the running game in memory from `ddraw.dll`; the exe file itself is not changed
(this keeps the mod publishable on the Steam Workshop). Gate every address on that timestamp.

How these were found: `objdump -d -M intel king.exe` on Linux (sections: `.text` 0x401000, `.rdata` 0x64B000,
`.data` 0x66C000), string cross-references (`push imm32` of a string address), and `DebugDraws` memory dumps
written by D2GI (`d2gi_vehicle_<frame>.bin`, `d2gi_vehscan_<frame>.txt`).

## Vehicles

| What | Where |
|------|-------|
| Player vehicle | `*(*(0x6D2078) + 0x268)` |
| Also point at the player / current vehicle | globals `0x696340`, `0x6F6910` |
| Axes (3 rows of 3 floats: X right, Y forward, Z up, in world space) | `vehicle + 0x204` |
| Position (3 floats, same space as the D3D world/view matrices) | `vehicle + 0x228` |
| Headlight switch: 0 off, 1 low, 2 high | `vehicle + 0x51B8` |
| Automatic lights by the hour (1 at night; set at `0x55057A`) | `vehicle + 0x51B4` |
| Model switch handles: HeadLightKey / GlowKey / effect_Glow_Particle / effect_Glow_Beam | `vehicle + 0x5014 / 0x5010 / 0x500C / 0x5008` (set in the constructor around `0x543700`) |
| Model index into per-model tables (`0x6D2130`, `0x6D211C`, …) | `vehicle + 0x28B4` |

- Key handler for the lights at `0x597D36`: key code `0x17` (L) toggles 0 ↔ 1; with key `4` (Ctrl) held it switches 1 ↔ 2.
- `0x546FEF`: HeadLightKey switch = on if `+0x51B8 != 0 || +0x51B4 != 0` (`SetSwitch` = `0x5E1A10`, node type `0x12`).
- Vtables (first dword of the object): `0x64CDF8` tractor trucks (player too), `0x64CFF4` cars, `0x65089C` buses/vans,
  `0x64CBFC` semitrailers (no headlights). All share the transform and light offsets above. Objects are ~0x9660 bytes.
- The heap holds arrays of pointers to all vehicles (seen at run-time, e.g. 14 entries). No static root found yet,
  so D2GI finds vehicles by scanning private RW memory for those vtables (4 MB per frame) and re-validates them.
- Vehicle tech record (from `vehicle.tech`, binary): `Fara_FL` at `+0x77C`, `Fara_FR` `+0x788`, also `Fara_BL/BLs/BR/BRs`,
  `flame_place`, `SmokeDirection2`, `Fara_Dir`, `Fara_Width` (parsed around `0x509C00`). Lamp positions in D2GI are
  learned from glow quads instead, because the pointer from the vehicle to its tech record is not known yet.
- The `lg_head.txr` glow quads are submitted only while the lamps face the camera (they are absent from frame dumps
  taken from behind). Anything Remix derives from them (effect lights) comes and goes with the view.
- Measured lamp positions (vehicle coordinates): start truck (±0.9, 6.1, 0.05); cars about (±0.5, 2.0, 0.3).

## TRUCK.INI `[RENDER]`

Parsed in the engine init around `0x5DC7E0`–`0x5DCD40` into a settings struct at `[engine + 0x1D8]`
(reader functions: `0x603CB0` double, `0x603B70` int, `GetPrivateProfileStringA` for strings).

| Key | Struct offset | Default | Notes |
|-----|---------------|---------|-------|
| `intensity` | +0x30 (double) | 0.3 | |
| `detail` | +0x70 (int) | 100 | ours: 90; also written at `0x512B11` (menu?) |
| `dcoeff` | +0x78 (double) | 1.0 | ours: 0.15. Tested 0.15 vs 5: same set of draws at the start. Not draw distance. |
| `fog` | "on start end n": +0x18 start, +0x10 end (doubles), +0x48 int | 300 / 900 | ours: `on 10000 20000` |
| `farplane` | +0x20 (double) | 100000 | not in our TRUCK.INI |
| `nearplane` | +0x28 (double) | 1.0 | |
| `skyzb` / `skyfun` | +0x60 / +0x68 | 0 / 1.0 | |
| `tanglimith` | global `0x682A08` | 314.159 | |
| `zbuf`, `antialiasing` | flags in `[engine + 0xBCC]` | | |

## World and draw distance

- The world is split into rooms (`ai_room.cpp`, `RoomId`, `updatePlayerRooms()`, `getPositionId(RoomId, ...)`,
  `$$$DefaultRoom`, `UNDEFINED_ROOM`, room names like `room_at_004`). Map data: `.b3d` files with clusters.
- **The view distance comes from WEATHER.INI** (found 28.10). TRUCK.INI `fog=on 10000 20000` and the far plane
  default 100000 are overwritten every frame: `0x5A9700..0x5A9776` interpolates the weather tables for the time of
  day with `0x5A9240` (cdecl `float, float, const float* table` → st0) and stores
  fog far (`fogf_*`, table `0x6FC9B8`) → settings `+0x10`, fog near (`fogn_*`, table `0x6FCA58`) → `+0x18`,
  far clip plane = `frpl_*` (table `0x6FD75C`) × fog far → `+0x20`. Only these three calls use `0x5A9240`
  (`0x5A972F`, `0x5A974D`, `0x5A976B`). Weather tables are filled when WEATHER.INI is read (`0x5A83B7..`).
- Stock WEATHER.INI: fog far 900 good day/night, 250 bad day, 100 bad night; `frpl` 0.6 (good) / 1 (bad).
  So the far plane is ~540 units on a clear day and 100 on a bad night: this is the "map loads close" limit.
- `DrawDistance` (#29) wraps the fog far and fog near calls and multiplies their result, so fog and the far plane
  move out together. The alternative (shipping an edited WEATHER.INI) would overwrite players' file.
- External references: AlexKimov/HardTruck-RignRoll-file-formats (b3d format wiki and 010 Editor template
  `B3D.bt`), KoTR Modern Patch, KOTRunity, Blender b3d plugins.

### Scene graph (b3d node classes)

- Node factory by b3d block type: `switch` at `0x5EF877`, jump table `0x5F0614` (types 0..41).
  Examples: type 10/11 → class vtable `0x652AA0` (size 0x44, +0x38 = 1 or 3); type 19/41 → `0x652D68` (size 0x140);
  type 29 → `0x6528D8`; type 30 → `0x65288C` (size 0xC4, a portal to another room).
- Generic group base class vtable `0x652CB4` (ctor `0x5F19B0`): children array `+0x30`, count `+0x34`.
  `SetSwitch(node, index)` = `0x5E1A10` (node type `0x12`).
- Render context (passed as the second argument to render methods): `+0x2108` = 100 − detail (clamped 0..100),
  `+0x2110` near plane, `+0x2118` far plane, `+0x2148` dcoeff, `+0x8C0 + 0x30*k` camera matrices,
  `+0x2020` / `+0x202C` stack indices. Filled each frame at `0x5EB310` from the settings struct.
- `detail` in the settings struct is overwritten from the player profile (`[*0x6CEC70 + 0x1A6]`, WORD, at `0x512B11`),
  i.e. the «Графика → Детальность» slider (ours is at max), so TRUCK.INI `detail` has no effect.
- Engine object: `*0x696CA0`; settings struct `[engine + 0x1D8]`.

### Culling candidates checked (28.10)

- Type 29 LOD (`0x5F9F20`): picks a child by comparing `detail% × depth × 0.01` (or a screen-size metric, mode 1)
  with per-node thresholds and draws nothing past the last one. Scaling the depth constant (`fmul [0x64B5A0]` at
  `0x5FA1A1`) by 1/4 changed nothing measurable.
- Type 10/11 distance switch (`0x5F8F10` → condition `0x5F88D0`): `|camera − center|` vs a radius, via the length
  helper `0x5AB4D0` (call sites `0x5F894C`, `0x5F89B1`, `0x5F8A99`, `0x5F8B93`). Dividing the length by 4 changed
  nothing measurable either.
- Type 30 portal (`0x5FB820`): checks the portal against the frustum (`0x5B4FC0`, uses near/far planes) and the camera
  side of the portal plane, then renders the connected room (`[portal + 0x80]`) clipped to the portal (`0x5B5710`).
  Not the limit after all: the far plane is (see WEATHER.INI above).
- Measurement: `DebugDraws` frame dumps now print each draw's depth in the world camera (`d<depth>`). At the start
  base world draws end at depth ~216; at the dealership road ~306, with and without the patches above. The start
  base is closed in by forest, so a spot with a long open view is needed for a fair comparison.
- b3d LOD data (scan of `ENV/*.b3d`, 28.10): 1036 rooms (type 19), 2215 portals (type 30), 2574 type 10 switches.
  Type 10 `Distance` p10 1059, p50 5000, p90 29062 (common 5000, 1000, 30000, 22500). The condition `0x5F88D0`
  compares it with a real length (`0x5AB4D0` takes sqrt), so these are radii in world units, mostly past the far plane.

## Untested leads

- `VVK_BEAMS` key command (string at `0x67A8A0`, used at `0x560BD6`).
- `effect_Glow_Beam` / `effect_Glow_Particle`: the game's own glow effects per vehicle.
