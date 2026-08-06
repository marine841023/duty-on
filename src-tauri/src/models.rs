//! Live2D model catalog — built-in models are compiled in (see build.rs);
//! user-uploaded models are scanned at runtime from `~/.dutyon/live2d`.
//! Built-ins use webview-relative URLs (embedded frontend assets); user
//! models return absolute filesystem paths which the renderer bridge
//! converts via `convertFileSrc` (Tauri asset protocol, scoped to that dir).

use crate::user_config;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// IDE type identifier for Trae, Qoder, Cursor, Codex and OpenCode.
///
/// Serialized as lowercase `"trae"` / `"qoder"` / `"cursor"` / `"codex"` /
/// `"opencode"` (see `#[serde(rename_all)]`) so the frontend contract stays
/// unchanged when swapping from plain strings.
#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum IdeKind {
    Trae,
    Qoder,
    Cursor,
    Codex,
    OpenCode,
}

impl IdeKind {
    pub fn as_str(&self) -> &'static str {
        match self {
            IdeKind::Trae => "trae",
            IdeKind::Qoder => "qoder",
            IdeKind::Cursor => "cursor",
            IdeKind::Codex => "codex",
            IdeKind::OpenCode => "opencode",
        }
    }
}

impl std::fmt::Display for IdeKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.as_str())
    }
}

/// A model entry delivered to the renderer.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModelEntry {
    pub name: String,
    pub url: String,
    /// True for models found in the user upload dir: `url` is then an
    /// absolute filesystem path (the bridge applies `convertFileSrc`).
    pub user_uploaded: bool,
}

/// Directory where users drop their own Live2D models. Created on demand
/// by the "upload Live2D" menu command.
pub fn user_models_dir() -> PathBuf {
    dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join(".dutyon")
        .join("live2d")
}

/// Directory where users drop optional per-state sound clips for the external
/// display. Created on demand by the "open sounds folder" menu command. Files
/// are named `{state}.{mp3,wav,ogg}` where state is one of
/// idle/working/alert/thinking/tool-use/confirmation-needed.
pub fn user_sounds_dir() -> PathBuf {
    dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join(".dutyon")
        .join("sounds")
}

/// Return all available models (built-in catalog + user uploads) and the
/// persisted current choice.
pub fn get_models() -> (Vec<ModelEntry>, Option<String>) {
    let mut models: Vec<ModelEntry> = MODELS
        .iter()
        .map(|(name, url)| ModelEntry {
            name: name.to_string(),
            url: url.to_string(),
            user_uploaded: false,
        })
        .collect();
    models.extend(scan_user_models());
    let current = user_config::load().model_url;
    (models, current)
}

/// Scan the user upload dir for `*.model3.json` at its root or up to two
/// levels of subfolders down — model archives often nest one extra level
/// (e.g. `Name/runtime/xxx.model3.json`). Missing dir → empty (not an error).
fn scan_user_models() -> Vec<ModelEntry> {
    let mut candidates: Vec<PathBuf> = Vec::new();
    collect_model_files(&user_models_dir(), 2, &mut candidates);
    candidates.sort();
    candidates
        .into_iter()
        .filter_map(|p| {
            let name = p
                .file_name()
                .and_then(|n| n.to_str())
                .map(|n| n.trim_end_matches(".model3.json").to_string())?;
            Some(ModelEntry {
                name,
                url: p.to_string_lossy().to_string(),
                user_uploaded: true,
            })
        })
        .collect()
}

/// Collect `*.model3.json` directly in `dir`, recursing into subdirectories
/// at most `depth` levels deep.
fn collect_model_files(dir: &Path, depth: u8, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let p = entry.path();
        if is_model_file(&p) {
            out.push(p);
        } else if p.is_dir() && depth > 0 {
            collect_model_files(&p, depth - 1, out);
        }
    }
}

fn is_model_file(p: &Path) -> bool {
    p.is_file()
        && p.file_name()
            .and_then(|n| n.to_str())
            .map(|n| n.ends_with(".model3.json"))
            .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    /// Write an empty file, creating parent dirs.
    fn touch(p: &Path) {
        fs::create_dir_all(p.parent().unwrap()).unwrap();
        fs::write(p, "{}").unwrap();
    }

    #[test]
    fn collect_model_files_recurses_two_levels_only() {
        let root = std::env::temp_dir().join(format!("dutyon-models-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        touch(&root.join("root.model3.json"));
        touch(&root.join("sub/one.model3.json"));
        touch(&root.join("sub/runtime/two.model3.json")); // archive-style nesting
        touch(&root.join("a/b/c/three.model3.json")); // 3 levels deep: excluded
        touch(&root.join("sub/not-a-model.json"));

        let mut found = Vec::new();
        collect_model_files(&root, 2, &mut found);
        let mut names: Vec<String> = found
            .iter()
            .map(|p| p.file_name().unwrap().to_string_lossy().to_string())
            .collect();
        names.sort();
        let _ = fs::remove_dir_all(&root);

        assert_eq!(
            names,
            vec!["one.model3.json", "root.model3.json", "two.model3.json"]
        );
    }
}

/// Multilingual README written into the user model dir on first open (see
/// `open_live2d_folder` in commands.rs). Keep the quoted menu labels in
/// sync with frontend/i18n.js (menu.switchModel / menu.uploadLive2D).
pub const USER_MODELS_README: &str = r##"This folder is where you add your own Live2D models for DutyOn (开工啦).
本文件夹用于给「开工啦 DutyOn」添加你自己的 Live2D 模型。

============================================================
简体中文
============================================================
如何添加新的 Live2D 模型：
1. 准备 Cubism 4 格式的模型文件夹，其中必须包含入口文件
   xxx.model3.json，以及它引用的 .moc3、贴图、动作等全部资源。
2. 将整个模型文件夹复制到本目录（xxx.model3.json 位于本目录根部
   或两层子文件夹内均可被识别，如 模型名/runtime/ 结构）。
3. 确认 model3.json 内的资源引用均为相对路径且文件齐全。
4. 重新打开桌宠菜单「切换形象」，新模型即出现在列表中，无需重启。
5. 请确认你拥有该模型的合法使用授权。
说明：本 README 可删除；再次点击菜单「上传 Live2D 模型」会重新生成。

============================================================
繁體中文
============================================================
如何新增 Live2D 模型：
1. 準備 Cubism 4 格式的模型資料夾，必須包含入口檔案
   xxx.model3.json 及其引用的 .moc3、貼圖、動作等所有資源。
2. 將整個模型資料夾複製到本目錄（xxx.model3.json 位於本目錄根部
   或兩層子資料夾內均可被識別）。
3. 確認 model3.json 內的資源引用均為相對路徑且檔案齊全。
4. 重新開啟桌寵選單「切換形象」，新模型即出現在清單中，無需重啟。
5. 請確認你擁有該模型的合法使用授權。
說明：本 README 可刪除；再次點擊選單「上傳 Live2D 模型」會重新生成。

============================================================
English
============================================================
How to add a new Live2D model:
1. Prepare a Cubism 4 model folder containing the entry file
   xxx.model3.json plus every resource it references (.moc3,
   textures, motions, etc.).
2. Copy the whole folder into this directory (xxx.model3.json is
   detected at the root or up to two subfolders deep, e.g. a
   Name/runtime/ archive layout).
3. Make sure all references inside model3.json are relative paths
   and all files are present.
4. Reopen the pet menu "Switch Model" — the new model shows up in
   the list, no restart needed.
5. Make sure you have the legal right to use the model.
Note: this README can be deleted; clicking "Upload Live2D Model"
in the menu regenerates it.

============================================================
日本語
============================================================
新しい Live2D モデルの追加方法：
1. Cubism 4 形式のモデルフォルダを用意します。入口ファイル
   xxx.model3.json と、参照される .moc3・テクスチャ・モーション等
   すべてのリソースが必要です。
2. フォルダごとこのディレクトリにコピーします（xxx.model3.json は
   直下でも 2 階層までのサブフォルダ内でも認識されます）。
3. model3.json 内の参照が相対パスで、ファイルが揃っていることを
   確認してください。
4. ペットのメニュー「モデル切替」を開き直すと、新しいモデルが
   リストに表示されます。再起動は不要です。
5. モデルの正当な使用権利を確認してください。
補足：この README は削除して構いません。メニュー「Live2D モデルを
追加」を再度クリックすると再生成されます。

============================================================
한국어
============================================================
새 Live2D 모델 추가 방법:
1. Cubism 4 형식의 모델 디렉터리를 준비합니다. 엔트리 파일
   xxx.model3.json과 참조되는 .moc3, 텍스처, 모션 등 모든 리소스가
   필요합니다.
2. 모델 디렉터리 전체를 이 디렉터리에 복사합니다(xxx.model3.json은
   루트 또는 두 단계까지 하위 디렉터리에서 인식됩니다).
3. model3.json 안의 참조가 상대 경로이고 파일이 모두 있는지
   확인하세요.
4. 펫 메뉴「모델 변경」을 다시 열어 보세요. 새 모델이 목록에
   표시됩니다. 재시작은 필요 없습니다.
5. 모델의 정당한 사용 권한을 확인하세요.
참고: 이 README는 삭제 가능합니다. 메뉴「Live2D 모델 추가」를 다시
클릭하면 다시 생성됩니다.

============================================================
Français
============================================================
Comment ajouter un nouveau modèle Live2D :
1. Préparez un dossier de modèle au format Cubism 4 contenant le
   fichier d'entrée xxx.model3.json ainsi que toutes les ressources
   référencées (.moc3, textures, mouvements, etc.).
2. Copiez le dossier complet dans ce répertoire (xxx.model3.json est
   détecté à la racine ou jusqu'à deux niveaux de sous-dossiers).
3. Vérifiez que les références dans model3.json sont des chemins
   relatifs et que tous les fichiers sont présents.
4. Rouvrez le menu « Changer de modèle » : le nouveau modèle apparaît
   dans la liste, sans redémarrage.
5. Assurez-vous de détenir les droits d'utilisation du modèle.
Remarque : ce README peut être supprimé ; cliquez à nouveau sur
« Ajouter un modèle Live2D » dans le menu pour le régénérer.

============================================================
Deutsch
============================================================
So fügst du ein neues Live2D-Modell hinzu:
1. Bereite einen Modellordner im Cubism-4-Format vor, der die
   Einstiegsdatei xxx.model3.json sowie alle referenzierten
   Ressourcen (.moc3, Texturen, Bewegungen usw.) enthält.
2. Kopiere den gesamten Ordner in dieses Verzeichnis
   (xxx.model3.json wird im Wurzelverzeichnis oder bis zu zwei
   Ordnerebenen darunter erkannt).
3. Stelle sicher, dass alle Verweise in model3.json relative Pfade
   sind und alle Dateien vorhanden sind.
4. Öffne das Menü „Modell wechseln“ erneut — das neue Modell
   erscheint in der Liste, kein Neustart nötig.
5. Stelle sicher, dass du die Nutzungsrechte am Modell besitzt.
Hinweis: Diese README kann gelöscht werden; ein erneuter Klick auf
„Live2D-Modell hinzufügen“ im Menü erstellt sie wieder.

============================================================
Español
============================================================
Cómo añadir un nuevo modelo Live2D:
1. Prepara una carpeta de modelo en formato Cubism 4 que contenga
   el archivo de entrada xxx.model3.json y todos los recursos
   referenciados (.moc3, texturas, movimientos, etc.).
2. Copia toda la carpeta en este directorio (xxx.model3.json se
   detecta en la raíz o hasta dos niveles de subdirectorios).
3. Asegúrate de que las referencias dentro de model3.json sean
   rutas relativas y de que todos los archivos estén presentes.
4. Vuelve a abrir el menú «Cambiar modelo»: el nuevo modelo
   aparece en la lista, sin reiniciar.
5. Asegúrate de tener derechos legales para usar el modelo.
Nota: este README se puede borrar; al hacer clic de nuevo en
«Añadir modelo Live2D» en el menú se regenera.
"##;

/// Multilingual README written into the user sounds dir on first open (see
/// `open_sounds_folder` in commands.rs). Sound files are optional per state —
/// the external display simply stays silent when no file exists for a state.
pub const SOUNDS_README: &str = r##"This folder holds optional sound clips for DutyOn (开工啦) external display.
本文件夹用于存放「开工啦 DutyOn」外接显示屏幕的可选声音文件。

============================================================
简体中文
============================================================
为每个宠物状态放置一个声音文件，外接屏幕在进入该状态时自动播放。
文件命名规则：{状态名}.{mp3|wav|ogg}
支持的状态名：
  idle                  空闲
  thinking              思考中
  tool-use              执行中（工具调用）
  working               忙碌中（旧状态，兼容）
  alert                 需要确认
  confirmation-needed   需要确认（同 alert）
示例：alert.mp3、thinking.wav、tool-use.ogg
说明：
- 声音是可选的，没有文件的状态保持静音。
- 每个状态只需一个文件，优先级 mp3 > wav > ogg。
- 本 README 可删除；再次点击菜单「打开声音文件夹」会重新生成。

============================================================
English
============================================================
Drop one sound file per pet state; the external display plays it when the
state is entered. Naming: {state}.{mp3|wav|ogg}
Supported states:
  idle                  Idle
  thinking              Thinking
  tool-use              Using tool
  working               Working (legacy, kept for compatibility)
  alert                 Confirmation needed
  confirmation-needed   Same as alert
Example: alert.mp3, thinking.wav, tool-use.ogg
Notes:
- Sounds are optional — states without a file stay silent.
- Only one file per state is needed; priority mp3 > wav > ogg.
- This README can be deleted; reopening the sounds folder regenerates it.

============================================================
日本語
============================================================
ペットの状態ごとに音声ファイルを置くと、外接画面がその状態に入った
時に自動再生します。ファイル名：{状態名}.{mp3|wav|ogg}
状態名：idle / thinking / tool-use / working / alert /
confirmation-needed
例：alert.mp3、thinking.wav、tool-use.ogg
音声は任意です。ファイルがない状態は無音のままです。

============================================================
한국어
============================================================
펫 상태별로 소리 파일을 넣으면 외부 화면이 해당 상태 진입 시 자동
재생합니다. 파일명: {상태명}.{mp3|wav|ogg}
상태명: idle / thinking / tool-use / working / alert /
confirmation-needed
예: alert.mp3, thinking.wav, tool-use.ogg
소리는 선택 사항이며 파일이 없는 상태는 조용히 유지됩니다.
"##;

include!(concat!(env!("OUT_DIR"), "/models.gen.rs"));
