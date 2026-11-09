@echo off
setlocal
set "MSBUILDDISABLENODEREUSE=1"
set "ODL_ARCH=%~1"
if not defined ODL_ARCH set "ODL_ARCH=all"
if /i "%ODL_ARCH%"=="all" (
  set "ODL_BOTH=1"
  set "ODL_ARCH=x64"
)
if /i "%ODL_ARCH%"=="x64" goto setup
if /i "%ODL_ARCH%"=="Win32" goto setup
echo Usage: build.cmd [all^|x64^|Win32]
exit /b 2
:setup
set "ODL_ROOT=%~dp0"
set "ODL_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%ODL_VSWHERE%" (
  echo Install Visual Studio 2022 with Desktop development with C++ and CMake.
  exit /b 1
)
for /f "usebackq tokens=*" %%I in (`call "%ODL_VSWHERE%" -latest -version "[17.0,18.0)" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ODL_VS=%%I"
if not defined ODL_VS exit /b 1
set "ODL_CMAKE=%ODL_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "ODL_CTEST=%ODL_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
if not exist "%ODL_CMAKE%" exit /b 1
:build
"%ODL_CMAKE%" -S "%ODL_ROOT%." -B "%ODL_ROOT%build\%ODL_ARCH%" -G "Visual Studio 17 2022" -A "%ODL_ARCH%"
if errorlevel 1 exit /b 1
"%ODL_CMAKE%" --build "%ODL_ROOT%build\%ODL_ARCH%" --config Release --parallel 4 -- /v:minimal /fl "/flp:logfile=%ODL_ROOT%build\%ODL_ARCH%\msbuild.log;verbosity=normal"
if errorlevel 1 exit /b 1
"%ODL_CTEST%" --test-dir "%ODL_ROOT%build\%ODL_ARCH%" -C Release --output-on-failure
if errorlevel 1 exit /b 1
set "ODL_LABEL=x64"
if /i "%ODL_ARCH%"=="Win32" set "ODL_LABEL=x86"
if not defined ODL_DIST set "ODL_DIST=%ODL_ROOT%dist"
if not exist "%ODL_DIST%\%ODL_LABEL%" mkdir "%ODL_DIST%\%ODL_LABEL%"
if errorlevel 1 exit /b 1
copy /y "%ODL_ROOT%build\%ODL_ARCH%\Release\OpenDllLoader.exe" "%ODL_DIST%\%ODL_LABEL%\" >nul
if errorlevel 1 exit /b 1
copy /y "%ODL_ROOT%build\%ODL_ARCH%\Release\open-dll-loader.exe" "%ODL_DIST%\%ODL_LABEL%\" >nul
if errorlevel 1 exit /b 1
copy /y "%ODL_ROOT%build\%ODL_ARCH%\Release\odl-demo-launcher.exe" "%ODL_DIST%\%ODL_LABEL%\" >nul
if errorlevel 1 exit /b 1
for %%F in (odl-plugin-host.exe reloadable-plugin-v1.dll reloadable-plugin-v2.dll) do (
  copy /y "%ODL_ROOT%build\%ODL_ARCH%\examples\reloadable-host\Release\%%F" "%ODL_DIST%\%ODL_LABEL%\" >nul
  if errorlevel 1 exit /b 1
)
copy /y "%ODL_ROOT%README.md" "%ODL_DIST%\" >nul
if errorlevel 1 exit /b 1
copy /y "%ODL_ROOT%LICENSE" "%ODL_DIST%\" >nul
if errorlevel 1 exit /b 1
if not exist "%ODL_DIST%\docs" mkdir "%ODL_DIST%\docs"
if exist "%ODL_ROOT%docs\interface.png" copy /y "%ODL_ROOT%docs\interface.png" "%ODL_DIST%\docs\" >nul
copy /y "%ODL_ROOT%docs\linux-wine-proton.md" "%ODL_DIST%\docs\" >nul
if errorlevel 1 exit /b 1
copy /y "%ODL_ROOT%examples\reloadable-host\README.md" "%ODL_DIST%\docs\hot-reload.md" >nul
if errorlevel 1 exit /b 1
python "%ODL_ROOT%scripts\checksums.py" write "%ODL_DIST%"
if errorlevel 1 exit /b 1
if defined ODL_BOTH if /i "%ODL_ARCH%"=="x64" (
  set "ODL_ARCH=Win32"
  goto build
)
exit /b 0
