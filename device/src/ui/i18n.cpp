// 2.0 客户端多语言实现 —— 移植自 1.x frontend/i18n.js（键值逐条对应）。
// 只移植客户端用到的键；翻译文本与 1.x 保持一字不差。

#include "ui/i18n.h"

#include <cstdio>
#include <cstring>

namespace dutyon {

namespace {

struct Entry {
    const char* key;
    const char* zhCN;
    const char* zhTW;
    const char* en;
    const char* ja;
    const char* ko;
    const char* fr;
    const char* de;
    const char* es;
};

// clang-format off
static const Entry kEntries[] = {
    // ---- 状态 / 状态栏 ----
    {"state.sleeping",     "空闲中",      "空閒中",      "Idle",                    "待機中",       "대기 중",    "Inactif",                  "Inaktiv",           "Inactivo"},
    {"state.working",      "忙碌中",      "忙碌中",      "Working",                 "作業中",       "작업 중",    "Occupé",                   "Beschäftigt",       "Ocupado"},
    {"state.alert",        "需要确认!",   "需要確認!",   "Confirmation needed!",    "確認が必要!",  "확인 필요!",  "Confirmation requise !",   "Bestätigung nötig!","¡Confirmación necesaria!"},
    {"status.waiting",     "等待 IDE 连接...", "等待 IDE 連接...", "Waiting for IDE...", "IDE接続待ち...", "IDE 연결 대기 중...", "En attente d'un IDE...", "Warte auf IDE-Verbindung...", "Esperando conexión IDE..."},
    {"status.busy",        "忙碌",        "忙碌",        "Busy",                    "忙しい",       "작업 중",    "Occupé",                   "Beschäftigt",       "Ocupado"},
    {"status.idle",        "空闲",        "空閒",        "Idle",                    "待機",         "대기",       "Inactif",                  "Inaktiv",           "Inactivo"},
    {"status.confirmationNeeded", "需要确认", "需要確認", "Confirmation needed",    "確認が必要",   "확인 필요",  "Confirmation requise",     "Bestätigung nötig", "Confirmación necesaria"},
    {"status.thinking",    "思考中",      "思考中",      "Thinking",                "思考中",       "생각 중",    "Réflexion",                "Denkt nach",        "Pensando"},
    {"status.toolUse",     "执行中",      "執行中",      "Using tool",              "ツール実行中", "도구 사용",  "Outil actif",              "Werkzeug aktiv",    "Usando herramienta"},
    // ---- 菜单 ----
    {"menu.switchModel",   "切换形象",    "切換形象",    "Switch Model",            "モデル切替",   "모델 변경",  "Changer de modèle",        "Modell wechseln",   "Cambiar modelo"},
    {"menu.uploadLive2D",  "上传 Live2D 模型", "上傳 Live2D 模型", "Upload Live2D Model", "Live2D モデルを追加", "Live2D 모델 추가", "Ajouter un modèle Live2D", "Live2D-Modell hinzufügen", "Añadir modelo Live2D"},
    {"menu.playMotion",    "播放动作",    "播放動作",    "Play Motion",             "モーション再生", "모션 재생", "Jouer un mouvement",     "Bewegung abspielen","Reproducir movimiento"},
    {"menu.actionSettings","角色设定",    "角色設定",    "Character Settings",      "キャラクター設定","캐릭터 설정", "Réglages du personnage",   "Charaktereinstellungen", "Ajustes del personaje"},
    {"menu.flipHorizontal","左右翻转",    "左右翻轉",    "Flip Horizontal",         "左右反転",     "좌우 반전",  "Retourner horizontalement","Horizontal spiegeln","Voltear horizontalmente"},
    {"menu.miniMode",      "迷你模式",    "迷你模式",    "Mini Mode",               "ミニモード",   "미니 모드",  "Mode mini",                "Mini-Modus",        "Modo mini"},
    {"menu.visibility",    "显示隐藏",    "顯示隱藏",    "Show / Hide",             "表示/非表示",  "표시/숨기기","Afficher / Masquer",      "Anzeigen / Ausblenden", "Mostrar / Ocultar"},
    {"menu.display",       "显示",        "顯示",        "Display",                 "表示",          "표시",       "Affichage",               "Anzeige",           "Pantalla"},
    {"menu.minimize",      "隐藏",        "隱藏",        "Hide",                    "隠す",          "숨기기",     "Masquer",                 "Verbergen",         "Ocultar"},
    {"menu.integration",   "集成",        "整合",        "Integration",             "統合",          "통합",       "Intégration",             "Integration",       "Integración"},
    {"menu.integrated",    "已集成",      "已整合",      "Integrated",              "統合済み",      "통합됨",     "Intégré",                 "Integriert",        "Integrado"},
    {"menu.notIntegrated", "未集成",      "未整合",      "Not integrated",          "未統合",        "통합 안 됨", "Non intégré",             "Nicht integriert",  "No integrado"},
    {"menu.systemMonitor", "系统监控",    "系統監控",    "System Monitor",          "システムモニター","시스템 모니터","Moniteur système",     "Systemmonitor",     "Monitor del sistema"},
    {"menu.autoLaunch",    "开机自启动",  "開機自啟動",  "Start on Boot",           "起動時に自動開始","부팅 시 자동 시작","Démarrage auto",  "Autostart",         "Inicio automático"},
    {"menu.deviceStatus",  "设备状态",    "設備狀態",    "Device Status",           "デバイス状態",  "장치 상태",  "État de l'appareil",       "Gerätestatus",      "Estado del dispositivo"},
    {"menu.deviceOnline",  "设备：已连接", "設備：已連接", "Device: Connected",      "デバイス：接続済み","장치: 연결됨","Appareil : connecté",   "Gerät: verbunden",  "Dispositivo: conectado"},
    {"menu.deviceOffline", "设备：未连接", "設備：未連接", "Device: Not Connected",  "デバイス：未接続","장치: 연결 안 됨","Appareil : non connecté","Gerät: nicht verbunden","Dispositivo: no conectado"},
    {"menu.deviceMode",    "设备模式",    "設備模式",    "Device Mode",             "デバイスモード","장치 모드",  "Mode de l'appareil",       "Gerätemodus",       "Modo del dispositivo"},
    {"menu.mode",          "模式",        "模式",        "Mode",                    "モード",      "모드",      "Mode",                    "Modus",             "Modo"},
    {"menu.modeSingle",    "单任务模式",  "單任務模式",  "Single Task",             "シングルタスク","단일 작업", "Tâche unique",             "Einzelaufgabe",     "Tarea única"},
    {"menu.modeMulti",     "多任务模式",  "多任務模式",  "Multi Task",              "マルチタスク",  "다중 작업", "Multi-tâches",             "Mehrfachaufgabe",   "Multitarea"},
    {"menu.modeFrame",     "电子相框模式", "電子相框模式", "Photo Frame",            "フォトフレーム","포토 프레임","Cadre photo",            "Fotorahmen",        "Marco digital"},
    {"menu.modeVoice",     "语音交互模式", "語音交互模式", "Voice Interaction",      "音声インタラクション", "음성 인터랙션", "Interaction vocale",  "Sprachinteraktion", "Interacción de voz"},
    // ---- 电子相框播放源（动作轮播 / 指定文件夹照片）----
    {"menu.frameSource",   "相框播放",    "相框播放",    "Frame Source",            "フレーム表示",  "프레임 소스", "Source du cadre photo",  "Rahmenquelle",      "Fuente del marco"},
    {"menu.frameMotion",   "动作轮播",    "動作輪播",    "Motion Carousel",         "モーション再生",  "모션 캐러셀", "Carrousel de mouvements","Bewegungs-Slideshow","Carrusel de movimientos"},
    {"menu.framePhotos",   "指定文件夹",  "指定文件夾",  "Photo Folder",            "フォルダー写真",  "지정 폴더",   "Dossier de photos",      "Fotos-Ordner",      "Carpeta de fotos"},
    {"menu.framePickFolder", "选择文件夹…", "選擇文件夾…", "Choose Folder…",          "フォルダーを選択…", "폴더 선택…", "Choisir le dossier…",    "Ordner wählen…",    "Elegir carpeta…"},
    {"menu.frameNoFolder", "未选择文件夹", "未選擇文件夾", "No folder chosen",        "フォルダー未選択", "폴더 미선택", "Aucun dossier choisi",   "Kein Ordner gewählt","Carpeta no elegida"},
    {"menu.framePhotoCount", "张照片",    "張照片",      "photos",                  "枚の写真",      "장 사진",     "photos",                   "Fotos",             "fotos"},
    {"menu.frameHint",     "每播完一张才向 PC 要下一张，照片不批量同步到设备", "每播完一張才向 PC 要下一張，照片不批次同步到裝置", "Photos are streamed one at a time — nothing is bulk-synced to the device", "写真は1枚ずつ配信され、デバイスには一括同期されません", "사진은 한 장씩 전달되며 장치로 일괄 동기화되지 않습니다", "Les photos sont transmises une à une : rien n'est synchronisé en masse", "Fotos werden einzeln übertragen – keine Massensynchronisierung auf das Gerät", "Las fotos se envían de una en una: nada se sincroniza en bloque"},
    {"menu.clockColor",    "时钟颜色",    "時鐘顏色",    "Clock Color",             "時計の色",    "시계 색상", "Couleur de l'horloge",    "Uhrenfarbe",        "Color del reloj"},
    {"menu.themeColor",    "主题颜色",    "主題顏色",    "Theme Color",             "テーマカラー", "테마 색상", "Couleur du thème",        "Themenfarbe",       "Color del tema"},
    {"menu.device",        "设备",        "設備",        "Device",                  "デバイス",    "장치",      "Appareil",                "Gerät",             "Dispositivo"},
    // ---- 设备配对（Wi-Fi 配对码方案）----
    {"menu.pairDevice",    "配对设备",    "配對設備",    "Pair Device",             "デバイス接続", "장치 연결",  "Associer l'appareil",     "Gerät koppeln",     "Vincular dispositivo"},
    {"menu.brightness",    "亮度",        "亮度",        "Brightness",              "明るさ",      "밝기",      "Luminosité",              "Helligkeit",        "Brillo"},
    {"menu.volume",        "音量",        "音量",        "Volume",                  "音量",        "볼륨",      "Volume",                  "Lautstärke",        "Volumen"},
    {"menu.screenRotate",  "屏幕旋转",    "螢幕旋轉",    "Screen Rotation",         "画面の回転",  "화면 회전", "Rotation de l'écran",     "Bilddrehung",       "Rotación de pantalla"},
    {"menu.rot0",          "0° 不旋转",  "0° 不旋轉",  "0° (No rotation)",        "0° 回転なし", "0° 회전 없음","0° (pas de rotation)",   "0° (keine Drehung)","0° (sin rotación)"},
    {"menu.rot90",         "90°",        "90°",        "90°",                     "90°",         "90°",       "90°",                     "90°",               "90°"},
    {"menu.rot180",        "180°",       "180°",       "180°",                    "180°",        "180°",      "180°",                    "180°",              "180°"},
    {"menu.rot270",        "270°",       "270°",       "270°",                    "270°",        "270°",      "270°",                    "270°",              "270°"},
    {"menu.colorAmber",    "琥珀橙",      "琥珀橙",      "Amber",                   "アンバー",    "앰버",     "Ambre",                    "Bernstein",         "Ámbar"},
    {"menu.colorIce",      "冰晶蓝",      "冰晶藍",      "Ice Blue",                "アイスブルー","아이스 블루","Bleu glace",             "Eisblau",           "Azul hielo"},
    {"menu.colorWhite",    "暖白",        "暖白",        "Warm White",              "ウォームホワイト","웜 화이트","Blanc chaud",            "Warmweiß",          "Blanco cálido"},
    {"menu.colorGreen",    "翠竹绿",      "翠竹綠",      "Bamboo Green",            "バンブーグリーン","대나무 녹색","Vert bambou",           "Bambusgrün",        "Verde bambú"},
    {"menu.colorPink",     "樱花粉",      "櫻花粉",      "Sakura Pink",             "サクラピンク","벚꽃 분홍","Rose sakura",             "Kirschrosa",        "Rosa sakura"},
    {"menu.syncDevice",    "同步程序到设备", "同步程式到裝置", "Sync App to Device",      "アプリをデバイスへ同期","앱을 기기로 동기화","Synchroniser vers l'appareil","App auf Gerät synchronisieren","Sincronizar app al dispositivo"},
    {"menu.soundManage",   "声音管理",    "聲音管理",    "Sound Settings",          "サウンド設定",  "사운드 설정", "Réglages du son",         "Toneinstellungen",  "Ajustes de sonido"},
    {"menu.soundMuted",    "已静音",      "已靜音",      "Muted",                   "ミュート中",   "음소거됨",   "En sourdine",             "Stumm",             "Silenciado"},
    {"menu.soundOn",       "声音开启",    "聲音開啟",    "Sound On",                "サウンド ON",  "사운드 켜짐", "Son activé",              "Ton an",            "Sonido activado"},
    {"menu.soundMuteAll",  "完全静音",    "完全靜音",    "Mute All",                "すべてミュート","전체 음소거", "Tout couper",             "Alles stumm",       "Silenciar todo"},
    {"menu.soundNoBinding","当前角色未绑定音频", "當前角色未綁定音頻", "No audio bound",     "音声が未設定",  "오디오 미연결","Aucun audio lié",        "Kein Audio gebunden","Sin audio asignado"},
    {"menu.audioBinding",  "音频",        "音頻",        "Audio",                   "音声",          "오디오",     "Audio",                    "Audio",             "Audio"},
    {"menu.audioClear",    "清除音频",    "清除音頻",    "Clear Audio",             "音声を解除",    "오디오 해제", "Retirer l'audio",         "Audio entfernen",   "Quitar audio"},
    {"menu.audioPreview",  "试听",        "試聽",        "Preview",                 "再生",          "미리듣기",   "Écouter",                 "Anhören",           "Escuchar"},
    {"menu.audioDefault",  "默认",        "預設",        "Default",                 "デフォルト",   "기본",       "Par défaut",              "Standard",          "Predeterminado"},
    {"menu.newChar",       "新建自定义角色…", "新增自訂角色…", "New Custom Character…",   "カスタムキャラを作成","커스텀 캐릭터 만들기","Nouveau personnage…",  "Neuer Charakter…",     "Nuevo personaje…"},
    {"menu.manageChar",    "编辑自定义角色", "編輯自訂角色",  "Edit Custom Characters",  "カスタムキャラを編集","커스텀 캐릭터 편집","Modifier les personnages","Charaktere bearbeiten","Editar personajes"},
    {"menu.deleteChar",    "删除角色",      "刪除角色",      "Delete Character",         "キャラを削除",  "캐릭터 삭제","Supprimer le personnage","Charakter löschen",  "Eliminar personaje"},
    {"menu.language",      "语言",        "語言",        "Language",                "言語",         "언어",       "Langue",                   "Sprache",           "Idioma"},
    {"menu.back",          "返回",        "返回",        "Back",                    "戻る",         "뒤로",       "Retour",                   "Zurück",            "Volver"},
    {"menu.installHooks",  "安装 IDE 集成", "安裝 IDE 整合", "Install IDE Integration","IDE統合をインストール","IDE 통합 설치","Installer l'intégration IDE","IDE-Integration installieren","Instalar integración IDE"},
    {"menu.hookStatus",    "Hook 状态",   "Hook 狀態",   "Hook Status",             "Hookステータス","Hook 상태",  "Statut des Hooks",         "Hook-Status",       "Estado de Hooks"},
    {"menu.quit",          "退出",        "退出",        "Quit",                    "終了",         "종료",       "Quitter",                  "Beenden",           "Salir"},
    // ---- 云端账户 + 应用内更新 ----
    {"menu.cloud",         "云端账户",    "雲端帳戶",    "Cloud",                   "クラウド",     "클라우드",   "Cloud",                    "Cloud",             "Nube"},
    {"menu.cloudLogin",    "登录云端账户", "登入雲端帳戶", "Sign In",                "ログイン",     "로그인",     "Se connecter",             "Anmelden",          "Iniciar sesión"},
    {"menu.cloudIntro",    "登录后同步角色 / 音频 / 配置", "登入後同步角色 / 音訊 / 設定", "Sync characters, audio & settings", "ログインでキャラ・音声・設定を同期", "로그인하면 캐릭터·오디오·설정 동기화", "Synchroniser personnages, audio et réglages", "Charaktere, Audio & Einstellungen synchronisieren", "Sincroniza personajes, audio y ajustes"},
    {"menu.cloudSync",     "立即同步",    "立即同步",    "Sync Now",                "今すぐ同期",    "지금 동기화", "Synchroniser",             "Jetzt synchronisieren", "Sincronizar"},
    {"menu.cloudRestore",  "从云端恢复",  "從雲端恢復",  "Restore from Cloud",      "クラウドから復元", "클라우드에서 복원", "Restaurer depuis le cloud", "Aus Cloud wiederherstellen", "Restaurar desde la nube"},
    {"menu.cloudLogout",   "退出登录",    "登出",        "Sign Out",                "ログアウト",    "로그아웃",   "Se déconnecter",           "Abmelden",          "Cerrar sesión"},
    {"menu.cloudLoggedIn", "已登录",      "已登入",      "Signed In",               "ログイン済み",  "로그인됨",   "Connecté",                 "Angemeldet",        "Conectado"},
    {"menu.cloudLoggedOut","未登录",      "未登入",      "Not Signed In",           "未ログイン",    "로그인 안 됨", "Non connecté",           "Nicht angemeldet",  "No conectado"},
    {"menu.checkUpdate",   "检查更新",    "檢查更新",    "Check for Updates",       "アップデート確認", "업데이트 확인", "Vérifier les mises à jour", "Nach Updates suchen", "Buscar actualizaciones"},
    {"menu.selectMotion",  "选择动作",    "選擇動作",    "Select Motion",           "モーション選択","모션 선택",  "Choisir un mouvement",    "Bewegung wählen",   "Elegir movimiento"},
    {"menu.newCharacter",  "+ 新建形象",  "+ 新建形象",  "+ New Character",         "+ 新規キャラクター","+ 새 캐릭터","+ Nouveau personnage",  "+ Neues Modell",    "+ Nuevo personaje"},
    // ---- 动作设定三状态 ----
    {"settings.sleeping",  "空闲中",      "空閒中",      "Idle",                    "待機中",       "대기 중",    "Inactif",                  "Inaktiv",           "Inactivo"},
    {"settings.working",   "忙碌中",      "忙碌中",      "Working",                 "作業中",       "작업 중",    "Occupé",                   "Arbeitet",          "Ocupado"},
    {"settings.alert",     "需要确认",    "需要確認",    "Alert",                   "確認",         "확인",       "Alerte",                   "Alarm",             "Alerta"},
    {"settings.welcome",   "欢迎",        "歡迎",        "Welcome",                 "ウェルカム",   "환영",       "Accueil",                  "Willkommen",        "Bienvenida"},
    // ---- 监控面板 ----
    {"monitor.title",      "系统监控",    "系統監控",    "System Monitor",          "システムモニター","시스템 모니터","Moniteur système",     "Systemmonitor",     "Monitor del sistema"},
    {"monitor.cpu",        "CPU",         "CPU",         "CPU",                     "CPU",          "CPU",        "CPU",                      "CPU",               "CPU"},
    // 行标签压缩：监控行内布局是 标签+数值+折线图 三段（面板 240px 窄栏），
    // 长词（Arbeitsspeicher/ネットワーク/視訊記憶體）会叠到数值区，
    // 技术缩写 RAM/VRAM/Netz/ネット 各语言通用且短
    {"monitor.ram",        "内存",        "記憶體",      "Memory",                  "メモリ",       "메모리",     "Mémoire",                  "RAM",               "Memoria"},
    {"monitor.gpu",        "显卡",        "顯示卡",      "GPU",                     "GPU",          "GPU",        "GPU",                      "GPU",               "GPU"},
    {"monitor.vram",       "显存",        "顯存",        "VRAM",                    "VRAM",         "VRAM",       "VRAM",                     "VRAM",              "VRAM"},
    {"monitor.net",        "网络",        "網路",        "Network",                 "ネット",       "네트워크",   "Réseau",                   "Netz",              "Red"},
    {"monitor.self",       "自身",        "自身",        "Self",                    "自身",         "자체",       "App",                      "App",               "App"},
    {"monitor.projectList","项目列表",    "專案列表",    "Project List",            "プロジェクト一覧","프로젝트 목록","Liste des projets",    "Projektliste",      "Lista de proyectos"},
    {"monitor.reset",      "恢复默认显示","恢復預設顯示","Restore Defaults",        "デフォルトに戻す","기본값 복원","Rétablir les valeurs par défaut","Standard wiederherstellen","Restaurar valores predeterminados"},
    {"monitor.expand",     "展开",        "展開",        "Expand",                  "展開",         "펼치기",     "Déplier",                  "Ausklappen",        "Desplegar"},
    {"monitor.collapse",   "收起",        "收起",        "Collapse",                "折りたたむ",   "접기",       "Replier",                  "Einklappen",        "Plegar"},
    // ---- Hook 状态提示 ----
    {"hook.installed",     "已安装",      "已安裝",      "Installed",               "インストール済み","설치됨",   "Installé",                 "Installiert",       "Instalado"},
    {"hook.notInstalled",  "未安装",      "未安裝",      "Not installed",           "未インストール","미설치",    "Non installé",             "Nicht installiert", "No instalado"},
    {"hook.notChecked",    "未检查",      "未檢查",      "Not checked",             "未確認",       "미확인",     "Non vérifié",              "Nicht geprüft",     "Sin verificar"},
    {"hook.connected",     "已连接",      "已連接",      "Connected",               "接続済み",     "연결됨",     "Connecté",                 "Verbunden",         "Conectado"},
    // ---- 内置模型动作显示名（nito 系；其余模型回退 "组 N"）----
    {"motion.Idle.0",      "发呆",        "發呆",        "Idle",                    "ぼんやり",     "멍때림",     "Rêverie",                  "Tagtraum",          "Soñar despierto"},
    {"motion.Idle.1",      "开心",        "開心",        "Happy",                   "嬉しい",       "행복",       "Joyeux",                   "Glücklich",         "Feliz"},
    {"motion.Idle.2",      "叹气",        "嘆氣",        "Sigh",                    "ため息",       "한숨",       "Soupir",                   "Seufzen",           "Suspiro"},
    {"motion.Idle.3",      "睡觉",        "睡覺",        "Sleep",                   "寝る",         "잠",         "Dormir",                   "Schlafen",          "Dormir"},
    {"motion.Tap.0",       "生气",        "生氣",        "Angry",                   "怒る",         "화남",       "En colère",                "Wütend",            "Enfadado"},
    {"motion.Tap.1",       "难过",        "難過",        "Sad",                     "悲しい",       "슬픔",       "Triste",                   "Traurig",           "Triste"},
    {"motion.Tap.2",       "哭泣",        "哭泣",        "Cry",                     "泣く",         "울음",       "Pleurer",                  "Weinen",            "Llorar"},
    {"motion.Tap.3",       "喜悦",        "喜悅",        "Joy",                     "喜び",         "기쁨",       "Joie",                     "Freude",            "Alegría"},
    {"motion.Tap.4",       "点头",        "點頭",        "Nod",                     "うなずく",     "고개 끄덕임", "Acquiescer",              "Nicken",            "Asentir"},
    {"motion.FlickUp.0",   "再见",        "再見",        "Bye",                     "さようなら",   "잘 가",      "Au revoir",                "Tschüss",           "Adiós"},
    {"motion.FlickUp.1",   "高兴",        "高興",        "Glad",                    "喜ぶ",         "즐거움",     "Ravi",                     "Froh",              "Contento"},
    {"motion.FlickUp.2",   "威胁",        "威脅",        "Threat",                  "脅す",         "위협",       "Menace",                   "Drohung",           "Amenaza"},
    {"motion.FlickDown.0", "肌肉",        "肌肉",        "Muscle",                  "筋肉",         "근육",       "Muscle",                   "Muskel",            "Músculo"},
    {"motion.FlickDown.1", "恐惧",        "恐懼",        "Fear",                    "恐怖",         "공포",       "Peur",                     "Angst",             "Miedo"},
    {"motion.FlickRight.0","惊讶",        "驚訝",        "Surprise",                "驚き",         "놀람",       "Surprise",                 "Überraschung",      "Sorpresa"},
    {"motion.Flick3.0",    "爱心",        "愛心",        "Love",                    "愛",           "사랑",       "Amour",                    "Liebe",             "Amor"},
    {"motion.Flick3.1",    "哈欠",        "哈欠",        "Yawn",                    "あくび",       "하품",       "Bâillement",               "Gähnen",            "Bostezo"},
    {"motion.FlickLeft.0", "yeah",        "yeah",        "Yeah",                    "イェイ",       "예",         "Yeah",                     "Yeah",              "Yeah"},
    {"motion.FlickLeft.1", "走路",        "走路",        "Walk",                    "歩く",         "걷기",       "Marcher",                  "Gehen",             "Caminar"},
    {"motion.Shake.0",     "踉跄",        "踉蹌",        "Stagger",                 "よろめき",     "비틀거림",   "Tituber",                  "Taumeln",           "Tambalearse"},
    {"motion.Shake.1",     "摇头",        "搖頭",        "Shake Head",              "首を振る",     "고개 젓기",  "Secouer la tête",          "Kopfschütteln",     "Negar con la cabeza"},
    // ---- 语音互动（设备端）----
    {"voice.hint",         "请说指令…",   "請說指令…",   "Say a command…",          "指示をどうぞ…", "명령을 말하세요…", "Dites une commande…", "Sagen Sie einen Befehl…", "Diga un comando…"},
    {"voice.wake",         "说【在吗扣扣】唤醒", "說【在嗎扣扣】喚醒", "Say 【zai ma kou kou】 to wake", "【ザイマコウコウ】と話しかけて", "【짜이마커우커우】라고 말하세요", "Dites 【zai ma kou kou】 pour réveiller", "Sagen Sie 【zai ma kou kou】 zum Aufwecken", "Di 【zai ma kou kou】 para despertar"},
    {"voice.playing",      "正在执行…",   "正在執行…",   "Executing…",              "実行中…",      "실행 중…",   "Exécution…",               "Ausführung…",       "Ejecutando…"},
    {"voice.unsupported",  "当前角色不支持语音指令", "當前角色不支持語音指令", "This character doesn't support voice commands", "このキャラクターは音声コマンド非対応", "현재 캐릭터는 음성 명령을 지원하지 않습니다", "Ce personnage ne prend pas en charge les commandes vocales", "Diese Figur unterstützt keine Sprachbefehle", "El personaje actual no admite comandos de voz"},
};
// clang-format on

constexpr int kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

// 语言在 Entry 中的列索引（0=zhCN .. 7=es）
int langIndex(const std::string& code) {
    static const std::pair<const char*, int> kOrder[] = {
        {"zh-CN", 0}, {"zh-TW", 1}, {"en", 2}, {"ja", 3},
        {"ko", 4},    {"fr", 5},    {"de", 6}, {"es", 7},
    };
    for (const auto& [c, idx] : kOrder) {
        if (code == c) return idx;
    }
    return 0;  // 未知语言回退 zh-CN
}

const char* pick(const Entry& e, int idx) {
    switch (idx) {
        case 0: return e.zhCN;
        case 1: return e.zhTW;
        case 2: return e.en;
        case 3: return e.ja;
        case 4: return e.ko;
        case 5: return e.fr;
        case 6: return e.de;
        case 7: return e.es;
    }
    return e.zhCN;
}

std::string g_lang = "zh-CN";

} // namespace

const std::vector<std::pair<std::string, std::string>>& I18n::languages() {
    static const std::vector<std::pair<std::string, std::string>> kLangs = {
        {"zh-CN", "简体中文"}, {"zh-TW", "繁體中文"}, {"en", "English"},
        {"ja", "日本語"},      {"ko", "한국어"},      {"fr", "Français"},
        {"de", "Deutsch"},     {"es", "Español"},
    };
    return kLangs;
}

void I18n::forEach(const std::function<void(const char*)>& fn) {
    for (int i = 0; i < kEntryCount; i++) {
        const Entry& e = kEntries[i];
        fn(e.zhCN); fn(e.zhTW); fn(e.en); fn(e.ja);
        fn(e.ko);   fn(e.fr);   fn(e.de); fn(e.es);
    }
    for (const auto& [code, name] : languages()) {
        (void)code;
        fn(name.c_str());
    }
}

void I18n::setLang(const std::string& code) {
    for (const auto& [c, name] : languages()) {
        (void)name;
        if (c == code) {
            g_lang = code;
            return;
        }
    }
    g_lang = "zh-CN";
}

const std::string& I18n::lang() { return g_lang; }

const char* I18n::t(const char* key) {
    const int idx = langIndex(g_lang);
    for (int i = 0; i < kEntryCount; i++) {
        const Entry& e = kEntries[i];
        if (strcmp(e.key, key) == 0) {
            const char* val = pick(e, idx);
            if (val && val[0]) return val;
            return e.zhCN;  // 该语言缺失时回退简体中文
        }
    }
    return key;
}

const char* I18n::motionName(const std::string& group, int index) {
    static std::string buf;
    char key[96];
    snprintf(key, sizeof(key), "motion.%s.%d", group.c_str(), index);
    const char* tr = t(key);
    if (tr != key) return tr;  // 命中翻译表
    buf = group + " " + std::to_string(index + 1);
    return buf.c_str();
}

} // namespace dutyon
