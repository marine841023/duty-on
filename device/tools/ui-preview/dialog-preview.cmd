@echo off
rem 弹窗外观预览：编译 dialog_preview.cpp 并把各弹窗截图输出到指定目录。
rem 用法（在仓库根目录）：device\tools\ui-preview\dialog-preview.cmd device\build\ui-preview\dialogs
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set OUT=%~1
if "%OUT%"=="" set OUT=device\build\ui-preview\dialogs
if not exist "%OUT%" mkdir "%OUT%"
cl.exe /nologo /std:c++17 /utf-8 /EHsc /O1 /DUNICODE /D_UNICODE /DNOMINMAX /Idevice\src ^
  device\tools\ui-preview\dialog_preview.cpp /Fo"%OUT%\dialog_preview.obj" /Fe"%OUT%\dialog_preview.exe" ^
  /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib dwmapi.lib gdiplus.lib ole32.lib
if errorlevel 1 exit /b 1
"%OUT%\dialog_preview.exe" %OUT%
dir /b "%OUT%\*.png"
