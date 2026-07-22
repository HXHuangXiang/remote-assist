@echo off
rem RemoteAssist 服务安装/更新脚本（需管理员权限）
setlocal EnableExtensions

set "SERVICE=remote-assist"
set "EXE=%~dp0RemoteAssist.exe"
set "FIREWALL_RULE=RemoteAssist 局域网控制（专用网络）"
set "PORT=7980"

if not exist "%EXE%" (
  echo 找不到 "%EXE%"，请把 install.bat 与 RemoteAssist.exe 放在同一目录。
  pause
  exit /b 1
)

rem 不能把普通用户可写目录中的文件注册为 LocalSystem 服务。与图形安装共用
rem RemoteAssist.exe 内置的 ACL 校验；失败时不执行任何 sc 操作。
"%EXE%" --check-service-install-dir
if errorlevel 1 (
  echo 当前目录不适合安装 LocalSystem 服务：其中的 exe、web 或父目录可被非管理员改写。
  echo 请将整个 dist 目录放到仅管理员和 LocalSystem 可写的本机固定磁盘目录后重试。
  pause
  exit /b 1
)

rem 已存在的服务直接更新路径，避免 stop/delete/create 连续执行导致 1072（服务标记删除）。
sc query "%SERVICE%" >nul 2>&1
if errorlevel 1 goto :create_service

echo 正在停止已有服务...
sc stop "%SERVICE%" >nul 2>&1
for /L %%I in (1,1,20) do (
  sc query "%SERVICE%" | findstr /R /C:"STATE.*STOPPED" >nul && goto :update_service
  timeout /t 1 /nobreak >nul
)
echo 服务在 20 秒内未停止，请先在服务管理器中停止后重试。
pause
exit /b 1

:update_service
sc config "%SERVICE%" binPath= "\"%EXE%\" --service" start= auto obj= LocalSystem
if errorlevel 1 goto :service_error
goto :configure_service

:create_service
sc create "%SERVICE%" binPath= "\"%EXE%\" --service" start= auto obj= LocalSystem
if errorlevel 1 goto :service_error

:configure_service
sc description "%SERVICE%" "RemoteAssist 远控被控端(LocalSystem)" >nul
rem 服务进程异常退出时由 SCM 自动重试；正常 stop 不受此策略影响。
sc failure "%SERVICE%" reset= 86400 actions= restart/5000/restart/10000/""/0 >nul

rem 如果已存在 config.json，按当前端口放行；读取失败时安全回退到默认 7980。
set "REMOTE_ASSIST_CONFIG=%~dp0config.json"
for /f %%P in ('powershell -NoProfile -Command "$p=$env:REMOTE_ASSIST_CONFIG; try {$c=Get-Content -Raw -LiteralPath $p ^| ConvertFrom-Json; if ($c.port -ge 1 -and $c.port -le 65535) {[Console]::Write($c.port)}} catch {}"') do set "PORT=%%P"
netsh advfirewall firewall delete rule name="%FIREWALL_RULE%" >nul 2>&1
netsh advfirewall firewall add rule name="%FIREWALL_RULE%" dir=in action=allow profile=private program="%EXE%" protocol=TCP localport=%PORT% >nul
if errorlevel 1 echo 警告：未能添加专用网络防火墙规则，其他电脑可能无法访问。

echo 正在启动服务...
sc start "%SERVICE%"
if errorlevel 1 goto :service_error

echo.
echo 安装完成。服务以 LocalSystem 启动，会拉起 agent 与托盘。
echo 配置位于 RemoteAssist.exe 同目录；日志位于同级 logs\，按 service、agent、tray、setup 分文件保存。
pause
exit /b 0

:service_error
echo 服务操作失败。请确认以管理员身份运行，并检查 logs\service.log。
pause
exit /b 1
