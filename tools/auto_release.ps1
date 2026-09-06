param(
    [string]$Type,
    [string]$NumberFile,
    [string]$Major,
    [string]$Minor,
    [string]$AppName,
    [string]$BinDir,
    [string]$Repo
)

try {
    $N = (Get-Content $NumberFile -Raw).Trim()
    $Version = "$Major.$Minor.$N"

    if ($Type -eq "launcher") {
        $Tag = "launcher-win-v$Version"
        $ZipName = "ObeyCraftLauncher-v$Version-windows-x64.zip"
        Remove-Item "$BinDir\ObeyCraftLauncher-v*-windows-x64.zip" -ErrorAction SilentlyContinue
    } else {
        $Tag = "game-win-v$Version"
        $ZipName = "ObeyCraft-v$Version-windows-x64.zip"
        Remove-Item "$BinDir\ObeyCraft-v*-windows-x64.zip" -ErrorAction SilentlyContinue
    }

    $ZipPath = "$BinDir\$ZipName"
    if ($Type -eq "game") {
        # data/ is NOT optional. The terrain generator resolves MC_DATA_ROOT to
        # "<game dir>/data"; without it every chunk fails to generate, the chunk
        # provider is torn down, and the player spawns into an empty world with
        # only "No chunk provider available" in the log. It was missing from this
        # list once, which is exactly how that shipped. macOS gets it through the
        # bundle's Contents/Resources (CMakeLists), which is why only Windows broke.
        $items = @("$BinDir\$AppName", "$BinDir\shaders", "$BinDir\assets", "$BinDir\data")
        $missing = $items | Where-Object { -not (Test-Path $_) }
        if ($missing) {
            # Fail the release rather than publish a zip that cannot generate a
            # world. A silently incomplete build is worse than no build at all.
            throw "[auto-release] Refusing to package: missing $($missing -join ', ')"
        }
        Compress-Archive -Path $items -DestinationPath $ZipPath -Force
    } else {
        Compress-Archive -Path "$BinDir\$AppName" -DestinationPath $ZipPath -Force
    }
    Write-Host "[auto-release] Zipped: $ZipName"

    if (Get-Command gh -ErrorAction SilentlyContinue) {
        $null = & gh release view $Tag --repo $Repo 2>&1
        if ($LASTEXITCODE -eq 0) {
            & gh release upload $Tag $ZipPath --repo $Repo --clobber
            Write-Host "[auto-release] Updated release: $Tag"
        } else {
            & gh release create $Tag --repo $Repo --title $Tag --notes "Auto-release $Tag" $ZipPath
            Write-Host "[auto-release] Created release: $Tag"
        }
    } else {
        Write-Host "[auto-release] gh CLI not found - zip created but not uploaded"
    }
} catch {
    Write-Host "[auto-release] Warning: $_"
}

exit 0
