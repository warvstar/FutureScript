use zed_extension_api as zed;

struct FutExtension;

impl zed::Extension for FutExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        let command = worktree
            .which("fut")
            .ok_or_else(|| "fut is required on the project's PATH to run fut lsp".to_string())?;

        Ok(zed::Command {
            command,
            args: vec!["lsp".into()],
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(FutExtension);
