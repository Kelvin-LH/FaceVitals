@echo off
set "QT_PLUGIN_PATH="
set "QT_QPA_PLATFORM_PLUGIN_PATH="
set "QT_QPA_PLATFORM=windows"
if exist "%~dp0dist\FaceVitals-0.6.0-windows-x64\FaceVitals.exe" (
  start "FaceVitals" "%~dp0dist\FaceVitals-0.6.0-windows-x64\FaceVitals.exe" %*
  exit /b 0
)
if exist "%~dp0build\windows\Release\FaceVitals.exe" (
  start "FaceVitals" "%~dp0build\windows\Release\FaceVitals.exe" %*
  exit /b 0
)
echo Build the project first, or download the Windows ZIP from GitHub Releases.
pause
exit /b 1
