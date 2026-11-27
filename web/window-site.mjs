// The browser document for an app using window()/present() and std/ui. Its Fut
// entry draws into #ui. The host owns the pixel and WebGPU presentation surfaces.
import { copyFileSync, writeFileSync } from "node:fs";
import { join } from "node:path";

// Fut reads @window/info for viewport capabilities and URL query parameters.
// @window/configure accepts a JSON map of virtual text files, local storage paths,
// input settings and optional session mailboxes. A supplied client exposes
// reset, snapshot, submit, retry and stop; its callback reports status changes.
// @window/action handles start, retry and exit. @window/status returns the last
// status as JSON. Apps without a service or live sources can omit client/follow.
export function writeWindowPage(out, options) {
  const {title, ...settings} = options;
  const escaped = title.replaceAll("&","&amp;").replaceAll("<","&lt;").replaceAll(">","&gt;");
  const args = JSON.stringify(settings).replaceAll("<","\\u003c");
  copyFileSync(new URL("./window-runtime.mjs", import.meta.url), join(out,"window-runtime.mjs"));
  writeFileSync(join(out,"index.html"), `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-capable" content="yes">
<meta name="theme-color" content="#0b0d10"><title>${escaped}</title><link rel="icon" href="data:,">
<link rel="preload" as="fetch" crossorigin href="fut.wasm?v=${encodeURIComponent(settings.version)}">
<link rel="preload" as="fetch" crossorigin href="files.json?v=${encodeURIComponent(settings.version)}">
<style>
html, body {margin:0;height:100%;background:#0b0d10;color:#e4ecdf}
canvas {position:absolute;display:none;touch-action:none;outline:0}
#boot,#live {position:fixed;top:4px;left:4px;z-index:5;background:#151918;max-width:min(90vw,60em);white-space:pre-wrap}
</style></head><body><canvas id="screen" tabindex="0"></canvas><canvas id="gpu" tabindex="0"></canvas>
<div id="ui"></div><div id="boot" role="status">Loading…</div><div id="live" hidden role="status"></div>
<script type="module">
import {runWindowSite} from "./window-runtime.mjs?v=${encodeURIComponent(settings.version)}";
runWindowSite(${args}).catch(error => { const el=document.getElementById("boot"); el.hidden=false; el.textContent=error.message; console.error(error); });
</script></body></html>\n`);
}
