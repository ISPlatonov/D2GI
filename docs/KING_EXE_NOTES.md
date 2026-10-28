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
- Fog end (20000) and far plane (100000) are far beyond what is drawn, so they are not the limit. Working hypothesis:
  the game draws the player's room and its neighbours (visibility lists in the map), so the distance is set by the
  room graph. Next step: find the room selection in the renderer and add more rooms (backlog item 9).

## Untested leads

- `VVK_BEAMS` key command (string at `0x67A8A0`, used at `0x560BD6`).
- `effect_Glow_Beam` / `effect_Glow_Particle`: the game's own glow effects per vehicle.
