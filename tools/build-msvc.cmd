@echo off
rem Build the compiler like the official Windows CI does (MSVC, Win32, sNAMEMAX=63).
rem   tools\build-msvc.cmd [build-dir] [extra C flags]
rem   tools\build-msvc.cmd build-fast
rem   tools\build-msvc.cmd build-prof "/Zi"      (add /DEBUG to the linker flags by hand for a PDB)
rem   tools\build-msvc.cmd build-trace "/D PERF_TRACE"
rem Needs Visual Studio 2022 Build Tools (C++ workload); cmake and ninja are taken from the same install.
setlocal
set "BT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%BT%\VC\Auxiliary\Build\vcvars32.bat" (
  echo Visual Studio 2022 Build Tools not found at "%BT%"
  exit /b 1
)
call "%BT%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
set "CM=%BT%\Common7\IDE\CommonExtensions\Microsoft\CMake"
set "PATH=%CM%\CMake\bin;%CM%\Ninja;%PATH%"
set "DIR=%~1"
if "%DIR%"=="" set "DIR=build-fast"
cd /d "%~dp0.."
cmake -S source\compiler -B "%DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "-DCMAKE_C_FLAGS=/D sNAMEMAX=63 %~2" || exit /b 1
cmake --build "%DIR%" || exit /b 1
echo built: %CD%\%DIR%\pawncc.exe and pawnc.dll
