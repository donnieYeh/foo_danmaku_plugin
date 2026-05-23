param(
    [ValidateSet("x64","Win32")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$PROJECT     = $PSScriptRoot
$BUILDROOT   = "$PROJECT\build"
$OUTDIR      = "$BUILDROOT\$Platform"
$OBJDIR      = "$BUILDROOT\obj\$Platform"
$MSVC        = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
$WINSDK_INC  = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$WINSDK_LIB  = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"

if ($Platform -eq "x64") {
    $toolBin = "Hostx64\x64"
    $libArch = "x64"
    $machine = "X64"
} else {
    $toolBin = "Hostx86\x86"
    $libArch = "x86"
    $machine = "X86"
}

$clExe   = "$MSVC\bin\$toolBin\cl.exe"
$linkExe = "$MSVC\bin\$toolBin\link.exe"

foreach ($d in @($BUILDROOT, $OUTDIR, $OBJDIR)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}

$srcFiles = @(
    "$PROJECT\src\logger.cpp",
    "$PROJECT\src\crypto.cpp",
    "$PROJECT\src\http.cpp",
    "$PROJECT\src\json.cpp",
    "$PROJECT\src\api.cpp",
    "$PROJECT\src\netease_client.cpp",
    "$PROJECT\src\provider_entry.cpp"   # MusicProviderVTable adapter
)

$MUSIC_CLIENT_INC = "$PROJECT\..\music_client\include"  # music_provider.h

$includeArgs = @(
    "/I$PROJECT\include",
    "/I$PROJECT\src",
    "/I$MUSIC_CLIENT_INC",
    "/I$WINSDK_INC\ucrt",
    "/I$WINSDK_INC\shared",
    "/I$WINSDK_INC\um",
    "/I$MSVC\include"
)

$compileFlags = @(
    "/nologo", "/EHsc", "/W3", "/MD", "/O2", "/std:c++17",
    "/DWIN32", "/D_WINDOWS", "/DUNICODE", "/D_UNICODE",
    "/DNOMINMAX", "/D_WIN32_WINNT=0x0A00", "/DWINVER=0x0A00",
    "/DNETEASE_CLIENT_EXPORTS",
    "/Zc:__cplusplus", "/utf-8",
    "/Fo$OBJDIR\"
    "/c"
)

Write-Host "Compiling netease_client ($Platform)..."
& $clExe @compileFlags @includeArgs @srcFiles
if ($LASTEXITCODE -ne 0) { Write-Host "COMPILE FAILED"; exit 1 }

$objFiles = $srcFiles | ForEach-Object {
    "$OBJDIR\$([System.IO.Path]::GetFileNameWithoutExtension($_)).obj"
}

$linkFlags = @(
    "/nologo", "/DLL", "/MACHINE:$machine",
    "/OUT:$OUTDIR\netease_client.dll",
    "/IMPLIB:$OUTDIR\netease_client.lib",
    "/LIBPATH:$WINSDK_LIB\um\$libArch",
    "/LIBPATH:$WINSDK_LIB\ucrt\$libArch",
    "/LIBPATH:$MSVC\lib\$libArch",
    "kernel32.lib", "user32.lib",
    "winhttp.lib",
    "bcrypt.lib"
)

Write-Host "Linking ($Platform)..."
& $linkExe @linkFlags @objFiles
if ($LASTEXITCODE -ne 0) { Write-Host "LINK FAILED"; exit 1 }

Write-Host "SUCCESS: $OUTDIR\netease_client.dll"
Write-Host "Headers : $PROJECT\include"
