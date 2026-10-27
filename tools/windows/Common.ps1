# Shared helpers for the HT2 RTX tools. Lives in <game>\_ht2rtx_tools\.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Repo     = 'ISPlatonov/D2GI'
$SteamApp = 4487840
$GameDir  = Split-Path -Parent $PSScriptRoot
$ModDir   = Join-Path $GameDir '_rtx_remix_mod'
$ToolsDir = Join-Path $GameDir '_ht2rtx_tools'
$BackupDir = Join-Path $GameDir '_backup_d2gi'

function Say([string]$text, [string]$color = 'Gray') { Write-Host $text -ForegroundColor $color }

function Assert-GameDir {
    if (-not (Test-Path (Join-Path $GameDir 'king.exe'))) {
        throw "Не нашёл king.exe в $GameDir. Скрипт должен лежать в папке игры, в _ht2rtx_tools."
    }
}

function Test-GameRunning {
    return [bool](Get-Process -Name king -ErrorAction SilentlyContinue)
}

function Get-InstalledVersion {
    $v = Join-Path $ModDir 'VERSION.txt'
    if (Test-Path $v) { return (Get-Content $v -Raw).Trim() }
    return 'неизвестна'
}

# WorkshopModSync copies this folder into the game on every Steam launch.
function Ensure-LocalModRegistered {
    $cfg = Join-Path $GameDir 'workshop.cfg'
    $line = "setf ~local_mod_folder {RTX Remix} $ModDir"
    if (-not (Test-Path $cfg)) { Set-Content -Path $cfg -Value $line -Encoding ASCII; return }
    $text = Get-Content $cfg -Raw
    if ($text -notmatch [regex]::Escape('_rtx_remix_mod')) {
        Add-Content -Path $cfg -Value "`r`n$line" -Encoding ASCII
        Say 'Добавил папку мода в workshop.cfg.'
    }
}
