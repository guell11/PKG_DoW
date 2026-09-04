@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"
title KytyPS5 - Preparar e Compilar Core

echo.
echo  KytyPS5 - bootstrap oficial do core Windows
echo  ===========================================
echo.

where winget.exe >nul 2>nul
if errorlevel 1 (
  echo [ERRO] O WinGet nao esta disponivel. Atualize o App Installer pela Microsoft Store.
  exit /b 1
)

call :INSTALL Ninja-build.Ninja
if errorlevel 1 exit /b 1
call :INSTALL LLVM.LLVM
if errorlevel 1 exit /b 1
call :INSTALL KhronosGroup.VulkanSDK
if errorlevel 1 exit /b 1

set "VSCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\LaunchDevCmd.bat"
if not exist "%VSCMD%" (
  echo [INFO] Instalando Visual Studio Build Tools com o workload C++...
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-package-agreements --accept-source-agreements --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
)
if not exist "%VSCMD%" (
  echo [ERRO] Visual Studio Build Tools C++ nao foi localizado apos a instalacao.
  exit /b 1
)

set "NINJA="
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined NINJA set "NINJA=%%I"
if not defined NINJA for /f "delims=" %%I in ('dir /b /s "%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_*\ninja.exe" 2^>nul') do if not defined NINJA set "NINJA=%%I"
if not defined NINJA (
  echo [ERRO] Ninja nao foi localizado.
  exit /b 1
)

if not exist "3rdparty\SDL2\CMakeLists.txt" (
  echo [INFO] Inicializando submodulos oficiais...
  git submodule update --init --recursive
  if errorlevel 1 (
    echo [ERRO] Nao foi possivel baixar os submodulos. Verifique Git e rede.
    exit /b 1
  )
)

call "%VSCMD%" -arch=x64 -host_arch=x64
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
for /d %%D in ("C:\VulkanSDK\*") do set "VULKAN_SDK=%%~fD"
if defined VULKAN_SDK set "PATH=%VULKAN_SDK%\Bin;%PATH%"

echo [INFO] Configurando CMake (Release, clang-cl, Ninja)...
cmake -S . -B _Build\windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl.exe -DCMAKE_CXX_COMPILER=clang-cl.exe -DCMAKE_MAKE_PROGRAM="%NINJA%" -DKYTY_BUILD_LAUNCHER=OFF
if errorlevel 1 exit /b 1

echo [INFO] Compilando core e testes...
cmake --build _Build\windows --target kyty_tests kyty_emulator --parallel
if errorlevel 1 exit /b 1

echo [INFO] Executando CTest...
ctest --test-dir _Build\windows --output-on-failure
if errorlevel 1 (
  echo [AVISO] O core foi compilado, mas existem regressões a investigar. Veja o resultado acima.
  exit /b 2
)

echo.
echo [OK] Core compilado em _Build\windows\kyty_emulator.exe
echo      Agora execute 1.bat para abrir a UX local.
exit /b 0

:INSTALL
echo [INFO] Garantindo %~1...
winget install --id %~1 --exact --silent --accept-package-agreements --accept-source-agreements
exit /b %ERRORLEVEL%
