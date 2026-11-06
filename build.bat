@echo off
rem ============================================================
rem  Stack Enhance build script (x64 + x86)
rem  Usage:  build.bat            build both plugins
rem          build.bat x64        build 64-bit only
rem          build.bat x86        build 32-bit only
rem          build.bat tests      build the offline test targets
rem  Overridable:
rem          X64DBG_SDK  path to the x64dbg pluginsdk folder
rem          VCVARS      full path to vcvarsall.bat
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

if "%X64DBG_SDK%"=="" if exist "%~dp0pluginsdk\bridgemain.h"       set "X64DBG_SDK=%~dp0pluginsdk"
if "%X64DBG_SDK%"=="" if exist "%~dp0..\pluginsdk\bridgemain.h"    set "X64DBG_SDK=%~dp0..\pluginsdk"
if "%X64DBG_SDK%"=="" if defined X64DBG_HOME if exist "%X64DBG_HOME%\pluginsdk\bridgemain.h" set "X64DBG_SDK=%X64DBG_HOME%\pluginsdk"
if "%X64DBG_SDK%"=="" if exist "%ProgramFiles%\x64dbg\pluginsdk\bridgemain.h" set "X64DBG_SDK=%ProgramFiles%\x64dbg\pluginsdk"
if "%X64DBG_SDK%"=="" if exist "%ProgramFiles(x86)%\x64dbg\pluginsdk\bridgemain.h" set "X64DBG_SDK=%ProgramFiles(x86)%\x64dbg\pluginsdk"
if "%X64DBG_SDK%"=="" (
    echo [ERROR] x64dbg plugin SDK not found. Set X64DBG_SDK to the pluginsdk folder.
    exit /b 1
)
if not exist "%X64DBG_SDK%\bridgemain.h" (
    echo [ERROR] %X64DBG_SDK% is not a pluginsdk folder.
    exit /b 1
)

if not exist "dist" mkdir "dist"
if not exist "obj" mkdir "obj"
if not exist "tests\bin\x64" mkdir "tests\bin\x64"
if not exist "tests\bin\x86" mkdir "tests\bin\x86"

if "%VCVARS%"=="" (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
        if defined VSPATH if exist "!VSPATH!\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=!VSPATH!\VC\Auxiliary\Build\vcvarsall.bat"
    )
)
if "%VCVARS%"=="" (
    echo [ERROR] vcvarsall.bat not found. Set VCVARS.
    exit /b 1
)
echo [INFO] vcvarsall : %VCVARS%
echo [INFO] plugin SDK: %X64DBG_SDK%

set "TARGETS=%*"
if "%TARGETS%"=="" set "TARGETS=x64 x86"

echo %TARGETS% | findstr /I /C:"test" >nul
if not errorlevel 1 (
    echo.
    echo ==================== building test targets ====================
    for %%A in (x64 x86) do (
        if /I "%%A"=="x64" (set "VCARG=x64") else (set "VCARG=x86")
        call "%VCVARS%" !VCARG! >nul
        if errorlevel 1 (
            echo [ERROR] vcvarsall !VCARG! failed
            exit /b 1
        )
        if exist "obj\tests\%%A" rmdir /s /q "obj\tests\%%A"
        mkdir "obj\tests\%%A"
        cl /nologo /utf-8 /Od /Oy- /Zi /MT /W3 ^
            tests\teststack.cpp ^
            /Fe"tests\bin\%%A\teststack.exe" /Fo"obj\tests\%%A\\" ^
            /link /DEBUG /PDB:"tests\bin\%%A\teststack.pdb" /INCREMENTAL:NO
        if errorlevel 1 (
            echo [ERROR] !VCARG! test target build failed
            exit /b 1
        )
        echo [OK] tests\bin\%%A\teststack.exe
    )
)

for %%A in (%TARGETS%) do (
    if /I "%%A"=="x64" (
        set "ARCH=x64"
        set "VCARG=x64"
        set "DBGLIB=x64dbg.lib"
        set "BRIDGELIB=x64bridge.lib"
        set "OUTEXT=dp64"
    ) else if /I "%%A"=="x86" (
        set "ARCH=x86"
        set "VCARG=x86"
        set "DBGLIB=x32dbg.lib"
        set "BRIDGELIB=x32bridge.lib"
        set "OUTEXT=dp32"
    ) else (
        set "ARCH="
    )
    if not "!ARCH!"=="" (
        echo.
        echo ==================== building !ARCH! ====================
        call "%VCVARS%" !VCARG! >nul
        if errorlevel 1 (
            echo [ERROR] vcvarsall !VCARG! failed
            exit /b 1
        )
        if exist "obj\!ARCH!" rmdir /s /q "obj\!ARCH!"
        mkdir "obj\!ARCH!"
        cl /nologo /utf-8 /std:c++17 /EHsc /W4 /O2 /MT /LD /guard:cf /wd4996 ^
            /Fo"obj\!ARCH!\\" ^
            /I"%X64DBG_SDK%" /I"src" ^
            src\pluginmain.cpp src\hook.cpp src\stackcomment.cpp src\util.cpp ^
            /Fe"dist\StackEnhance.!OUTEXT!" ^
            /link /LIBPATH:"%X64DBG_SDK%" !DBGLIB! !BRIDGELIB! user32.lib shell32.lib ^
            /IMPLIB:"obj\!ARCH!\StackEnhance.lib" /INCREMENTAL:NO
        if errorlevel 1 (
            echo.
            echo [ERROR] !ARCH! plugin build failed
            exit /b 1
        )
        echo [OK] dist\StackEnhance.!OUTEXT!
    )
)

echo.
echo ==================== done ====================
dir /b dist
endlocal
