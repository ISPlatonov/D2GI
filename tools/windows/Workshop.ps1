# Build <game>\_workshop_stage for the Steam Workshop and open the official uploader on it.
# Contents: the installed mod (_rtx_remix_mod) + the RTX Remix runtime (d3d9.dll, .trex) + licenses,
# with the Workshop descriptor from _ht2rtx_tools\workshop. .workshop_id (written by the uploader after the
# first publish) is kept, so publishing again updates the same Workshop item.
. (Join-Path $PSScriptRoot 'Common.ps1')
Assert-GameDir

$Stage = Join-Path $GameDir '_workshop_stage'
$Desc  = Join-Path $ToolsDir 'workshop'
$Trex  = Join-Path $GameDir '.trex'
$D3D9  = Join-Path $GameDir 'd3d9.dll'
$Uploader = Join-Path $GameDir 'Workshop Uploader GUI - Hard Truck 2.exe'

if (-not (Test-Path (Join-Path $ModDir 'ddraw.dll'))) { Say 'Мод не установлен. Сначала запусти RTX-Update.cmd.' Red; exit 1 }
if (-not (Test-Path $Trex) -or -not (Test-Path $D3D9)) { Say 'RTX Remix не найден в папке игры (нет d3d9.dll или .trex).' Red; exit 1 }
if (-not (Test-Path (Join-Path $Desc 'mod.cfg'))) { Say "Нет описания для Workshop: $Desc\mod.cfg. Запусти RTX-Update.cmd." Red; exit 1 }

New-Item -ItemType Directory $Stage -Force | Out-Null
Get-ChildItem $Stage -Force | Where-Object { $_.Name -ne '.workshop_id' } | Remove-Item -Recurse -Force

Get-ChildItem $ModDir -Force | Where-Object { $_.Name -notin @('mod.cfg', 'preview.png', 'VERSION.txt') } |
    Copy-Item -Destination $Stage -Recurse -Force
Copy-Item (Join-Path $Desc 'preview.png') $Stage -Force

$version = ((Get-InstalledVersion) -split "`r?`n")[0].Trim()
$cfg = [IO.File]::ReadAllText((Join-Path $Desc 'mod.cfg'))
$cfg = [regex]::Replace($cfg, '(?m)^setf ~version [^\r\n]*', "setf ~version $version")
[IO.File]::WriteAllText((Join-Path $Stage 'mod.cfg'), $cfg, (New-Object Text.UTF8Encoding $false))

Copy-Item $D3D9 $Stage -Force
$trexOut = Join-Path $Stage '.trex'
New-Item -ItemType Directory $trexOut -Force | Out-Null
# .trex\rtx-remix holds runtime logs, not part of the runtime
Get-ChildItem $Trex -Force | Where-Object { $_.Name -ne 'rtx-remix' } | Copy-Item -Destination $trexOut -Recurse -Force

$lic = Join-Path $Stage 'rtx-remix-licenses'
New-Item -ItemType Directory $lic -Force | Out-Null
$remixLicense = Join-Path $GameDir 'LICENSE.txt'
if ((Test-Path $remixLicense) -and ((Get-Content $remixLicense -TotalCount 1) -match 'NVIDIA')) {
    Copy-Item $remixLicense (Join-Path $lic 'RTX-Remix-LICENSE.txt')
}
Get-ChildItem $GameDir -Filter 'ThirdPartyLicenses-*.txt' | Copy-Item -Destination $lic

$size = (Get-ChildItem $Stage -Recurse -Force -File | Measure-Object Length -Sum).Sum / 1MB
Say ("Папка для Workshop готова: {0} ({1:N0} МБ), версия {2}" -f $Stage, $size, $version) Green
if (Test-Path (Join-Path $Stage '.workshop_id')) { Say 'Публикация обновит уже существующий мод в Workshop.' }

# The uploader must NOT get --folder: with it, it treats that folder as its own config folder and writes a
# default workshop.cfg (logging off, no local mod folder) into the mod, which would then be published and
# overwrite the player's workshop.cfg. Start it plain and pick the folder with Browse.
Get-ChildItem $Stage -Filter 'workshop.cfg' -File -ErrorAction SilentlyContinue | Remove-Item -Force
if (Test-Path $Uploader) {
    Start-Process -FilePath $Uploader -WorkingDirectory $GameDir
    Say 'Открываю загрузчик: Browse -> папка игры -> _workshop_stage, потом Update (или Publish).' Green
}
