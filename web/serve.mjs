#!/usr/bin/env node
// Serve fut in the browser: node web/serve.mjs [port]  then open http://localhost:8000/?file=examples/paddle.fut
// Files are served from the repository root (fut reads them with synchronous requests); /events streams the paths of
// changed files (server-sent events) so the page hot-reloads them.
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { watch } from "node:fs";
import { extname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";

const root = fileURLToPath(new URL("..", import.meta.url)), port = +(process.argv[2] ?? 8000);
const types = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm" };
const clients = new Set();
watch(root, { recursive: true }, (_, f) => {
  if (f && f.endsWith(".fut")) for (const c of clients) c.write(`data: /${f}\n\n`);
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
  try {
    const body = await readFile(file);
    res.writeHead(200, { "content-type": types[extname(file)] ?? "text/plain; charset=utf-8", "cache-control": "no-store" }).end(body);
  } catch { res.writeHead(404).end(); }
}).listen(port, "127.0.0.1", () => console.log(`fut: http://localhost:${port}/?file=examples/paddle.fut`));
