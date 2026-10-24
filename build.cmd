@echo off
rem Uses the CMake bundled with Visual Studio when cmake is not on PATH.
where cmake >nul 2>nul || set "PATH=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cmake -S . -B _build/x64 -G "Visual Studio 18 2026" -A x64 || exit /b 1
cmake --build _build/x64 --config Release || exit /b 1
