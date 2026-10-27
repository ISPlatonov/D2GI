# Tools

## For players (Windows)

The bundle installs four launchers into the game folder:

| File | What it does |
|------|--------------|
| `RTX-Update.cmd` | Downloads the latest build from GitHub, backs up the current one, installs it. `RTX-Update.cmd -Pr 10` installs the build of pull request #10. `-Restore` rolls back. `-Zip file.zip` installs a local bundle. |
| `RTX-Play.cmd` | Starts the game through Steam (Steam applies the mod). Does nothing if the game is already running. |
| `RTX-Logs.cmd` | Zips logs and configs to the Desktop for a bug report. |
| `RTX-Workshop.cmd` | Builds `_workshop_stage` (mod + RTX Remix runtime + licenses, ~900 MB) and opens the official Workshop uploader on it. Press Publish there. After the first publish the uploader keeps `.workshop_id` in that folder, so later publishes update the same item. |

First install (once): download `HT2-RTX.zip` from the
[latest release](https://github.com/ISPlatonov/D2GI/releases/latest), unpack it into the game folder
and run `RTX-Update.cmd`. After that, `RTX-Update.cmd` is all you need.

## For CI and agents

- `make_bundle.py` — builds `HT2-RTX.zip` from a built `ddraw.dll` and `remix-mod/package`.
- `windows/` — sources of the scripts above (CRLF, UTF-8 with BOM for Windows PowerShell 5.1).
