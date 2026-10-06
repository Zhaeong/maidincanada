@echo off
rem gamut.bat -- run trudeau.jpg through a gamut of aspect ratios and
rem pixel sizes, writing BMPs into the output folder.
rem
rem   ratios: 21:9 16:9 3:2 4:3 5:4 1:1 4:5 2:3 9:16 1:2
rem   pixels: 4 8 16 32 64
rem
rem output names: output\trudeau_<WxH>_p<NN>.bmp

setlocal enabledelayedexpansion

cd /d "%~dp0"

set "IN=images\trudeau.jpg"
set "OUTDIR=output"
set "FAILS=0"

if not exist pixelizer.exe (
    echo pixelizer.exe not found - run build.bat first
    exit /b 1
)
if not exist "%IN%" (
    echo input image not found: %IN%
    exit /b 1
)
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem aspect ratio gamut: ultra-wide .. square .. tall
for %%R in (21:9 16:9 3:2 4:3 5:4 1:1 4:5 2:3 9:16 1:2) do (
    set "RNAME=%%R"
    set "RNAME=!RNAME::=x!"
    rem pixel size gamut: subtle .. chunky
    for %%P in (4 8 16 32 64) do (
        if %%P LSS 10 (set "P2=0%%P") else (set "P2=%%P")
        pixelizer.exe "%IN%" "%OUTDIR%\trudeau_!RNAME!_p!P2!.bmp" -a %%R -p %%P
        if errorlevel 1 (
            set /a FAILS+=1
            echo FAILED: -a %%R -p %%P
        )
    )
)

echo.
echo done - %FAILS% failures
echo files in %OUTDIR%:
dir /b "%OUTDIR%\trudeau_*.bmp" | find /c /v ""

if %FAILS% GTR 0 exit /b 1
endlocal
