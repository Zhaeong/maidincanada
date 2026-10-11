@echo off
setlocal

:: Set up the Emscripten environment for this shell, then build.
call "%~dp0..\..\emsdk\emsdk_env.bat" >nul 2>&1

if not exist "%~dp0out" mkdir "%~dp0out"

:: Strict C89 enforcement: the build hard-fails on anything that is not C89.
::   -std=c89                            compile in ISO C89 (C90) mode
::   -pedantic-errors                    any non-C89 construct (C99 syntax,
::                                       GNU extensions, ...) is an error
::   -Werror=declaration-after-statement mixed declarations/code are a
::                                       warning even in C89 mode, so make
::                                       that specific warning fatal too
call emcc "%~dp0src\main.c" -O2 -o "%~dp0out\index.html" ^
    -sUSE_SDL=2 ^
    -std=c89 -pedantic-errors -Werror=declaration-after-statement

if ERRORLEVEL 1 (
  echo Build failed.
  exit /b 1
)

echo Build succeeded. Output in out\

:: Also copy the three build outputs to the repository root
:: (the parent directory of this script; %%~fI resolves it).
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

copy /y "%~dp0out\index.html"  "%ROOT%\" >nul
if ERRORLEVEL 1 (echo Copy failed: index.html & exit /b 1)
copy /y "%~dp0out\index.js"    "%ROOT%\" >nul
if ERRORLEVEL 1 (echo Copy failed: index.js & exit /b 1)
copy /y "%~dp0out\index.wasm" "%ROOT%\" >nul
if ERRORLEVEL 1 (echo Copy failed: index.wasm & exit /b 1)

echo Copied index.html, index.js, index.wasm to %ROOT%
echo Serve it with:  python -m http.server --directory out
:: (Opening out\index.html directly with file:// will not work in most
:: browsers because .wasm must be fetched over HTTP.)
