@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0\.."

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "%~dp0\.."
set "BUILD_DIR=_Build\codex-windows"
cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl.exe -DCMAKE_CXX_COMPILER=clang-cl.exe -DCMAKE_RC_COMPILER=llvm-rc.exe -DKYTY_BUILD_LAUNCHER=OFF
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD_DIR%" --target kyty_emulator --parallel
if errorlevel 1 exit /b %errorlevel%

if not exist "_Build\windows" mkdir "_Build\windows"
cmake -E copy_if_different "%BUILD_DIR%\kyty_emulator.exe" "_Build\windows\kyty_emulator.exe"
if errorlevel 1 exit /b %errorlevel%
cmake -E copy_if_different "%BUILD_DIR%\kyty_emulator.pdb" "_Build\windows\kyty_emulator.pdb"
if errorlevel 1 exit /b %errorlevel%
cmake -E copy_if_different "%BUILD_DIR%\kyty_emulator_clang_lld_link.map" "_Build\windows\kyty_emulator_clang_lld_link.map"
exit /b %errorlevel%
