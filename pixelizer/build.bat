@echo off
cd /d "%~dp0"
call "E:\visualstudio2026\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /Za /W4 /O2 pixelizer.c /Fe:pixelizer.exe
if errorlevel 1 exit /b 1
pixelizer.exe "images\trudeau.jpg" "pixelized.bmp"
