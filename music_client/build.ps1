param(
    [ValidateSet("x64","Win32")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$PROJECT    = $PSScriptRoot
$BUILDROOT  = "$PROJECT\build"
$OUTDIR     = "$BUILDROOT\$Platform"
$OBJDIR     = "$BUILDROOT\obj\$Platform"
$MSVC       = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
$WINSDK_INC = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$WINSDK_LIB = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"

if ($Platform -eq "x64") {
    $toolBin = "Hostx64\x64"
    $libArch = "x64"
    $machine = "X64"
} else {
    $toolBin = "Hostx86\x86"
    $libArch = "x86"
    $machine = "X86"
}

$clExe  = "$MSVC\bin\$toolBin\cl.exe"
$libExe = "$MSVC\bin\$toolBin\lib.exe"   # static-library archiver

foreach ($d in @($BUILDROOT, $OUTDIR, $OBJDIR)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}

$srcFiles = @(
    "$PROJECT\src\music_client.cpp"
)

$includeArgs = @(
    "/I$PROJECT\include",
    "/I$WINSDK_INC\ucrt",
    "/I$WINSDK_INC\shared",
    "/I$WINSDK_INC\um",
    "/I$MSVC\include"
)

$compileFlags = @(
    "/nologo", "/EHsc", "/W3", "/MD", "/O2", "/std:c++17",
    "/DWIN32", "/D_WINDOWS", "/DUNICODE", "/D_UNICODE",
    "/DNOMINMAX", "/D_WIN32_WINNT=0x0A00", "/DWINVER=0x0A00",
    "/Zc:__cplusplus", "/utf-8",
    "/Fo$OBJDIR\",
    "/c"
)

Write-Host "Compiling music_client ($Platform)..."
& $clExe @compileFlags @includeArgs @srcFiles
if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED"; exit 1 }

$objFiles = @(foreach ($src in $srcFiles) {
    Join-Path $OBJDIR ("$([System.IO.Path]::GetFileNameWithoutExtension($src)).obj")
})

# Build a static library (.lib) — no /DLL, use lib.exe not link.exe
$libFlags = @(
    "/nologo",
    "/MACHINE:$machine",
    "/OUT:$OUTDIR\music_client.lib"
)

Write-Host "Archiving static lib ($Platform)..."
& $libExe @libFlags @objFiles
if ($LASTEXITCODE -ne 0) { Write-Host "ARCHIVE FAILED"; exit 1 }

Write-Host "SUCCESS: $OUTDIR\music_client.lib"
Write-Host "Headers : $PROJECT\include"
