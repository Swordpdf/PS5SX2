@echo off
setlocal
rem Puts a Discord webhook into the PS5SX2 log relay (DISCORD_WEBHOOK_URL), e.g. after you replace the webhook.
rem First copy it in Discord: Edit Channel, Integrations, Webhooks, click the webhook, Copy Webhook URL.
rem This takes it from the clipboard (nothing to paste), checks it is a webhook URL, hands it to wrangler
rem without showing it, then asks the relay whether it has one. Needs the Cloudflare login deploy.cmd did.
cd /d "%~dp0"
set "PATH=%ProgramFiles%\nodejs;%PATH%"
set "TMPF=%TEMP%\ps5sx2-webhook-%RANDOM%.tmp"
powershell -NoProfile -Command "$u = Get-Clipboard -Raw; if ($u) { $u = $u.Trim() }; if ($u -notmatch '^https://(ptb\.|canary\.)?discord(app)?\.com/api/webhooks/[0-9]+/[A-Za-z0-9_-]+$') { exit 1 }; [Console]::Out.Write($u)" > "%TMPF%"
if errorlevel 1 goto noclip
call npx --yes wrangler@4 secret put DISCORD_WEBHOOK_URL < "%TMPF%"
set RC=%errorlevel%
del "%TMPF%" 2>nul
if not "%RC%"=="0" goto fail
echo.
echo Asking the relay whether it has the webhook now...
powershell -NoProfile -Command "Start-Sleep -Seconds 5"
curl.exe -s -X POST -H "content-type: text/plain" --data x https://ps5sx2-logs.ps5sx2.workers.dev/v1/logs | findstr /c:"bad console id" >nul
if errorlevel 1 (
  echo The relay doesn't have a webhook yet. Tell Claude.
) else (
  echo The relay has the webhook. Done.
)
pause
exit /b 0

:noclip
del "%TMPF%" 2>nul
echo The clipboard doesn't hold a Discord webhook URL. In Discord: Edit Channel, Integrations, Webhooks,
echo click the webhook, Copy Webhook URL. Then run this again.
pause
exit /b 1

:fail
echo.
echo That failed: the reason is in the lines above.
pause
exit /b 1
