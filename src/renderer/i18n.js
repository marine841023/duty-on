/**
 * Internationalization (i18n) module.
 *
 * Supports: 简体中文 (zh-CN), 繁體中文 (zh-TW), English (en), 日本語 (ja), 한국어 (ko).
 * Default language is detected from the OS locale (via main process); falls back
 * to English if the detected language isn't in the supported list.
 *
 * Usage:
 *   i18n.setLanguage('ja');
 *   const text = i18n.t('state.sleeping');           // -> '待機中'
 *   const ago = i18n.t('hook.secondsAgo', { n: 5 }); // -> '5s前'
 *
 * Language names in the picker are shown in their own native script so users
 * can always recognize their language regardless of the current UI language.
 */

const TRANSLATIONS = {
  'zh-CN': {
    'state.sleeping': '空闲中',
    'state.working': '⚡ 忙碌中',
    'state.alert': '🔔 需要确认!',
    'status.initializing': '初始化中...',
    'status.waiting': '等待 Trae IDE 连接...',
    'status.busy': '忙碌',
    'status.idle': '空闲',
    'status.confirmationNeeded': '需要确认',
    'menu.switchModel': '切换形象',
    'menu.playMotion': '播放动作',
    'menu.actionSettings': '动作设定',
    'menu.flipHorizontal': '左右翻转',
    'menu.installHooks': '安装 Hook 集成',
    'menu.hookStatus': 'Hook 状态',
    'menu.quit': '退出',
    'menu.previewAlert': '预览提醒效果',
    'menu.selectMotion': '选择动作',
    'menu.language': '语言',
    'menu.back': '返回',
    'settings.sleeping': '空闲中',
    'settings.working': '忙碌中',
    'settings.alert': '需要确认',
    'hook.notInstalled': '未安装',
    'hook.connected': '已连接',
    'hook.needsEnable': '需在IDE启用',
    'hook.notChecked': '未检查',
    'hook.secondsAgo': '{n}s前',
    'hook.minutesAgo': '{n}m前',
    'hook.installSuccess': '✅ 已写入配置 -> 请设为本地自动运行',
    'hook.installFailed': '❌ Hook 安装失败',
    'hook.pleaseInstall': '⚠ 请点☰→安装 Hook 集成',
    'hook.pleaseEnable': '⚠ 请设为本地自动运行',
    'hook.dialogConfig': 'Hook 配置',
    'hook.dialogBridge': 'bridge 脚本',
    'hook.dialogHooksJson': 'hooks.json',
    'hook.installed': '已安装',
    'hook.notInstalledLabel': '未安装',
    'hook.exists': '存在',
    'hook.missing': '缺失',
    'hook.dialogInstalledMsg': '✅ Hook 已安装',
    'hook.dialogNotInstalledMsg': '⚠ Hook 未安装',
    'hook.dialogHint': '若 IDE 不显示：设置→Hooks 启用 → 开新 AI 会话',
    'model.switching': '⏳ 切换形象中...',
    'model.switchFailed': '❌ 形象加载失败，已回退',
    'motion.Idle.0': '发呆',
    'motion.Idle.1': '开心',
    'motion.Idle.2': '叹气',
    'motion.Idle.3': '睡觉',
    'motion.Tap.0': '生气',
    'motion.Tap.1': '难过',
    'motion.Tap.2': '哭泣',
    'motion.Tap.3': '喜悦',
    'motion.Tap.4': '点头',
    'motion.FlickUp.0': '再见',
    'motion.FlickUp.1': '高兴',
    'motion.FlickUp.2': '威胁',
    'motion.FlickDown.0': '肌肉',
    'motion.FlickDown.1': '恐惧',
    'motion.FlickRight.0': '惊讶',
    'motion.Flick3.0': '爱心',
    'motion.Flick3.1': '哈欠',
    'motion.FlickLeft.0': 'yeah',
    'motion.FlickLeft.1': '走路',
    'motion.Shake.0': '踉跄',
    'motion.Shake.1': '摇头',
    'menu.autoLaunch': '开机自启动',
    'menu.uninstall': '卸载',
    'menu.checkUpdates': '检查新版本',
    'update.checking': '正在检查更新...',
    'update.available': '发现新版本 v{version}，正在下载...',
    'update.notAvailable': '当前已是最新版本',
    'update.downloading': '正在下载更新... {percent}%',
    'update.downloaded': 'v{version} 已下载，点击安装',
    'update.error': '更新检查失败: {message}',
    'update.networkError': '无法连接升级服务器，请到网站下载最新安装包进行覆盖安装',
    'update.devMode': '开发模式下不可用',
  },

  'zh-TW': {
    'state.sleeping': '空閒中',
    'state.working': '⚡ 忙碌中',
    'state.alert': '🔔 需要確認!',
    'status.initializing': '初始化中...',
    'status.waiting': '等待 Trae IDE 連接...',
    'status.busy': '忙碌',
    'status.idle': '空閒',
    'status.confirmationNeeded': '需要確認',
    'menu.switchModel': '切換形象',
    'menu.playMotion': '播放動作',
    'menu.actionSettings': '動作設定',
    'menu.flipHorizontal': '左右翻轉',
    'menu.installHooks': '安裝 Hook 集成',
    'menu.hookStatus': 'Hook 狀態',
    'menu.quit': '退出',
    'menu.previewAlert': '預覽提醒效果',
    'menu.selectMotion': '選擇動作',
    'menu.language': '語言',
    'menu.back': '返回',
    'settings.sleeping': '空閒中',
    'settings.working': '忙碌中',
    'settings.alert': '需要確認',
    'hook.notInstalled': '未安裝',
    'hook.connected': '已連接',
    'hook.needsEnable': '需在IDE啟用',
    'hook.notChecked': '未檢查',
    'hook.secondsAgo': '{n}s前',
    'hook.minutesAgo': '{n}m前',
    'hook.installSuccess': '✅ 已寫入配置 -> 請設為本地自動運行',
    'hook.installFailed': '❌ Hook 安裝失敗',
    'hook.pleaseInstall': '⚠ 請點☰→安裝 Hook 集成',
    'hook.pleaseEnable': '⚠ 請設為本地自動運行',
    'hook.dialogConfig': 'Hook 配置',
    'hook.dialogBridge': 'bridge 腳本',
    'hook.dialogHooksJson': 'hooks.json',
    'hook.installed': '已安裝',
    'hook.notInstalledLabel': '未安裝',
    'hook.exists': '存在',
    'hook.missing': '缺失',
    'hook.dialogInstalledMsg': '✅ Hook 已安裝',
    'hook.dialogNotInstalledMsg': '⚠ Hook 未安裝',
    'hook.dialogHint': '若 IDE 不顯示：設定→Hooks 啟用 → 開新 AI 會話',
    'model.switching': '⏳ 切換形象中...',
    'model.switchFailed': '❌ 形象載入失敗，已回退',
    'motion.Idle.0': '發呆',
    'motion.Idle.1': '開心',
    'motion.Idle.2': '嘆氣',
    'motion.Idle.3': '睡覺',
    'motion.Tap.0': '生氣',
    'motion.Tap.1': '難過',
    'motion.Tap.2': '哭泣',
    'motion.Tap.3': '喜悅',
    'motion.Tap.4': '點頭',
    'motion.FlickUp.0': '再見',
    'motion.FlickUp.1': '高興',
    'motion.FlickUp.2': '威脅',
    'motion.FlickDown.0': '肌肉',
    'motion.FlickDown.1': '恐懼',
    'motion.FlickRight.0': '驚訝',
    'motion.Flick3.0': '愛心',
    'motion.Flick3.1': '哈欠',
    'motion.FlickLeft.0': 'yeah',
    'motion.FlickLeft.1': '走路',
    'motion.Shake.0': '踉蹌',
    'motion.Shake.1': '搖頭',
    'menu.autoLaunch': '開機自啟動',
    'menu.uninstall': '解除安裝',
    'menu.checkUpdates': '檢查新版本',
    'update.checking': '正在檢查更新...',
    'update.available': '發現新版本 v{version}，正在下載...',
    'update.notAvailable': '當前已是最新版本',
    'update.downloading': '正在下載更新... {percent}%',
    'update.downloaded': 'v{version} 已下載，點擊安裝',
    'update.error': '更新檢查失敗: {message}',
    'update.networkError': '無法連接升級伺服器，請到網站下載最新安裝包進行覆蓋安裝',
    'update.devMode': '開發模式下不可用',
  },

  'en': {
    'state.sleeping': 'Idle',
    'state.working': '⚡ Working',
    'state.alert': '🔔 Confirmation needed!',
    'status.initializing': 'Initializing...',
    'status.waiting': 'Waiting for Trae IDE...',
    'status.busy': 'Busy',
    'status.idle': 'Idle',
    'status.confirmationNeeded': 'Confirmation needed',
    'menu.switchModel': 'Switch Model',
    'menu.playMotion': 'Play Motion',
    'menu.actionSettings': 'Motion Settings',
    'menu.flipHorizontal': 'Flip Horizontal',
    'menu.installHooks': 'Install Hook Integration',
    'menu.hookStatus': 'Hook Status',
    'menu.quit': 'Quit',
    'menu.previewAlert': 'Preview Alert',
    'menu.selectMotion': 'Select Motion',
    'menu.language': 'Language',
    'menu.back': 'Back',
    'settings.sleeping': 'Idle',
    'settings.working': 'Working',
    'settings.alert': 'Alert',
    'hook.notInstalled': 'Not installed',
    'hook.connected': 'Connected',
    'hook.needsEnable': 'Enable in IDE',
    'hook.notChecked': 'Not checked',
    'hook.secondsAgo': '{n}s ago',
    'hook.minutesAgo': '{n}m ago',
    'hook.installSuccess': '✅ Config written -> set to local auto-run',
    'hook.installFailed': '❌ Hook installation failed',
    'hook.pleaseInstall': '⚠ Click ☰→Install Hook Integration',
    'hook.pleaseEnable': '⚠ Set to local auto-run',
    'hook.dialogConfig': 'Hook config',
    'hook.dialogBridge': 'Bridge script',
    'hook.dialogHooksJson': 'hooks.json',
    'hook.installed': 'Installed',
    'hook.notInstalledLabel': 'Not installed',
    'hook.exists': 'Exists',
    'hook.missing': 'Missing',
    'hook.dialogInstalledMsg': '✅ Hook installed',
    'hook.dialogNotInstalledMsg': '⚠ Hook not installed',
    'hook.dialogHint': 'If IDE doesn\'t show: Settings→Hooks enable → start new AI session',
    'model.switching': '⏳ Switching model...',
    'model.switchFailed': '❌ Model load failed, reverted',
    'motion.Idle.0': 'Idle',
    'motion.Idle.1': 'Happy',
    'motion.Idle.2': 'Sigh',
    'motion.Idle.3': 'Sleep',
    'motion.Tap.0': 'Angry',
    'motion.Tap.1': 'Sad',
    'motion.Tap.2': 'Cry',
    'motion.Tap.3': 'Joy',
    'motion.Tap.4': 'Nod',
    'motion.FlickUp.0': 'Bye',
    'motion.FlickUp.1': 'Glad',
    'motion.FlickUp.2': 'Threat',
    'motion.FlickDown.0': 'Muscle',
    'motion.FlickDown.1': 'Fear',
    'motion.FlickRight.0': 'Surprise',
    'motion.Flick3.0': 'Love',
    'motion.Flick3.1': 'Yawn',
    'motion.FlickLeft.0': 'Yeah',
    'motion.FlickLeft.1': 'Walk',
    'motion.Shake.0': 'Stagger',
    'motion.Shake.1': 'Shake Head',
    'menu.autoLaunch': 'Start on Boot',
    'menu.uninstall': 'Uninstall',
    'menu.checkUpdates': 'Check for Updates',
    'update.checking': 'Checking for updates...',
    'update.available': 'New version v{version} available, downloading...',
    'update.notAvailable': 'You are on the latest version',
    'update.downloading': 'Downloading update... {percent}%',
    'update.downloaded': 'v{version} downloaded, click to install',
    'update.error': 'Update check failed: {message}',
    'update.networkError': 'Cannot connect to the update server. Please download the latest installer from the website and install it over the current version.',
    'update.devMode': 'Not available in dev mode',
  },

  'ja': {
    'state.sleeping': '待機中',
    'state.working': '⚡ 作業中',
    'state.alert': '🔔 確認が必要!',
    'status.initializing': '初期化中...',
    'status.waiting': 'Trae IDE接続待ち...',
    'status.busy': '忙しい',
    'status.idle': '待機',
    'status.confirmationNeeded': '確認が必要',
    'menu.switchModel': 'モデル切替',
    'menu.playMotion': 'モーション再生',
    'menu.actionSettings': 'モーション設定',
    'menu.flipHorizontal': '左右反転',
    'menu.installHooks': 'Hook統合をインストール',
    'menu.hookStatus': 'Hookステータス',
    'menu.quit': '終了',
    'menu.previewAlert': 'アラートプレビュー',
    'menu.selectMotion': 'モーション選択',
    'menu.language': '言語',
    'menu.back': '戻る',
    'settings.sleeping': '待機中',
    'settings.working': '作業中',
    'settings.alert': '確認',
    'hook.notInstalled': '未インストール',
    'hook.connected': '接続済み',
    'hook.needsEnable': 'IDEで有効化',
    'hook.notChecked': '未確認',
    'hook.secondsAgo': '{n}s前',
    'hook.minutesAgo': '{n}m前',
    'hook.installSuccess': '✅ 設定を書き込みました -> ローカル自動実行に設定してください',
    'hook.installFailed': '❌ Hookインストール失敗',
    'hook.pleaseInstall': '⚠ ☰→Hook統合をインストール',
    'hook.pleaseEnable': '⚠ ローカル自動実行に設定してください',
    'hook.dialogConfig': 'Hook設定',
    'hook.dialogBridge': 'ブリッジスクリプト',
    'hook.dialogHooksJson': 'hooks.json',
    'hook.installed': 'インストール済み',
    'hook.notInstalledLabel': '未インストール',
    'hook.exists': '存在',
    'hook.missing': '不在',
    'hook.dialogInstalledMsg': '✅ Hookインストール済み',
    'hook.dialogNotInstalledMsg': '⚠ Hook未インストール',
    'hook.dialogHint': 'IDEに表示されない場合：設定→Hooks有効化 → 新しいAIセッション開始',
    'model.switching': '⏳ モデル切替中...',
    'model.switchFailed': '❌ モデル読み込み失敗、復帰しました',
    'motion.Idle.0': 'ぼんやり',
    'motion.Idle.1': '嬉しい',
    'motion.Idle.2': 'ため息',
    'motion.Idle.3': '寝る',
    'motion.Tap.0': '怒る',
    'motion.Tap.1': '悲しい',
    'motion.Tap.2': '泣く',
    'motion.Tap.3': '喜び',
    'motion.Tap.4': 'うなずく',
    'motion.FlickUp.0': 'さようなら',
    'motion.FlickUp.1': '喜ぶ',
    'motion.FlickUp.2': '脅す',
    'motion.FlickDown.0': '筋肉',
    'motion.FlickDown.1': '恐怖',
    'motion.FlickRight.0': '驚き',
    'motion.Flick3.0': '愛',
    'motion.Flick3.1': 'あくび',
    'motion.FlickLeft.0': 'イェイ',
    'motion.FlickLeft.1': '歩く',
    'motion.Shake.0': 'よろめき',
    'motion.Shake.1': '首を振る',
    'menu.autoLaunch': '起動時に自動開始',
    'menu.uninstall': 'アンインストール',
    'menu.checkUpdates': 'アップデート確認',
    'update.checking': 'アップデートを確認中...',
    'update.available': '新しいバージョン v{version} があります。ダウンロード中...',
    'update.notAvailable': '最新バージョンです',
    'update.downloading': 'アップデートをダウンロード中... {percent}%',
    'update.downloaded': 'v{version} ダウンロード完了、クリックしてインストール',
    'update.error': 'アップデート確認失敗: {message}',
    'update.networkError': 'アップデートサーバーに接続できません。Webサイトから最新のインストーラーをダウンロードして上書きインストールしてください。',
    'update.devMode': '開発モードでは利用できません',
  },

  'ko': {
    'state.sleeping': '대기 중',
    'state.working': '⚡ 작업 중',
    'state.alert': '🔔 확인 필요!',
    'status.initializing': '초기화 중...',
    'status.waiting': 'Trae IDE 연결 대기 중...',
    'status.busy': '작업 중',
    'status.idle': '대기',
    'status.confirmationNeeded': '확인 필요',
    'menu.switchModel': '모델 변경',
    'menu.playMotion': '모션 재생',
    'menu.actionSettings': '모션 설정',
    'menu.flipHorizontal': '좌우 반전',
    'menu.installHooks': 'Hook 통합 설치',
    'menu.hookStatus': 'Hook 상태',
    'menu.quit': '종료',
    'menu.previewAlert': '알림 미리보기',
    'menu.selectMotion': '모션 선택',
    'menu.language': '언어',
    'menu.back': '뒤로',
    'settings.sleeping': '대기 중',
    'settings.working': '작업 중',
    'settings.alert': '확인',
    'hook.notInstalled': '미설치',
    'hook.connected': '연결됨',
    'hook.needsEnable': 'IDE에서 활성화',
    'hook.notChecked': '미확인',
    'hook.secondsAgo': '{n}s전',
    'hook.minutesAgo': '{n}m전',
    'hook.installSuccess': '✅ 설정 저장됨 -> 로컬 자동 실행으로 설정하세요',
    'hook.installFailed': '❌ Hook 설치 실패',
    'hook.pleaseInstall': '⚠ ☰→Hook 통합 설치 클릭',
    'hook.pleaseEnable': '⚠ 로컬 자동 실행으로 설정하세요',
    'hook.dialogConfig': 'Hook 설정',
    'hook.dialogBridge': '브리지 스크립트',
    'hook.dialogHooksJson': 'hooks.json',
    'hook.installed': '설치됨',
    'hook.notInstalledLabel': '미설치',
    'hook.exists': '존재',
    'hook.missing': '없음',
    'hook.dialogInstalledMsg': '✅ Hook 설치됨',
    'hook.dialogNotInstalledMsg': '⚠ Hook 미설치',
    'hook.dialogHint': 'IDE에 표시 안 됨: 설정→Hooks 활성화 → 새 AI 세션 시작',
    'model.switching': '⏳ 모델 변경 중...',
    'model.switchFailed': '❌ 모델 로드 실패, 복구됨',
    'motion.Idle.0': '멍때림',
    'motion.Idle.1': '행복',
    'motion.Idle.2': '한숨',
    'motion.Idle.3': '잠',
    'motion.Tap.0': '화남',
    'motion.Tap.1': '슬픔',
    'motion.Tap.2': '울음',
    'motion.Tap.3': '기쁨',
    'motion.Tap.4': '고개 끄덕임',
    'motion.FlickUp.0': '잘 가',
    'motion.FlickUp.1': '즐거움',
    'motion.FlickUp.2': '위협',
    'motion.FlickDown.0': '근육',
    'motion.FlickDown.1': '공포',
    'motion.FlickRight.0': '놀람',
    'motion.Flick3.0': '사랑',
    'motion.Flick3.1': '하품',
    'motion.FlickLeft.0': '예',
    'motion.FlickLeft.1': '걷기',
    'motion.Shake.0': '비틀거림',
    'motion.Shake.1': '고개 젓기',
    'menu.autoLaunch': '부팅 시 자동 시작',
    'menu.uninstall': '제거',
    'menu.checkUpdates': '업데이트 확인',
    'update.checking': '업데이트 확인 중...',
    'update.available': '새 버전 v{version} 발견, 다운로드 중...',
    'update.notAvailable': '최신 버전입니다',
    'update.downloading': '업데이트 다운로드 중... {percent}%',
    'update.downloaded': 'v{version} 다운로드 완료, 클릭하여 설치',
    'update.error': '업데이트 확인 실패: {message}',
    'update.networkError': '업데이트 서버에 연결할 수 없습니다. 웹사이트에서 최신 설치 프로그램을 다운로드하여 덮어쓰기 설치하세요.',
    'update.devMode': '개발 모드에서 사용 불가',
  },
};

/**
 * Supported languages for the picker. `name` is the native endonym so users
 * always recognize their language regardless of the current UI language.
 */
const SUPPORTED_LANGUAGES = [
  { code: 'zh-CN', name: '简体中文' },
  { code: 'zh-TW', name: '繁體中文' },
  { code: 'en',    name: 'English' },
  { code: 'ja',    name: '日本語' },
  { code: 'ko',    name: '한국어' },
];

/**
 * Map an Electron/Chromium locale string to a supported language code.
 * Electron locales look like 'zh-CN', 'zh-TW', 'en-US', 'ja-JP', 'ko-KR'.
 * We normalize to our 5 supported codes, falling back to 'en'.
 */
function normalizeLocale(locale) {
  if (!locale || typeof locale !== 'string') return 'en';
  const lower = locale.toLowerCase();
  // Exact match first (e.g. 'zh-cn', 'zh-tw').
  for (const lang of SUPPORTED_LANGUAGES) {
    if (lower === lang.code.toLowerCase()) return lang.code;
  }
  // Prefix match on language part (e.g. 'en-us' -> 'en', 'ja-jp' -> 'ja').
  const prefix = lower.split('-')[0];
  if (prefix === 'zh') {
    // zh-TW / zh-HK / zh-MO -> Traditional; everything else -> Simplified.
    return (lower.includes('tw') || lower.includes('hk') || lower.includes('mo')) ? 'zh-TW' : 'zh-CN';
  }
  for (const lang of SUPPORTED_LANGUAGES) {
    if (prefix === lang.code.toLowerCase()) return lang.code;
  }
  return 'en';
}

const i18n = {
  currentLanguage: 'en',

  /**
   * Get the list of supported languages for the picker.
   */
  getLanguages() {
    return SUPPORTED_LANGUAGES;
  },

  /**
   * Set the current UI language. No-op if the language isn't supported.
   */
  setLanguage(lang) {
    if (TRANSLATIONS[lang]) {
      this.currentLanguage = lang;
    }
  },

  /**
   * Normalize an arbitrary locale string to a supported language code.
   */
  normalizeLocale,

  /**
   * Translate a key, with optional {placeholder} substitution.
   * Falls back to English, then to the key itself if missing.
   */
  t(key, params) {
    const table = TRANSLATIONS[this.currentLanguage] || TRANSLATIONS['en'];
    let str = table[key];
    if (str === undefined) {
      str = TRANSLATIONS['en'][key];
    }
    if (str === undefined) {
      return key;
    }
    if (params) {
      for (const [k, v] of Object.entries(params)) {
        str = str.replace(new RegExp(`\\{${k}\\}`, 'g'), String(v));
      }
    }
    return str;
  },
};

window.i18n = i18n;
