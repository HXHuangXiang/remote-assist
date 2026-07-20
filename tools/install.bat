@echo off
rem RemoteAssist 安装脚本(需管理员权限)
setlocal
set EXE=%~dp0RemoteAssist.exe
if not exist "%EXE%" (
  echo 找不到 %EXE%,请把 install.bat 与 RemoteAssist.exe 放在同一目录。
  pause & exit /b 1
)
sc stop RemoteAssist >nul 2>&1
sc delete RemoteAssist >nul 2>&1
sc create RemoteAssist binPath= "%EXE% --service" start= auto obj= LocalSystem
if errorlevel 1 ( echo 创建服务失败,请用管理员身份运行。 & pause & exit /b 1 )
sc description RemoteAssist "RemoteAssist 远控被控端(LocalSystem)"
sc start RemoteAssist
echo.
echo 安装完成。服务以 LocalSystem 启动,会拉起 agent 与托盘。
echo 首次启动托盘会弹出访问密码;也可在 %%ProgramData%%\RemoteAssist 下查看。
pause

