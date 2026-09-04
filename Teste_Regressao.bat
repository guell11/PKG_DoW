@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "PYEXE="
set "PYARGS="

where py.exe >nul 2>nul
if not errorlevel 1 (
  set "PYEXE=py"
  set "PYARGS=-3"
) else (
  where python.exe >nul 2>nul
  if not errorlevel 1 set "PYEXE=python"
)

if not defined PYEXE (
  echo [Regressao] Python 3.10+ nao encontrado.
  exit /b 2
)

"%PYEXE%" %PYARGS% -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)" >nul 2>nul
if errorlevel 1 (
  echo [Regressao] Python 3.10+ e obrigatorio.
  exit /b 2
)

"%PYEXE%" %PYARGS% "%CD%\tools\run_regression.py" %*
exit /b %ERRORLEVEL%
