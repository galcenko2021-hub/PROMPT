@echo off
rem Build PROMPT (C++) with MSVC.
rem   build.bat         - game:  bin\prompt.exe
rem   build.bat test    - neural network console test: bin\brain_test.exe
rem   build.bat debug   - game with debug info
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio Installer not found. Install Visual Studio with "Desktop development with C++".
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio has no x64 C++ tools installed.
    exit /b 1
)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

:have_cl
if not exist bin mkdir bin
if not exist obj mkdir obj

set "FLAGS=/nologo /std:c++20 /utf-8 /EHsc /W3 /MP /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Fo:obj\\ /Fd:obj\\"
set "OPT=/O2 /fp:fast /DNDEBUG /Zi"
if /i "%~1"=="debug" set "OPT=/Od /Zi /RTC1"

if /i "%~1"=="test" goto build_test

cl %FLAGS% %OPT% src\*.cpp /Fe:bin\prompt.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib xaudio2.lib windowscodecs.lib dbghelp.lib /DEBUG /OPT:REF /OPT:ICF
exit /b %errorlevel%

:build_test
cl %FLAGS% %OPT% tests\brain_test.cpp src\core.cpp src\nn.cpp src\language.cpp src\brain.cpp /Fe:bin\brain_test.exe /link user32.lib
exit /b %errorlevel%
