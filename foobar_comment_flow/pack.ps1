param(
    [ValidateSet("x64","Win32")]
    [string]$Platform = "x64",
    [string]$Version  = "2.0.2"
)

$ErrorActionPreference = "Stop"

$PROJECT    = $PSScriptRoot
$FOO_DANMAKU_DLL = "$PROJECT\foo_danmaku\build\$Platform\foo_danmaku.dll"
$NETEASE_DLL     = "$PROJECT\..\netease_client\build\$Platform\netease_client.dll"
$DIST            = "$PROJECT\dist"
$COMPONENT_NAME  = "foo_danmaku"
$OUTFILE         = "$DIST\${COMPONENT_NAME}-${Version}.fb2k-component"

# ── Prerequisite checks ────────────────────────────────
foreach ($f in @($FOO_DANMAKU_DLL, $NETEASE_DLL)) {
    if (-not (Test-Path $f)) {
        throw "Required artifact not found: $f`nRun the build scripts first."
    }
}

Write-Host "Packaging $COMPONENT_NAME v$Version ($Platform) ..."

# ── Stage area ─────────────────────────────────────────
# Use a temp subfolder so we don't accidentally include stale files.
$stage = "$env:TEMP\fb2k_pack_$COMPONENT_NAME"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path "$stage\$Platform" | Out-Null

# ── Copy payload ───────────────────────────────────────
# Per the foobar2000 packaging spec:
#   root\           <- legacy x86 foobar2000 (we have no x86 build; omit)
#   x64\            <- foobar2000 x64 (and ARM64EC via emulation)
#
# Both DLLs land in the architecture subfolder.
# foobar2000 will extract them together into the components directory,
# so netease_client.dll ends up next to foo_danmaku.dll at install time.

Copy-Item $FOO_DANMAKU_DLL "$stage\$Platform\foo_danmaku.dll"
Copy-Item $NETEASE_DLL     "$stage\$Platform\netease_client.dll"

Write-Host "  -> $Platform\foo_danmaku.dll"
Write-Host "  -> $Platform\netease_client.dll"

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
Write-Host "Install:"
Write-Host "  Double-click the .fb2k-component file, or drag it onto foobar2000."
Write-Host "  The installer will copy both DLLs to the components directory."
