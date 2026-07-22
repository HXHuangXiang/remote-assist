@echo off
rem RemoteAssist 服务安装/更新脚本（需管理员权限）
setlocal EnableExtensions

set "SERVICE=remote-assist"
set "EXE=%~dp0RemoteAssist.exe"

if not exist "%EXE%" (
  echo 找不到 "%EXE%"，请把 install.bat 与 RemoteAssist.exe 放在同一目录。
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

echo 正在启动服务...
sc start "%SERVICE%"
if errorlevel 1 goto :service_error

echo.
echo 安装完成。服务以 LocalSystem 启动，会拉起 agent 与托盘。
echo 配置与日志位于 RemoteAssist.exe 同目录。
pause
exit /b 0

:service_error
echo 服务操作失败。请确认以管理员身份运行，并检查 logs\remote-assist.log。
pause
exit /b 1
