param([switch]$Headset, [int]$Hold = 10)
$bat = Join-Path $PSScriptRoot 'oot_vr_build.bat'
Remove-Item D:\lxc_build.log -ErrorAction SilentlyContinue
Start-Process -NoNewWindow -FilePath cmd.exe -ArgumentList '/c', "$bat > D:\lxc_build.log 2>&1"
Start-Sleep 2
& 'D:\Games\Emulators\recomp\tools\build_progress.ps1' -Log 'D:\lxc_build.log'
if (-not (Select-String -Path D:\lxc_build.log -Pattern '^BUILD DONE exit 0' -Quiet)) { Write-Host 'build failed: no test' -ForegroundColor Red; return }
Remove-Item D:\lxc\vrtest\*.png -ErrorAction SilentlyContinue
$args = @('-I', 'tools\nx2\oot_vr_test.py', '--hold', $Hold, '--exe', 'D:\lxc\bin\lichii.exe',
          '--game', 'D:\Games\Emulators\SW2 roms\The Legend of Zelda Ocarina of Time [0400486024dec000][v131072].nup',
          '--out', 'D:\lxc\vrtest')
if ($Headset) { $args += '--headset' }
& python @args
