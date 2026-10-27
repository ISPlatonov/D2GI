# Start the game through Steam (Steam runs WorkshopModSync, which applies _rtx_remix_mod).
# Refuses to start a second copy.
. (Join-Path $PSScriptRoot 'Common.ps1')
Assert-GameDir
if (Test-GameRunning) { Say 'Игра уже запущена — второй раз не запускаю.' Yellow; exit 0 }
Say "Версия мода: $(Get-InstalledVersion)"
Start-Process "steam://rungameid/$SteamApp"
Say 'Запускаю через Steam...' Green
