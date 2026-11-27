@echo off
setlocal EnableExtensions DisableDelayedExpansion

rem Double-click: Release VST3 and Standalone. Optional: build.bat debug nopause
rem Set HPSAMPLER_JUCE_DIR to use a specific JUCE 8 checkout.
set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"
set "BUILD_DIR=%PROJECT_DIR%\build"
set "CONFIG=Release"
set "DO_PAUSE=1"
set "JUCE_ROOT="

:arguments
if "%~1"=="" goto :configure
if /i "%~1"=="debug" (
    set "CONFIG=Debug"
    shift
    goto :arguments
)
if /i "%~1"=="release" (
    set "CONFIG=Release"
    shift
    goto :arguments
)
if /i "%~1"=="nopause" (
    set "DO_PAUSE=0"
    shift
    goto :arguments
)
echo [ERROR] Unknown argument: %~1
echo Usage: build.bat [release^|debug] [nopause]
goto :failed

:configure
echo.
echo ===== HPSAMPLER: VST3 + Standalone [%CONFIG%] =====
where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] CMake is missing from PATH. Install CMake and Visual Studio with C++ tools.
    goto :failed
)
if defined HPSAMPLER_JUCE_DIR if not exist "%HPSAMPLER_JUCE_DIR%\CMakeLists.txt" (
    echo [ERROR] HPSAMPLER_JUCE_DIR does not point to a JUCE checkout.
    goto :failed
)
if defined HPSAMPLER_JUCE_DIR set "JUCE_ROOT=%HPSAMPLER_JUCE_DIR%"
if not defined JUCE_ROOT if exist "%BUILD_DIR%\CMakeCache.txt" (
    for /f "usebackq tokens=1,* delims==" %%A in ("%BUILD_DIR%\CMakeCache.txt") do if "%%A"=="HPSAMPLER_JUCE_DIR:PATH" set "JUCE_ROOT=%%B"
)
if defined JUCE_ROOT if not exist "%JUCE_ROOT%\CMakeLists.txt" set "JUCE_ROOT="
call :try_juce "%PROJECT_DIR%\JUCE"
call :try_juce "%PROJECT_DIR%\..\HPDG808\JUCE"
call :try_juce "%PROJECT_DIR%\..\DRUMENGINE\JUCE"
call :try_juce "%USERPROFILE%\Documents\DRUMENGINE\JUCE"
call :try_juce "C:\DRUMENGINE\JUCE"
if not defined JUCE_ROOT (
    echo [ERROR] JUCE 8 was not found.
    echo Place HPDG808 next to this project, or set HPSAMPLER_JUCE_DIR to your JUCE folder.
    goto :failed
)
for %%J in ("%JUCE_ROOT%\.") do set "JUCE_ROOT=%%~fJ"
echo JUCE: "%JUCE_ROOT%"
if exist "%BUILD_DIR%\CMakeCache.txt" goto :configure_existing
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" -A x64 "-DHPSAMPLER_JUCE_DIR=%JUCE_ROOT%"
if errorlevel 1 goto :failed
goto :build

:configure_existing
cmake -S "%PROJECT_DIR%" -B "%BUILD_DIR%" "-DHPSAMPLER_JUCE_DIR=%JUCE_ROOT%"
if errorlevel 1 goto :failed

:build
cmake --build "%BUILD_DIR%" --config %CONFIG% --target HPSAMPLER_VST3 HPSAMPLER_Standalone HPStems --parallel 4
if errorlevel 1 goto :failed
if not exist "%BUILD_DIR%\HPSAMPLER_artefacts\%CONFIG%\VST3\HPSAMPLER.vst3\Contents\x86_64-win\HPSAMPLER.vst3" goto :failed
if not exist "%BUILD_DIR%\HPSAMPLER_artefacts\%CONFIG%\Standalone\HPSAMPLER.exe" goto :failed
echo.
echo ===== BUILD SUCCEEDED =====
echo VST3 folder: "%BUILD_DIR%\HPSAMPLER_artefacts\%CONFIG%\VST3\HPSAMPLER.vst3"
echo Standalone:  "%BUILD_DIR%\HPSAMPLER_artefacts\%CONFIG%\Standalone\HPSAMPLER.exe"
echo Copy the entire VST3 folder to your DAW's VST3 location when installing.
set "BUILD_RESULT=0"
goto :finish

:failed
echo.
echo ===== BUILD FAILED - see the error above =====
set "BUILD_RESULT=1"

:finish
if "%DO_PAUSE%"=="1" pause
exit /b %BUILD_RESULT%

:try_juce
if defined JUCE_ROOT exit /b 0
if exist "%~1\CMakeLists.txt" for %%J in ("%~1") do set "JUCE_ROOT=%%~fJ"
exit /b 0
