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
    {"menu.actionSettings","动作设定",    "動作設定",    "Motion Settings",         "モーション設定","모션 설정", "Réglages des mouvements",  "Bewegungseinstellungen", "Ajustes de movimientos"},
    {"menu.flipHorizontal","左右翻转",    "左右翻轉",    "Flip Horizontal",         "左右反転",     "좌우 반전",  "Retourner horizontalement","Horizontal spiegeln","Voltear horizontalmente"},
    {"menu.miniMode",      "迷你模式",    "迷你模式",    "Mini Mode",               "ミニモード",   "미니 모드",  "Mode mini",                "Mini-Modus",        "Modo mini"},
    {"menu.visibility",    "显示隐藏",    "顯示隱藏",    "Show / Hide",             "表示/非表示",  "표시/숨기기","Afficher / Masquer",      "Anzeigen / Ausblenden", "Mostrar / Ocultar"},
    {"menu.systemMonitor", "系统监控",    "系統監控",    "System Monitor",          "システムモニター","시스템 모니터","Moniteur système",     "Systemmonitor",     "Monitor del sistema"},
    {"menu.autoLaunch",    "开机自启动",  "開機自啟動",  "Start on Boot",           "起動時に自動開始","부팅 시 자동 시작","Démarrage auto",  "Autostart",         "Inicio automático"},
    {"menu.language",      "语言",        "語言",        "Language",                "言語",         "언어",       "Langue",                   "Sprache",           "Idioma"},
    {"menu.back",          "返回",        "返回",        "Back",                    "戻る",         "뒤로",       "Retour",                   "Zurück",            "Volver"},
    {"menu.installHooks",  "安装 IDE 集成", "安裝 IDE 整合", "Install IDE Integration","IDE統合をインストール","IDE 통합 설치","Installer l'intégration IDE","IDE-Integration installieren","Instalar integración IDE"},
    {"menu.hookStatus",    "Hook 状态",   "Hook 狀態",   "Hook Status",             "Hookステータス","Hook 상태",  "Statut des Hooks",         "Hook-Status",       "Estado de Hooks"},
    {"menu.quit",          "退出",        "退出",        "Quit",                    "終了",         "종료",       "Quitter",                  "Beenden",           "Salir"},
    {"menu.previewAlert",  "预览提醒效果","預覽提醒效果","Preview Alert",           "アラートプレビュー","알림 미리보기","Aperçu de l'alerte",    "Alarm-Vorschau",    "Vista previa de alerta"},
    {"menu.selectMotion",  "选择动作",    "選擇動作",    "Select Motion",           "モーション選択","모션 선택",  "Choisir un mouvement",    "Bewegung wählen",   "Elegir movimiento"},
    {"menu.newCharacter",  "+ 新建形象",  "+ 新建形象",  "+ New Character",         "+ 新規キャラクター","+ 새 캐릭터","+ Nouveau personnage",  "+ Neues Modell",    "+ Nuevo personaje"},
    // ---- 动作设定三状态 ----
    {"settings.sleeping",  "空闲中",      "空閒中",      "Idle",                    "待機中",       "대기 중",    "Inactif",                  "Inaktiv",           "Inactivo"},
    {"settings.working",   "忙碌中",      "忙碌中",      "Working",                 "作業中",       "작업 중",    "Occupé",                   "Arbeitet",          "Ocupado"},
    {"settings.alert",     "需要确认",    "需要確認",    "Alert",                   "確認",         "확인",       "Alerte",                   "Alarm",             "Alerta"},
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
