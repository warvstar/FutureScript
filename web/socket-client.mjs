// Copy beside a compiled library's runtime.js and bundle.json for a browser or Node socket client.
import { SocketService } from "./socket-service.mjs";
import { readProgramBundle, openWorkerProgram } from "./runtime.js";

const base = new URL(".", import.meta.url);
let bundle;

async function read(url) {
  if (url.protocol === "file:") return (await import("node:fs/promises")).readFile(url);
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Could not load ${url.pathname} (HTTP ${response.status})`);
  return new Uint8Array(await response.arrayBuffer());
}

async function open() {
  bundle ??= readProgramBundle(base, read).catch(error => { bundle = null; throw error; });
  const compiled = await bundle;
  const program = await openWorkerProgram(compiled);
  return request => program.applySync(compiled.library, [request]);
}

export class SocketClient extends SocketService {
  constructor(onStatus, origin = location.href, clock) { super(open, onStatus, origin, clock); }
}
