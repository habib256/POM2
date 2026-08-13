@echo off
REM package_windows_release.bat — stage dist\POM2-Windows\ and zip it.
REM
REM Expects POM2.exe to already be built in build\Release (the release workflow
REM configures with the vcpkg x64-windows-static triplet and builds first).
REM
REM The package is deliberately ONE self-contained exe plus data: no DLL is
REM copied next to POM2.exe. See the POM2_WIN_STATIC_RUNTIME comment in
REM CMakeLists.txt for why an app-local msvcp140.dll breaks the GPU vendor's
REM OpenGL ICD. The workflow asserts the absence structurally after this runs.
REM
REM The full roms\ tree ships beside POM2.exe so the zip boots out of the box.
REM POM2 also probes %%LOCALAPPDATA%%\POM2 for overrides.
setlocal EnableDelayedExpansion

if "%POM2_VERSION%"=="" (
    for /f "tokens=3" %%v in ('findstr /b /c:"project(pom2_imgui VERSION" CMakeLists.txt') do set POM2_VERSION=%%v
)
if "%POM2_VERSION%"=="" set POM2_VERSION=0.0

set STAGE=dist\POM2-Windows
echo === Staging %STAGE% (v%POM2_VERSION%) ===

if exist "%STAGE%" rmdir /s /q "%STAGE%" || goto :fail
mkdir "%STAGE%" || goto :fail
mkdir "%STAGE%\fonts" || goto :fail
mkdir "%STAGE%\pic" || goto :fail
mkdir "%STAGE%\roms" || goto :fail

REM --- the binary -----------------------------------------------------------
if exist build\Release\POM2.exe (
    copy /y build\Release\POM2.exe "%STAGE%\POM2.exe" >nul || goto :fail
) else (
    copy /y build\POM2.exe "%STAGE%\POM2.exe" >nul || goto :fail
)
if not exist "%STAGE%\POM2.exe" (
    echo ERROR: POM2.exe not found in build\Release or build
    exit /b 1
)

REM --- read-only assets (mirrors the FHS install rules, including roms\) -----
copy /y fonts\DejaVuSans.ttf   "%STAGE%\fonts\" >nul || goto :fail
copy /y fonts\fa-solid-900.ttf "%STAGE%\fonts\" >nul || goto :fail
copy /y pic\Apple_II_plus.jpg  "%STAGE%\pic\"   >nul || goto :fail
if exist roms (
    xcopy /e /i /q /y roms\* "%STAGE%\roms\" >nul || goto :fail
) else (
    echo ERROR: required roms directory is missing
    exit /b 1
)
copy /y packaging\roms_README.txt "%STAGE%\roms\README.txt" >nul || goto :fail

REM --- docs -----------------------------------------------------------------
copy /y README.md    "%STAGE%\" >nul || goto :fail
copy /y CHANGELOG.md "%STAGE%\" >nul || goto :fail
copy /y LICENSE      "%STAGE%\" >nul || goto :fail

for %%F in (POM2.exe fonts\DejaVuSans.ttf fonts\fa-solid-900.ttf pic\Apple_II_plus.jpg roms\apple2e.rom roms\disk2.rom README.md CHANGELOG.md LICENSE) do (
    if not exist "%STAGE%\%%F" (
        echo ERROR: staged package is missing %%F
        exit /b 1
    )
)

REM --- zip ------------------------------------------------------------------
set ZIP=dist\POM2-Windows-v%POM2_VERSION%.zip
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command ^
  "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -Force" || goto :fail
if not exist "%ZIP%" (
    echo ERROR: Compress-Archive did not produce %ZIP%
    exit /b 1
)

echo === Wrote %ZIP% ===
dir "%ZIP%"
endlocal
exit /b 0

:fail
echo ERROR: Windows release packaging command failed with errorlevel %ERRORLEVEL%
exit /b 1
