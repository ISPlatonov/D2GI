# Collect logs and configs into one zip on the Desktop, to attach to a chat or a GitHub issue.
. (Join-Path $PSScriptRoot 'Common.ps1')
Assert-GameDir
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$tmp = Join-Path $env:TEMP "ht2rtx-logs-$stamp"
New-Item -ItemType Directory $tmp | Out-Null
$items = @(
    'd2gi.log', 'd2gi_remix.log', 'd2gi.ini', 'rtx.conf', 'user.conf', 'workshop.cfg',
    'WorkshopModSync_log.txt', 'ht2_steam.log', 'warn.log', 'TRUCK.INI',
    '_rtx_remix_mod\VERSION.txt', 'rtx-remix\mods\HT2_RTX\mod.usda'
)
foreach ($i in $items) {
    $p = Join-Path $GameDir $i
    if (Test-Path $p) { Copy-Item $p (Join-Path $tmp ($i -replace '\\', '_')) }
}
$rl = Join-Path $GameDir 'rtx-remix\logs'
if (Test-Path $rl) { Get-ChildItem $rl -Filter '*.log' | Copy-Item -Destination $tmp }
$out = Join-Path ([Environment]::GetFolderPath('Desktop')) "HT2RTX-logs-$stamp.zip"
Compress-Archive -Path (Join-Path $tmp '*') -DestinationPath $out
Remove-Item $tmp -Recurse -Force
Say "Логи собраны: $out" Green
Start-Process explorer.exe "/select,`"$out`""
