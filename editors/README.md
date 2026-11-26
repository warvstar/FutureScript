# fut in Zed

The extension adds highlighting, matching brackets, indentation, an outline, and Vim text objects for `.fut` files. It starts the language server with `fut lsp` using the `fut` on the project's PATH.

## Install the dev extension

1. In `editors/zed/extension.toml`, set `repository` under `[grammars.fut]` to the absolute `file://` URL of this checkout. From the repository root, `python3 -c 'from pathlib import Path; print(Path.cwd().as_uri())'` prints the URL. Set `rev` to a commit containing the generated parser, such as the output of `git rev-parse HEAD` after checking out this change. Keep `path = "editors/tree-sitter-fut"`.
2. In Zed's command palette, run **zed: install dev extension** and select `editors/zed`. Open a `.fut` file to use the extension. Zed uses Rust and the WASI SDK to build the extension and grammar. See the [Zed dev extension guide](https://zed.dev/docs/extensions/developing-extensions) for toolchain setup.
3. Build a revision that includes `fut lsp` with `python3 build.py` from the repository root. Put its `build/` directory on the project's shell PATH, or copy the binary to a directory already on that PATH. Zed uses the first result from `worktree.which("fut")`.

Zed fetches the grammar from the configured Git revision, including for a local dev extension. To try a grammar edit in Zed, regenerate and commit the parser, update `rev`, then rebuild the dev extension from Zed's Extensions page.

## Regenerate and check the grammar

From the repository root:

```sh
cd editors/tree-sitter-fut
npm ci
npm run generate
npm test
```

`npm test` runs the grammar's syntax-tree tests and `./check.sh`. The script checks the repository's `.fut` files with `tree-sitter parse --quiet`. It reports the expected parse failures for the deliberately malformed fixtures `tests/err_brace.fut` and `tests/err_char.fut` separately.

The queries are in `editors/zed/languages/fut/`. To check the highlights query from the grammar directory:

```sh
npx tree-sitter query --quiet ../zed/languages/fut/highlights.scm ../../examples/views.fut
```

To build the Rust extension directly from the repository root with the `wasm32-wasip2` target installed:

```sh
cargo build --locked --manifest-path editors/zed/Cargo.toml --target wasm32-wasip2
```
