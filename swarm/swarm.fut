// A swarm of Claude agents building a fut project (swarm/targets/<target>.md) from scratch, written in fut.
// It doubles as a benchmark: hidden acceptance tests (swarm/targets/<target>/accept/) score main as it grows,
// and each run's progress by time and spend goes to swarm/results/<run>.json.
// Planner -> approver choose the next tasks; workers implement them in their own git worktrees with tools;
// the harness runs the tests; a reviewer approves each change before it merges into main.
// Runs until $SWARM_DIR/STOP exists or the budget is spent. Start it with swarm/run.sh.

var ROOT = env("SWARM_ROOT")
var DIR = env("SWARM_DIR")
var FUT = env("FUT")
var KEY = env("SWARM_API_KEY")
var URL = env("SWARM_API_URL") || "https://api.anthropic.com/v1/messages"
var TARGET = env("SWARM_TARGET") || "fsql"
var MODEL = env("SWARM_MODEL") || "claude-haiku-5-5"   // every role's default; SWARM_MODEL_<ROLE> overrides one
var MODELS = {worker: env("SWARM_MODEL_WORKER") || MODEL, planner: env("SWARM_MODEL_PLANNER") || MODEL,
              approver: env("SWARM_MODEL_APPROVER") || MODEL, reviewer: env("SWARM_MODEL_REVIEWER") || MODEL}
var MINUTES = parseNum(env("SWARM_MINUTES") || "0")    // run time limit, summed over restarts; 0: none
var CF = env("SWARM_CF_URL")                            // set: test files run on this interpreting Worker, one per request
var CAP = parseNum(env("SWARM_CAP") || "20")          // dollars, hard: no request starts that could cross it
var NWORKERS = parseNum(env("SWARM_WORKERS") || "8")
var MAXSTEPS = 90
var MAXCTX = 240000                                     // chars of a worker's conversation (stays under 100K tokens)
var WORK = DIR + "/work"
var T0 = time()

var ledger = {spent: 0, calls: 0, input: 0, output: 0, cacheRead: 0, cacheWrite: 0, rounds: 0, merged: 0, rejected: 0,
              elapsed: 0, byModel: {}, run: nil, timeline: []}
var tasks = []          // {id, title, detail, files, status: queued|active|done|failed, attempts, notes}
var proposals = []      // from the planner, waiting for the approver
var rejections = []     // recent: "task: why"
var events = []         // recent, for status.md
var workers = []
var mainTests = {ok: true, report: "", summary: "no tests yet"}
var planWait = 0        // rounds to wait before planning again
var nextId = 1
var COMMON = ""
var ROLE = {}
var TDIR = ""           // swarm/targets/<target>
var lastScore = -1000   // elapsed seconds at the last acceptance run
var accept = nil        // {passed, total} on main, when the target has acceptance tests

// ---- small helpers ----
fn log(s) {
  var line = "[" + str(int(time() - T0)) + "s $" + str(int(ledger.spent * 10000) / 10000) + "] " + s
  print(line)
  events.push(line)
  if events.len() > 40 { events = events.slice(events.len() - 40, events.len()) }
}
fn clip(s, n) {
  if s.len() <= n { return s }
  return s.slice(0, n) + "\n... (" + str(s.len() - n) + " more characters cut)"
}
fn sh(cmd, cwd) { return exec({cmd: ["sh", "-c", cmd], cwd: cwd, timeout: 300}) }
fn lines(s) {
  var out = []
  for l in s.split("\n") { if l.trim() != "" { out.push(l.trim()) } }
  return out
}
fn safePath(p) { return p != nil && p != "" && p[0] != "/" && !p.has("..") }

// ---- money ----
var PRICES = {}         // dollars per million tokens; Haiku 5.5 has a second card for prompts over 100K tokens
PRICES["claude-haiku-5-5"] = {inp: 0.1, out: 0.5, longInp: 0.5, longOut: 2.5, maxTok: 16000}
PRICES["claude-sonnet-5-5"] = {inp: 2, out: 10, maxTok: 32000}
PRICES["claude-opus-5-5"] = {inp: 4, out: 20, maxTok: 32000}
fn price(model, promptTokens) {
  var p = PRICES[model]
  if p.longInp && promptTokens > 100000 { return {inp: p.longInp, out: p.longOut} }
  return p
}
fn charge(u, model) {
  var inp = u.input_tokens || 0
  var cw = u.cache_creation_input_tokens || 0
  var cr = u.cache_read_input_tokens || 0
  var out = u.output_tokens || 0
  var p = price(model, inp + cw + cr)
  // conservative: cache writes at 1.25x, cache reads at the full input price
  var cost = (inp + cw * 1.25 + cr) * p.inp / 1000000 + out * p.out / 1000000
  ledger.spent += cost
  ledger.byModel[model] = (ledger.byModel[model] || 0) + cost
  ledger.calls += 1
  ledger.input += inp
  ledger.output += out
  ledger.cacheRead += cr
  ledger.cacheWrite += cw
  return cost
}
fn worstCase(r) {  // the most a request can cost: ~2.5 chars a token, all of it a cache write, full output
  var est = r.body.len() / 2.5
  var p = price(r.model, est)
  return est * 1.25 * p.inp / 1000000 + PRICES[r.model].maxTok * p.out / 1000000
}

// ---- the API ----
fn request(role, tools, messages) {  // {body, model}
  var model = MODELS[role]
  var body = {model: model, max_tokens: PRICES[model].maxTok, system: system(role), messages: messages, cache_control: {type: "ephemeral"}}
  if tools != nil { body.tools = tools }
  return {body: json(body), model: model}
}
fn http(r) {
  return {url: URL, method: "POST", body: r.body, timeout: 600,
          headers: {"x-api-key": KEY, "anthropic-version": "2023-06-01", "content-type": "application/json"}}
}
fn system(role) {
  return [{type: "text", text: COMMON, cache_control: {type: "ephemeral"}}, {type: "text", text: ROLE[role]}]
}
fn toolUses(msg) {
  var out = []
  for b in msg.content { if b.type == "tool_use" { out.push(b) } }
  return out
}
fn tool(name, desc, props, required) {
  return {name: name, description: desc, input_schema: {type: "object", properties: props, required: required}}
}
var S = {type: "string"}
var WORKER_TOOLS = [
  tool("map", "Every file's declarations (fut map of all.fut). Cheap: look here first.", {}, []),
  tool("ls", "List the repository's files.", {}, []),
  tool("read", "Read a file.", {path: S}, ["path"]),
  tool("write", "Create or replace a whole file.", {path: S, content: S}, ["path", "content"]),
  tool("edit", "Replace exact text in a file: old must occur exactly once.", {path: S, old: S, new: S}, ["path", "old", "new"]),
  tool("test", "Run every tests/*_test.fut and report failures.", {}, []),
  tool("done", "Submit the task: the harness reruns the tests and a reviewer checks the diff.", {summary: S}, ["summary"])]
var PLAN_TOOLS = [tool("propose", "Propose the next tasks.", {tasks: {type: "array", items: {type: "object",
  properties: {title: S, detail: S, files: {type: "array", items: S}}, required: ["title", "detail", "files"]}}}, ["tasks"])]
var DECIDE_TOOLS = [tool("decide", "Approve or reject each proposed task, by its index.", {decisions: {type: "array", items: {type: "object",
  properties: {index: {type: "number"}, approve: {type: "boolean"}, reason: S, amendment: S}, required: ["index", "approve", "reason"]}}}, ["decisions"])]
var VERDICT_TOOLS = [tool("verdict", "Approve the change for main, or reject it with specific feedback.",
  {approve: {type: "boolean"}, feedback: S}, ["approve", "feedback"])]

// ---- tests ----
var CFTOKEN = nil
fn cfRun(dir, files) {  // each test file in its own request to the Worker, all at once; results shaped as exec's
  if CFTOKEN == nil { CFTOKEN = (readFile(env("SWARM_CF_TOKEN_FILE") || DIR + "/.cftoken") || "").trim() }
  var src = {}
  for f in lines(sh("find . -path ./.git -prune -o -path '*/.fut-cache' -prune -o -type f \\( -name '*.fut' -o -name '*.api' -o -name fut.pkg \\) -print", dir).out) {
    src[f.slice(2, f.len())] = readFile(dir + "/" + f)
  }
  var reqs = []
  for f in files {
    reqs.push({url: CF + "/run", method: "POST", timeout: 300, body: json({files: src, tests: [f]}),
               headers: {authorization: "Bearer " + CFTOKEN, "content-type": "application/json"}})
  }
  var t0 = time()
  var got = fetchAll(reqs)
  var ms = (time() - t0) * 1000
  // Workers Paid: $0.30 a million requests, $0.02 a million CPU ms. CPU time isn't reported back, so each request is
  // charged the whole batch's wall time: an upper bound.
  var cost = reqs.len() * (0.3 + ms * 0.02) / 1000000
  ledger.spent += cost
  ledger.byModel["cloudflare"] = (ledger.byModel["cloudflare"] || 0) + cost
  var out = []
  for r in got {
    if r.status == 200 { out.push({code: 0, out: r.body, err: "", timedOut: false}) }
    else { out.push({code: 1, out: "", err: "Cloudflare Worker: " + str(r.status) + " " + clip(r.body || r.error || "", 300), timedOut: false}) }
  }
  return out
}
fn testFiles(dir) { return lines(sh("ls tests/*_test.fut 2>/dev/null", dir).out) }
fn runTests(dir) {
  var files = testFiles(dir)
  if files.len() == 0 { return {ok: true, report: "no test files yet (tests/*_test.fut)", summary: "no tests", n: {passed: 0}} }
  var api = []
  for f in files { api.push({cmd: [FUT, "api", f], cwd: dir, timeout: 60}) }
  execAll(api)
  var res = nil
  if CF { res = cfRun(dir, files) }
  else {
    var runs = []
    for f in files { runs.push({cmd: [FUT, "test", "--json", "--timeout", "20", f], cwd: dir, timeout: 240}) }
    res = execAll(runs)
  }
  var n = {passed: 0, failed: 0, errors: 0, pending: 0, broken: 0}
  var bad = []
  var pend = []
  for k in 0..res.len() {
    var r = res[k]
    if r.timedOut { n.broken += 1 bad.push(files[k] + ": the test run timed out") continue }
    var seen = false
    for l in lines(r.out) {
      if l[0] != "\{" { continue }
      var j = nil
      try { j = parseJson(l) } catch e { continue }
      seen = true
      if j.type == "test" {
        if j.status == "pass" { n.passed += 1 }
        else if j.status == "pending" { n.pending += 1 pend.push(j.name + ": " + (j.reason || "")) }
        else if j.status == "fail" {
          n.failed += 1
          var x = j.expect || {}
          var d = "FAIL " + j.name + " (" + j.file + ":" + str(j.line) + ") expect " + (x.expr || "")
          if x.left { d = d + "\n    left " + x.left.expr + " = " + x.left.value }
          if x.right { d = d + "\n    right " + x.right.expr + " = " + x.right.value }
          if x.diff { d = d + "\n    first difference at " + json(x.diff) }
          if x.reason { d = d + "\n    " + x.reason }
          if j.output { d = d + "\n    output: " + clip(j.output, 400) }
          bad.push(d)
        } else {
          n.errors += 1
          bad.push("ERROR " + j.name + " (" + j.file + ":" + str(j.line) + "): " + (j.message || "") + "\n    " + clip(json(j.trace || ""), 600))
        }
      } else if j.type == "compile_error" {
        n.broken += 1
        bad.push("COMPILE ERROR " + j.file + ":" + str(j.line) + ":" + str(j.col) + ": " + j.message + "\n    " + (j.text || ""))
      } else if j.type == "error" {
        n.broken += 1
        bad.push("ERROR in top-level code of " + files[k] + ": " + j.message + "\n    " + clip(json(j.trace || ""), 600))
      }
    }
    if !seen && r.code != 0 { n.broken += 1 bad.push(files[k] + ": exit " + str(r.code) + "\n" + clip(r.err, 1500)) }
  }
  var summary = str(n.passed) + " passed, " + str(n.failed) + " failed, " + str(n.errors) + " errors, " + str(n.pending) + " pending, " + str(n.broken) + " broken files"
  var report = summary
  if bad.len() > 0 { report = report + "\n" + clip(bad.join("\n"), 7000) }
  if pend.len() > 0 { report = report + "\npending:\n  " + clip(pend.join("\n  "), 1500) }
  return {ok: n.failed == 0 && n.errors == 0 && n.broken == 0, report: report, summary: summary, n: n}
}

// ---- tasks ----
fn byStatus(s) {
  var out = []
  for t in tasks { if t.status == s { out.push(t) } }
  return out
}
fn titles(list) {
  var out = []
  for t in list { out.push("#" + str(t.id) + " " + t.title + " [" + t.files.join(", ") + "]") }
  if out.len() == 0 { return "(none)" }
  return out.join("\n")
}
fn saveState() {
  writeFile(DIR + "/ledger.json", json(ledger))
  writeFile(DIR + "/tasks.json", json({tasks: tasks, nextId: nextId, rejections: rejections}))
  var w = []
  for x in workers {
    var t = "idle"
    if x.task { t = x.phase + " #" + str(x.task.id) + " " + x.task.title + " (step " + str(x.steps) + ")" }
    w.push("- w" + str(x.i) + ": " + t)
  }
  writeFile(DIR + "/status.md", "# swarm\n\nspent $" + str(int(ledger.spent * 10000) / 10000) + " of $" + str(CAP) +
    ", " + str(ledger.calls) + " calls, " + str(ledger.rounds) + " rounds, " + str(int(time() - T0)) + "s\n" +
    "tasks: " + str(byStatus("done").len()) + " done, " + str(byStatus("active").len()) + " active, " +
    str(byStatus("queued").len()) + " queued, " + str(byStatus("failed").len()) + " failed; merges " + str(ledger.merged) +
    ", review rejections " + str(ledger.rejected) + "\nmain: " + mainTests.summary + "\n\n## workers\n" + w.join("\n") +
    "\n\n## recent\n" + events.join("\n") + "\n")
}
fn loadState() {
  var l = readFile(DIR + "/ledger.json")
  if l {
    ledger = parseJson(l)
    for k in ["elapsed", "byModel", "timeline"] { if ledger[k] == nil { ledger[k] = {elapsed: 0, byModel: {}, timeline: []}[k] } }
  }
  var t = readFile(DIR + "/tasks.json")
  if t {
    var s = parseJson(t)
    tasks = s.tasks
    nextId = s.nextId
    rejections = s.rejections
    for x in tasks { if x.status == "active" { x.status = "queued" } }   // their workers are gone
  }
}

// ---- workers ----
fn newWorker(i) { return {i: i, wt: DIR + "/wt/w" + str(i), task: nil, messages: [], steps: 0, phase: "idle", size: 0} }
fn intro(t) {
  var notes = ""
  if t.notes && t.notes.len() > 0 { notes = "\n\nNotes from earlier attempts and reviews:\n- " + t.notes.join("\n- ") }
  return "Your task (#" + str(t.id) + "): " + t.title + "\n\n" + t.detail + "\n\nFiles this task owns: " + t.files.join(", ") +
    notes + "\n\nmain's tests now: " + mainTests.summary
}
fn progress(w) {  // what the worktree already has, for a conversation that starts over on the same files
  var stat = sh("git add -A && git diff --cached --stat main", w.wt).out.trim()
  if stat == "" { return "" }
  return "\n\nYou already started this task, and the repository keeps your changes so far (compared with main):\n" + clip(stat, 3000) +
    "\nRead the files you need, run test to see where things stand, and continue."
}
fn startTask(w, t) {
  var resume = t.worker == w.i   // the harness restarted mid-task: this worktree still has the work
  if !resume { sh("git checkout -q -B w" + str(w.i) + " main && git reset -q --hard main && git clean -qfd", w.wt) }
  t.status = "active"
  if !resume { t.attempts = (t.attempts || 0) + 1 }
  t.worker = w.i
  w.task = t
  w.steps = 0
  w.refreshes = 0
  w.reviews = 0
  w.phase = "work"
  var more = ""
  if resume { sync(w) more = progress(w) }
  w.messages = [{role: "user", content: intro(t) + more}]
  var how = " starts #"
  if resume { how = " resumes #" }
  log("w" + str(w.i) + how + str(t.id) + " " + t.title)
}
fn sync(w) {  // bring the worktree up to main, keeping the worker's changes; false if they conflict
  if sh("git rev-parse HEAD", w.wt).out == sh("git rev-parse main", w.wt).out { return true }
  if sh("git status --porcelain", w.wt).out.trim() == "" && sh("git rev-list --count main..HEAD", w.wt).out.trim() == "0" {
    sh("git reset -q --hard main", w.wt)
    return true
  }
  var wip = sh("git add -A", w.wt).code == 0 && sh("git commit -q -m wip", w.wt).code == 0  // nothing staged: no commit
  var ok = sh("git rebase -q main", w.wt).code == 0
  if !ok { sh("git rebase --abort", w.wt) }
  if wip { sh("git reset -q HEAD~1", w.wt) }  // the changes go back to being uncommitted
  return ok
}
fn refresh(w) {  // the conversation grew too long: start it over, keeping the files
  w.refreshes += 1
  sync(w)
  w.messages = [{role: "user", content: intro(w.task) + progress(w) + "\n\n(Your earlier conversation was cleared to save space.)"}]
  log("w" + str(w.i) + " starts its conversation over on #" + str(w.task.id) + " (" + str(w.refreshes) + ")")
}
fn giveUp(w, why) {
  var t = w.task
  t.worker = nil
  t.notes = t.notes || []
  t.notes.push("attempt " + str(t.attempts) + " stopped: " + why)
  if t.attempts >= 3 { t.status = "failed" rejections.push(t.title + ": failed 3 times, last: " + why) }
  else { t.status = "queued" }
  log("w" + str(w.i) + " gives up #" + str(t.id) + ": " + why)
  w.task = nil
  w.phase = "idle"
  w.messages = []
}
fn runTool(w, name, inp) {
  var wt = w.wt
  if name == "map" {
    var r = exec({cmd: [FUT, "map", "all.fut"], cwd: wt, timeout: 60})
    if r.code != 0 { return {text: "fut map failed (fix all.fut):\n" + clip(r.err + r.out, 3000), err: true} }
    return {text: clip(r.out, 14000)}
  }
  if name == "ls" { return {text: clip(sh("git ls-files -co --exclude-standard", wt).out, 6000)} }
  if !safePath(inp.path) && (name == "read" || name == "write" || name == "edit") { return {text: "path must be relative, inside the repository", err: true} }
  if name == "read" {
    var s = readFile(wt + "/" + inp.path)
    if s == nil { return {text: "no such file: " + inp.path, err: true} }
    return {text: clip(s, 24000)}
  }
  if name == "write" {
    writeFile(wt + "/" + inp.path, inp.content || "")
    return {text: "wrote " + inp.path + " (" + str(lines(inp.content || "").len()) + " lines)"}
  }
  if name == "edit" {
    var s = readFile(wt + "/" + inp.path)
    if s == nil { return {text: "no such file: " + inp.path, err: true} }
    if inp.old == nil || inp.old == "" { return {text: "old must be non-empty", err: true} }
    var parts = s.split(inp.old)
    if parts.len() != 2 { return {text: "old occurs " + str(parts.len() - 1) + " times in " + inp.path + "; it must occur exactly once (include more surrounding text)", err: true} }
    writeFile(wt + "/" + inp.path, parts[0] + (inp.new || "") + parts[1])
    return {text: "edited " + inp.path}
  }
  if name == "test" {
    var r = runTests(wt)
    return {text: r.report, err: !r.ok}
  }
  if name == "done" { return submit(w, inp.summary || "") }
  return {text: "unknown tool " + name, err: true}
}
fn conflicted(w) {  // a rebase onto main failed: start again from main, with the old diff to go by
  sh("git rebase --abort", w.wt)
  var diff = sh("git diff main...HEAD", w.wt).out
  sh("git reset -q --hard main && git clean -qfd", w.wt)
  return "your change conflicts with changes merged into main meanwhile, so your files were reset to the current main. " +
    "Read the files again and redo the change on top of them. Your previous diff, for reference:\n" + clip(diff, 30000)
}
fn submit(w, summary) {
  w.summary = summary
  sh("git add -A", w.wt)
  exec({cmd: ["git", "commit", "-q", "-F", "-"], cwd: w.wt, input: "#" + str(w.task.id) + " " + w.task.title + "\n\n" + summary})
  w.empty = sh("git rev-list --count main..HEAD", w.wt).out.trim() == "0"  // main may already do it: the reviewer decides
  if sh("git rebase -q main", w.wt).code != 0 { return {text: conflicted(w), err: true} }
  var r = runTests(w.wt)
  if !r.ok { return {text: "not submitted: the tests fail.\n" + r.report, err: true} }
  w.phase = "review"
  w.tests = r.summary
  var how = ""
  if w.empty { how = " with no changes" }
  log("w" + str(w.i) + " submits #" + str(w.task.id) + how + " (" + r.summary + ")")
  return {text: "submitted; tests: " + r.summary + ". Waiting for review."}
}
fn workerRound(w, msg) {  // the reply to a worker's request
  if w.phase == "work" && sh("git status --porcelain", w.wt).out.trim() == "" && sh("git rev-list --count main..HEAD", w.wt).out.trim() == "0" {
    sh("git reset -q --hard main", w.wt)  // nothing written yet: follow main, so new modules are there to use
  }
  w.messages.push({role: "assistant", content: msg.content})
  w.steps += 1
  var uses = toolUses(msg)
  if uses.len() == 0 {
    var nudge = "Use the tools to continue, and call done when the task is complete and the tests pass."
    if msg.stop_reason == "max_tokens" { nudge = "Your reply was cut off at the output limit. Work in smaller steps: write files in parts or use edit." }
    w.messages.push({role: "user", content: nudge})
    return nil
  }
  var results = []
  for u in uses {
    var r = runTool(w, u.name, u.input || {})
    results.push({type: "tool_result", tool_use_id: u.id, content: r.text, is_error: r.err || false})
  }
  w.messages.push({role: "user", content: results})
  if w.phase != "work" { return nil }
  if w.steps >= MAXSTEPS { giveUp(w, "ran out of steps after " + str(w.steps) + "; split the task or simplify") }
  else if json(w.messages).len() > MAXCTX {
    if w.refreshes >= 3 { giveUp(w, "its conversation outgrew the context 3 times; split the task or simplify") } else { refresh(w) }
  }
}

// ---- review and merge ----
fn onMain(w) {  // an empty submission: show what main has in the task's files
  var out = "(none)\n\nThe worker changed nothing and says main already does the task. Approve only if the files below show that it does.\n"
  for f in w.task.files { out += "\n--- " + f + " ---\n" + clip(readFile(w.wt + "/" + f) || "(missing)", 20000) }
  return out + "\n--- files tracked on main (git ls-files) ---\n" + clip(sh("git ls-files", w.wt).out, 8000)
}
fn reviewRequest(w) {
  var diff = sh("git diff main...HEAD", w.wt).out
  var stat = sh("git diff --stat main...HEAD", w.wt).out
  if w.empty { diff = onMain(w) }
  return request("reviewer", VERDICT_TOOLS, [{role: "user", content: "Task #" + str(w.task.id) + ": " + w.task.title +
    "\n\n" + w.task.detail + "\n\nWorker's summary: " + w.summary + "\n\nTests: " + w.tests + "\n\nDiff stat:\n" + stat +
    "\n\nDiff:\n" + clip(diff, 60000)}])
}
fn reviewed(w, msg) {
  var v = nil
  for u in toolUses(msg) { if u.name == "verdict" { v = u.input } }
  if v == nil { log("reviewer gave no verdict for #" + str(w.task.id) + "; asking again") return nil }
  if !v.approve {
    ledger.rejected += 1
    w.task.notes = w.task.notes || []
    w.task.notes.push("review: " + clip(v.feedback, 600))
    rejections.push(w.task.title + ": " + clip(v.feedback, 300))
    log("review rejects #" + str(w.task.id) + ": " + clip(v.feedback, 200))
    w.phase = "work"
    w.messages.push({role: "user", content: "The reviewer rejected the change. Fix it, run test, and call done again.\nFeedback: " + v.feedback})
    w.reviews = (w.reviews || 0) + 1
    if w.reviews >= 3 { w.reviews = 0 giveUp(w, "rejected in review 3 times") }
    return nil
  }
  merge(w)
}
fn merge(w) {
  if sh("git rebase -q main", w.wt).code != 0 {
    w.phase = "work"
    w.messages.push({role: "user", content: "Approved, but " + conflicted(w)})
    log("#" + str(w.task.id) + " conflicts with main; back to w" + str(w.i))
    return nil
  }
  var r = runTests(w.wt)
  if !r.ok {
    w.phase = "work"
    w.messages.push({role: "user", content: "Approved, but after rebasing onto the latest main the tests fail:\n" + r.report})
    return nil
  }
  if sh("git merge -q --ff-only w" + str(w.i), WORK).code != 0 { log("merge of #" + str(w.task.id) + " failed; retrying next round") return nil }
  w.task.status = "done"
  ledger.merged += 1
  mainTests = r
  log("MERGED #" + str(w.task.id) + " " + w.task.title + " (" + r.summary + ")")
  w.task = nil
  w.phase = "idle"
  w.messages = []
  w.reviews = 0
}

// ---- planning ----
fn context() {
  var m = exec({cmd: [FUT, "map", "all.fut"], cwd: WORK, timeout: 60})
  var recent = rejections
  if recent.len() > 12 { recent = recent.slice(recent.len() - 12, recent.len()) }
  return "Project map (main):\n" + clip(m.out + m.err, 14000) + "\n\nmain's tests: " + mainTests.report +
    "\n\nDone:\n" + titles(byStatus("done")) + "\n\nIn progress:\n" + titles(byStatus("active")) +
    "\n\nQueued:\n" + titles(byStatus("queued")) + "\n\nFailed:\n" + titles(byStatus("failed")) +
    "\n\nRecent rejections:\n" + recent.join("\n")
}
fn planRequest(n) {
  return request("planner", PLAN_TOOLS, [{role: "user", content: context() + "\n\nPropose " + str(n) + " next tasks."}])
}
fn planned(msg) {
  proposals = []
  for u in toolUses(msg) { if u.name == "propose" { proposals = u.input.tasks || [] } }
  log("planner proposes " + str(proposals.len()) + " tasks")
  if proposals.len() == 0 { planWait = 3 }
}
fn decideRequest() {
  var list = []
  for k in 0..proposals.len() {
    var p = proposals[k]
    list.push("[" + str(k) + "] " + p.title + "\n" + p.detail + "\nfiles: " + (p.files || []).join(", "))
  }
  return request("approver", DECIDE_TOOLS, [{role: "user", content: context() + "\n\nProposed tasks:\n\n" + list.join("\n\n")}])
}
fn decided(msg) {
  var ds = nil
  for u in toolUses(msg) { if u.name == "decide" { ds = u.input.decisions } }
  if ds == nil { log("approver gave no decisions; asking again") return nil }
  var ok = 0
  for d in ds {
    if d.index == nil || d.index < 0 || d.index >= proposals.len() { continue }
    var p = proposals[d.index]
    if d.approve {
      var detail = p.detail
      if d.amendment { detail = detail + "\n\nApprover's amendment: " + d.amendment }
      tasks.push({id: nextId, title: p.title, detail: detail, files: p.files || [], status: "queued", attempts: 0, notes: []})
      nextId += 1
      ok += 1
    } else { rejections.push("proposal " + p.title + ": " + d.reason) }
  }
  log("approver approves " + str(ok) + " of " + str(proposals.len()))
  proposals = []
  if ok == 0 { planWait = 2 }
}

// ---- the benchmark ----
fn acceptTotal() {  // the number of acceptance tests, counted in the source so a file that fails to compile still counts
  var n = 0
  for f in lines(sh("ls accept/*_test.fut 2>/dev/null", TDIR).out) {
    for l in readFile(TDIR + "/" + f).split("\n") { if l.trim().find("test \"") == 0 { n += 1 } }
  }
  return n
}
fn score() {  // run the hidden acceptance tests against main, in a scratch copy the workers never see
  var total = acceptTotal()
  if total == 0 { return nil }
  var dir = DIR + "/score"
  sh("rm -rf '" + dir + "' && mkdir -p '" + dir + "' && git archive main | tar -x -C '" + dir + "' && rm -rf '" + dir + "/tests' && mkdir '" +
     dir + "/tests' && cp '" + TDIR + "'/accept/*_test.fut '" + dir + "/tests/'", WORK)
  var r = runTests(dir)
  accept = {passed: r.n.passed, total: total}
  log("acceptance: " + str(accept.passed) + " of " + str(total))
  return accept
}
fn mark() {  // one point on the run's timeline
  var p = {t: int(ledger.elapsed), spent: int(ledger.spent * 10000) / 10000, merged: ledger.merged, tests: mainTests.n.passed}
  if accept { p.accept = accept.passed }
  ledger.timeline.push(p)
}
fn at(key, limit) {  // the last timeline point with key <= limit
  var best = nil
  for p in ledger.timeline { if p[key] <= limit { best = p } }
  return best
}
fn results() {  // swarm/results/<run>.json: what the run reached, and when
  var checks = {}
  for m in [10, 30, 60, 120, 240] { if ledger.elapsed >= m * 60 { checks[str(m) + "m"] = at("t", m * 60) } }
  for d in [1, 2, 5, 10, 20] { if ledger.spent >= d { checks["$" + str(d)] = at("spent", d) } }
  var srcLines = parseNum(sh("cat src/*.fut 2>/dev/null | wc -l", WORK).out.trim()) || 0
  var testLines = parseNum(sh("cat tests/*.fut 2>/dev/null | wc -l", WORK).out.trim()) || 0
  var out = {run: ledger.run.id, target: TARGET, fut: ledger.run.fut, models: MODELS, workers: NWORKERS, cap: CAP, minutes: MINUTES,
    elapsed: int(ledger.elapsed), spent: int(ledger.spent * 10000) / 10000, byModel: ledger.byModel, calls: ledger.calls,
    merged: ledger.merged, rejected: ledger.rejected, failedTasks: byStatus("failed").len(), tests: mainTests.n.passed,
    accept: accept, srcLines: srcLines, testLines: testLines, checkpoints: checks, timeline: ledger.timeline}
  writeFile((env("SWARM_RESULTS") || ROOT + "/swarm/results") + "/" + ledger.run.id + ".json", json(out) + "\n")
}

// ---- the loop ----
fn setup() {
  TDIR = ROOT + "/swarm/targets/" + TARGET
  if readFile(WORK + "/README.md") == nil {
    writeFile(WORK + "/README.md", readFile(TDIR + ".md"))
    writeFile(WORK + "/all.fut", "// imports every module, so `fut map all.fut` shows the project\n")
    writeFile(WORK + "/.gitignore", "*.api\n.fut-cache/\n")
    sh("git init -q -b main && git config user.name swarm && git config user.email swarm@localhost && git add -A && git commit -qm 'project start'", WORK)
  }
  for i in 0..NWORKERS {
    if readFile(DIR + "/wt/w" + str(i) + "/README.md") == nil {
      sh("git worktree add -q -B w" + str(i) + " " + DIR + "/wt/w" + str(i) + " main", WORK)
    }
    workers.push(newWorker(i))
  }
  var spec = readFile(ROOT + "/docs/spec.md")
  COMMON = readFile(ROOT + "/swarm/prompts/common.md").split("\{\{SPEC}}").join(spec).split("\{\{TARGET}}").join(readFile(TDIR + ".md"))
  for r in ["worker", "planner", "approver", "reviewer"] { ROLE[r] = readFile(ROOT + "/swarm/prompts/" + r + ".md") }
}

fn main() {
  if KEY == nil || DIR == nil || ROOT == nil || FUT == nil { print("set SWARM_API_KEY, SWARM_DIR, SWARM_ROOT and FUT (swarm/run.sh does)") return nil }
  setup()
  loadState()
  for role in ["worker", "planner", "approver", "reviewer"] {
    if PRICES[MODELS[role]] == nil { print("no price for model " + MODELS[role] + "; add it to PRICES") return nil }
  }
  if ledger.run == nil {
    ledger.run = {id: TARGET + "-" + sh("date -u +%Y%m%d-%H%M", ROOT).out.trim(), fut: sh("git rev-parse --short HEAD", ROOT).out.trim()}
  }
  mainTests = runTests(WORK)
  log("swarm starts: " + TARGET + " (run " + ledger.run.id + "), " + str(NWORKERS) + " workers, models " + json(MODELS) + ", cap $" + str(CAP) +
    ", spent so far $" + str(ledger.spent))
  score()
  mark()
  var backoff = 0
  var last = time()
  while true {
    var now = time()
    ledger.elapsed += now - last
    last = now
    if readFile(DIR + "/STOP") != nil { log("STOP file: stopping") break }
    if MINUTES > 0 && ledger.elapsed >= MINUTES * 60 { log("time limit: " + str(MINUTES) + " minutes; stopping") break }
    var merged = ledger.merged
    ledger.rounds += 1
    // idle workers take queued tasks
    for w in workers {  // a task resumes on the worker whose worktree has it
      if w.phase != "idle" { continue }
      for t in byStatus("queued") { if t.worker == w.i { startTask(w, t) break } }
    }
    for w in workers {
      if w.phase != "idle" { continue }
      var q = []
      for t in byStatus("queued") { if t.worker == nil { q.push(t) } }
      if q.len() > 0 { startTask(w, q[0]) }
    }
    // this round's requests
    var reqs = []
    var who = []
    for w in workers {
      if w.phase == "work" { reqs.push(request("worker", WORKER_TOOLS, w.messages)) who.push({kind: "worker", w: w}) }
      else if w.phase == "review" { reqs.push(reviewRequest(w)) who.push({kind: "review", w: w}) }
    }
    var idle = 0
    for w in workers { if w.phase == "idle" { idle += 1 } }
    if proposals.len() > 0 { reqs.push(decideRequest()) who.push({kind: "decide"}) }
    else if planWait > 0 { planWait -= 1 }
    else if byStatus("queued").len() < idle + 2 {
      var n = idle + 2
      if n < 3 { n = 3 }
      reqs.push(planRequest(n))
      who.push({kind: "plan"})
    }
    // the budget: drop what could cross the cap
    var room = CAP - ledger.spent - 0.02
    var keep = []
    var keepWho = []
    for k in 0..reqs.len() {
      var c = worstCase(reqs[k])
      if c <= room { room -= c keep.push(reqs[k]) keepWho.push(who[k]) }
    }
    if keep.len() == 0 {
      if reqs.len() > 0 { log("budget reached: $" + str(ledger.spent) + " of $" + str(CAP) + "; stopping") break }
      saveState()
      sleep(3)
      continue
    }
    var http_ = []
    for b in keep { http_.push(http(b)) }
    var res = fetchAll(http_)
    var limited = false
    for k in 0..res.len() {
      var r = res[k]
      var h = keepWho[k]
      if r.status != 200 {
        if r.status == 429 || r.status >= 500 || r.status == 0 { limited = true }
        log(h.kind + " request failed: " + str(r.status) + " " + clip(r.body || r.error || "", 300))
        if r.status == 400 && h.kind == "worker" { giveUp(h.w, "the API rejected the conversation (400)") }
        if r.status == 400 && h.kind == "decide" { proposals = [] }
        continue
      }
      var msg = parseJson(r.body)
      charge(msg.usage || {}, keep[k].model)
      if h.kind == "worker" { workerRound(h.w, msg) }
      else if h.kind == "review" { reviewed(h.w, msg) }
      else if h.kind == "plan" { planned(msg) }
      else if h.kind == "decide" { decided(msg) }
    }
    if limited { backoff = backoff * 2 + 2 if backoff > 60 { backoff = 60 } sleep(backoff) } else { backoff = 0 }
    if ledger.merged != merged {
      if ledger.elapsed - lastScore >= 120 { lastScore = ledger.elapsed score() }
      mark()
      results()
    }
    saveState()
  }
  score()
  mark()
  results()
  saveState()
}
