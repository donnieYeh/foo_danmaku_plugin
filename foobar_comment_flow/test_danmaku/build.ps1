param([string]$clPath, [string]$inc)

$ErrorActionPreference = "Stop"

$WINSDK_INC = "C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
$WINSDK_LIB = "C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0"
$MSVC = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
$clExe = "$MSVC\bin\Hostx86\x86\cl.exe"

$args = @(
    "/nologo",
    "/EHsc",
    "/W3",
    "/MD",
    "/I`"$WINSDK_INC\ucrt`"",
    "/I`"$WINSDK_INC\shared`"",
    "/I`"$WINSDK_INC\um`"",
    "/I`"$MSVC\include`"",
    "main_test.cpp",
    "user32.lib",
    "gdi32.lib",
    "wininet.lib",
    "kernel32.lib",
    "/Fe:build/test_danmaku.exe",
    "/link",
    "/LIBPATH:`"$WINSDK_LIB\um\x86`"",
    "/LIBPATH:`"$WINSDK_LIB\ucrt\x86`"",
    "/LIBPATH:`"$MSVC\lib\x86`""
)

Write-Host "Invoking cl.exe..."
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $clExe
$psi.Arguments = $args -join " "
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
$proc.Start() | Out-Null
$stdout = $proc.StandardOutput.ReadToEnd()
$stderr = $proc.StandardError.ReadToEnd()
$proc.WaitForExit()

Write-Host $stdout
if ($stderr) { Write-Host $stderr }
Write-Host "Exit code: $($proc.ExitCode)"

if ($proc.ExitCode -eq 0) {
    Write-Host "SUCCESS: build/test_danmaku.exe"
} else {
    Write-Host "FAILED"
}