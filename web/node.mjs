// fut's host options for Node: what web/run.mjs gives createFut, and what each thread of fut-mt.wasm (the threads
// build) gives it too, on a worker of its own (web/thread.mjs): the same files, output and child processes. cfg is
// plain data (it crosses to workers): {verbose, seed, threads, interp, std}.
import { spawnSync } from "node:child_process";
import { readFileSync, rmSync, statSync, unlinkSync, writeFileSync, writeSync } from "node:fs";
import { availableParallelism, tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { Worker, threadId } from "node:worker_threads";

const runMjs = fileURLToPath(new URL("./run.mjs", import.meta.url));
function writeAll(fd, b) {  // stdout may be a non-blocking pipe
  for (let o = 0; o < b.length;) {
    try { o += writeSync(fd, b, o); } catch (e) { if (e.code != "EAGAIN") throw e; }
  }
}
let runs = 0;
export function nodeOptions(cfg) {
  return {
    std: cfg.std,
    verbose: cfg.verbose,
    seed: cfg.seed,
    poolSize: cfg.threads,
    interp: cfg.interp,
    cores: availableParallelism(),
    read(p) { try { return readFileSync(p); } catch { return null; } },
    save(p, text) { try { return text == null ? rmSync(p) : writeFileSync(p, text), true; } catch { return false; } },
    write: writeAll,
    getenv: (n) => process.env[n],
    importModule: (p, stamp) => import(pathToFileURL(p).href + "?v=" + (stamp ?? 0)),  // a new URL loads it again
    stamp(p) { try { return statSync(p).mtimeMs; } catch { return 0; } },
    run(args, file, source, timeout) {  // run(): run.mjs again, in a child process
      let tmp = null;
      if (source != null) writeFileSync((tmp = join(tmpdir(), `fut-run-${process.pid}-${threadId}-${runs++}.fut`)), source), (file = tmp);
      const r = spawnSync(process.execPath, [runMjs, ...args, file], { timeout: timeout * 1000, stdio: ["ignore", "pipe", "pipe"], encoding: "utf8" });
      if (tmp) unlinkSync(tmp);
      const timedOut = r.error?.code == "ETIMEDOUT";
      return { code: r.status ?? 128 + (r.signal == "SIGKILL" ? 9 : r.signal == "SIGTERM" ? 15 : 6), out: r.stdout ?? "", err: r.stderr ?? "", timedOut };
    },
    threads: {  // fut-mt.wasm asks for a thread: a worker, which instantiates it over the same memory
      start(thread) { new Worker(new URL("./thread.mjs", import.meta.url), { workerData: { cfg, thread } }).unref(); },
    },
  };
}
