@echo off
setlocal
cd /d "%~dp0dashboard"

where node >nul 2>&1
if errorlevel 1 (
  echo 没有找到 Node.js。需要 20 或更新。
  exit /b 1
)

if not exist "node_modules\.bin\tsx.cmd" (
  echo dashboard 依赖还没装。先在 dashboard 目录运行 npm ci
  exit /b 1
)

call node_modules\.bin\tsx.cmd src\server.ts
exit /b %ERRORLEVEL%
