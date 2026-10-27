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

# king.exe is a 32-bit exe without the large-address-aware flag, so it only gets 2 GB of address space.
# With RTX Remix (the bridge lives in the game process) that runs out when a new region loads, and the
# game crashes with "Failed to create texture". Setting the flag gives it 4 GB on 64-bit Windows.
# A Steam update or file check restores the original exe; RTX-Update.cmd sets the flag again.
function Ensure-LargeAddressAware {
    $exe = Join-Path $GameDir 'king.exe'
    $bytes = [IO.File]::ReadAllBytes($exe)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    $off = $pe + 4 + 18
    $ch = [BitConverter]::ToUInt16($bytes, $off)
    if ($ch -band 0x20) { return }
    New-Item -ItemType Directory $BackupDir -Force | Out-Null
    $orig = Join-Path $BackupDir 'king.exe.orig'
    if (-not (Test-Path $orig)) { Copy-Item $exe $orig }
    $ch = $ch -bor 0x20
    $bytes[$off] = [byte]($ch -band 0xFF)
    $bytes[$off + 1] = [byte](($ch -shr 8) -band 0xFF)
    [IO.File]::WriteAllBytes($exe, $bytes)
    Say 'king.exe: включил 4 ГБ памяти (оригинал в _backup_d2gi\king.exe.orig).' Green
}
