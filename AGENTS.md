# Agent guide: Hard Truck 2 RTX

Read this before changing anything. Most of it was learned the hard way.

## What this is

- **Game:** «Дальнобойщики 2» / Hard Truck 2: King of the Road, the Steam release (AppID `4487840`). It is a 32-bit
  DirectX 7 game. On the owner's PC it lives in
  `D:\SteamLibrary\steamapps\common\Hard Truck 2 King of the Road`, 3840x2160, borderless.
- **D2GI** (this repo, a fork) builds `ddraw.dll`, a DX7→DX9 wrapper. Our additions are fixes
  that make **RTX Remix** (the x86 runtime: `d3d9.dll` + `.trex/`) work with the game. Each fix is a switch in
  the `[REMIX]` section of `d2gi.ini`.
- **Remix mod** (`remix-mod/`): `rtx.conf` (texture categories) and `mod.usda` (material replacements).
- **Owner:** not a programmer. Talk to them in Russian, in plain words, and keep it short. Never ask them
  to edit code or configs by hand. Ship a build and tell them which button to press.

## Map

| Path | What |
|------|------|
| `D2GI/src/d2gi/d2gi.cpp` | Most Remix fixes (device keep, HUD replay, static blit textures, untextured colors) |
| `D2GI/src/d2gi/d2gi_config.*` | `[REMIX]` switches: static bool, read with `GetPrivateProfileInt`, logged in one `Remix options:` line |
| `D2GI/src/d2gi/d2gi_hooks.cpp` | Patches into `king.exe` (UI, minimap, texture-name injection) |
| `D2GI/d2gi.ini` | Shipped ini; every switch is documented by a comment above it |
| `remix-mod/package/` | Contents of the game's `_rtx_remix_mod` folder (see Install) |
| `tools/make_bundle.py` | Builds `HT2-RTX.zip` |
| `tools/windows/` | `RTX-Update.cmd`, `RTX-Play.cmd`, `RTX-Logs.cmd`, `RTX-Workshop.cmd` for the gaming PC |
| `remix-mod/workshop/` | Steam Workshop descriptor (`mod.cfg`: title, description, visibility, priority -5) and preview |
| `docs/REMIX_BACKLOG.md` | The board (GitHub Issues are off). Update it in the same PR as the change. |

## Build → release → install

1. **Build only in CI** (windows-2022, msbuild v143, x86). There is no local build on Linux. Check a run:
   `gh api repos/ISPlatonov/D2GI/actions/runs?head_sha=<sha> --jq '.workflow_runs[]|.status+" "+(.conclusion//"")'`.
2. **CI publishes `HT2-RTX.zip`** as a release: `latest` for master, `pr-N` for each PR (deleted when the PR closes).
   URL: `https://github.com/ISPlatonov/D2GI/releases/download/<tag>/HT2-RTX.zip`. Do not use Actions artifacts:
   they need auth and the owner had to download them by hand.
3. **Install into `<game>\_rtx_remix_mod\` only.** On every Steam launch WorkshopModSync copies that folder into the
   game root (priority -10, wins over Modern Patch and stock D2GI). Anything you copy straight into the game root,
   or into `Workshop Backups\`, gets reverted or corrupts its bookkeeping. On the PC: `RTX-Update.cmd -Pr N`.
   Without Windows shell access, see the `ht2-test-build` skill.
4. **Launch through Steam** (`RTX-Play.cmd` / `steam://rungameid/4487840`). Starting `king.exe` directly skips
   WorkshopModSync.

## Steam Workshop

The game has an official uploader (`Workshop Uploader GUI - Hard Truck 2.exe`) and mod format (`Mod Template\mod.cfg`).
`RTX-Workshop.cmd` builds `<game>\_workshop_stage` from the installed build plus the RTX Remix runtime and opens the
uploader. The owner presses Publish (it is their Steam account; never publish or change visibility without asking).
The uploader warns about every .exe/.dll (43 warnings): that is expected, 0 errors means it can be published.
Never start the uploader with `--folder`: it then writes a default `workshop.cfg` into that folder, which would be
published and overwrite players' `workshop.cfg`. Its folder field is read-only; use Browse (a folder tree dialog:
double-click to expand, click the folder, OK). With computer use the uploader window is granted and usable.
The item is `3828304518` (friends only); `_workshop_stage\.workshop_id` keeps it, so the button says Update.
Priorities: Workshop item -5 (wins over D2GI / Modern Patch at 0), local test folder `_rtx_remix_mod` -10 (wins over
the Workshop item, so test builds still apply).

## Hard rules

- **One game instance.** Check `Get-Process king` before launching. Never "focus" the game by opening the app
  (computer-use `open_application` starts another `king.exe`); click its window instead. The owner asked for this.
- **Do not press Escape in game while the Remix menu is open.** It goes to the game, which re-enumerates modes and
  crashes the Remix bridge. Close the menu with Alt+X.
- **New `[REMIX]` switches default to off** in code (`FALSE` in `GetPrivateProfileInt`) and are turned on in
  `D2GI/d2gi.ini`. Old configs then keep their old behaviour. Add the switch to the `Remix options:` log line.
- **Remix texture hashes are content hashes.** If a texture's pixels change, its hash changes and every
  `rtx.conf`/`mod.usda` entry for it stops matching. This is why `UntexturedColors` (color-dependent hashes) and
  `StaticBlitTextures` (constant content for per-frame blits) exist. Keep `rtx.recomputeTextureHashOnWrite`
  **off**: it makes per-frame textures rehash every frame.
- **`rtx.viewModel.enable = True` is required.** Without it the hood is not drawn. Side effect: everything the cab
  camera draws (rain layer, glass) becomes a view model. Check cab-camera effects after any change there.
- **`KeepDevice` is required.** The Remix bridge crashes when the game recreates the D3D9 device on entering or leaving 3D.
- **Match draws narrowly.** When skipping or changing a draw, match it by texture name or hash, not by a generic
  shape ("any 4-vertex fan"), or you will hit mirrors, HUD or minimap draws too. Log the first hit once.
- **Done means seen in game.** Do not mark a backlog item done, or tell the owner it is fixed, until it was checked
  in game (your own screenshot, or the owner confirmed it).
- Never use the owner's tokens, cookies or passwords, even if offered. Ask them to grant access instead.

## Logs (game root unless noted)

| File | What |
|------|------|
| `d2gi.log` | D2GI: `Remix options:` line, device keep/reset, `Remix: ...` one-time notices |
| `d2gi_remix.log` | Per-draw signatures when `DebugDraws = 1` (heavy; turn off after) |
| `rtx-remix/logs/remix-dxvk.log` | Remix runtime; category changes are logged as `<category> - added (positive opinion) <HASH>` |
| `rtx-remix/logs/bridge32.log`, `crash-upload.log` | Bridge crashes |
| `WorkshopModSync_log.txt` | What Steam's mod sync applied (`applied RTX Remix (priority -10) - N file(s)`) |

`RTX-Logs.cmd` zips all of them to the Desktop.

## Workflow

Board-driven (see the `board-driven-dev` skill, if installed): take an item from `docs/REMIX_BACKLOG.md`, branch
`<type>/<slug>`, conventional commits, one small PR per item with a test plan. The PR gets its own `pr-N` build.
Tell the owner `RTX-Update.cmd -Pr N`, or install it yourself (`ht2-test-build`). Move the item to "In review",
and to "Done" only after the in-game check.

Project skills live in `.cursor/skills/`:
- `ht2-test-build`: install a build on the gaming PC, launch, read logs, check in game.
- `ht2-remix-texture-hash`: find a texture's Remix hash and categorize or replace it.
- `ht2-remix-fix`: add a D2GI `[REMIX]` fix end to end.
