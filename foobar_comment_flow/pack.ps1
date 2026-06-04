param(
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

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-LatestGitTagVersion
}

$DIST            = "$PROJECT\dist"
$COMPONENT_NAME  = "foo_danmaku"
$OUTFILE         = "$DIST\${COMPONENT_NAME}-${Version}.fb2k-component"

# ── DLL Paths ─────────────────────────────────────────
$WIN32_FOO_DLL = "$PROJECT\foo_danmaku\build\Win32\foo_danmaku.dll"
$WIN32_NETEASE_DLL = "$PROJECT\..\netease_client\build\Win32\netease_client.dll"
$WIN32_QQMUSIC_DLL = "$PROJECT\..\qqmusic_client\build\Win32\qqmusic_client.dll"

$X64_FOO_DLL = "$PROJECT\foo_danmaku\build\x64\foo_danmaku.dll"
$X64_NETEASE_DLL = "$PROJECT\..\netease_client\build\x64\netease_client.dll"
$X64_QQMUSIC_DLL = "$PROJECT\..\qqmusic_client\build\x64\qqmusic_client.dll"

# ── Prerequisite checks ────────────────────────────────
$hasWin32 = (Test-Path $WIN32_FOO_DLL) -and (Test-Path $WIN32_NETEASE_DLL)
$hasX64   = (Test-Path $X64_FOO_DLL) -and (Test-Path $X64_NETEASE_DLL)

if (-not $hasWin32 -and -not $hasX64) {
    throw "No build artifacts found. Run the build scripts for Win32 and/or x64 first."
}

Write-Host "Packaging $COMPONENT_NAME v$Version ..."

# ── Stage area ─────────────────────────────────────────
$stage = "$env:TEMP\fb2k_pack_$COMPONENT_NAME"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

# ── Copy Win32 Payload (goes to root) ──────────────────
if ($hasWin32) {
    Write-Host "  Adding Win32 binaries to root..."
    Copy-Item $WIN32_FOO_DLL "$stage\foo_danmaku.dll"
    Copy-Item $WIN32_NETEASE_DLL     "$stage\netease_client.dll"
    if (Test-Path $WIN32_QQMUSIC_DLL) {
        Copy-Item $WIN32_QQMUSIC_DLL "$stage\qqmusic_client.dll"
    }
}

# ── Copy x64 Payload (goes to x64/ subdirectory) ───────
if ($hasX64) {
    Write-Host "  Adding x64 binaries to x64/..."
    $stageX64 = "$stage\x64"
    New-Item -ItemType Directory -Path $stageX64 | Out-Null
    Copy-Item $X64_FOO_DLL "$stageX64\foo_danmaku.dll"
    Copy-Item $X64_NETEASE_DLL     "$stageX64\netease_client.dll"
    if (Test-Path $X64_QQMUSIC_DLL) {
        Copy-Item $X64_QQMUSIC_DLL "$stageX64\qqmusic_client.dll"
    }
}

# ── Create zip and rename to .fb2k-component ──────────
if (-not (Test-Path $DIST)) { New-Item -ItemType Directory -Path $DIST | Out-Null }

$zipTmp = "$DIST\${COMPONENT_NAME}-${Version}.zip"
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
Write-Host "Cross-architecture structure:"
Write-Host "  Win32 (x86) included: $hasWin32"
Write-Host "  x64 included        : $hasX64"
Write-Host ""
Write-Host "Install:"
Write-Host "  Double-click the .fb2k-component file, or drag it onto foobar2000."
Write-Host "  The installer will copy all DLLs to the components directory."
