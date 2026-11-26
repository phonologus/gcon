//! Zed extension for Icon as gcon reads it: starts gcon's language server,
//! `gcon lsp`.
//!
//! gcon is found on the PATH of the project's shell environment, unless
//! Zed's settings give its path:
//!
//! ```json
//! "lsp": {
//!   "gcon": {
//!     "binary": { "path": "/path/to/gcon/bin/gcon" },
//!     "initialization_options": { "icon95": true, "ipath": ["/path/to/lib"] }
//!   }
//! }
//! ```
//!
//! `binary.arguments`, if given, replace the default `["lsp"]`.

use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, Result};

const SERVER: &str = "gcon";

struct GconExtension;

impl zed::Extension for GconExtension {
    fn new() -> Self {
        GconExtension
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let binary = LspSettings::for_worktree(SERVER, worktree)
            .ok()
            .and_then(|s| s.binary);
        let path = binary
            .as_ref()
            .and_then(|b| b.path.clone())
            .or_else(|| worktree.which("gcon"))
            .ok_or_else(|| {
                "gcon not found on PATH: build it (https://github.com/phonologus/gcon, \
                 ./build.sh) and put its bin directory on your PATH, \
                 or set lsp.gcon.binary.path in Zed's settings"
                    .to_string()
            })?;
        let args = binary
            .as_ref()
            .and_then(|b| b.arguments.clone())
            .unwrap_or_else(|| vec!["lsp".to_string()]);
        let mut env = worktree.shell_env();
        if let Some(extra) = binary.and_then(|b| b.env) {
            env.extend(extra);
        }
        Ok(zed::Command {
            command: path,
            args,
            env,
        })
    }

    fn language_server_initialization_options(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<Option<zed::serde_json::Value>> {
        Ok(LspSettings::for_worktree(SERVER, worktree)
            .ok()
            .and_then(|s| s.initialization_options))
    }
}

zed::register_extension!(GconExtension);
