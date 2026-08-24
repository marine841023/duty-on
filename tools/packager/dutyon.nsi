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
  !define APP_VERSION "2.0.2"
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
  nsExec::Exec 'taskkill /F /IM duty-on.exe /T'
  Pop $0
  Sleep 500
!macroend

; ---- 旧安装位置探测（升级安装装回原位）----
; 优先级：1.x Tauri 恢复键 > 2.0 命名值 > 全盘特征文件扫描 > 默认目录。
; 权威依据（tauri-bundler 2.9.4 installer.nsi，实测核对）：
;   1.x RestorePreviousInstallLocation 读 SHCTX "Software\<MANUFACTURER>\<PRODUCTNAME>"
;   的默认值 —— 即 HKCU "Software\DutyOn\DutyOn" 默认值（两层子键）。
;   MANUFACTURER/DutyOn 与 PRODUCTNAME/DutyOn 同名，实测本机 1.3.1 安装后
;   写入 [HKCU\Software\DutyOn\DutyOn] 默认值 = 安装目录。
; 1.x 与 2.0 均读写该键 => 双向交替恢复闭环。
!macro TryOldRoot root
  ${If} $R9 == ""
    ${IfThen} ${FileExists} "${root}\dutyon-pet.exe" ${|} StrCpy $R9 "${root}" ${|}
    ${If} $R9 == ""
      ${IfThen} ${FileExists} "${root}\duty-on.exe" ${|} StrCpy $R9 "${root}" ${|}
      ${If} $R9 == ""
        ${IfThen} ${FileExists} "${root}\TraePet.exe" ${|} StrCpy $R9 "${root}" ${|}
      ${EndIf}
    ${EndIf}
  ${EndIf}
!macroend

Function DetectPreviousInstallDir
  StrCpy $R9 ""
  ; 1) 1.x Tauri 恢复键（HKCU Software\DutyOn\DutyOn 默认值；2.0 也写）
  ReadRegStr $0 HKCU "Software\${APP_NAME}\${APP_NAME}" ""
  ${IfThen} $0 != "" ${|} StrCpy $R9 $0 ${|}
  ; 2) 2.0 命名值（老版本 2.0.0 写过）
  ${If} $R9 == ""
    ReadRegStr $0 HKCU "${APP_REGKEY}" "InstallDir"
    ${IfThen} $0 != "" ${|} StrCpy $R9 $0 ${|}
  ${EndIf}
  ; 3) 全盘特征文件扫描（C~G 常见自定义位置；注册表被清的机器）
  ${If} $R9 == ""
    ${For} $R8 67 71   ; 'C'..'G'
      IntFmt $R7 "%c:" $R8
      !insertmacro TryOldRoot "$R7\DutyOn"
      !insertmacro TryOldRoot "$R7\program\DutyOn"
      !insertmacro TryOldRoot "$R7\Program Files\DutyOn"
      !insertmacro TryOldRoot "$R7\Program Files (x86)\DutyOn"
      !insertmacro TryOldRoot "$R7\TraePet"
    ${Next}
  ${EndIf}
  ${If} $R9 != ""
    StrCpy $INSTDIR $R9
    DetailPrint "检测到既有安装: $INSTDIR（升级覆盖）"
  ${EndIf}
FunctionEnd

Function .onInit
  ; 语言选择对话框（安装时）
  !insertmacro MUI_LANGDLL_DISPLAY
  Call DetectPreviousInstallDir
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

  ; 就地升级清理：删除 1.x 主程序与 Tauri 专属目录（resources/_up_），
  ; 用户数据在 ~/.dutyon 不受影响；旧卸载器一并移除（由 2.0 接管）
  Delete "$INSTDIR\duty-on.exe"
  Delete "$INSTDIR\TraePet.exe"
  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\Uninstall TraePet.exe"
  RMDir /r "$INSTDIR\resources"
  RMDir /r "$INSTDIR\_up_"
  Delete "$SMPROGRAMS\DutyOn.lnk"

  ; 1.0 僵尸自启项清理（目录已删但 Run 项残留，指向失效路径）
  ${IfNot} ${FileExists} "D:\TraePet\TraePet.exe"
    DeleteRegValue HKCU "${AUTORUN_REGKEY}" "TraePet"
  ${EndIf}

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
  ; 双版本交替安装闭环（实测核对 tauri-bundler 2.9.4 installer.nsi）:
  ; 1.x 的 RestorePreviousInstallLocation 读 HKCU "Software\DutyOn\DutyOn"
  ; 默认值（MANUFACTURER\PRODUCTNAME 两层）恢复安装目录 —— 1.x 回装时
  ; 装回本目录。2.0 的 DetectPreviousInstallDir 优先读同一键。
  WriteRegStr HKCU "Software\${APP_NAME}\${APP_NAME}" "" "$INSTDIR"
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
  ; 只删自己的命名值，不删 Software\DutyOn 整键（会连带恢复键子键
  ; DutyOn\DutyOn）。保留位置记忆：卸载 2.0 改装 1.x 时仍能装回原目录，
  ; 对交替使用场景友好；残留空键无害（重装双方都会重写）。
  DeleteRegValue HKCU "${APP_REGKEY}" "InstallDir"

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
