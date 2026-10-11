@echo off
setlocal

:: ---------------- 1. WebAssembly build (emcc) ----------------
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
  echo WASM build failed.
  exit /b 1
)

echo WASM build succeeded. Output in out\

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

:: ---------------- 2. Native build (MSVC cl) ----------------
set "SDL2=%~dp0third_party\SDL2-2.32.10"
set "CLANG=%~dp0..\..\emsdk\upstream\bin\clang.exe"
set "OUTWIN=%~dp0out_win"

if not exist "%OUTWIN%" mkdir "%OUTWIN%"

:: C89 hard-fail gate for the native code path (where __EMSCRIPTEN__ is
:: not defined).  MSVC has no switch that rejects C99 mixed declarations
:: or // comments in C mode -- not even /Za -- so the native branch is
:: conformance-checked with clang first, using the same flags as the
:: emcc stage above.  The executable itself is still built by cl below.
"%CLANG%" -std=c89 -pedantic-errors -Werror=declaration-after-statement ^
    -fsyntax-only -I"%SDL2%\include" "%~dp0src\main.c"

if ERRORLEVEL 1 (
  echo C89 conformance check failed for the native branch.
  exit /b 1
)

call "E:\visualstudio2026\VC\Auxiliary\Build\vcvarsall.bat" x64

:: The cl build itself, mirroring the pixelizer project:
::   /Za  disables Microsoft language extensions (ANSI C89 mode)
::   /W4  high warning level
cl /Za /W4 "%~dp0src\main.c" ^
    /I"%SDL2%\include" ^
    /Fe:"%OUTWIN%\app.exe" /Fo:"%OUTWIN%\app.obj" ^
    /link /LIBPATH:"%SDL2%\lib\x64" SDL2.lib

if ERRORLEVEL 1 (
  echo Native build failed.
  exit /b 1
)

if exist "%OUTWIN%\app.obj" del "%OUTWIN%\app.obj" >nul

:: app.exe needs SDL2.dll beside it to run.
copy /y "%SDL2%\lib\x64\SDL2.dll" "%OUTWIN%\" >nul
if ERRORLEVEL 1 (echo Copy failed: SDL2.dll & exit /b 1)

echo Native build succeeded: out_win\app.exe (needs out_win\SDL2.dll)
echo Serve the web build with:  python -m http.server --directory out
:: (Opening out\index.html directly with file:// will not work in most
:: browsers because .wasm must be fetched over HTTP.)
