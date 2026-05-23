param(
    [string]$Version  = "2.0.2",
    [ValidateSet("x64","Win32")]
    [string]$Platform = "x64"   # 仅用于选择构建产物来源，不影响包内结构
)

$ErrorActionPreference = "Stop"

$PROJECT    = $PSScriptRoot
$FOO_DANMAKU_DLL = "$PROJECT\foo_danmaku\build\$Platform\foo_danmaku.dll"
$NETEASE_DLL     = "$PROJECT\..\netease_client\build\$Platform\netease_client.dll"
$QQMUSIC_DLL     = "$PROJECT\..\qqmusic_client\build\$Platform\qqmusic_client.dll"
$DIST            = "$PROJECT\dist"
$COMPONENT_NAME  = "foo_danmaku"
$OUTFILE         = "$DIST\${COMPONENT_NAME}-${Version}.fb2k-component"

# ── Prerequisite checks ────────────────────────────────
foreach ($f in @($FOO_DANMAKU_DLL, $NETEASE_DLL)) {
    if (-not (Test-Path $f)) {
        throw "Required artifact not found: $f`nRun the build scripts first."
    }
}
# qqmusic_client is optional — only bundled when the build artifact exists.
$includeQQMusic = Test-Path $QQMUSIC_DLL

Write-Host "Packaging $COMPONENT_NAME v$Version ($Platform) ..."

# ── Stage area ─────────────────────────────────────────
# Use a temp subfolder so we don't accidentally include stale files.
$stage = "$env:TEMP\fb2k_pack_$COMPONENT_NAME"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null

# ── Copy payload ───────────────────────────────────────
# foobar2000 直接识别 zip 根目录下的 DLL 作为组件。
# 只需将两个 DLL 放根目录即可，无需架构子目录。

Copy-Item $FOO_DANMAKU_DLL "$stage\foo_danmaku.dll"
Copy-Item $NETEASE_DLL     "$stage\netease_client.dll"

Write-Host "  -> foo_danmaku.dll"
Write-Host "  -> netease_client.dll"

if ($includeQQMusic) {
    Copy-Item $QQMUSIC_DLL "$stage\qqmusic_client.dll"
    Write-Host "  -> qqmusic_client.dll"
} else {
    Write-Host "  (qqmusic_client.dll not found — skipped; run qqmusic_client/build.ps1 to include it)"
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
Write-Host "Install:"
Write-Host "  Double-click the .fb2k-component file, or drag it onto foobar2000."
Write-Host "  The installer will copy all DLLs to the components directory."
