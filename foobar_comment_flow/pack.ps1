param(
    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",
    [string]$Version  = ""
)

$ErrorActionPreference = "Stop"

$PROJECT    = $PSScriptRoot
$REPO_ROOT   = Resolve-Path "$PROJECT\.."

function Get-LatestGitTagVersion {
    Push-Location $REPO_ROOT
    try {
        $tag = (git tag --sort=-v:refname | Select-Object -First 1)
        if (-not $tag) {
            throw "No Git tags found. Pass -Version explicitly or create a version tag."
        }
        return ($tag -replace '^[vV]', '')
    } finally {
        Pop-Location
    }
}

function Get-VersionFromHeader {
    $headerPath = "$PROJECT\foo_danmaku\foo_danmaku.h"
    if (Test-Path $headerPath) {
        $content = Get-Content $headerPath
        $major = $content | Select-String "MY_COMPONENT_VERSION_MAJOR\s+(\d+)" | ForEach-Object { $_.Matches.Groups[1].Value }
        $minor = $content | Select-String "MY_COMPONENT_VERSION_MINOR\s+(\d+)" | ForEach-Object { $_.Matches.Groups[1].Value }
        $build = $content | Select-String "MY_COMPONENT_VERSION_BUILD\s+(\d+)" | ForEach-Object { $_.Matches.Groups[1].Value }
        if ($null -ne $major -and $null -ne $minor -and $null -ne $build) {
            return "${major}.${minor}.${build}"
        }
    }
    return "1.0.0"
}

if ([string]::IsNullOrWhiteSpace($Version)) {
    try {
        $Version = Get-LatestGitTagVersion
    } catch {
        $Version = Get-VersionFromHeader
        Write-Host "Fallback to version from header: $Version"
    }
}

$DIST            = "$PROJECT\dist"
$COMPONENT_NAME  = "foo_danmaku"
$OUTFILE         = "$DIST\${COMPONENT_NAME}-${Platform}-${Version}.fb2k-component"

# ── DLL Paths ─────────────────────────────────────────
if ($Platform -eq "Win32") {
    $FOO_DLL = "$PROJECT\foo_danmaku\build\Win32\foo_danmaku.dll"
    $NETEASE_DLL = "$PROJECT\..\netease_client\build\Win32\netease_client.dll"
    $QQMUSIC_DLL = "$PROJECT\..\qqmusic_client\build\Win32\qqmusic_client.dll"
} else {
    $FOO_DLL = "$PROJECT\foo_danmaku\build\x64\foo_danmaku.dll"
    $NETEASE_DLL = "$PROJECT\..\netease_client\build\x64\netease_client.dll"
    $QQMUSIC_DLL = "$PROJECT\..\qqmusic_client\build\x64\qqmusic_client.dll"
}

# ── Prerequisite checks ────────────────────────────────
if (-not (Test-Path $FOO_DLL) -or -not (Test-Path $NETEASE_DLL)) {
    throw "No build artifacts found for $Platform. Run the build scripts first."
}

Write-Host "Packaging $COMPONENT_NAME ($Platform) v$Version ..."

# ── Stage area ─────────────────────────────────────────
$stage = "$env:TEMP\fb2k_pack_${COMPONENT_NAME}_${Platform}"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

# ── Copy Payload (always goes to root for single architecture package) ──
Copy-Item $FOO_DLL "$stage\foo_danmaku.dll"
Copy-Item $NETEASE_DLL "$stage\netease_client.dll"
if (Test-Path $QQMUSIC_DLL) {
    Copy-Item $QQMUSIC_DLL "$stage\qqmusic_client.dll"
}

# ── Create zip and rename to .fb2k-component ──────────
if (-not (Test-Path $DIST)) { New-Item -ItemType Directory -Path $DIST | Out-Null }

$zipTmp = "$DIST\${COMPONENT_NAME}-${Platform}-${Version}.zip"
if (Test-Path $zipTmp)    { Remove-Item $zipTmp    -Force }
if (Test-Path $OUTFILE)   { Remove-Item $OUTFILE   -Force }

Compress-Archive -Path "$stage\*" -DestinationPath $zipTmp
Rename-Item $zipTmp $OUTFILE

# ── Cleanup ────────────────────────────────────────────
Remove-Item $stage -Recurse -Force

# ── Summary ────────────────────────────────────────────
$size = (Get-Item $OUTFILE).Length
Write-Host ""
Write-Host "SUCCESS  $OUTFILE"
Write-Host "  Size : $([math]::Round($size/1KB, 1)) KB"
Write-Host ""
Write-Host "Architecture: $Platform"
Write-Host ""
Write-Host "Install:"
Write-Host "  Double-click the .fb2k-component file, or drag it onto foobar2000."
Write-Host "  The installer will copy all DLLs to the components directory."
