$ISCC = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
$ISS  = "$PSScriptRoot\create_installer.iss"
& $ISCC $ISS
Write-Host "Installer created: cmake-build-release\ObeyCraftLauncherSetup.exe"
