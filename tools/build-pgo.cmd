@echo off
rem Whole-program (LTCG) and profile-guided (PGO) builds of the compiler, MSVC Win32 like tools\build-msvc.cmd.
rem   tools\build-pgo.cmd <build-dir> ltcg   /GL + /LTCG, no profile
rem   tools\build-pgo.cmd <build-dir> gen    instrumented pawnc.dll (/GENPROFILE); run it on real sources
rem                                          (pgort140.dll must be on PATH), it writes pawnc!N.pgc next to itself
rem   tools\build-pgo.cmd <build-dir> use    relink the same objects with the collected profile (/USEPROFILE)
rem An optional third argument adds C flags (e.g. "/arch:AVX2").
rem gen and use must run on the same build dir: "use" only relinks, the .pgd and .pgc files stay there.
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
set "MODE=%~2"
set "EXTRA=%~3"
if "%DIR%"=="" goto usage
set "DLLFLAGS="
if /i "%MODE%"=="ltcg" set "DLLFLAGS=/machine:X86 /LTCG"
if /i "%MODE%"=="gen"  set "DLLFLAGS=/machine:X86 /LTCG /GENPROFILE"
if /i "%MODE%"=="use"  set "DLLFLAGS=/machine:X86 /LTCG /USEPROFILE"
if "%DLLFLAGS%"=="" goto usage
cd /d "%~dp0.."
cmake -S source\compiler -B "%DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  "-DCMAKE_C_FLAGS=/D sNAMEMAX=63 /GL %EXTRA%" ^
  "-DCMAKE_SHARED_LINKER_FLAGS=%DLLFLAGS%" ^
  "-DCMAKE_EXE_LINKER_FLAGS=/machine:X86 /LTCG" || exit /b 1
cmake --build "%DIR%" || exit /b 1
echo built (%MODE%): %CD%\%DIR%\pawncc.exe and pawnc.dll
exit /b 0
:usage
echo usage: tools\build-pgo.cmd ^<build-dir^> ltcg^|gen^|use
exit /b 2
