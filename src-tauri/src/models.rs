//! Live2D model catalog — build-time generated list of available models.
//! Replaces `scanModels()` in `src/main/index.js` (which scanned the disk at
//! runtime). The catalog is compiled in, so it works in the packaged binary
//! where frontend assets are embedded, not on disk.

use crate::user_config;
use serde::Serialize;

/// A model entry delivered to the renderer.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModelEntry {
    pub name: String,
    pub url: String,
}

/// Return all available models (catalog) and the persisted current choice.
pub fn get_models() -> (Vec<ModelEntry>, Option<String>) {
    let models: Vec<ModelEntry> = MODELS
        .iter()
        .map(|(name, url)| ModelEntry {
            name: name.to_string(),
            url: url.to_string(),
        })
        .collect();
    let current = user_config::load().model_url;
    (models, current)
}

include!(concat!(env!("OUT_DIR"), "/models.gen.rs"));
