// node --test tests/worker-program.mjs after python3 build.py wasm
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

test("compiled libraries preserve per-instance state and reject reserved reference keys", async t => {
  const dir = await mkdtemp(join(tmpdir(), "fut-library-"));
  t.after(() => rm(dir, {recursive:true, force:true}));
  const main = join(dir, "counter.fut"), out = join(dir, "bundle");
  await writeFile(main, `var count=0
fn dispatch(arg) {
  if arg=="next" { count+=1 return count }
  if typeOf(arg)=="string" { return parseJson(arg) }
  return arg
}`);
  execFileSync(process.execPath, [fileURLToPath(new URL("../web/worker.mjs",import.meta.url)), "build",
    "--library", "dispatch", "--heap-mb", "8", main, out]);
  const base = pathToFileURL(out + "/");
  const {readProgramBundle, openWorkerProgram} = await import(new URL("runtime.js",base));
  const bundle = await readProgramBundle(base, readFile);
  const a = await openWorkerProgram(bundle), b = await openWorkerProgram(bundle);
  assert.equal(a.applySync("dispatch", ["next"]), 1);
  assert.equal(await a.apply("dispatch", ["next"]), 2);
  assert.equal(b.applySync("dispatch", ["next"]), 1);
  const data = {self:{item:[null,0,false]}};
  assert.deepEqual(a.applySync("dispatch", [data]), data);
  const tagged = {nested:{"\0":"reserved"}};
  assert.throws(() => a.applySync("dispatch", [tagged]), /reserved NUL key/);
  assert.throws(() => a.applySync("dispatch", [JSON.stringify(tagged)]), /reserved NUL key/);
  assert.equal(a.applySync("dispatch", ["next"]), 3);
});
