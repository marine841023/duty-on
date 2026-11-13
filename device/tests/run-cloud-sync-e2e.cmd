@echo off
rem Cloud sync end-to-end test against a locally started dutyon-cloud server (see cloud_sync_e2e.cpp).
rem Usage (repo root): device\tests\run-cloud-sync-e2e.cmd http://127.0.0.1:8799
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set OUT=device\build\cloud-sync-e2e
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /utf-8 /MD /Idevice/src /Idevice/third_party/deps/httplib /Idevice/third_party/deps/sha256 /Idevice/third_party/deps/json-3.11.3/include ^
  device/tests/cloud_sync_e2e.cpp device/src/cloud/cloud_client.cpp device/src/config/user_config.cpp ^
  /Fo%OUT%\ /Fe%OUT%\cloud_sync_e2e.exe /link winhttp.lib ws2_32.lib crypt32.lib
if errorlevel 1 exit /b 1
%OUT%\cloud_sync_e2e.exe %1
