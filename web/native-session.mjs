import { execFileSync, spawn } from "node:child_process";
import {
  copyFileSync, mkdirSync, mkdtempSync, readFileSync, readlinkSync, renameSync,
  rmSync, statSync, symlinkSync, unlinkSync, writeFileSync,
} from "node:fs";
import { join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { SocketService } from "./socket-service.mjs";
import { openWorkerProgram, readProgramBundle } from "./worker-runtime.js";
import { readFile } from "node:fs/promises";

function read(path) {
  try { return readFileSync(path, "utf8"); }
  catch (error) { if (error.code !== "ENOENT") throw error; return null; }
}

function copyIfPresent(source, target) {
  try { copyFileSync(source, target); return true; }
  catch (error) { if (error.code !== "ENOENT") throw error; return false; }
}

function writeAtomic(path, text) {
  writeFileSync(path + ".tmp", text, { mode: 0o600 });
  renameSync(path + ".tmp", path);
}

function lockProfile(profile) {
  const lock = join(profile, "active");
  const owner = String(process.pid);
  try { symlinkSync(owner, lock); }
  catch (error) {
    if (error.code !== "EEXIST") throw error;
    const previous = readlinkSync(lock);
    if (!/^[1-9][0-9]*$/.test(previous)) throw new Error("Invalid profile lock");
    try { process.kill(Number(previous), 0); }
    catch (check) {
      if (check.code !== "ESRCH") throw check;
      if (readlinkSync(lock) === previous) unlinkSync(lock);
      symlinkSync(owner, lock);
      return () => unlinkSync(lock);
    }
    throw new Error("This profile is in use. Choose a different profile for a second instance.");
  }
  return () => unlinkSync(lock);
}

export async function runNativeSession(app, config, profileName) {
  const origin = new URL(config.endpoint);
  if (!["http:","https:"].includes(origin.protocol) || origin.username || origin.password) throw Error("Use an HTTP(S) endpoint.");
  if (!/^[a-zA-Z0-9_-]{1,40}$/.test(profileName)) throw Error("Invalid profile name.");
  const {save, legacy, mailbox} = config;
  if (typeof WebSocket !== "function") throw new Error("Native networking needs a Node runtime with WebSocket support.");
  const data = join(app, config.data);
  const profile = join(data, "profiles", profileName);
  mkdirSync(profile, { recursive: true, mode: 0o700 });
  const unlock = lockProfile(profile);
  let run, service, child, timer, interrupt, terminate;
  try {
    run = mkdtempSync(join(data, "run-"));
    const path = name => join(run, name);
    for (const name of config.resources ?? []) symlinkSync(join(app,name),path(name),"dir");
    const hadSave = copyIfPresent(join(profile,save),path(save));
    if (!hadSave && legacy) copyIfPresent(join(app,legacy),path(legacy));
    for (const [name,value] of Object.entries(config.files ?? {})) writeAtomic(path(name),JSON.stringify(value));
    let saveStamp = hadSave ? statSync(path(save), { bigint: true }).mtimeNs : null;
    function persistSave() {
      let stamp;
      try { stamp = statSync(path(save), { bigint: true }).mtimeNs; }
      catch (error) { if (error.code !== "ENOENT") throw error; return; }
      if (stamp === saveStamp) return;
      const text = read(path(save));
      // Fut may be in the middle of writing a new projection. Retry a partial read on the next poll.
      try { if (!JSON.parse(text)) return; } catch { return; }
      writeAtomic(join(profile, save), text);
      saveStamp = stamp;
    }

    const compiler = fileURLToPath(new URL("./worker.mjs",import.meta.url));
    const bundleDir = path("program");
    execFileSync(process.execPath,[compiler,"build","--library","dispatch","--heap-mb","8",
      "--root",app,join(app,config.network),bundleDir], {stdio:["ignore","ignore","inherit"]});
    const bundle = await readProgramBundle(pathToFileURL(bundleDir+"/"),readFile);
    const vm = await openWorkerProgram(bundle);
    service = new SocketService(async () => event => vm.applySync("dispatch",[event]), status => console.log(status), origin);
    console.log(`Opening ${origin.origin} (profile ${profileName})`);
    child = spawn(process.env.FUT || "fut", [join(app,config.entry)], {
      cwd:run, env:{...process.env,...config.env}, stdio:"inherit",
    });
    interrupt = () => child.kill("SIGINT");
    terminate = () => child.kill("SIGTERM");
    process.once("SIGINT", interrupt);
    process.once("SIGTERM", terminate);
    let sent = 0, received = 0, retryAt = 0, retryDelay = 2000, failed = false;
    const finished = new Promise((resolve, reject) => {
      child.once("error", reject);
      child.once("close", (code, signal) => resolve(code ?? (signal === "SIGINT" ? 130 : 1)));
    });
    timer = setInterval(() => {
      try {
        const ready = read(path(mailbox.outReady));
        if (ready === String(received + 1)) {
          const output = JSON.parse(read(path(mailbox.outData)));
          if (!output) throw new Error("Missing native mailbox data");
          service.submit(output);
          received++;
          writeAtomic(path(mailbox.outAck), ready);
        }
        if (!sent || read(path(mailbox.inAck)) === String(sent)) {
          const state = service.snapshot();
          if (state != null) {
            writeAtomic(path(mailbox.inData), state);
            writeAtomic(path(mailbox.inReady), String(++sent));
          }
        }
        persistSave();
        const now = performance.now();
        if (!service.present() && service.canRetry && !service.loggingIn && now >= retryAt) {
          service.retry();
          retryAt = now + retryDelay;
          retryDelay = Math.min(30_000, 2 * retryDelay);
        } else if (service.present()) { retryDelay = 2000; retryAt = now + retryDelay; }
      } catch (error) {
        console.error(`Native session stopped: ${error.message}`);
        failed = true;
        clearInterval(timer);
        child.kill();
      }
    }, 20);
    const exitCode = await finished;
    persistSave();
    return failed ? 1 : exitCode;
  } finally {
    clearInterval(timer);
    service?.stop();
    if (interrupt) process.removeListener("SIGINT", interrupt);
    if (terminate) process.removeListener("SIGTERM", terminate);
    if (run) rmSync(run, { recursive: true, force: true });
    unlock();
  }
}
