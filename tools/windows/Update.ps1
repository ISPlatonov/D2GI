# Download the HT2 RTX bundle from GitHub releases and install it.
#   RTX-Update.cmd             latest build from master
#   RTX-Update.cmd -Pr 10      build of pull request #10 (for testing before merge)
#   RTX-Update.cmd -Zip x.zip  install a bundle that is already on disk
#   RTX-Update.cmd -Restore    roll back to the previous install
param([int]$Pr = 0, [string]$Tag = 'latest', [string]$Zip = '', [switch]$Restore)
. (Join-Path $PSScriptRoot 'Common.ps1')

Assert-GameDir
if (Test-GameRunning) { Say 'Игра запущена. Закрой её и запусти обновление ещё раз.' Yellow; exit 1 }

if ($Restore) {
    $last = Get-ChildItem $BackupDir -Filter 'rtx_mod-*.zip' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if (-not $last) { Say 'Бэкапов нет.' Yellow; exit 1 }
    Get-ChildItem $ModDir -Force | Remove-Item -Recurse -Force
    Expand-Archive -Path $last.FullName -DestinationPath $ModDir -Force
    Say "Откатил на $($last.Name). Версия: $(Get-InstalledVersion)" Green
    exit 0
}

if ($Pr -gt 0) { $Tag = "pr-$Pr" }
$tmp = Join-Path $env:TEMP ("ht2rtx-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    if ($Zip) {
        $bundle = (Resolve-Path $Zip).Path
    } else {
        $bundle = Join-Path $tmp 'HT2-RTX.zip'
        $url = "https://github.com/$Repo/releases/download/$Tag/HT2-RTX.zip"
        Say "Скачиваю $url"
        try { Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $bundle }
        catch { Say "Не получилось скачать сборку '$Tag'. Есть ли такой релиз на GitHub? $_" Red; exit 1 }
    }

    $x = Join-Path $tmp 'x'
    Expand-Archive -Path $bundle -DestinationPath $x -Force
    if (-not (Test-Path (Join-Path $x 'mod\ddraw.dll'))) { throw 'В архиве нет mod\ddraw.dll — это не сборка HT2 RTX.' }

    Say "Было:  $(Get-InstalledVersion)"
    if (Test-Path $ModDir) {
        New-Item -ItemType Directory $BackupDir -Force | Out-Null
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        Compress-Archive -Path (Join-Path $ModDir '*') -DestinationPath (Join-Path $BackupDir "rtx_mod-$stamp.zip")
        Get-ChildItem $BackupDir -Filter 'rtx_mod-*.zip' | Sort-Object Name | Select-Object -SkipLast 5 | Remove-Item -Force
        Get-ChildItem $ModDir -Force | Remove-Item -Recurse -Force
    } else {
        New-Item -ItemType Directory $ModDir | Out-Null
    }
    Copy-Item (Join-Path $x 'mod\*') $ModDir -Recurse -Force

    New-Item -ItemType Directory $ToolsDir -Force | Out-Null
    Copy-Item (Join-Path $x '_ht2rtx_tools\*') $ToolsDir -Recurse -Force
    Get-ChildItem $x -Filter '*.cmd' | Copy-Item -Destination $GameDir -Force

    Ensure-LocalModRegistered
    Ensure-LargeAddressAware
    Say "Стало: $(Get-InstalledVersion)" Green
    Say 'Готово. Запускай игру через RTX-Play.cmd или Steam — Steam сам применит мод при запуске.' Green
}
finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
