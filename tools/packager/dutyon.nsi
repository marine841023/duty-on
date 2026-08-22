; DutyOn（开工啦）2.0 NSIS 安装包脚本
; 构建：tools\build-package.ps1（内部调用 makensis）
;
; 需求对应（见项目约定）：
;   - 当前用户安装（无 UAC，同 1.x tauri nsis installMode=currentUser）
;   - 安装时语言选择（简/繁/英/日/韩，启动即弹语言对话框）
;   - 安装时可选开机自启（HKCU Run，默认勾选）
;   - 覆盖安装：安装前关闭运行中的 dutyon-pet.exe（及旧版 TraePet.exe）
;   - 无自动更新检查（更新仅手动，升级失败提示到网站下载覆盖安装）
;   - 产物 exe 由 build-package.ps1 再包一层 DutyOn-v<版本>.zip 规避
;     SmartScreen

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define APP_NAME "DutyOn"
!define APP_NAME_CN "开工啦"
!define APP_EXE "dutyon-pet.exe"
; 版本可由命令行覆盖：makensis /DAPP_VERSION=x.y.z
!ifndef APP_VERSION
  !define APP_VERSION "2.0.0"
!endif
!define APP_PUBLISHER "DutyOn"
!define APP_REGKEY "Software\DutyOn"
!define APP_UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\DutyOn"
!define AUTORUN_REGKEY "Software\Microsoft\Windows\CurrentVersion\Run"

Name "${APP_NAME} ${APP_VERSION} (${APP_NAME_CN})"
OutFile "..\dist\DutyOn_${APP_VERSION}_x64-setup.exe"
Unicode True
InstallDir "$LOCALAPPDATA\${APP_NAME}"
InstallDirRegKey HKCU "${APP_REGKEY}" "InstallDir"
RequestExecutionLevel user

; ---- 图标与界面 ----
!define MUI_ICON "..\dutyon.ico"
!define MUI_UNICON "..\dutyon.ico"
!define MUI_ABORTWARNING
!define MUI_LANGDLL_ALLLANGUAGES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "$(LaunchApp)"

; ---- 页面 ----
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom AutoStartPageCreate AutoStartPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; ---- 语言（安装启动时弹选择对话框）----
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "TradChinese"
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Japanese"
!insertmacro MUI_LANGUAGE "Korean"

; ---- 语言文案（自定义控件）----
LangString AutoStartTitle ${LANG_SIMPCHINESE} "安装选项"
LangString AutoStartTitle ${LANG_TRADCHINESE} "安裝選項"
LangString AutoStartTitle ${LANG_ENGLISH} "Install Options"
LangString AutoStartTitle ${LANG_JAPANESE} "インストールオプション"
LangString AutoStartTitle ${LANG_KOREAN} "설치 옵션"

LangString AutoStartSubTitle ${LANG_SIMPCHINESE} "设置开机自启"
LangString AutoStartSubTitle ${LANG_TRADCHINESE} "設定開機自啟"
LangString AutoStartSubTitle ${LANG_ENGLISH} "Configure auto-start"
LangString AutoStartSubTitle ${LANG_JAPANESE} "自動起動の設定"
LangString AutoStartSubTitle ${LANG_KOREAN} "자동 시작 설정"

LangString AutoStartCheckbox ${LANG_SIMPCHINESE} "开机自动启动 ${APP_NAME_CN}（之后可在宠物菜单中随时开关）"
LangString AutoStartCheckbox ${LANG_TRADCHINESE} "開機自動啟動 ${APP_NAME_CN}（之後可在寵物選單中隨時開關）"
LangString AutoStartCheckbox ${LANG_ENGLISH} "Start ${APP_NAME} automatically at logon (can be toggled later from the pet's menu)"
LangString AutoStartCheckbox ${LANG_JAPANESE} "ログイン時に ${APP_NAME} を自動起動する（後からペットのメニューで切替可能）"
LangString AutoStartCheckbox ${LANG_KOREAN} "로그인 시 ${APP_NAME} 자동 시작（이후 펫 메뉴에서 언제든 변경 가능）"

LangString LaunchApp ${LANG_SIMPCHINESE} "立即运行 ${APP_NAME_CN}"
LangString LaunchApp ${LANG_TRADCHINESE} "立即執行 ${APP_NAME_CN}"
LangString LaunchApp ${LANG_ENGLISH} "Run ${APP_NAME} now"
LangString LaunchApp ${LANG_JAPANESE} "今すぐ ${APP_NAME} を起動"
LangString LaunchApp ${LANG_KOREAN} "지금 ${APP_NAME} 실행"

LangString UninstallLink ${LANG_SIMPCHINESE} "卸载开工啦"
LangString UninstallLink ${LANG_TRADCHINESE} "解除安裝開工啦"
LangString UninstallLink ${LANG_ENGLISH} "Uninstall DutyOn"
LangString UninstallLink ${LANG_JAPANESE} "DutyOn をアンインストール"
LangString UninstallLink ${LANG_KOREAN} "DutyOn 제거"

LangString ClosingApp ${LANG_SIMPCHINESE} "正在关闭运行中的 ${APP_NAME_CN}..."
LangString ClosingApp ${LANG_TRADCHINESE} "正在關閉執行中的 ${APP_NAME_CN}..."
LangString ClosingApp ${LANG_ENGLISH} "Closing running ${APP_NAME}..."
LangString ClosingApp ${LANG_JAPANESE} "実行中の ${APP_NAME} を終了しています..."
LangString ClosingApp ${LANG_KOREAN} "실행 중인 ${APP_NAME}을(를) 종료하는 중..."

Var AutoStartCheckbox
Var AutoStartState

; ---- 关闭运行中的进程（覆盖安装必需；卸载共用）----
!macro CloseRunningApp
  DetailPrint "$(ClosingApp)"
  nsExec::Exec 'taskkill /F /IM ${APP_EXE} /T'
  Pop $0
  nsExec::Exec 'taskkill /F /IM TraePet.exe /T'
  Pop $0
  Sleep 500
!macroend

Function .onInit
  ; 语言选择对话框（安装时）
  !insertmacro MUI_LANGDLL_DISPLAY
FunctionEnd

; ---- 开机自启选项页 ----
Function AutoStartPageCreate
  !insertmacro MUI_HEADER_TEXT "$(AutoStartTitle)" "$(AutoStartSubTitle)"
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateCheckbox} 0 20u 100% 12u "$(AutoStartCheckbox)"
  Pop $AutoStartCheckbox
  ; 默认勾选（已装过且显式关闭过的用户，宠物菜单内也可随时开关）
  StrCpy $AutoStartState ${BST_CHECKED}
  ${NSD_SetState} $AutoStartCheckbox $AutoStartState
  nsDialogs::Show
FunctionEnd

Function AutoStartPageLeave
  ${NSD_GetState} $AutoStartCheckbox $AutoStartState
FunctionEnd

Section "install"
  SetOutPath "$INSTDIR"
  SetOverwrite on

  ; 覆盖安装：先关旧进程再写文件
  !insertmacro CloseRunningApp

  File "..\..\device\build\Release\${APP_EXE}"
  File "..\..\device\build\Release\glfw3.dll"
  File /nonfatal "..\..\LICENSE"

  ; 内置 Live2D 模型（发布布局 <exe>/assets/live2d，main.cpp 约定）
  SetOutPath "$INSTDIR\assets\live2d"
  File /r "..\..\frontend\assets\live2d\*.*"

  ; 开机自启（按选项页勾选状态）
  SetOutPath "$INSTDIR"
  ${If} $AutoStartState == ${BST_CHECKED}
    WriteRegStr HKCU "${AUTORUN_REGKEY}" "${APP_NAME}" '"$INSTDIR\${APP_EXE}"'
  ${Else}
    DeleteRegValue HKCU "${AUTORUN_REGKEY}" "${APP_NAME}"
  ${EndIf}

  ; 安装信息 + 卸载入口（当前用户）
  WriteRegStr HKCU "${APP_REGKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "DisplayName" "${APP_NAME} (${APP_NAME_CN})"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "DisplayIcon" '"$INSTDIR\${APP_EXE}"'
  WriteRegStr HKCU "${APP_UNINSTKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKCU "${APP_UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${APP_UNINSTKEY}" "NoRepair" 1

  ; 开始菜单快捷方式
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME} ${APP_NAME_CN}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\$(UninstallLink).lnk" "$INSTDIR\uninstall.exe" "" "$INSTDIR\uninstall.exe" 0

  WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
  !insertmacro CloseRunningApp

  DeleteRegValue HKCU "${AUTORUN_REGKEY}" "${APP_NAME}"
  DeleteRegKey HKCU "${APP_UNINSTKEY}"
  DeleteRegKey HKCU "${APP_REGKEY}"

  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME} ${APP_NAME_CN}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\$(UninstallLink).lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"

  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\glfw3.dll"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\LICENSE"
  RMDir /r "$INSTDIR\assets"
  RMDir "$INSTDIR"
SectionEnd
