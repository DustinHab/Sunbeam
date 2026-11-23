@echo off
rem Builds out\Sunbeam.exe. With the argument "shader" only the shaders are compiled.
rem Needs the Visual Studio C++ build tools and the Windows SDK (fxc, rc).
setlocal
cd /d "%~dp0"
rem A stray quote in PATH makes vcvars fail.
set PATH=%PATH:"=%

where cl >nul 2>nul && goto tools
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto notools
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT goto notools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 goto notools
:tools
if not exist out\sh mkdir out\sh

set FXC=fxc /nologo /O2
%FXC% /T vs_5_0 /E Full        /Vn sh_full        /Fh out\sh\full.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Noise       /Vn sh_noise       /Fh out\sh\noise.h       shader\noise.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Air         /Vn sh_air         /Fh out\sh\air.h         shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Near        /Vn sh_near        /Fh out\sh\near.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E NearBlock   /Vn sh_nearblock   /Fh out\sh\nearblock.h   shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Base        /Vn sh_base        /Fh out\sh\base.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Haze        /Vn sh_haze        /Fh out\sh\haze.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Face        /Vn sh_face        /Fh out\sh\face.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Pick        /Vn sh_pick        /Fh out\sh\pick.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Rain        /Vn sh_rain        /Fh out\sh\rain.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Shade       /Vn sh_shade       /Fh out\sh\shade.h       shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Column      /Vn sh_column      /Fh out\sh\column.h      shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E ColumnBlock /Vn sh_columnblock /Fh out\sh\columnblock.h shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Bolt        /Vn sh_bolt        /Fh out\sh\bolt.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E BoltFar     /Vn sh_boltfar     /Fh out\sh\boltfar.h     shader\sky.hlsl >nul || goto failed
%FXC% /T vs_5_0 /E BoltVS      /Vn sh_boltvs      /Fh out\sh\boltvs.h      shader\sky.hlsl >nul || goto failed
%FXC% /T vs_5_0 /E BoltFarVS   /Vn sh_boltfarvs   /Fh out\sh\boltfarvs.h   shader\sky.hlsl >nul || goto failed
%FXC% /T vs_5_0 /E PropVS      /Vn sh_propvs      /Fh out\sh\propvs.h      shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E PropIn      /Vn sh_propin      /Fh out\sh\propin.h      shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Prop        /Vn sh_prop        /Fh out\sh\prop.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Lamp        /Vn sh_lamp        /Fh out\sh\lamp.h        shader\sky.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Scatter     /Vn sh_scatter     /Fh out\sh\scatter.h     shader\light.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E BloomPre    /Vn sh_bloompre    /Fh out\sh\bloompre.h    shader\light.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E BloomDown   /Vn sh_bloomdown   /Fh out\sh\bloomdown.h   shader\light.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E BloomUp     /Vn sh_bloomup     /Fh out\sh\bloomup.h     shader\light.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Composite   /Vn sh_composite   /Fh out\sh\composite.h   shader\light.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E Flash       /Vn sh_flash       /Fh out\sh\flash.h       shader\light.hlsl >nul || goto failed
%FXC% /T vs_5_0 /E DustVS      /Vn sh_dustvs      /Fh out\sh\dustvs.h      shader\light.hlsl >nul || goto failed
%FXC% /T ps_5_0 /E DustPS      /Vn sh_dustps      /Fh out\sh\dustps.h      shader\light.hlsl >nul || goto failed
echo Shaders done
if /i "%1"=="shader" exit /b 0

rc /nologo /fo out\Sunbeam.res src\Sunbeam.rc || goto failed

cl /nologo /std:c++20 /O2 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Iout /Fo:out\ /Fe:out\Sunbeam.exe src\*.cpp ^
  /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\Sunbeam.manifest out\Sunbeam.res d3d11.lib d2d1.lib dwrite.lib dxgi.lib dcomp.lib dwmapi.lib user32.lib gdi32.lib shell32.lib ^
  ole32.lib oleaut32.lib windowscodecs.lib shlwapi.lib uuid.lib advapi32.lib shcore.lib winhttp.lib >out\cl.log
if errorlevel 1 (type out\cl.log & goto failed)
echo Sunbeam.exe done
exit /b 0

:notools
echo Visual Studio C++ build tools not found
exit /b 1

:failed
echo Build failed
exit /b 1
