@echo off
rem Build with an x64 "Developer Command Prompt for VS" (cl.exe and ml64.exe on PATH).
setlocal
where cl.exe >nul 2>&1 || (echo Run this from an x64 Native Tools Command Prompt. & exit /b 1)
where ml64.exe >nul 2>&1 || (echo ml64.exe not found - need the x64 toolchain. & exit /b 1)

ml64.exe /nologo /c /Fochacha-x86_64.obj chacha-x86_64.asm || exit /b 1
ml64.exe /nologo /c /Fopoly1305-x86_64.obj poly1305-x86_64.asm || exit /b 1
cl.exe /nologo /O2 /W4 /MT bench.c chacha-x86_64.obj poly1305-x86_64.obj /Febench.exe || exit /b 1

echo.
echo Built bench.exe - run it with no arguments.
