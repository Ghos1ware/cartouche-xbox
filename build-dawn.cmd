@echo off
rem Dawn (WebGPU natif) pour l'appli UWP : la bibliotheque monolithique webgpu_dawn.dll, backend DirectX 12
rem seul, et le compilateur de shaders FXC du SDK. Le resultat va dans third_party\dawn-uwp (include, lib,
rem bin), que moonlight-xbox-dx.vcxproj lie et met dans le paquet. Voir Bridge\PROTOCOL.md.
rem
rem   build-dawn.cmd            construit si third_party\dawn-uwp manque
rem   build-dawn.cmd force      reconstruit
rem
rem Les sources de Dawn : DAWN_SRC (par defaut ..\dawn), au commit DAWN_COMMIT. Il faut Python 3 (DAWN_PYTHON,
rem par defaut celui de uv) et CMake. Le premier build prend une heure environ.
setlocal
set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%
set OUT=%ROOT%\third_party\dawn-uwp
if "%DAWN_SRC%"=="" set DAWN_SRC=%ROOT%\..\dawn
set DAWN_COMMIT=d61d3434d3bed43fba3ace885b06fa77161a4db1
if "%DAWN_PYTHON%"=="" set DAWN_PYTHON=%USERPROFILE%\.local\share\uv\python\cpython-3.12-windows-x86_64-none\python.exe

if exist "%OUT%\bin\webgpu_dawn.dll" if not "%1"=="force" exit /b 0

for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.ComponentGroup.UWP.VC.BuildTools -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio Build Tools with the UWP C++ tools not found & exit /b 1)
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul

if not exist "%DAWN_SRC%\.git" git clone https://dawn.googlesource.com/dawn "%DAWN_SRC%" || exit /b 1
cd /d "%DAWN_SRC%"
git fetch origin %DAWN_COMMIT% 2>nul
git checkout --detach %DAWN_COMMIT% || exit /b 1
"%DAWN_PYTHON%" tools\fetch_dawn_dependencies.py -s || exit /b 1

set FLAGS=/DWIN32 /D_WINDOWS /EHsc /D_CRT_SECURE_NO_WARNINGS
cmake -S . -B out\uwp -G "Visual Studio 17 2022" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 ^
  -DPython3_EXECUTABLE="%DAWN_PYTHON%" -DCMAKE_CXX_FLAGS="%FLAGS%" -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS" ^
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED -DDAWN_ENABLE_D3D12=ON -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_VULKAN=OFF ^
  -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_NULL=OFF -DDAWN_ENABLE_METAL=OFF ^
  -DDAWN_ENABLE_SWIFTSHADER=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF ^
  -DDAWN_USE_GLFW=OFF -DDAWN_USE_WINDOWS_UI=ON -DDAWN_USE_BUILT_DXC=OFF -DDAWN_BUILD_PROTOBUF=OFF ^
  -DDAWN_ENABLE_SPIRV_VALIDATION=OFF -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_SPV_READER=OFF ^
  -DTINT_BUILD_SPV_WRITER=OFF -DTINT_BUILD_GLSL_WRITER=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF -DTINT_BUILD_MSL_WRITER=OFF ^
  -DTINT_BUILD_HLSL_WRITER=ON -DTINT_BUILD_WGSL_READER=ON -DTINT_BUILD_WGSL_WRITER=ON -DDAWN_WERROR=OFF || exit /b 1
cmake --build out\uwp --config Release --target webgpu_dawn -- -m -v:minimal || exit /b 1

if not exist "%OUT%\include\webgpu" mkdir "%OUT%\include\webgpu"
if not exist "%OUT%\include\dawn" mkdir "%OUT%\include\dawn"
if not exist "%OUT%\lib" mkdir "%OUT%\lib"
if not exist "%OUT%\bin" mkdir "%OUT%\bin"
copy /y out\uwp\gen\include\dawn\webgpu.h "%OUT%\include\webgpu\webgpu.h" >nul
copy /y out\uwp\gen\include\dawn\webgpu_cpp.h "%OUT%\include\webgpu\webgpu_cpp.h" >nul
copy /y out\uwp\gen\include\dawn\webgpu_cpp.h "%OUT%\include\dawn\webgpu_cpp.h" >nul
copy /y out\uwp\gen\include\dawn\webgpu.h "%OUT%\include\dawn\webgpu.h" >nul
copy /y out\uwp\gen\include\dawn\dawn_version.h "%OUT%\include\dawn\" >nul
copy /y out\uwp\gen\include\webgpu\webgpu_cpp_chained_struct.h "%OUT%\include\webgpu\" >nul
copy /y include\webgpu\webgpu_enum_class_bitmasks.h "%OUT%\include\webgpu\" >nul
if not exist "%OUT%\include\dawn\native" mkdir "%OUT%\include\dawn\native"
if not exist "%OUT%\include\dawn\platform" mkdir "%OUT%\include\dawn\platform"
copy /y include\dawn\native\DawnNative.h "%OUT%\include\dawn\native\" >nul
copy /y include\dawn\native\dawn_native_export.h "%OUT%\include\dawn\native\" >nul
copy /y include\dawn\platform\DawnPlatform.h "%OUT%\include\dawn\platform\" >nul
copy /y include\dawn\platform\dawn_platform_export.h "%OUT%\include\dawn\platform\" >nul
copy /y out\uwp\gen\include\dawn\dawn_proc_table.h "%OUT%\include\dawn\" >nul
copy /y out\uwp\src\dawn\native\Release\webgpu_dawn.lib "%OUT%\lib\" >nul
copy /y out\uwp\Release\webgpu_dawn.dll "%OUT%\bin\" >nul
copy /y "%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll" "%OUT%\bin\" >nul
echo Dawn: %OUT%
exit /b 0
