@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo vcvars64 FAILED
  exit /b 1
)
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD=%ROOT%\build"
"C:\Program Files\CMake\bin\cmake.exe" -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
if errorlevel 1 (
  echo CMAKE_CONFIGURE_FAILED
  exit /b 1
)
"C:\Users\darkm\AppData\Local\Microsoft\WinGet\Packages\Ninja-build.Ninja_Microsoft.Winget.Source_8wekyb3d8bbwe\ninja.exe" -C "%BUILD%"
set "NINJA_RC=%errorlevel%"
echo NINJA_RC=%NINJA_RC%
if not "%NINJA_RC%"=="0" (
  echo NINJA_BUILD_FAILED
  exit /b 1
)
echo BUILD_OK
exit /b 0
