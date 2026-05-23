# test_build.ps1 — build and run test_qqmusic.exe
param(
    [string]$Title  = "",
    [string]$Artist = ""
)

$ErrorActionPreference = "Stop"

$PROJECT    = Split-Path $PSScriptRoot -Parent
$MSVC       = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
$WINSDK_INC = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$WINSDK_LIB = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"
$toolBin    = "Hostx64\x64"
$libArch    = "x64"
$clExe      = "$MSVC\bin\$toolBin\cl.exe"
$linkExe    = "$MSVC\bin\$toolBin\link.exe"
$OUTDIR     = "$PSScriptRoot\out"
$DLL        = "$PROJECT\build\x64\qqmusic_client.dll"

if (-not (Test-Path $OUTDIR)) { New-Item -ItemType Directory $OUTDIR | Out-Null }

# Make sure qqmusic_client.dll is built first
if (-not (Test-Path $DLL)) {
    Write-Host "qqmusic_client.dll not found, building..."
    & powershell -ExecutionPolicy Bypass -File "$PROJECT\build.ps1" -Platform x64
    if ($LASTEXITCODE -ne 0) { Write-Host "DLL build failed"; exit 1 }
}

# Copy DLL next to exe so LoadLibrary finds it
Copy-Item $DLL "$OUTDIR\qqmusic_client.dll" -Force
Write-Host "DLL copied to $OUTDIR"

Write-Host "Compiling test_qqmusic..."
$clArgs = @(
    "/nologo", "/EHsc", "/W3", "/MD", "/std:c++17",
    "/DUNICODE", "/D_UNICODE", "/DWIN32", "/D_WIN32_WINNT=0x0A00",
    "/utf-8",
    "/I$PROJECT\include",
    "/I$WINSDK_INC\ucrt",
    "/I$WINSDK_INC\shared",
    "/I$WINSDK_INC\um",
    "/I$MSVC\include",
    "/Fo$OUTDIR\test_qqmusic.obj",
    "/c",
    "$PSScriptRoot\test_qqmusic.cpp"
)
& $clExe @clArgs
if ($LASTEXITCODE -ne 0) { Write-Host "Compile FAILED"; exit 1 }

Write-Host "Linking..."
$linkArgs = @(
    "/nologo", "/MACHINE:X64",
    "/OUT:$OUTDIR\test_qqmusic.exe",
    "/LIBPATH:$WINSDK_LIB\um\$libArch",
    "/LIBPATH:$WINSDK_LIB\ucrt\$libArch",
    "/LIBPATH:$MSVC\lib\$libArch",
    "kernel32.lib", "user32.lib",
    "$OUTDIR\test_qqmusic.obj"
)
& $linkExe @linkArgs
if ($LASTEXITCODE -ne 0) { Write-Host "Link FAILED"; exit 1 }

Write-Host "Build OK. Running test..."
Write-Host ("-" * 60)

$exe = "$OUTDIR\test_qqmusic.exe"
if ($Title -and $Artist) {
    & $exe $Title $Artist
} elseif ($Title) {
    & $exe $Title
} else {
    & $exe
}
$rc = $LASTEXITCODE

Write-Host ("-" * 60)
Write-Host "Exit code: $rc"
