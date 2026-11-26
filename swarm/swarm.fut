// A swarm of Claude agents building a fut project (swarm/targets/<target>.md) from scratch, written in fut.
// It doubles as a benchmark: hidden acceptance tests (swarm/targets/<target>/accept/) score main as it grows,
// and each run's progress by time and spend goes to swarm/results/<run>.json.
// Planner -> approver choose the next tasks; workers implement them in their own git worktrees with tools;
// the harness runs the tests; a reviewer approves each change before it merges into main.
// Runs until $SWARM_DIR/STOP exists or the budget is spent. Start it with swarm/run.sh <scenario>.
// A scenario (swarm/scenarios/<name>.json, or edited on the dashboard) sets the target, the team and the limits;
// SWARM_* variables override it. With SWARM_DASH_URL, the dashboard on that Worker can change a running swarm (workers,
// models, cap, time, pause, stop) and start the next run; all runs together stay under SWARM_GLOBAL_CAP.

var ROOT = env("SWARM_ROOT")
var DIR = env("SWARM_DIR")
var FUT = env("FUT")
var KEY = env("SWARM_API_KEY")
var URL = env("SWARM_API_URL") || "https://api.anthropic.com/v1/messages"
var SCENARIO = env("SWARM_SCENARIO")                   // configure() reads it; until then, the defaults below
var TARGET = "fsql"
var MODEL = "claude-haiku-5-5"                          // every role's default; models: {role: model} overrides one
var ROLES = ["worker", "planner", "approver", "reviewer"]
var MODELS = {worker: MODEL, planner: MODEL, approver: MODEL, reviewer: MODEL}
var MINUTES = 0                                         // run time limit, summed over restarts; 0: none
var CF = env("SWARM_CF_URL")                            // set: test files run on this interpreting Worker, one per request
var DASH = env("SWARM_DASH_URL")                        // set: the dashboard is published to this Worker, and controls the swarm
var lastPush = 0
var CAP = 20                                            // this run's dollars, hard: no request starts that could cross it
var GLOBAL_CAP = parseNum(env("SWARM_GLOBAL_CAP") || "20")   // every run in swarm/results together, hard
var NWORKERS = 8
var scenarios = {}      // name -> {target, workers, model, models, cap, minutes, cfTests}
var control = nil       // the dashboard's last control document: {rev, run, live: {workers, cap, minutes, models}, pause, stop, next}
var lastPoll = -1000
var paused = false
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
var stopReason = nil
var accept = nil        // {passed, total} on main, when the target has acceptance tests

// ---- configuration: scenario, then SWARM_* variables, then the dashboard's live changes ----
fn safeName(s) {  // names that become paths
  if s == nil || s == "" { return false }
  for k in 0..s.len() { if !"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-".has(s[k]) { return false } }
  return s[0] != "."
}
fn kvGet(key) {
  var r = fetch({url: DASH + "/kv/" + key, headers: {authorization: "Bearer " + cfToken()}, timeout: 15})
  if r.status != 200 { return nil }
  var v = nil
  try { v = parseJson(r.body) } catch e { return nil }
  return v
}
fn kvPut(key, v) {
  return fetch({url: DASH + "/kv/" + key, method: "PUT", body: json(v), timeout: 15,
                headers: {authorization: "Bearer " + cfToken(), "content-type": "application/json"}}).status == 200
}
fn loadScenarios() {  // the files, and with a dashboard its copies (which win: the dashboard edits them)
  var all = {}
  for f in lines(sh("ls *.json 2>/dev/null", ROOT + "/swarm/scenarios").out) {
    try { all[f.slice(0, f.len() - 5)] = parseJson(readFile(ROOT + "/swarm/scenarios/" + f)) } catch e { print("bad scenario " + f) }
  }
  if DASH {
    var kv = kvGet("scenarios") || {}
    var add = false
    for k in all.keys() { if kv[k] == nil { kv[k] = all[k] add = true } }
    if add { kvPut("scenarios", kv) }
    all = kv
  }
  scenarios = all
}
fn configure() {  // false: can't run
  loadScenarios()
  var sc = {}
  if SCENARIO {
    sc = scenarios[SCENARIO]
    if sc == nil { print("no scenario " + SCENARIO + " (swarm/scenarios/, or the dashboard's)") return false }
  }
  TARGET = env("SWARM_TARGET") || sc.target || "fsql"
  if !safeName(TARGET) || readFile(ROOT + "/swarm/targets/" + TARGET + ".md") == nil { print("no target " + TARGET + " (swarm/targets/)") return false }
  MODEL = env("SWARM_MODEL") || sc.model || "claude-haiku-5-5"
  var m = sc.models || {}
  var ENV = {worker: "SWARM_MODEL_WORKER", planner: "SWARM_MODEL_PLANNER", approver: "SWARM_MODEL_APPROVER", reviewer: "SWARM_MODEL_REVIEWER"}
  for role in ROLES {
    MODELS[role] = env(ENV[role]) || m[role] || MODEL
    if PRICES[MODELS[role]] == nil { print("no price for model " + MODELS[role] + "; add it to PRICES") return false }
  }
  MINUTES = parseNum(env("SWARM_MINUTES") || str(sc.minutes || 0))
  CAP = parseNum(env("SWARM_CAP") || str(sc.cap || 20))
  NWORKERS = parseNum(env("SWARM_WORKERS") || str(sc.workers || 8))
  if CF == nil && sc.cfTests { CF = DASH }
  return true
}
fn otherSpent() {  // what the other saved runs spent
  var n = 0
  for r in savedRuns() { if r.run != ledger.run.id { n += r.spent || 0 } }
  return n
}
fn budgetLeft() { return min2(CAP, GLOBAL_CAP - otherSpent()) - ledger.spent }  // dollars this run may still spend
fn min2(a, b) { if a < b { return a } return b }
fn pollControl() {  // the dashboard's changes to this run, read every 10 s
  if time() - lastPoll < 10 { return nil }
  lastPoll = time()
  for r in ["worker", "planner", "approver", "reviewer"] { ROLE[r] = readFile(ROOT + "/swarm/prompts/" + r + ".md") || ROLE[r] }  // edits apply without a restart
  if DASH == nil { return nil }
  var sc = kvGet("scenarios")
  if sc { scenarios = sc }
  var c = kvGet("control")
  if c == nil || (control && c.rev == control.rev) { return nil }
  control = c
  if c.run != ledger.run.id { return nil }
  var l = c.live || {}
  var did = []
  if l.workers && l.workers != NWORKERS && l.workers >= 1 && l.workers <= 32 {
    NWORKERS = int(l.workers)
    addWorkers()
    did.push(str(NWORKERS) + " workers")
  }
  if l.cap != nil && l.cap != CAP { CAP = l.cap did.push("cap $" + str(CAP)) }
  if l.minutes != nil && l.minutes != MINUTES { MINUTES = l.minutes did.push("time limit " + str(MINUTES) + " min") }
  var lm = l.models || {}
  for role in ROLES {
    var mm = lm[role]
    if mm && mm != MODELS[role] {
      if PRICES[mm] == nil { did.push("unknown model " + mm + " ignored") } else { MODELS[role] = mm did.push(role + " " + mm) }
    }
  }
  if (c.pause || false) != paused {
    paused = c.pause || false
    if paused { did.push("paused") } else { did.push("resumed") }
  }
  if c.stop {  // done: a later restart of this run must not stop again
    stopReason = "stopped from the dashboard"
    c.stop = false
    c.rev = (c.rev || 0) + 1
    if kvPut("control", c) { control = c }
  }
  if did.len() > 0 { log("dashboard: " + did.join(", ")) }
}
fn idle() {  // the run is over: publish the dashboard and wait for it to start the next run (build/swarm-next)
  log("idle: start the next run from the dashboard")
  dashboard(stopReason, true)
  while readFile(DIR + "/STOP") == nil {
    var c = kvGet("control")
    if c && c.next {
      if !safeName(c.next) || scenarios[c.next] == nil { log("no scenario " + str(c.next)) }
      else if GLOBAL_CAP - otherSpent() - ledger.spent < 0.25 { log("the global budget is spent: not starting " + c.next) }
      else {
        writeFile(ROOT + "/build/swarm-next", c.next)
        log("next run: " + c.next)
        c.next = nil
        c.rev = (c.rev || 0) + 1
        kvPut("control", c)
        dashboard("starting the next run", true)
        return nil
      }
      c.next = nil
      c.rev = (c.rev || 0) + 1
      kvPut("control", c)
    }
    var sc = kvGet("scenarios")
    if sc { scenarios = sc }
    dashboard(stopReason, false)
    sleep(10)
  }
}

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
// read: a cache read's price as a fraction of input (Sonnet 5.5's is $0.10 of $2); cache writes (5 min) are 1.25x
PRICES["claude-haiku-5-5"] = {inp: 0.1, out: 0.5, longInp: 0.5, longOut: 2.5, read: 0.1, maxTok: 16000}
PRICES["claude-sonnet-5-5"] = {inp: 2, out: 10, read: 0.05, maxTok: 32000}
PRICES["claude-opus-5-5"] = {inp: 4, out: 20, read: 0.1, maxTok: 32000}
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
  // list prices: cache writes at 1.25x input, cache reads at the model's read fraction (92% of prompt tokens are reads)
  var cost = (inp + cw * 1.25 + cr * PRICES[model].read) * p.inp / 1000000 + out * p.out / 1000000
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
fn request(role, tools, messages) { return requestAs(role, MODELS[role], tools, messages) }
fn requestAs(role, model, tools, messages) { return requestWith(system(role), model, tools, messages) }
fn workerRequest(w) {  // a conversation keeps the prompt it started with: its thinking blocks are bound to that prefix
  if w.prompt == nil || w.messages.len() <= 1 { w.prompt = ROLE.worker }
  return requestWith([{type: "text", text: COMMON, cache_control: {type: "ephemeral"}}, {type: "text", text: w.prompt}], w.model || MODELS.worker, WORKER_TOOLS, w.messages)
}
fn stripThinking(w) {  // the API refused the conversation's thinking blocks: go on without them
  for m in w.messages {
    if m.role == "assistant" {  // always a list of blocks (msg.content)
      var keep = []
      for b in m.content { if b.type != "thinking" && b.type != "redacted_thinking" { keep.push(b) } }
      m.content = keep
    }
  }
}
fn requestWith(sys, model, tools, messages) {  // {body, model}
  var body = {model: model, max_tokens: PRICES[model].maxTok, system: sys, messages: messages, cache_control: {type: "ephemeral"}}
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
  tool("map", "Every module's declarations (fut map over src/*.fut). Cheap: look here first.", {}, []),
  tool("ls", "List the repository's files.", {}, []),
  tool("read", "Read a file.", {path: S}, ["path"]),
  tool("write", "Create or replace a whole file.", {path: S, content: S}, ["path", "content"]),
  tool("edit", "Replace exact text in a file: old must occur exactly once.", {path: S, old: S, new: S}, ["path", "old", "new"]),
  tool("delete", "Delete a file.", {path: S}, ["path"]),
  tool("test", "Run the tests and report failures: every tests/*_test.fut, or only the file given as path.", {path: S}, []),
  tool("blocked", "Give the task back: it can't be done now (it needs work that isn't on main yet, say). Say what it waits for.", {reason: S}, ["reason"]),
  tool("done", "Submit the task: the harness reruns the tests and a reviewer checks the diff.", {summary: S}, ["summary"])]
var PLAN_TOOLS = [tool("propose", "Propose the next tasks.", {tasks: {type: "array", items: {type: "object",
  properties: {title: S, detail: S, files: {type: "array", items: S}}, required: ["title", "detail", "files"]}}}, ["tasks"])]
var DECIDE_TOOLS = [tool("decide", "Approve or reject each proposed task, by its index.", {decisions: {type: "array", items: {type: "object",
  properties: {index: {type: "number"}, approve: {type: "boolean"}, reason: S, amendment: S}, required: ["index", "approve", "reason"]}}}, ["decisions"])]
var VERDICT_TOOLS = [tool("verdict", "Approve the change for main, or reject it with specific feedback.",
  {approve: {type: "boolean"}, feedback: S}, ["approve", "feedback"])]

// ---- tests ----
var CFTOKEN = nil
fn cfToken() {
  if CFTOKEN == nil { CFTOKEN = (readFile(env("SWARM_CF_TOKEN_FILE") || DIR + "/.cftoken") || "").trim() }
  return CFTOKEN
}
fn cfRun(dir, files) {  // each test file in its own request to the Worker, all at once; results shaped as exec's
  cfToken()
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
fn runTests(dir, only) {
  var files = testFiles(dir)
  if only { files = [only] }
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
    for k in 0..res.len() {  // a file that fails is run once more on its own: timing checks fail under load
      var o = res[k].out
      if !res[k].timedOut && (o.has("\"status\":\"fail\"") || o.has("\"status\":\"error\"")) {
        var again = exec(runs[k])
        if !again.timedOut && !again.out.has("\"status\":\"fail\"") && !again.out.has("\"status\":\"error\"") { res[k] = again }
      }
    }
  }
  var n = {passed: 0, failed: 0, errors: 0, pending: 0, broken: 0}
  var bad = []
  var pend = []
  var fails = []  // keys of the tests that didn't pass ("file: name", or "file: (file)" for the whole file)
  var pends = []
  for k in 0..res.len() {
    var r = res[k]
    if r.timedOut { n.broken += 1 bad.push(files[k] + ": the test run timed out") fails.push(files[k] + ": (file)") continue }
    var seen = false
    for l in lines(r.out) {
      if l[0] != "\{" { continue }
      var j = nil
      try { j = parseJson(l) } catch e { continue }
      seen = true
      if j.type == "test" {
        if j.status == "pass" { n.passed += 1 }
        else if j.status == "pending" { n.pending += 1 pend.push(j.name + ": " + (j.reason || "")) pends.push(files[k] + ": " + j.name) }
        else if j.status == "fail" {
          n.failed += 1
          fails.push(files[k] + ": " + j.name)
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
          fails.push(files[k] + ": " + j.name)
          bad.push("ERROR " + j.name + " (" + j.file + ":" + str(j.line) + "): " + (j.message || "") + "\n    " + clip(json(j.trace || ""), 600))
        }
      } else if j.type == "compile_error" {
        n.broken += 1
        fails.push(files[k] + ": (file)")
        bad.push("COMPILE ERROR " + j.file + ":" + str(j.line) + ":" + str(j.col) + ": " + j.message + "\n    " + (j.text || ""))
      } else if j.type == "error" {
        n.broken += 1
        fails.push(files[k] + ": (file)")
        bad.push("ERROR in top-level code of " + files[k] + ": " + j.message + "\n    " + clip(json(j.trace || ""), 600))
      }
    }
    if !seen && r.code != 0 { n.broken += 1 fails.push(files[k] + ": (file)") bad.push(files[k] + ": exit " + str(r.code) + "\n" + clip(r.err, 1500)) }
  }
  var summary = str(n.passed) + " passed, " + str(n.failed) + " failed, " + str(n.errors) + " errors, " + str(n.pending) + " pending, " + str(n.broken) + " broken files"
  var report = summary
  if bad.len() > 0 { report = report + "\n" + clip(bad.join("\n"), 7000) }
  if pend.len() > 0 { report = report + "\npending:\n  " + clip(pend.join("\n  "), 1500) }
  return {ok: n.failed == 0 && n.errors == 0 && n.broken == 0, report: report, summary: summary, n: n, fails: fails, pends: pends}
}
// The merge rule: nothing that passes on main may break. Tests that fail or are pending on main may fail (a pending
// test fails once what it waits for exists: that shows a gap, it doesn't block the change that exposed it).
fn regressions(r) {
  var known = {}
  for k in mainTests.fails || [] { known[k] = true }
  for k in mainTests.pends || [] { known[k] = true }
  var out = []
  for k in r.fails || [] { if !known[k] { out.push(k) } }
  return out
}
fn gateReport(r) {
  var reg = regressions(r)
  if reg.len() == 0 && !r.ok { return r.report + "\n\nNone of these failures blocks you: each also fails, or is pending, on main." }
  if reg.len() > 0 && reg.len() < (r.fails || []).len() { return r.report + "\n\nThese block (they pass on main, or are new): " + clip(reg.join("; "), 1500) + ". The other failures also fail, or are pending, on main." }
  return r.report
}

// ---- tasks ----
fn ready() {  // queued tasks a worker can take: one given back as blocked waits until something merges and the tasks
  var out = []  // in progress when it was given back (one of them may be what it waits for) are finished
  for t in byStatus("queued") {
    if t.blockedAt == nil { out.push(t) continue }
    if t.blockedAt == ledger.merged { continue }
    var waiting = false
    for id in t.waitFor || [] { for u in tasks { if u.id == id && u.status == "active" { waiting = true } } }
    if !waiting { out.push(t) }
  }
  return out
}
fn byStatus(s) {
  var out = []
  for t in tasks { if t.status == s { out.push(t) } }
  return out
}
// fut map of every src/*.fut, each on its own, whatever all.fut says: agents let all.fut go stale (modules with clashing
// names can't all be imported into one file), then took modules that are on main for missing
fn mapAll(dir) {
  var files = lines(sh("git ls-files -co --exclude-standard 'src/*.fut'", dir).out)
  var cmds = []
  for f in files { cmds.push({cmd: [FUT, "map", f, f], cwd: dir, timeout: 60}) }
  var out = ""
  var first = true
  for r in execAll(cmds) {
    for l in (r.out + r.err).split("\n") {  // not lines(): the map's indentation is its structure
      if l.trim() == "" { continue }
      if l.find("pkg ") == 0 { if !first { continue } first = false }
      out += l + "\n"
    }
  }
  return {code: 0, out: out, err: ""}
}
fn titles(list) {
  var out = []
  for t in list {
    var line = "#" + str(t.id) + " " + t.title + " [" + t.files.join(", ") + "]"
    if t.blockedAt != nil && t.notes && t.notes.len() > 0 { line += "\n    last attempt: " + clip(t.notes[t.notes.len() - 1], 400) }
    out.push(line)
  }
  if out.len() == 0 { return "(none)" }
  return out.join("\n")
}
fn saveState() {
  dashboard(nil, false)
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
  w.model = MODELS.worker   // a conversation keeps its model: thinking blocks belong to the model that wrote them
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
  w.stripped = false
}
fn runTool(w, name, inp) {
  var wt = w.wt
  if name == "map" {
    var r = mapAll(wt)
    if r.code != 0 { return {text: "fut map failed:\n" + clip(r.err + r.out, 3000), err: true} }
    return {text: clip(r.out, 14000)}
  }
  if name == "ls" { return {text: clip(sh("git ls-files -co --exclude-standard", wt).out, 6000)} }
  if !safePath(inp.path) && (name == "read" || name == "write" || name == "edit" || name == "delete") { return {text: "path must be relative, inside the repository", err: true} }
  if name == "delete" {
    if readFile(wt + "/" + inp.path) == nil { return {text: "no such file: " + inp.path, err: true} }
    exec({cmd: ["rm", "-f", "--", inp.path], cwd: wt})
    return {text: "deleted " + inp.path}
  }
  if name == "blocked" {
    var t = w.task
    t.blockedAt = ledger.merged
    t.waitFor = []
    for u in tasks { if u.status == "active" && u.id != t.id { t.waitFor.push(u.id) } }
    t.blocked = (t.blocked || 0) + 1
    if t.blocked < 6 { t.attempts -= 1 }  // waiting on other work is not a failed attempt (up to 5 times)
    giveUp(w, "the worker says it is blocked: " + clip(inp.reason || "", 600))
    return {text: "task given back"}
  }
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
    if inp.path && (!safePath(inp.path) || readFile(wt + "/" + inp.path) == nil) { return {text: "no such test file: " + inp.path, err: true} }
    var r = runTests(wt, inp.path)
    return {text: gateReport(r), err: regressions(r).len() > 0}
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
  var r = runTests(w.wt, nil)
  if regressions(r).len() > 0 { return {text: "not submitted: the tests fail.\n" + gateReport(r), err: true} }
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
    w.idleReplies = (w.idleReplies || 0) + 1
    var said = ""
    for b in msg.content { if b.type == "text" { said += b.text } }
    if w.idleReplies == 1 || w.idleReplies % 10 == 0 {
      log("w" + str(w.i) + " replied without a tool (" + str(w.idleReplies) + " in a row, stop: " + str(msg.stop_reason) + "): " + clip(said, 200))
    }
    if w.idleReplies >= 5 || w.steps >= MAXSTEPS { return giveUp(w, str(w.idleReplies) + " replies in a row without using a tool") }
    var nudge = "Use the tools to continue, and call done when the task is complete and the tests pass."
    if msg.stop_reason == "max_tokens" { nudge = "Your reply was cut off at the output limit. Work in smaller steps: write files in parts or use edit." }
    w.messages.push({role: "user", content: nudge})
    return nil
  }
  w.idleReplies = 0
  var results = []
  for u in uses {
    if w.phase != "work" { break }  // it gave the task back (blocked) or submitted it
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
  var r = runTests(w.wt, nil)
  if regressions(r).len() > 0 {
    w.phase = "work"
    w.messages.push({role: "user", content: "Approved, but after rebasing onto the latest main the tests fail:\n" + gateReport(r)})
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
  w.stripped = false
  w.reviews = 0
}

// ---- planning ----
fn context() {
  var m = mapAll(WORK)
  var recent = rejections
  if recent.len() > 12 { recent = recent.slice(recent.len() - 12, recent.len()) }
  return "Project map (main):\n" + clip(m.out + m.err, 14000) + "\n\nmain's tests: " + mainTests.report +
    "\n\nDone:\n" + titles(byStatus("done")) + "\n\nIn progress:\n" + titles(byStatus("active")) +
    "\n\nQueued:\n" + titles(byStatus("queued")) + "\n\nFailed:\n" + titles(byStatus("failed")) +
    "\n\nRecent rejections:\n" + recent.join("\n")
}
fn planRequest(n) {
  return request("planner", PLAN_TOOLS, [{role: "user", content: context() + "\n\n" + str(n) + " workers are waiting for work: propose " + str(n) +
    " next tasks (fewer only if there are not that many independent ones). None may repeat a task that is done or queued."}])
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
  var r = runTests(dir, nil)
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
  var out = {run: ledger.run.id, scenario: SCENARIO, target: TARGET, fut: ledger.run.fut, models: MODELS, workers: NWORKERS, cap: CAP, minutes: MINUTES,
    elapsed: int(ledger.elapsed), spent: int(ledger.spent * 10000) / 10000, byModel: ledger.byModel, calls: ledger.calls,
    merged: ledger.merged, rejected: ledger.rejected, failedTasks: byStatus("failed").len(), tests: mainTests.n.passed,
    accept: accept, srcLines: srcLines, testLines: testLines, checkpoints: checks, timeline: ledger.timeline}
  writeFile(RESULTS() + "/" + ledger.run.id + ".json", json(out) + "\n")
  runs = nil
}
fn RESULTS() { return env("SWARM_RESULTS") || ROOT + "/swarm/results" }
var runs = nil          // the saved runs (nil: read them again)
fn savedRuns() {
  if runs == nil {
    runs = []
    for f in lines(sh("ls *.json 2>/dev/null", RESULTS()).out) {
      var r = nil
      try { r = parseJson(readFile(RESULTS() + "/" + f)) } catch e { continue }
      runs.push(r)
    }
  }
  return runs
}
fn dashboard(stopped, force) {  // $SWARM_DIR/dashboard.html: swarm/dashboard.html with this run's data in it
  var ws = []
  for w in workers {
    var t = nil
    if w.task { t = {id: w.task.id, title: w.task.title} }
    ws.push({i: w.i, phase: w.phase, task: t, steps: w.steps})
  }
  var ts = []
  for t in tasks { ts.push({id: t.id, title: t.title, status: t.status, attempts: t.attempts, files: t.files}) }
  var counts = {}
  for k in ["active", "queued", "done", "failed"] { counts[k] = byStatus(k).len() }
  var d = {now: time(), run: ledger.run, target: TARGET, models: MODELS, nworkers: NWORKERS, cap: CAP, minutes: MINUTES,
    elapsed: ledger.elapsed, spent: ledger.spent, byModel: ledger.byModel, calls: ledger.calls, merged: ledger.merged,
    rejected: ledger.rejected, tests: mainTests.n, accept: accept, counts: counts, tasks: ts, workerList: ws, events: events,
    timeline: ledger.timeline, runs: savedRuns(), stopped: stopped, paused: paused, scenario: SCENARIO, scenarios: scenarios,
    control: control, globalCap: GLOBAL_CAP, globalSpent: otherSpent() + ledger.spent, knownModels: PRICES.keys(),
    targets: lines(sh("ls *.md 2>/dev/null | sed 's/[.]md$//'", ROOT + "/swarm/targets").out), dash: DASH != nil}
  var page = readFile(ROOT + "/swarm/dashboard.html").split("/*DATA*/").join(json(d).split("</").join("<\\/"))
  writeFile(DIR + "/dashboard.html", page)
  if DASH && (force || time() - lastPush >= 15) {  // the Worker serves it at / and at /<run>
    lastPush = time()
    var h = {authorization: "Bearer " + cfToken(), "content-type": "text/html"}
    var got = fetchAll([{url: DASH + "/pages/index", method: "PUT", body: page, headers: h, timeout: 20},
                        {url: DASH + "/pages/" + ledger.run.id, method: "PUT", body: page, headers: h, timeout: 20}])
    if got[0].status != 200 { log("dashboard push failed: " + str(got[0].status) + " " + clip(got[0].body || got[0].error || "", 200)) }
  }
}

// ---- the loop ----
fn addWorkers() {  // up to NWORKERS, each with its worktree; fewer: the extra ones finish their task and stop
  for i in workers.len()..NWORKERS {
    if readFile(DIR + "/wt/w" + str(i) + "/README.md") == nil {
      sh("git worktree add -q -B w" + str(i) + " " + DIR + "/wt/w" + str(i) + " main", WORK)
    }
    workers.push(newWorker(i))
  }
}
fn setup() {
  TDIR = ROOT + "/swarm/targets/" + TARGET
  if readFile(WORK + "/README.md") == nil {
    writeFile(WORK + "/README.md", readFile(TDIR + ".md"))
    writeFile(WORK + "/all.fut", "// imports every module, so `fut map all.fut` shows the project\n")
    writeFile(WORK + "/.gitignore", "*.api\n.fut-cache/\n")
    sh("git init -q -b main && git config user.name swarm && git config user.email swarm@localhost && git add -A && git commit -qm 'project start'", WORK)
  }
  addWorkers()
  var spec = readFile(ROOT + "/docs/spec.md")
  COMMON = readFile(ROOT + "/swarm/prompts/common.md").split("\{\{SPEC}}").join(spec).split("\{\{TARGET}}").join(readFile(TDIR + ".md"))
  for r in ["worker", "planner", "approver", "reviewer"] { ROLE[r] = readFile(ROOT + "/swarm/prompts/" + r + ".md") }
}

fn main() {
  if KEY == nil || DIR == nil || ROOT == nil || FUT == nil { print("set SWARM_API_KEY, SWARM_DIR, SWARM_ROOT and FUT (swarm/run.sh does)") return nil }
  if !configure() { return nil }
  setup()
  loadState()
  if ledger.run == nil {
    ledger.run = {id: (SCENARIO || TARGET) + "-" + sh("date -u +%Y%m%d-%H%M", ROOT).out.trim(), fut: sh("git rev-parse --short HEAD", ROOT).out.trim()}
  }
  ledger.run.scenario = SCENARIO
  mainTests = runTests(WORK, nil)
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
    if readFile(DIR + "/STOP") != nil { log("STOP file: stopping") stopReason = "STOP file" break }
    if MINUTES > 0 && ledger.elapsed >= MINUTES * 60 { log("time limit: " + str(MINUTES) + " minutes; stopping") stopReason = "time limit" break }
    pollControl()
    if stopReason { log(stopReason) break }
    if paused { saveState() sleep(5) last = time() continue }
    var merged = ledger.merged
    ledger.rounds += 1
    // idle workers take queued tasks
    for w in workers {  // a task resumes on the worker whose worktree has it
      if w.phase != "idle" || w.i >= NWORKERS { continue }
      for t in byStatus("queued") { if t.worker == w.i { startTask(w, t) break } }
    }
    for w in workers {
      if w.phase != "idle" || w.i >= NWORKERS { continue }
      var q = []
      for t in ready() { if t.worker == nil || t.worker >= NWORKERS { q.push(t) } }
      if q.len() > 0 { startTask(w, q[0]) }
    }
    // this round's requests
    var reqs = []
    var who = []
    for w in workers {
      if w.phase == "work" { reqs.push(workerRequest(w)) who.push({kind: "worker", w: w}) }
      else if w.phase == "review" { reqs.push(reviewRequest(w)) who.push({kind: "review", w: w}) }
    }
    var idle = 0
    for w in workers { if w.phase == "idle" && w.i < NWORKERS { idle += 1 } }
    if proposals.len() > 0 { reqs.push(decideRequest()) who.push({kind: "decide"}) }
    else if planWait > 0 { planWait -= 1 }
    else if ready().len() < idle + 2 {
      var n = idle + 2
      if n < 3 { n = 3 }
      reqs.push(planRequest(n))
      who.push({kind: "plan"})
    }
    // the budget: drop what could cross the cap
    var room = budgetLeft() - 0.05 * CAP  // a margin: requests are also checked at their worst case below
    var keep = []
    var keepWho = []
    for k in 0..reqs.len() {
      var c = worstCase(reqs[k])
      if c <= room { room -= c keep.push(reqs[k]) keepWho.push(who[k]) }
    }
    if keep.len() == 0 {
      if reqs.len() > 0 { log("budget reached: $" + str(ledger.spent) + " (cap $" + str(CAP) + ", all runs $" + str(GLOBAL_CAP) + "); stopping") stopReason = "budget" break }
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
        if r.status == 400 && h.kind == "worker" {
          if (r.body || "").has("thinking") && !h.w.stripped { stripThinking(h.w) h.w.stripped = true log("w" + str(h.w.i) + ": dropped its thinking blocks, which the API refused") }
          else { giveUp(h.w, "the API rejected the conversation (400)") }
        }
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
    var lastMark = ledger.timeline[ledger.timeline.len() - 1]
    if ledger.merged == merged && ledger.elapsed - lastMark.t >= 60 { mark() }
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
  dashboard(stopReason, true)
  if DASH && stopReason != "STOP file" { idle() }
}
