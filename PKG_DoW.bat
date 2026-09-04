@echo off
cd /d "%~dp0"
call "%~dp01.bat"
exit /b %ERRORLEVEL%
