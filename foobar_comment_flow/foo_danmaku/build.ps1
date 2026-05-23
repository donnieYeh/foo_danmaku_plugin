param(
    [ValidateSet("x64", "Win32")]
    [string]$Platform = "x64",
    [string]$config = "Release"
)

$ErrorActionPreference = "Stop"

$PROJECT = $PSScriptRoot
$SDK = "$PROJECT\SDK"
$BUILDROOT = "$PROJECT\build"
$OUTDIR = "$BUILDROOT\$Platform"
$OBJDIR = "$BUILDROOT\obj\$Platform"
$MSVC = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
$WINSDK_INC = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$WINSDK_LIB = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"

if ($Platform -eq "x64") {
    $toolBin = "Hostx64\x64"
    $libArch = "x64"
    $machine = "X64"
    $sharedLib = "$SDK\foobar2000\shared\shared-x64.lib"
} else {
    $toolBin = "Hostx86\x86"
    $libArch = "x86"
    $machine = "X86"
    $sharedLib = "$SDK\foobar2000\shared\shared-Win32.lib"
}

$clExe = "$MSVC\bin\$toolBin\cl.exe"
$linkExe = "$MSVC\bin\$toolBin\link.exe"

foreach ($dir in @($BUILDROOT, $OUTDIR, $OBJDIR)) {
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
}

foreach ($required in @($clExe, $linkExe, $sharedLib)) {
    if (-not (Test-Path $required)) { throw "Required file not found: $required" }
}

# netease_client SDK slot (populated by netease_client/build.ps1)
$NETEASE_SDK = "$SDK\netease_client"
if (-not (Test-Path "$NETEASE_SDK\netease_client.lib")) {
    throw "netease_client.lib not found in $NETEASE_SDK.`nRun netease_client/build.ps1 first."
}

$projectSrc = @(
    "$PROJECT\foo_danmaku.cpp",
    "$PROJECT\ui\danmaku_ui.cpp",
    "$PROJECT\ui\danmaku_preferences.cpp",
    "$PROJECT\core\danmaku_engine.cpp",
    "$PROJECT\core\playback_monitor.cpp"
    # netEase/api.cpp and signature.cpp replaced by netease_client.dll
)

$sdkSrc = @(
    Get-ChildItem -LiteralPath "$SDK\foobar2000\SDK" -Filter "*.cpp" |
        Where-Object { $_.Name -ne "stdafx.cpp" } |
        ForEach-Object { $_.FullName }
)
$sdkSrc += "$SDK\foobar2000\foobar2000_component_client\component_client.cpp"

$pfcSrc = @(
    "$SDK\pfc\audio_math.cpp",
    "$SDK\pfc\audio_sample.cpp",
    "$SDK\pfc\base64.cpp",
    "$SDK\pfc\bigmem.cpp",
    "$SDK\pfc\bit_array.cpp",
    "$SDK\pfc\bsearch.cpp",
    "$SDK\pfc\charDownConvert.cpp",
    "$SDK\pfc\cpuid.cpp",
    "$SDK\pfc\crashWithMessage.cpp",
    "$SDK\pfc\filehandle.cpp",
    "$SDK\pfc\filetimetools.cpp",
    "$SDK\pfc\guid.cpp",
    "$SDK\pfc\other.cpp",
    "$SDK\pfc\pathUtils.cpp",
    "$SDK\pfc\printf.cpp",
    "$SDK\pfc\SmartStrStr.cpp",
    "$SDK\pfc\sort.cpp",
    "$SDK\pfc\splitString2.cpp",
    "$SDK\pfc\string-compare.cpp",
    "$SDK\pfc\string-conv-lite.cpp",
    "$SDK\pfc\string-lite.cpp",
    "$SDK\pfc\string_base.cpp",
    "$SDK\pfc\string_conv.cpp",
    "$SDK\pfc\threads.cpp",
    "$SDK\pfc\timers.cpp",
    "$SDK\pfc\unicode-normalize.cpp",
    "$SDK\pfc\utf8.cpp",
    "$SDK\pfc\wildcard.cpp",
    "$SDK\pfc\win-objects.cpp"
)

$allSrc = $projectSrc + $sdkSrc + $pfcSrc

$includeArgs = @(
    "/I$PROJECT",
    "/I$WINSDK_INC\ucrt",
    "/I$WINSDK_INC\shared",
    "/I$WINSDK_INC\um",
    "/I$MSVC\include",
    "/I$SDK",
    "/I$SDK\pfc",
    "/I$SDK\libPPUI",
    "/I$SDK\foobar2000",
    "/I$SDK\foobar2000\SDK",
    "/I$SDK\foobar2000\helpers",
    "/I$NETEASE_SDK\include"   # netease_client.h
)

$compileFlags = @(
    "/nologo", "/EHsc", "/W3", "/MD", "/O2", "/std:c++17",
    "/D_USING_NTDDI_MAX", "/DWIN32", "/D_WINDOWS", "/DUNICODE", "/D_UNICODE",
    "/DNOMINMAX", "/D_WIN32_WINNT=0x0A00", "/DWINVER=0x0A00",
    "/Zc:__cplusplus", "/utf-8",
    "/Fo$OBJDIR\", "/c"
)

$clArgs = $compileFlags + $includeArgs + $allSrc

Write-Host "Compiling foo_danmaku ($Platform)..."
& $clExe @clArgs
$clExit = $LASTEXITCODE
Write-Host "Compile exit code: $clExit"
if ($clExit -ne 0) {
    Write-Host "FAILED"
    exit 1
}

Write-Host "Linking into DLL ($Platform)..."

$objFiles = $allSrc | ForEach-Object { Join-Path $OBJDIR ([System.IO.Path]::GetFileNameWithoutExtension($_) + ".obj") }

$linkFlags = @(
    "/nologo", "/DLL", "/MACHINE:$machine",
    "/OUT:$OUTDIR\foo_danmaku.dll",
    "/IMPLIB:$OUTDIR\foo_danmaku.lib",
    "/LIBPATH:$WINSDK_LIB\um\$libArch",
    "/LIBPATH:$WINSDK_LIB\ucrt\$libArch",
    "/LIBPATH:$MSVC\lib\$libArch",
    "/LIBPATH:$NETEASE_SDK",    # netease_client.lib
    "kernel32.lib", "user32.lib", "gdi32.lib",
    "advapi32.lib", "shell32.lib", "ole32.lib", "uuid.lib",
    "netease_client.lib",
    $sharedLib
)

$linkArgs = $linkFlags + $objFiles

& $linkExe @linkArgs
$linkExit = $LASTEXITCODE
Write-Host "Link exit code: $linkExit"

if ($linkExit -eq 0) {
    Write-Host "SUCCESS: $OUTDIR\foo_danmaku.dll"
} else {
    Write-Host "FAILED"
    exit 1
}

# ── Deploy both DLLs to foobar2000 components ──────────
$fb2k = "C:\Program Files\foobar2000\components"
if (Test-Path $fb2k) {
    $proc = Get-Process foobar2000 -ErrorAction SilentlyContinue
    if ($proc) { $proc | Stop-Process -Force; Start-Sleep -Seconds 2 }
    try {
        Copy-Item "$OUTDIR\foo_danmaku.dll"          "$fb2k\foo_danmaku.dll"    -Force -ErrorAction Stop
        Copy-Item "$NETEASE_SDK\netease_client.dll"  "$fb2k\netease_client.dll" -Force -ErrorAction Stop
        Write-Host "Deployed to $fb2k"
    } catch {
        Write-Host "WARN: Deploy needs admin rights. Copy manually:"
        Write-Host "  $OUTDIR\foo_danmaku.dll  →  $fb2k\"
        Write-Host "  $NETEASE_SDK\netease_client.dll  →  $fb2k\"
    }
}
