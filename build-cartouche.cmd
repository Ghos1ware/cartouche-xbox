@echo off
rem Cartouche for Xbox: vcpkg packages once at the root, the CMake projects of moonlight-common-c
rem and libgamestream on them, then the signed sideload bundle in output\. Called by
rem `npm run xbox` in Cartouche (apps/desktop/scripts/xbox.mjs). Needs Visual Studio Build Tools
rem 2022 with the UWP C++ tools and the Windows 10 SDK 10.0.19041, and the sideload key in
rem %USERPROFILE%\.cartouche (xbox-sideload.pfx, xbox-sideload.pass).
setlocal
set NoDefaultCurrentDirectoryInExePath=
set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%
set NUGET=%USERPROFILE%\Xbox\nuget.exe
set KEY=%USERPROFILE%\.cartouche\xbox-sideload.pfx

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.ComponentGroup.UWP.VC.BuildTools -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio Build Tools with the UWP C++ tools not found & exit /b 1)
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
cd /d "%ROOT%"

if not exist vcpkg\vcpkg.exe call vcpkg\bootstrap-vcpkg.bat -disableMetrics
cd /d "%ROOT%"
vcpkg\vcpkg.exe install --triplet x64-uwp --x-install-root="%ROOT%\vcpkg_installed" || exit /b 1

set CMAKE_COMMON=-DCMAKE_TOOLCHAIN_FILE="%ROOT%\vcpkg\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-uwp -G "Visual Studio 17 2022" -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DTARGET_UWP=ON -DBUILD_SHARED_LIBS=off -DVCPKG_MANIFEST_MODE=ON -DVCPKG_MANIFEST_DIR="%ROOT%" -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR="%ROOT%\vcpkg_installed"
if not exist third_party\moonlight-common-c\build\CMakeCache.txt cmake -B third_party\moonlight-common-c\build -S third_party\moonlight-common-c %CMAKE_COMMON% || exit /b 1
if not exist libgamestream\build\CMakeCache.txt cmake -B libgamestream\build -S libgamestream %CMAKE_COMMON% || exit /b 1

if not exist "%NUGET%" (echo nuget.exe not found: %NUGET% & exit /b 1)
"%NUGET%" restore moonlight-xbox-dx.sln || exit /b 1

set /p PASS=<"%USERPROFILE%\.cartouche\xbox-sideload.pass"
msbuild /m /nologo /v:m /p:Configuration=Release /p:Platform=x64 /p:AppxBundle=Always /p:AppxBundlePlatforms=x64 /p:AppxPackageDir="%ROOT%\output\\" /p:PackageCertificateKeyFile="%KEY%" /p:PackageCertificatePassword=%PASS% /p:PackageCertificateThumbprint= /p:UapAppxPackageBuildMode=SideLoadOnly moonlight-xbox-dx.sln || exit /b 1
