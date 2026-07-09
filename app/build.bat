@echo off
setlocal

:: Set up the Emscripten environment for this shell, then build.
call "%~dp0..\emsdk\emsdk_env.bat" >nul 2>&1

if not exist "%~dp0out" mkdir "%~dp0out"

call emcc "%~dp0src\main.c" -O2 -o "%~dp0out\index.html" ^
    -sUSE_SDL=2

if ERRORLEVEL 1 (
  echo Build failed.
  exit /b 1
)

echo Build succeeded. Output in out\
echo Serve it with:  python -m http.server --directory out
:: (Opening out\index.html directly with file:// will not work in most
:: browsers because .wasm must be fetched over HTTP.)
