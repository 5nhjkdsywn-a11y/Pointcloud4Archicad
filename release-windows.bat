@echo off
rem ===========================================================================
rem  Pointcloud4Archicad - Windows-Release bauen und packen
rem
rem    release-windows.bat            alle vier Versionen, Clean Build
rem    release-windows.bat 26 29      nur diese Versionen
rem    release-windows.bat schnell    ohne Loeschen der Build-Ordner
rem
rem  Erzeugt dist\Pointcloud4Archicad_win_x64_<AC>-v<Version>.zip
rem  Die Versionsnummer kommt aus CMakeLists.txt - sie kann im Dateinamen
rem  also nicht mehr veralten (genau das war am 10.10.2026 passiert: ZIPs
rem  mit "v0.4.0" im Namen, die den 0.4.1-Stand gar nicht enthielten).
rem
rem  NICHT als Administrator ausfuehren - das Verteilen nach
rem  "C:\Program Files\GRAPHISOFT\..." macht weiterhin deploy_windows.bat.
rem
rem  Voraussetzungen (siehe memory\project_windows_build.md):
rem    - CMake und Python im PATH
rem    - Visual Studio 2022 mit Toolset v142 (AC26-28) und v143 (AC29)
rem    - vcpkg mit xerces-c:x64-windows-static-md
rem ===========================================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

set VERSIONEN=26 27 28 29
set CLEAN=1

rem --- Argumente: Versionsnummern und/oder "schnell" -------------------------
set ARGVERS=
for %%a in (%*) do (
    if /i "%%a"=="schnell" (
        set CLEAN=0
    ) else (
        set ARGVERS=!ARGVERS! %%a
    )
)
if not "!ARGVERS!"=="" set VERSIONEN=!ARGVERS!

rem --- CMake vorhanden? ------------------------------------------------------
where cmake >nul 2>&1
if errorlevel 1 (
    echo FEHLER: cmake nicht im PATH.
    echo         z.B.  set PATH=%%PATH%%;C:\Program Files\CMake\bin
    exit /b 2
)

rem --- Version aus CMakeLists.txt lesen --------------------------------------
rem  Zeile:  set (PW_VERSION "0.4.1" CACHE STRING "...")
set PWVER=
for /f tokens^=2^ delims^=^" %%v in ('findstr /b /c:"set (PW_VERSION" CMakeLists.txt') do (
    if not defined PWVER set PWVER=%%v
)
if not defined PWVER (
    echo FEHLER: PW_VERSION liess sich nicht aus CMakeLists.txt lesen.
    exit /b 2
)

echo ===========================================================================
echo  Pointcloud4Archicad %PWVER%  -  Windows-Release
echo  Versionen: %VERSIONEN%
if "%CLEAN%"=="1" (echo  Clean Build: ja) else (echo  Clean Build: nein ^(schnell^))
echo ===========================================================================
echo.

if not exist dist mkdir dist

set FEHLER=0
set GEBAUT=

for %%v in (%VERSIONEN%) do (
    echo --- Archicad %%v ------------------------------------------------------

    if "%CLEAN%"=="1" (
        if exist "build\win-ac%%v" rmdir /s /q "build\win-ac%%v"
    )

    rem Clean Build ist Absicht: ein stehengebliebenes Stamp-File ueberspringt
    rem die Ressourcen-Kompilierung, das Add-on wird dann stillschweigend
    rem nicht geladen (memory\project_build_pitfalls.md).

    cmake --preset ac%%v-win-release -DPW_VERSION="%PWVER%" >nul
    if errorlevel 1 (
        echo   FEHLER beim Konfigurieren.
        set /a FEHLER+=1
    ) else (
        cmake --build --preset ac%%v-win-release --target AddOn
        if errorlevel 1 (
            echo   FEHLER beim Bauen.
            set /a FEHLER+=1
        ) else (
            set APX=build\win-ac%%v\Release\Pointcloud4Archicad_x64_%%v.apx
            if not exist "!APX!" (
                echo   FEHLER: !APX! wurde nicht erzeugt.
                set /a FEHLER+=1
            ) else (
                set ZIEL=dist\Pointcloud4Archicad_win_x64_%%v-v%PWVER%.zip
                if exist "!ZIEL!" del /q "!ZIEL!"
                powershell -NoProfile -Command "Compress-Archive -Path '!APX!' -DestinationPath '!ZIEL!' -Force" >nul
                if errorlevel 1 (
                    echo   FEHLER beim Packen.
                    set /a FEHLER+=1
                ) else (
                    echo   fertig: !ZIEL!
                    set GEBAUT=!GEBAUT! %%v
                )
            )
        )
    )
    echo.
)

echo ===========================================================================
if "%FEHLER%"=="0" (
    echo  Alle Versionen gebaut:!GEBAUT!
    echo.
    echo  Naechste Schritte:
    echo    1. deploy_windows.bat als Administrator ausfuehren ^(installiert^)
    echo    2. In Archicad testen - Palette oeffnen, E57 importieren
    echo    3. Erst dann den Tag v%PWVER% setzen
) else (
    echo  %FEHLER% Version^(en^) fehlgeschlagen. Ausgabe oben pruefen.
)
echo ===========================================================================

endlocal & exit /b %FEHLER%
