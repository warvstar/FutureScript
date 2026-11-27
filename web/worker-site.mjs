// Bundle the Cloudflare exports named in an application's descriptor. All handlers live in its Fut program.
import { copyFileSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

const ident = /^[A-Za-z_$][\w$]*$/;
function identifier(name) {
  if (!ident.test(name) || typeof name !== "string") throw new Error("Invalid Worker export name: " + name);
  return name;
}
const quoted = JSON.stringify;

export function writeWorkerEntry(out, config) {
  for (const file of ["durable-runtime.js", ...(config.live ? ["live-source-worker.mjs", "source-delta.mjs"] : [])]) {
    copyFileSync(new URL(file, import.meta.url), join(out, file));
  }
  const lines = [
    'import { openProgram } from "./worker.js";',
    'import { durableClass, workerHandler } from "./durable-runtime.js";',
  ];
  if (config.requestActions) {
    const folder = join(out, "std/cloudflare");
    mkdirSync(folder, { recursive: true });
    copyFileSync(new URL("../std/cloudflare/bindings.js", import.meta.url), join(folder, "bindings.js"));
    lines.push('import * as io from "./std/cloudflare/bindings.js";');
  }
  if (config.live) {
    const { className, ...options } = config.live;
    lines.push('import { liveSourceService } from "./live-source-worker.mjs";',
      `const live = liveSourceService(${quoted(options)});`, `export class ${identifier(className)} extends live.Durable {}`);
  }
  for (const {className, handler, rpc} of config.objects) {
    lines.push(`export class ${identifier(className)} extends durableClass(openProgram, ${quoted(handler)}) {`);
    // Compatibility with callers already using positional RPCs. New callers use invoke(method, value).
    for (const [method, args] of Object.entries(rpc ?? {})) {
      const keys = Array.isArray(args) ? args.map(identifier) : args == null ? [] : [identifier(args)];
      const arg = Array.isArray(args) ? `{${keys.join(", ")}}` : keys[0] ?? "null";
      lines.push(`  ${identifier(method)}(${keys.join(", ")}) { return this.invoke(${quoted(method)}, ${arg}); }`);
    }
    lines.push("}");
  }
  lines.push(`const handle = workerHandler(openProgram, ${quoted(config.handler)}${config.requestActions ? ", io" : ""});`,
    "export default { fetch(request, env) {");
  if (config.live) lines.push("  const liveResponse = live.fetch(request, env);", "  if (liveResponse) return liveResponse;");
  if (config.assets) lines.push(
    `  if (!new URL(request.url).pathname.startsWith(${quoted(config.prefix)})) return env[${quoted(config.assets)}].fetch(request);`);
  lines.push("  return handle(request, env);", "} };", "");
  writeFileSync(join(out, "entry.js"), lines.join("\n"));
}
