param(
    [string]$Type,
    [string]$NumberFile,
    [string]$Major,
    [string]$Minor,
    [string]$AppName,
    [string]$BinDir,
    [string]$Repo,
    [string]$Obpatch = ""
)

# Game releases also get a binary patch (ObeyCraft-v<version>-binary.obpatch)
# from the previous game release's zip, made by the obpatch tool; the launcher
# applies it to the installed files instead of downloading the changed ones.
# The previous zip is kept in <BinDir>\.release_cache (downloaded once when
# missing). The patch name must not contain a platform tag: launchers from
# before delta updates pick the asset whose name contains "windows".

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# Zips $Items the way Compress-Archive does (a file at the root, a folder as
# itself with everything under it), but with '/' separators and audio and
# images stored rather than deflated: they are already compressed, and an
# install would only spend time inflating them.
function New-ReleaseZip([string[]]$Items, [string]$ZipPath) {
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    $stored = @('.ogg', '.png', '.jpg', '.zip')
    $zip = [System.IO.Compression.ZipFile]::Open($ZipPath, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($item in $Items) {
            $full = (Resolve-Path -LiteralPath $item).Path
            $parent = Split-Path -Parent $full
            if (Test-Path -LiteralPath $full -PathType Container) {
                $files = Get-ChildItem -LiteralPath $full -Recurse -File
            } else {
                $files = @(Get-Item -LiteralPath $full)
            }
            foreach ($f in $files) {
                $name = $f.FullName.Substring($parent.Length).TrimStart('\', '/') -replace '\\', '/'
                if ($stored -contains $f.Extension.ToLowerInvariant()) {
                    $level = [System.IO.Compression.CompressionLevel]::NoCompression
                } else {
                    $level = [System.IO.Compression.CompressionLevel]::Optimal
                }
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $name, $level)
            }
        }
    } finally {
        $zip.Dispose()
    }
}

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
        Remove-Item "$BinDir\ObeyCraft-v*-binary.obpatch" -ErrorAction SilentlyContinue
    }

    $ZipPath = "$BinDir\$ZipName"
    if ($Type -eq "game") {
        # What ships is DERIVED from the build, never listed here. CMake writes
        # package_manifest.txt from GAME_PAYLOAD_DIRS plus the Sentry crashpad
        # helpers, so a new runtime dependency is added in exactly one place and
        # this script picks it up automatically.
        #
        # It used to be a hand-written list, and that list omitted data/. Every
        # published Windows build therefore could not generate terrain: players
        # spawned into an empty world with "No chunk provider available" in the
        # log while the build tree worked fine. An include-list fails open - a
        # forgotten entry ships a broken game - which is why it is gone.
        $manifestPath = "$BinDir\package_manifest.txt"
        if (-not (Test-Path $manifestPath)) {
            throw "[auto-release] No package_manifest.txt in $BinDir - reconfigure CMake (it is written by file(GENERATE))."
        }
        $entries = Get-Content $manifestPath | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
        if (-not $entries) {
            throw "[auto-release] package_manifest.txt is empty - refusing to publish an exe with no payload."
        }
        $items = @("$BinDir\$AppName") + ($entries | ForEach-Object { Join-Path $BinDir $_ })
        $missing = $items | Where-Object { -not (Test-Path $_) }
        if ($missing) {
            # Fail the release rather than publish a zip that cannot generate a
            # world. A silently incomplete build is worse than no build at all.
            throw "[auto-release] Refusing to package: missing $($missing -join ', ')"
        }
        Write-Host "[auto-release] Packaging: $AppName + $($entries -join ', ')"
        New-ReleaseZip -Items $items -ZipPath $ZipPath
    } else {
        New-ReleaseZip -Items @("$BinDir\$AppName") -ZipPath $ZipPath
    }
    Write-Host "[auto-release] Zipped: $ZipName"

    $assets = @($ZipPath)
    $haveGh = [bool](Get-Command gh -ErrorAction SilentlyContinue)

    # Binary patch from the previous game release (non-fatal: without it the
    # launcher fetches the changed files from the zip).
    if ($Type -eq "game" -and $haveGh -and $Obpatch -and (Test-Path $Obpatch)) {
        try {
            $current = [version]$Version
            $prevTag = & gh release list --repo $Repo --limit 200 --json tagName --jq '.[].tagName' 2>$null |
                Where-Object { $_ -match '^game-win-v\d+\.\d+\.\d+$' -and $_ -ne $Tag } |
                Where-Object { [version]($_ -replace '^game-win-v', '') -lt $current } |
                Sort-Object { [version]($_ -replace '^game-win-v', '') } |
                Select-Object -Last 1
            if ($prevTag) {
                $prevVersion = $prevTag -replace '^game-win-v', ''
                $cache = "$BinDir\.release_cache"
                New-Item -ItemType Directory -Force -Path $cache | Out-Null
                $prevZip = "$cache\ObeyCraft-v$prevVersion-windows-x64.zip"
                if (-not (Test-Path $prevZip)) {
                    Write-Host "[auto-release] Downloading previous release $prevTag for the binary patch..."
                    & gh release download $prevTag --repo $Repo --pattern "*windows-x64.zip" --dir $cache --clobber 2>$null | Out-Null
                }
                if (Test-Path $prevZip) {
                    $patchPath = "$BinDir\ObeyCraft-v$Version-binary.obpatch"
                    Write-Host "[auto-release] Diffing against $prevTag..."
                    & $Obpatch make $prevZip $ZipPath $patchPath
                    if ($LASTEXITCODE -eq 0) {
                        & $Obpatch verify $prevZip $ZipPath $patchPath | Out-Null
                    }
                    if ($LASTEXITCODE -eq 0 -and (Test-Path $patchPath)) {
                        $assets += $patchPath
                        Write-Host "[auto-release] Patch: $(Split-Path -Leaf $patchPath)"
                    } else {
                        Remove-Item $patchPath -ErrorAction SilentlyContinue
                        Write-Host "[auto-release] No binary patch for this release"
                    }
                }
            }
        } catch {
            Write-Host "[auto-release] Binary patch skipped: $_"
        }
    }

    if ($haveGh) {
        $null = & gh release view $Tag --repo $Repo 2>&1
        if ($LASTEXITCODE -eq 0) {
            & gh release upload $Tag @assets --repo $Repo --clobber
            Write-Host "[auto-release] Updated release: $Tag"
        } else {
            & gh release create $Tag --repo $Repo --title $Tag --notes "Auto-release $Tag" @assets
            Write-Host "[auto-release] Created release: $Tag"
        }

        # The next release diffs against this one: keep its zip, and only its.
        if ($Type -eq "game") {
            $cache = "$BinDir\.release_cache"
            New-Item -ItemType Directory -Force -Path $cache | Out-Null
            Remove-Item "$cache\ObeyCraft-v*-windows-x64.zip" -ErrorAction SilentlyContinue
            Copy-Item $ZipPath $cache
        }
    } else {
        Write-Host "[auto-release] gh CLI not found - zip created but not uploaded"
    }
} catch {
    Write-Host "[auto-release] Warning: $_"
}

exit 0
