@echo off
setlocal
rem PS5SX2 log relay: puts the Worker on your Cloudflare account, then stores the Discord webhook in it.
rem Needs Node.js (https://nodejs.org, the LTS version). Safe to run again: it updates the same Worker,
rem so this is also how to swap in a new webhook.
cd /d "%~dp0"
where npx >nul 2>nul
if errorlevel 1 (
  echo Node.js isn't installed. Get the LTS version from https://nodejs.org, then run this again.
  pause
  exit /b 1
)
echo.
echo [1/3] A browser window opens: log in to Cloudflare and click Allow.
call npx --yes wrangler@4 login
if errorlevel 1 goto fail
echo.
echo [2/3] Putting the Worker online. The first time, it may ask you to pick a workers.dev name: any name works.
call npx --yes wrangler@4 deploy
if errorlevel 1 goto fail
echo.
echo [3/3] Paste the Discord webhook URL, then press Enter.
call npx --yes wrangler@4 secret put DISCORD_WEBHOOK_URL
if errorlevel 1 goto fail
echo.
echo Done. Send Claude the https://ps5sx2-logs.....workers.dev address shown in step 2.
pause
exit /b 0

:fail
echo.
echo That step failed: the reason is in the lines above.
pause
exit /b 1
