param(
    [string]$Type,
    [string]$NumberFile,
    [string]$Major,
    [string]$Minor,
    [string]$SourceRoot
)

$N = 0
if (Test-Path $NumberFile) { $N = [int](Get-Content $NumberFile -Raw).Trim() }
$N++
Set-Content $NumberFile $N

$Version = "$Major.$Minor.$N"

if ($Type -eq "launcher") {
    $Config = "$SourceRoot/src/launcher/LauncherConfig.hpp"
    $Pattern = 'LauncherVersion = "[^"]*"'
    $Replacement = "LauncherVersion = `"$Version`""
} else {
    $Config = "$SourceRoot/src/common/core/Config.hpp"
    $Pattern = 'GAME_VERSION "[^"]*"'
    $Replacement = "GAME_VERSION `"$Version`""
}

$content = [System.IO.File]::ReadAllText($Config)
$content = [System.Text.RegularExpressions.Regex]::Replace($content, $Pattern, $Replacement)
[System.IO.File]::WriteAllText($Config, $content)

Write-Host "[bump-version] $Type version -> $Version"
