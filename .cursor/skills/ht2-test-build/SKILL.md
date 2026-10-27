---
name: ht2-test-build
description: Install a D2GI/RTX Remix build of Hard Truck 2 on the gaming PC, launch the game once, and verify the change from logs and screenshots. Use after CI built a PR or master, or when the owner reports a bug and you need to reproduce it.
---

# Test a build on the gaming PC

Game folder: `D:\SteamLibrary\steamapps\common\Hard Truck 2 King of the Road` (below: `<game>`).
Build tags: `pr-N` for pull request N, `latest` for master.

## 1. Wait for the build

```bash
gh api "repos/ISPlatonov/D2GI/actions/runs?head_sha=$(git rev-parse HEAD)" \
  --jq '.workflow_runs[] | "\(.name) \(.status) \(.conclusion // "")"'
gh api repos/ISPlatonov/D2GI/releases/tags/pr-N --jq '.assets[].name'   # expect HT2-RTX.zip
```

If the build fails, fix it first. Do not hand the owner a build that does not exist.

## 2. Make sure the game is closed

Never install into a running game, and never start a second copy.
- Windows shell: `Get-Process king -ErrorAction SilentlyContinue`.
- Without a shell: take a screenshot. If the game is open, ask the owner to close it.

## 3. Install

**A. You have a Windows shell on the PC** (e.g. Cursor running there):
```powershell
& "<game>\_ht2rtx_tools\Update.ps1" -Pr N     # or no args for latest
```

**B. You only have file access to the game folder** (e.g. Claude with remote-device tools, where the device
shell is Linux and cannot run PowerShell):
1. Download in your own sandbox:
   `curl -sSLo HT2-RTX.zip https://github.com/ISPlatonov/D2GI/releases/download/pr-N/HT2-RTX.zip`
2. Copy the zip into the game folder (file-commit tool), then in the device shell:
   ```bash
   cd "$HOME/mnt/Hard Truck 2 King of the Road"
   mkdir -p _backup_d2gi && (cd _rtx_remix_mod && zip -qr "../_backup_d2gi/rtx_mod-$(date +%Y%m%d-%H%M%S).zip" .)
   unzip -qo HT2-RTX.zip 'mod/*' '_ht2rtx_tools/*' 'RTX-*.cmd' -d _incoming
   cp -r _incoming/mod/. _rtx_remix_mod/ && cp -r _incoming/_ht2rtx_tools/. _ht2rtx_tools/ && cp _incoming/RTX-*.cmd .
   cat _rtx_remix_mod/VERSION.txt
   ```
   Deleting may be blocked there. Overwriting is enough, but say if a file was renamed or removed in the bundle.

**C. No access at all:** tell the owner, in Russian: «Запусти `RTX-Update.cmd -Pr N` в папке игры (или просто
`RTX-Update.cmd`), потом `RTX-Play.cmd`».

Never copy `ddraw.dll`/`d2gi.ini`/`rtx.conf` straight into `<game>`. WorkshopModSync owns those and copies them from
`_rtx_remix_mod` at launch.

## 4. Launch

`RTX-Play.cmd` (or `steam://rungameid/4487840`). With computer-use, double-click `RTX-Play.cmd` in Explorer. To
focus the game later, click its window. Never use open_application on the game.

## 5. Verify

- `WorkshopModSync_log.txt` ends with `applied RTX Remix (priority -10) - N file(s)` for this launch.
- `d2gi.log`: the last `Remix options:` line shows the expected switches; look for your one-time `Remix: ...` notice.
- `rtx-remix/logs/remix-dxvk.log`: no new errors; `crash-upload.log` not updated.
- In game: drive to the situation (rain, night, cab view) and take a screenshot. Compare with the PR's test plan.
  - **Rain:** there is no reliable way yet. The «Погода» slider in «Новая игра» (cloud icon left, sun right; all the
    way left = «погода: дождливая») only makes rain more frequent. `txr\rain.txr` drawn once right after loading is
    not rain; check the frame dumps (`F<frame> ...` lines) for it. Do not edit `wstart`/`wtemp` in TRUCK.INI: it did
    not start rain, and `wtemp=0` hangs the game on loading. Leaving a running game: Esc → «Выход» → «Не сохранять».
    In the main menu Enter opens «Игра» (useful when the Claude window covers the right-hand buttons).
  - The game cursor follows the OS mouse. The Claude desktop window can sit over the right-hand menu buttons and is
    hidden in screenshots, so clicks there land in it. If a click on «Игра» does nothing, ask the owner to minimize it.
  If you cannot see the screen, ask the owner for a screenshot and say exactly what to look at.

Report in one or two lines what you saw. Then tick the PR test plan and update `docs/REMIX_BACKLOG.md`.

## Roll back

`RTX-Update.cmd -Restore` (restores the last backup from `_backup_d2gi`), or `RTX-Update.cmd` for latest master.
