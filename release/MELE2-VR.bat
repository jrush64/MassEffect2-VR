@echo off
setlocal enableextensions enabledelayedexpansion
title MELE2 VR
color 0B

echo(
echo  --------------------------------------------------
echo    MELE2 VR - Mass Effect Legendary Edition ^(ME2^)
echo  --------------------------------------------------
echo(

REM  NO auto-detection of the game folder. This bat, dxgi.dll, openxr_loader.dll
REM  and MELE2VR.ini must ALL be dropped directly into your game's
REM  ...\ME2\Binaries\Win64 folder (next to MassEffect2.exe), then run FROM there.
REM  Auto-detection was tried on ME1 and dropped: a found folder under Program
REM  Files silently failed to write without Administrator rights. Running from
REM  a folder the user already put files into sidesteps that.
set "TARGET=%~dp0"
if "!TARGET:~-1!"=="\" set "TARGET=!TARGET:~0,-1!"

if not exist "!TARGET!\MassEffect2.exe" (
  echo   ERROR: MassEffect2.exe is not next to this installer.
  echo(
  echo   This bat has to run FROM your game folder, not from the zip
  echo   or your Downloads folder. Fix:
  echo(
  echo     1. Extract the whole MELE2-VR.zip ^(MELE2-VR.bat, dxgi.dll,
  echo        openxr_loader.dll, MELE2VR.ini - all four files^).
  echo     2. Copy or move ALL FOUR into your game's Win64 folder:
  echo          ...\Mass Effect Legendary Edition\Game\ME2\Binaries\Win64
  echo     3. Run MELE2-VR.bat from THAT folder.
  echo(
  echo   ^(Not sure where that is: right-click Mass Effect 2 in Steam -^>
  echo   Manage -^> Browse Local Files, then open Game\ME2\Binaries\Win64.^)
  echo(
  pause
  exit /b 1
)

set "MISSING="
if not exist "!TARGET!\dxgi.dll" set "MISSING=1"
if not exist "!TARGET!\openxr_loader.dll" set "MISSING=1"
if defined MISSING (
  echo   ERROR: dxgi.dll and/or openxr_loader.dll aren't in this folder.
  echo(
  echo   You only copied PART of the mod here. Go back to the extracted
  echo   MELE2-VR folder and copy dxgi.dll and openxr_loader.dll in too -
  echo   all four files ^(this .bat, both .dll files, MELE2VR.ini^) need to
  echo   sit together in your Win64 folder.
  echo(
  pause
  exit /b 1
)

REM --- game must be closed before touching its files ---
tasklist /fi "imagename eq MassEffect2.exe" 2>nul | find /i "MassEffect2.exe" >nul
if not errorlevel 1 (
  echo   Please CLOSE Mass Effect 2 first, then run this again.
  echo(
  pause
  exit /b 1
)

REM  WRITE-ACCESS CHECK + SELF-ELEVATE
REM  If this folder needs Administrator rights to write to (common when Steam is
REM  under C:\Program Files (x86)), don't fail with a cryptic copy error - test
REM  for it up front and relaunch elevated so the user sees ONE Windows
REM  permission popup instead of a confusing error.
set "WTEST=!TARGET!\.mele2vr_writetest"
(echo test) > "!WTEST!" 2>nul
if not exist "!WTEST!" (
  net session >nul 2>&1
  if errorlevel 1 (
    echo   This folder needs Administrator rights to install into
    echo   ^(this happens when Steam is under Program Files^).
    echo   Requesting permission - click YES on the popup...
    echo(
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -WorkingDirectory '%~dp0' -Verb RunAs" >nul 2>&1
    exit /b 0
  ) else (
    echo   ERROR: cannot write to this folder even as Administrator.
    echo   Check it isn't read-only, or that antivirus isn't blocking it.
    echo(
    pause
    exit /b 1
  )
)
del /f /q "!WTEST!" >nul 2>&1

echo   Found ME2 at:
echo     !TARGET!
echo(

REM  "Already installed?" signal. MELE2VR.ini is NOT usable for this - it ships
REM  INSIDE the zip (drop-in-place model), so it exists the moment the user
REM  extracts, before this wizard has ever run. ME1 used its expectedResX
REM  watchdog key; ME2 has no such key (the DLL syncs GamerSettings from
REM  RenderW/RenderH itself at attach), so this installer sets its own marker instead.
REM  InstallerRun is written ONLY by this bat and is ignored by the mod -
REM  GetPrivateProfileString simply never asks for it.
set "INSTALLED=0"
set "CURMODE="
if exist "!TARGET!\MELE2VR.ini" (
  for /f "tokens=2 delims==" %%V in ('findstr /b /r "InstallerRun=" "!TARGET!\MELE2VR.ini" 2^>nul') do if not "%%V"=="0" set "INSTALLED=1"
  REM Current Mode, so a re-run defaults the mode picker to what's already configured
  REM instead of silently resetting to Stereo. Hitting Enter KEEPS your mode.
  for /f "tokens=2 delims==" %%V in ('findstr /b /r "Mode=" "!TARGET!\MELE2VR.ini" 2^>nul') do set "CURMODE=%%V"
)
if "!INSTALLED!"=="1" ( echo   MELE2 VR is currently INSTALLED here. ) else ( echo   MELE2 VR is NOT installed here yet. )
echo(
echo   What do you want to do?
echo     [1] Install / Reconfigure MELE2 VR
echo     [2] Uninstall MELE2 VR
echo     [3] Cancel  ^(do nothing^)
echo(
set /p "ACTION=Enter 1-3 [1]: "
if not defined ACTION set "ACTION=1"
if "!ACTION!"=="3" ( echo   Cancelled. Nothing was changed. & pause & exit /b 0 )
if "!ACTION!"=="2" (
  if "!INSTALLED!"=="0" ( echo   Nothing to uninstall - MELE2 VR isn't configured here. & pause & exit /b 0 )
  goto UNINSTALL
)
goto INSTALL


REM  INSTALL
:INSTALL
echo  --------------------------------------------------
echo    INSTALL
echo  --------------------------------------------------
echo(
echo   This installer turns HDR off for you - it's the #1 thing that
echo   makes the headset image blue or doubled.
echo(

REM  VR MODE. ME2's mode numbers are NOT ME1's - do not copy them across:
REM     0 = Mono      2 = AER      3 = DIBR      4 = Stereo (SFR)
REM  (ME1 uses vrMode 2 for Stereo; ME2 uses Mode 4. The key name differs too:
REM   ME1 "vrMode = 2" with spaces, ME2 "Mode=4" with none.)
set "VRMODE=4"
set "MNAME=Stereo"

set "DEFPICK=1"
if "!CURMODE!"=="4" set "DEFPICK=1"
if "!CURMODE!"=="0" set "DEFPICK=2"
if "!CURMODE!"=="2" set "DEFPICK=3"
if "!CURMODE!"=="3" set "DEFPICK=4"

echo   Choose your VR mode:
echo(
echo     [1] Stereo  - true 3D, full render per eye   ^(recommended^)
echo     [2] Mono    - flat image + head tracking ^(lightest^)
echo     [3] AER     - alternate-eye, lighter, can flicker
echo     [4] DIBR    - depth reprojection
echo(
set /p "PICK=Enter 1-4 [!DEFPICK!]: "
if not defined PICK set "PICK=!DEFPICK!"
if "!PICK!"=="1" ( set "VRMODE=4" & set "MNAME=Stereo" )
if "!PICK!"=="2" ( set "VRMODE=0" & set "MNAME=Mono" )
if "!PICK!"=="3" ( set "VRMODE=2" & set "MNAME=AER" )
if "!PICK!"=="4" ( set "VRMODE=3" & set "MNAME=DIBR" )
echo(
echo   Selected: !MNAME!.
echo(

REM  RESOLUTION - one 16:9 ladder (LE2 rewrites any other aspect at startup
REM  regardless, so a square tier would be a lie). The mod renders ABOVE your
REM  monitor's resolution - your GPU is the limit, not your screen. Stereo
REM  renders every frame TWICE, so Balanced (not Sharp) is the recommended tier:
REM  measured locked 60fps at 4096x2304, dropped frames at 6144x3456.
set "RX=4096"
set "RY=2304"
set "QNAME=Balanced"

echo   Choose your image quality:
echo(
echo     [1] Low         -  2048x1152  weakest GPUs, softest image
echo     [2] Performance -  3072x1728  lightest
echo     [3] Balanced    -  4096x2304  ^(recommended - measured locked 60fps^)
echo     [4] Sharp       -  5120x2880
echo     [5] Max         -  6144x3456  heavy in Stereo
echo     [6] Extreme     -  8192x4608  expect dropped frames
echo     [7] Ultra       - 10240x5760  needs a very strong GPU
echo(
set /p "QPICK=Enter 1-7 [3]: "
if not defined QPICK set "QPICK=3"
if "!QPICK!"=="1" ( set "RX=2048"  & set "RY=1152" & set "QNAME=Low" )
if "!QPICK!"=="2" ( set "RX=3072"  & set "RY=1728" & set "QNAME=Performance" )
if "!QPICK!"=="3" ( set "RX=4096"  & set "RY=2304" & set "QNAME=Balanced" )
if "!QPICK!"=="4" ( set "RX=5120"  & set "RY=2880" & set "QNAME=Sharp" )
if "!QPICK!"=="5" ( set "RX=6144"  & set "RY=3456" & set "QNAME=Max" )
if "!QPICK!"=="6" ( set "RX=8192"  & set "RY=4608" & set "QNAME=Extreme" )
if "!QPICK!"=="7" ( set "RX=10240" & set "RY=5760" & set "QNAME=Ultra" )
echo(
echo   Selected: !QNAME! ^(!RX!x!RY!^).
echo(

REM --- back up a non-mod dxgi.dll if one is somehow already here (rare in the
REM     drop-in-place model - the mod's own dxgi.dll usually arrived via the same
REM     extraction) ---
if exist "!TARGET!\dxgi.dll.premele2vr.bak" goto SKIPBAK
findstr /m "ME2VR" "!TARGET!\dxgi.dll" >nul 2>&1
if errorlevel 1 copy /y "!TARGET!\dxgi.dll" "!TARGET!\dxgi.dll.premele2vr.bak" >nul 2>&1
:SKIPBAK

echo   Mod files present.

REM  MELE2VR.ini writes. Do NOT add -Encoding utf8 to Set-Content below - it
REM  writes a UTF-8 BOM, which breaks the [VR] section lookup and silently
REM  reverts every setting to defaults. Plain Set-Content only.
REM  Key format here is "Key=Value" (no spaces) - ME1's ini uses "key = value".
REM  Written every run as surgical per-key replaces; the rest of the user's
REM  tuning is untouched.
powershell -NoProfile -Command "$p='!TARGET!\MELE2VR.ini'; $c=Get-Content -LiteralPath $p; if($c -match '^^Mode='){$c=$c -replace '^^Mode=.*','Mode=!VRMODE!'}else{$c+='Mode=!VRMODE!'}; Set-Content -LiteralPath $p $c" >nul 2>&1
echo   Wrote VR mode to MELE2VR.ini

REM --- Resolution. ME2 differs from ME1 here: the mod itself writes ResX/ResY into
REM     GamerSettings.ini at attach ([VRRES]) straight from these two keys, so there
REM     are no per-mode resolution keys to keep in sync (ME1's sfr2ResX/aerResX/...
REM     desync bug simply cannot happen here). GamerSettings is still set below so the
REM     very first launch is already correct. ---
powershell -NoProfile -Command "$p='!TARGET!\MELE2VR.ini'; $c=Get-Content -LiteralPath $p; if($c -match '^^RenderW='){$c=$c -replace '^^RenderW=.*','RenderW=!RX!' -replace '^^RenderH=.*','RenderH=!RY!'}else{$c+=@('RenderW=!RX!','RenderH=!RY!')}; Set-Content -LiteralPath $p $c" >nul 2>&1
echo   Wrote render resolution to MELE2VR.ini

powershell -NoProfile -Command "$p='!TARGET!\MELE2VR.ini'; $c=Get-Content -LiteralPath $p; if($c -match '^^DisableDof='){$c=$c -replace '^^DisableDof=.*','DisableDof=0'}else{$c+='DisableDof=0'}; Set-Content -LiteralPath $p $c" >nul 2>&1

REM --- the "wizard has run" marker (see the INSTALLED note above) ---
powershell -NoProfile -Command "$p='!TARGET!\MELE2VR.ini'; $c=Get-Content -LiteralPath $p; if($c -match '^^InstallerRun='){$c=$c -replace '^^InstallerRun=.*','InstallerRun=1'}else{$c+='InstallerRun=1'}; Set-Content -LiteralPath $p $c" >nul 2>&1

REM  GamerSettings.ini writer. Forces off: DynamicShadows, FilmGrain, MotionBlur.
REM  DepthOfField is deliberately NOT written here - see the DoF note above; it is
REM  the confirmed cause of the washed-out image. Forces HDR off before the game runs, same
REM  as ME1 (a live write from inside the DLL loses a race against the launcher).
REM  AntiAliasing/AmbientOcclusion left alone - player's own call.
REM  CREATES the file/folder if missing (EA App sometimes never makes
REM  BioGame\Config), ADDS missing keys, REPLACES existing ones, then reads back
REM  and verifies every key before saying "ok".
set "GS=!TARGET!\..\..\BioGame\Config\GamerSettings.ini"
powershell -NoProfile -Command "$p='!GS!'; $nl=[char]13+[char]10; $d=Split-Path -Parent $p; if(-not(Test-Path -LiteralPath $d)){New-Item -ItemType Directory -Force -Path $d | Out-Null}; if(-not(Test-Path -LiteralPath $p)){Set-Content -LiteralPath $p ('[SystemSettings]'+$nl+$nl+'[HDR]'+$nl)}; $c=Get-Content -LiteralPath $p -Raw; if($c -notmatch '(?m)^^\[SystemSettings\]'){$c=$c.TrimEnd()+$nl+$nl+'[SystemSettings]'+$nl}; if($c -notmatch '(?m)^^\[HDR\]'){$c=$c.TrimEnd()+$nl+$nl+'[HDR]'+$nl}; $sys=@('ResX=!RX!','ResY=!RY!','BorderlessWindow=True','Fullscreen=False','DynamicShadows=False','FilmGrain=False','MotionBlur=False'); foreach($kv in $sys){$n=$kv.Split('=')[0]; if($c -match ('(?m)^^'+$n+'=')){$c=[regex]::Replace($c,'(?m)^^'+$n+'=[^^\r\n]*',$kv)} else {$c=([regex]'(?m)^^\[SystemSettings\]').Replace($c,'[SystemSettings]'+$nl+$kv,1)}}; if($c -match '(?m)^^DesiredRange='){$c=[regex]::Replace($c,'(?m)^^DesiredRange=[^^\r\n]*','DesiredRange=DynamicRange_SDR')} else {$c=([regex]'(?m)^^\[HDR\]').Replace($c,'[HDR]'+$nl+'DesiredRange=DynamicRange_SDR',1)}; Set-Content -LiteralPath $p $c; $v=Get-Content -LiteralPath $p -Raw; $need=$sys+@('DesiredRange=DynamicRange_SDR'); $bad=@($need | Where-Object { $v -notmatch ('(?m)^^'+[regex]::Escape($_)+'\s*$') }); if($bad.Count -eq 0){'ok'}else{'FAIL '+($bad -join ' ')}" >"%TEMP%\mele2vr_gs.txt" 2>nul
set /p "GSOK="<"%TEMP%\mele2vr_gs.txt"
del "%TEMP%\mele2vr_gs.txt" >nul 2>&1
if /i "!GSOK!"=="ok" (
  echo   Resolution set to !RX!x!RY!.
  echo   HDR, motion blur, film grain and dynamic shadows: OFF.  Depth of field: LEFT ALONE.
) else (
  echo   WARNING: could not write/verify GamerSettings.ini ^(!GSOK!^).
  echo(
  echo   The file and its folder are created automatically now, so this almost
  echo   always means a PERMISSIONS problem. Right-click this bat -^> Run as
  echo   administrator, and try again.
  echo(
  echo   ^(EA App / EA Desktop installs land under Program Files more often than
  echo   Steam ones do, so this is more common there.^)
  echo(
  echo   Until it succeeds, set these yourself in the in-game video menu:
  echo     - HDR                OFF   ^(#1 cause of a blue or doubled image^)
  echo     - Motion blur        OFF   ^(makes reflections/flares shake in VR^)
  echo     - Film grain         OFF
  echo     - Dynamic shadows    OFF
  echo     - Resolution         !RX! x !RY!
)

echo(
echo  --------------------------------------------------
echo    DONE. MELE2 VR is installed.
echo  --------------------------------------------------
echo(
echo    1. Put your headset on and launch Mass Effect 2.
echo(
echo    2. INSERT = mod menu,   R = recenter,   K = first person.
echo       Gamepad: BACK tapped twice = recenter.
echo       Keys 1-4 = settings profiles.
echo(
echo    Run this installer again any time to change settings or uninstall.
echo(
pause
exit /b 0


REM  UNINSTALL
:UNINSTALL
echo  --------------------------------------------------
echo    MELE2 VR is already installed here.
echo  --------------------------------------------------
echo(
set /p "GO=Uninstall MELE2 VR? [Y/N] "
if /i not "!GO!"=="Y" ( echo   Cancelled. Nothing was changed. & pause & exit /b 0 )

del /f /q "!TARGET!\dxgi.dll" >nul 2>&1
del /f /q "!TARGET!\openxr_loader.dll" >nul 2>&1
if exist "!TARGET!\dxgi.dll.premele2vr.bak" move /y "!TARGET!\dxgi.dll.premele2vr.bak" "!TARGET!\dxgi.dll" >nul
echo   Removed the mod ^(dxgi.dll + openxr_loader.dll^). Mass Effect 2 will
echo   run completely vanilla now.

echo(
set /p "DELS=Also delete your settings and logs? [Y/N] "
if /i "!DELS!"=="Y" (
  if exist "!TARGET!\MELE2VR.ini" del /f /q "!TARGET!\MELE2VR.ini" >nul 2>&1
  if exist "!TARGET!\ME2PassiveDxgiProbe.log" del /f /q "!TARGET!\ME2PassiveDxgiProbe.log" >nul 2>&1
  echo   Deleted MELE2VR.ini and logs.
) else (
  echo   Kept your MELE2VR.ini so your settings survive a reinstall.
)

echo(
echo   MELE2 VR uninstalled.
echo(
pause
exit /b 0
