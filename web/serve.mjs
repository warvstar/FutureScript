#!/usr/bin/env node
// Serve fut in the browser: node web/serve.mjs [port]  then open http://localhost:8000/?file=examples/paddle.fut
// Files are served from the repository root (fut reads them with synchronous requests); /events streams the paths of
// changed files (server-sent events) so the page hot-reloads them (fut files, fut.pkg manifests, js and wasm modules).
import { createServer } from "node:http";
import { readFile, stat } from "node:fs/promises";
import { watch } from "node:fs";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("..", import.meta.url)), port = +(process.argv[2] ?? 8000);
const types = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm" };
const clients = new Set();
// Cross-origin isolated: the page may share memory (SharedArrayBuffer), so it runs fut-mt.wasm, a worker each thread
const isolated = { "cross-origin-opener-policy": "same-origin", "cross-origin-embedder-policy": "require-corp" };
const settling = new Map();  // path -> timer: an editor's save can be several writes (truncate, then write)
watch(root, { recursive: true }, (_, f) => {
  if (!f || !/(\.fut|\.js|\.wasm|\/fut\.pkg|^fut\.pkg)$/.test(f)) return;  // a program's files, its packages' manifests and modules
  clearTimeout(settling.get(f));
  settling.set(f, setTimeout(() => { settling.delete(f); for (const c of clients) c.write(`data: /${f}\n\n`); }, 60));  // once it settles
});
createServer(async (req, res) => {
  const url = new URL(req.url, "http://x");
  if (url.pathname == "/events") {
    res.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-store" }), res.write(": hi\n\n");
    return clients.add(res), req.on("close", () => clients.delete(res));
  }
  const p = url.pathname == "/" ? "/web/index.html" : decodeURIComponent(url.pathname);
  const file = join(root, normalize(p));
  if (!file.startsWith(root)) return res.writeHead(403).end();
  // The browser may keep a file but asks each time whether it changed (an edit shows at once): a copy it has is
  // answered 304, so a page loaded again doesn't download fut.wasm again (on a slow link, most of its start)
  try {
    const st = await stat(file), tag = `"${st.size}-${st.mtimeMs}"`;
    const head = { "content-type": types[extname(file)] ?? "text/plain; charset=utf-8", "cache-control": "no-cache", etag: tag, ...isolated };
    if (req.headers["if-none-match"] == tag) return res.writeHead(304, head).end();
    res.writeHead(200, head).end(await readFile(file));
  } catch { res.writeHead(404).end(); }
}).listen(port, "127.0.0.1", () => console.log(`fut: http://localhost:${port}/?file=examples/paddle.fut`));
