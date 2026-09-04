@echo off
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\LaunchDevCmd.bat" -arch=x64 -host_arch=x64
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
echo [INFO] Re-running CMake configure (GLOB picked up new files)...
cmake -S . -B _Build\windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl.exe -DCMAKE_CXX_COMPILER=clang-cl.exe -DKYTY_BUILD_LAUNCHER=OFF
if errorlevel 1 exit /b 1
echo [INFO] Building kyty_emulator...
cmake --build _Build\windows --target kyty_emulator --parallel
if errorlevel 1 exit /b 1
echo [OK] Build done: _Build\windows\kyty_emulator.exe
exit /b 0
