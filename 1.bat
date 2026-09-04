@echo off
setlocal EnableExtensions
cd /d "%~dp0"
title PKG_DoW
set "PYTHONUTF8=1"
set "PYTHONDONTWRITEBYTECODE=1"
set "PYEXE=%CD%\.runtime\venv\Scripts\python.exe"

if not exist "%PYEXE%" (
  echo PKG_DoW ainda nao instalado.
  call "%CD%\Setup.cmd"
  if errorlevel 1 exit /b %ERRORLEVEL%
)

"%PYEXE%" "%CD%\tools\pkg_dow_desktop.py"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo.
  echo Falha ao iniciar. Codigo: %RC%
  echo Consulte pasta logs.
  pause
)
exit /b %RC%
