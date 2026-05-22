@echo off
setlocal enabledelayedexpansion

set "WINSDK=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0"
set "MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207"
set "CL=%MSVC%\bin\Hostx86\x86\cl.exe"
set "OUT=test_danmaku.exe"

echo Compiling main_test.cpp...

"%CL%" /nologo /EHsc /W3 /MD ^
  /I"%WINSDK%\ucrt" ^
  /I"%WINSDK%\shared" ^
  /I"%WINSDK%\um" ^
  /I"%MSVC%\include" ^
  main_test.cpp ^
  user32.lib gdi32.lib wininet.lib kernel32.lib ^
  /Fe:"build\%OUT%"

if %ERRORLEVEL% EQU 0 (
    echo SUCCESS: build\%OUT%
) else (
    echo FAILED: exit code %ERRORLEVEL%
)

endlocal