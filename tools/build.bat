@echo off
rem Configures and builds stellaris_guiexpand.dll and the examples (Visual Studio 2022, x64).
rem   tools\build.bat [path to vcvars64.bat]
rem Without an argument vcvars64.bat is found with vswhere (the latest Visual Studio with the C++ tools).
rem Dear ImGui and MinHook are fetched by CMake; to build offline, point FETCHCONTENT_SOURCE_DIR_IMGUI / FETCHCONTENT_SOURCE_DIR_MINHOOK at local checkouts
rem (ImGui v1.85, MinHook v1.3.4) in the environment of this script.
setlocal
set "VCVARS=%~1"
if not "%VCVARS%"=="" goto have_vcvars

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vcvars
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if not "%VCVARS%"=="" goto have_vcvars

:no_vcvars
echo vcvars64.bat not found: install Visual Studio 2022 with the C++ tools, or pass its path as the first argument
exit /b 1

:have_vcvars
call "%VCVARS%" || exit /b 1
cd /d "%~dp0\.."
set "EXTRA="
if not "%FETCHCONTENT_SOURCE_DIR_IMGUI%"=="" set "EXTRA=%EXTRA% -DFETCHCONTENT_SOURCE_DIR_IMGUI=%FETCHCONTENT_SOURCE_DIR_IMGUI%"
if not "%FETCHCONTENT_SOURCE_DIR_MINHOOK%"=="" set "EXTRA=%EXTRA% -DFETCHCONTENT_SOURCE_DIR_MINHOOK=%FETCHCONTENT_SOURCE_DIR_MINHOOK%"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 %EXTRA% || exit /b 1
cmake --build build --config Release --parallel || exit /b 1
echo.
echo plugin folder: %CD%\build\plugin\stellaris-guiexpand
