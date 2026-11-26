// Driver: run a file, hot-patch it on save (-w), dump its machine code (--asm), or start a REPL.
#include "fut.h"
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mach-o/dyld.h>

// The standard library: $FUT_STD, else std/ beside the directory holding this binary (build/fut -> std/).
static void findStd() {
  if (const char* e = getenv("FUT_STD"); e && *e) return void(stdRoot = canonicalPath(e));
  char b[4096];
  uint32_t n = sizeof b;
  if (_NSGetExecutablePath(b, &n)) return;
  char real[PATH_MAX];
  if (!realpath(b, real)) return;
  std::string d = real;
  d = d.substr(0, d.rfind('/'));
  d = d.substr(0, d.rfind('/')) + "/std";
  struct stat st;
  if (!stat(d.c_str(), &st) && S_ISDIR(st.st_mode)) stdRoot = d;
}

static std::string path;
static pthread_mutex_t watchMu = PTHREAD_MUTEX_INITIALIZER;
static std::vector<std::string> watched;  // the program file and everything it imports
static std::vector<std::string> inbox;    // lines typed into a running program (both guarded by watchMu)
static std::string replBuf;               // REPL input so far (more lines may follow)
static bool inMain;
static VM* mainVM;  // the watcher threads set its poll flag (vm is each thread's own)
static int fileChanged;

static Compiled load(bool reload) {
  double t0 = now();
  try {
    Compiled c = compile(path, reload, false);
    pthread_mutex_lock(&watchMu), watched = c.files, pthread_mutex_unlock(&watchMu);
    if (reload) fprintf(stderr, "[fut] %s (%.2f ms, %zu KB code)\n", c.log.c_str(), (now() - t0) * 1e3, jitLive() >> 10);
    return c;
  } catch (std::exception& e) {
    fflush(stdout);
    fprintf(stderr, "%s\n", e.what());
    return {};
  }
}
static int run(void* code) {  // 0 ok, 1 runtime error, 2 restart requested
  jmp_buf jb, *outer = errJmp;
  void* handler = vm->handler;  // REPL lines typed into a running program must not land in its try blocks
  u32 txn = vm->txn;            // an uncaught error rolls back the transactions opened since
  int st = 0;
  u32 eff = effEnter();        // unchecked, with no handlers, whatever the code it interrupts may do
  errJmp = &jb, vm->handler = nullptr;
  if (int r = setjmp(jb)) st = r, txnUnwind(txn);
  else vmCall(code);
  errJmp = outer, vm->handler = handler, effLeave(eff);
  return st;
}
static Global* fnNamed(int sym) {
  Global* g = findGlobal(sym);
  return g && g->kind == 'f' && g->o ? g : nullptr;
}
static int start(Compiled& c) {  // run the top level, then main() (rerun when a reload changes it)
  int st = run(c.chunk);
  jitRetire((u32*)c.chunk), inMain = true;
  for (Global* m; !st && (m = fnNamed(S_main)) && (st = run(m->stub)) == 2; st = 0) fputs("[fut] main() changed: restarting it\n", stderr);
  inMain = false;
  return st;
}
// ---- --isolates N: the same code in N isolates at once, each on a thread of its own ----
// Isolates share no mutable state, so each must behave exactly as it would alone: the same output, the same end.
struct IsoJob { std::vector<void*> code; std::string out, err; int st = 0; };
static void* isolateMain(void* p) {
  IsoJob& j = *(IsoJob*)p;
  vm = vmNew();
  Heap* h = heapNew(vm->runs);
  heapUse(h), threadInit();
  for (auto& g : globals) if (g.slot >= 0 && g.o) vm->globals[g.slot] = box(g.o);  // its functions and classes
  outSink = &j.out, errSink = &j.err;
  for (void* c : j.code) if ((j.st = run(c))) break;
  outSink = errSink = nullptr;
  heapFree(h), vmFree(vm), vm = nullptr;
  return nullptr;
}
static std::vector<IsoJob> inIsolates(int n, const std::vector<void*>& code) {
  std::vector<IsoJob> jobs(n, IsoJob{code});
  std::vector<pthread_t> th(n);
  pthread_attr_t a;
  pthread_attr_init(&a), pthread_attr_setstacksize(&a, 64 << 20);
  isolates = n + 1;  // this one too: it waits, but its stack holds code
  for (int i = 0; i < n; i++) pthread_create(&th[i], &a, isolateMain, &jobs[i]);
  for (auto t : th) pthread_join(t, nullptr);
  isolates = 1;
  return jobs;
}
static std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }
static std::string differs(const std::vector<IsoJob>& jobs) {  // how isolate k (if any) ended up unlike the first
  for (size_t k = 1; k < jobs.size(); k++)
    if (jobs[k].out != jobs[0].out || jobs[k].st != jobs[0].st || firstLine(jobs[k].err) != firstLine(jobs[0].err))
      return "isolate " + std::to_string(k) + " ended differently: " + (jobs[k].st ? "error: " + firstLine(jobs[k].err) : "ok") +
             (jobs[k].out != jobs[0].out ? " (different output)" : "");
  return "";
}
static int startIsolates(Compiled& c, int n) {  // the program: its top level, then main()
  std::vector<void*> code{c.chunk};
  if (Global* m = fnNamed(S_main)) code.push_back(m->stub);
  auto jobs = inIsolates(n, code);
  jitRetire((u32*)c.chunk);
  fwrite(jobs[0].out.data(), 1, jobs[0].out.size(), stdout), fflush(stdout);
  if (jobs[0].st) fprintf(stderr, "runtime error: %s", jobs[0].err.c_str());
  if (std::string d = differs(jobs); !d.empty()) return fprintf(stderr, "[fut] %s\n", d.c_str()), 1;
  return jobs[0].st ? 1 : 0;
}

static void eval(bool more) {  // compile and run replBuf, unless it is unfinished and `more` may follow
  try {
    Compiled c = compile("<repl>", false, true, &replBuf);
    run(c.chunk), jitRetire((u32*)c.chunk);
  } catch (CompileError& e) {
    if (e.atEnd && more) return;
    fprintf(stderr, "%s\n", e.what());
  } catch (std::exception& e) { fprintf(stderr, "%s\n", e.what()); }
  replBuf.clear(), fflush(stdout);
}
static void evalInbox() {
  pthread_mutex_lock(&watchMu);
  std::vector<std::string> lines = std::move(inbox);
  inbox.clear(), pthread_mutex_unlock(&watchMu);
  for (auto& l : lines) replBuf += l, eval(l.size() > 1);  // an empty line gives up on unfinished input
}
// Runs at a safepoint (loop back-edge) on the main thread when a watched file changed or a line was typed. Inside
// a transaction (atomic { }) both wait for it to end.
static void hotReload() {
  vm->poll = 0;
  if (vm->txn) return void(txnPollLater = true);
  evalInbox();
  if (!__atomic_exchange_n(&fileChanged, 0, __ATOMIC_ACQUIRE)) return;
  Compiled c = load(true);
  if (!c.chunk) return;  // compile error: keep running the old code
  // The new top level and the reload() hook (which migrates state) run as one transaction. If either fails, the
  // reload is undone: the data they changed, then the code. The program goes on as if the file had not changed.
  txnBegin();
  int st = run(c.chunk);
  jitRetire((u32*)c.chunk);
  if (Global* r = fnNamed(S_reload); !st && r && ((Fn*)r->o)->arity == 0) st = run(r->stub);
  if (st) {
    txnAbort();
    std::string err;
    if (revertReload(c.newVars, &err)) fputs("[fut] reload rolled back: the program's data and code are as they were before it\n", stderr);
    else fprintf(stderr, "[fut] reload rolled back its data, but the previous code did not compile again: %s\n", err.c_str());
    return;
  }
  txnCommit();
  if (c.mainChanged && inMain) c = {}, longjmp(*errJmp, 2);  // unwind and rerun main()
}
static long long stamp() {
  pthread_mutex_lock(&watchMu);
  long long s = 0;
  for (auto& f : watched) {
    struct stat st;
    if (!stat(f.c_str(), &st)) s = s * 31 + st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
  }
  return pthread_mutex_unlock(&watchMu), s;
}
static void waitChange() {  // typed lines still run meanwhile, so the stopped program's state can be inspected
  for (;; gfxPump(), usleep(50000))
    if (__atomic_exchange_n(&vm->poll, 0, __ATOMIC_ACQUIRE) && (evalInbox(), __atomic_exchange_n(&fileChanged, 0, __ATOMIC_ACQUIRE))) return;
}

// One test in a child process (fork, after the top level ran): its globals, a crash or a hang stay in it.
// The child sends its report (status letter, then the text) back over a pipe.
static void isolated(const TestCase& t, void* code, double timeout) {
  int p[2];
  fflush(stdout), fflush(stderr);
  if (pipe(p)) return testFinish(t, true, "cannot create a pipe for the test process\n");
  pid_t pid = fork();
  if (pid < 0) return close(p[0]), close(p[1]), testFinish(t, true, "cannot start the test process\n");
  if (!pid) {
    jitForked(), close(p[0]);
    std::string err;
    errSink = &err;
    char status;
    int st = run(code);
    std::string text = testReport(t, st != 0, err, status);
    text.insert(text.begin(), status);
    for (size_t o = 0; o < text.size();) {
      ssize_t n = write(p[1], text.data() + o, text.size() - o);
      if (n <= 0) break;
      o += n;
    }
    _exit(0);
  }
  close(p[1]);
  std::string got;
  bool timedOut = false;
  int st = reap(pid, {{p[0], &got}}, timeout, timedOut);
  if (!got.empty()) return testRecord(got[0], got.substr(1));
  char why[96];
  if (timedOut) snprintf(why, sizeof why, "timed out after %g s\n", timeout);
  else if (WIFSIGNALED(st)) snprintf(why, sizeof why, "the test process crashed: %s\n", strsignal(WTERMSIG(st)));
  else snprintf(why, sizeof why, "the test process exited (status %d) without a report\n", WEXITSTATUS(st));
  testFinish(t, true, why);
}
// fut test: the top level runs once, then each `test` block in order (main() is not called), each in its own process.
static int runTests(const TestRun& o) {
  testsBegin(o);
  Compiled c;
  try {
    c = compile(path, false, false, nullptr, true);
  } catch (std::exception& e) { return reportCompileError(e.what()), 2; }
  if (o.isolates) {  // each test in o.isolates isolates at once, each running the top level first
    int bad = 0;
    for (auto& t : c.tests) {
      if (!t.pending.empty() || (!o.filter.empty() && t.name.find(o.filter) == std::string::npos)) continue;
      auto jobs = inIsolates(o.isolates, {c.chunk, t.code});
      std::string why = jobs[0].st ? "error: " + firstLine(jobs[0].err) : differs(jobs);
      printf("%s%s%s%s\n", why.empty() ? "ok      " : "FAIL    ", t.name.c_str(), why.empty() ? "" : "\n  ", why.c_str());
      bad += !why.empty();
    }
    jitRetire((u32*)c.chunk);
    printf("%s (%d isolates each)\n", bad ? (std::to_string(bad) + " failed").c_str() : "all passed", o.isolates);
    return bad ? 1 : 0;
  }
  std::string err;
  errSink = &err;
  int st = testsToRun(c.tests) ? run(c.chunk) : 0;  // --changed with every test cached: nothing runs, not even the top level
  jitRetire((u32*)c.chunk);
  // A public interface out of date fails the run too, reported last so it is what the reader sees with the status.
  auto api = [&](int st) { return reportInterfaces(checkInterfaces(c), o.json, path) ? std::max(st, 1) : st; };
  if (st) return errSink = nullptr, reportTopError(err), api(1);
  for (auto& t : c.tests)
    if (void* code = testStart(t)) isolated(t, code, o.timeout);
  errSink = nullptr;
  return api(testsEnd());
}
// fut api [--check] file.fut: write (or with --check, just report) the .api file of each file it compiles
static int apiCommand(bool check) {
  Compiled c;
  try {
    c = compile(path, false, false);
  } catch (std::exception& e) { return fflush(stdout), fprintf(stderr, "%s\n", e.what()), 2; }
  if (check) return reportInterfaces(checkInterfaces(c), false, path) ? 1 : 0;
  return writeInterfaces(c, path) < 0 ? 1 : 0;  // a breaking change without a version bump is held back
}
// fut map [--pub] [--json] file.fut [package | file]: the packages, files and declarations the program uses
static int mapCommand(bool pubOnly, bool json, const std::string& only) {
  Compiled c;
  try {
    c = compile(path, false, false);
  } catch (std::exception& e) { return fflush(stdout), fprintf(stderr, "%s\n", e.what()), 2; }
  return printMap(c, pubOnly, json, only, path) || only.empty() ? 0 : (fprintf(stderr, "nothing matches '%s'\n", only.c_str()), 1);
}

static int repl() {
  char* line = nullptr;
  size_t cap = 0;
  for (ssize_t n;;) {
    fputs(replBuf.empty() ? "> " : ". ", stdout), fflush(stdout);
    if ((n = getline(&line, &cap, stdin)) < 0) return puts(""), 0;
    replBuf.append(line, n), eval(n > 1);
  }
}
static int dumpAsm() {  // assemble the emitted words with clang and disassemble with otool
  std::string listing = ".text\n", tmp = "/tmp/fut-asm-" + std::to_string(getpid());
  asmListing = &listing;
  Compiled c = load(false);
  asmListing = nullptr;
  if (!c.chunk) return 1;
  FILE* f = fopen((tmp + ".s").c_str(), "w");
  fputs(listing.c_str(), f), fclose(f);
  std::string cmd = "clang -c -arch arm64 " + tmp + ".s -o " + tmp + ".o && otool -tV " + tmp + ".o | tail -n +2; rm -f " + tmp + ".s " + tmp + ".o";
  return system(cmd.c_str()) ? 1 : 0;
}

int main(int argc, char** argv) {
  findStd();
  bool watch = false, asmMode = false, testMode = argc > 1 && std::string(argv[1]) == "test";
  bool apiMode = argc > 1 && std::string(argv[1]) == "api", apiCheck = false;
  bool mapMode = argc > 1 && std::string(argv[1]) == "map", mapPub = false, mapJson = false;
  std::string mapOnly;
  TestRun topts;
  const char* usage = "usage: fut [-w | -v | --asm | --isolates n] [file.fut]   (no file: REPL)\n"
                      "       fut test [--changed] [--json] [--filter text] [--timeout s] [--isolates n] file.fut\n"
                      "       fut api [--check] file.fut\n       fut map [--pub] [--json] file.fut [package | file]\n";
  for (int i = 1 + (testMode || apiMode || mapMode); i < argc; i++) {
    std::string a = argv[i];
    if (mapMode) {
      if (a == "--pub") mapPub = true;
      else if (a == "--json") mapJson = true;
      else if (a[0] == '-') return fputs(usage, stderr), 2;
      else (path.empty() ? path : mapOnly) = a;
    } else if (apiMode) {
      if (a == "--check") apiCheck = true;
      else if (a[0] == '-') return fputs(usage, stderr), 2;
      else path = a;
    } else if (a == "--isolates" && i + 1 < argc && atoi(argv[i + 1]) > 0 && !WASM) topts.isolates = atoi(argv[++i]);
    else if (testMode && a == "--json") topts.json = true;
    else if (testMode && a == "--filter" && i + 1 < argc) topts.filter = argv[++i];
    else if (testMode && a == "--changed") topts.changed = true;
    else if (testMode && a == "--timeout" && i + 1 < argc && atof(argv[i + 1]) > 0) topts.timeout = atof(argv[++i]);
    else if (a == "-w" && !testMode) watch = verbose = true;
    else if (a == "-v") verbose = true;
    else if (a == "--asm") asmMode = true;
    else if (a[0] == '-') return fputs(usage, stderr), 2;
    else path = a;
  }
  rtInit();
  if ((testMode || apiMode || mapMode) && path.empty()) return fputs(usage, stderr), 2;
  if (mapMode) return mapCommand(mapPub, mapJson, mapOnly);
  if (testMode) return runTests(topts);
  if (apiMode) return apiCommand(apiCheck);
  if (path.empty()) return repl();
  if (asmMode) return dumpAsm();
  watched = {path};
  Compiled c = load(false);
  if (topts.isolates && !watch) return c.chunk ? startIsolates(c, topts.isolates) : 1;
  if (watch) {  // started after the first load, so the baseline covers every imported file
    // The watcher only raises a flag; the main thread recompiles at its next safepoint.
    pollHook = hotReload, mainVM = vm;
    pthread_t th;
    pthread_create(&th, nullptr, [](void*) -> void* {
      for (long long t = stamp(), n;; usleep(100000))
        if ((n = stamp()) != t) usleep(30000), t = stamp(), __atomic_store_n(&fileChanged, 1, __ATOMIC_RELAXED), __atomic_store_n(&mainVM->poll, 1, __ATOMIC_RELEASE);
    }, nullptr);
    pthread_create(&th, nullptr, [](void*) -> void* {  // a REPL into the running program: lines run at the next safepoint
      char* line = nullptr;
      size_t cap = 0;
      for (ssize_t n; (n = getline(&line, &cap, stdin)) > 0; __atomic_store_n(&mainVM->poll, 1, __ATOMIC_RELEASE))
        pthread_mutex_lock(&watchMu), inbox.emplace_back(line, n), pthread_mutex_unlock(&watchMu);
      return nullptr;
    }, nullptr);
  }
  bool loaded = c.chunk;
  int st = loaded ? start(c) : 1;
  while (st && watch) {  // after an error: wait for a fix, patch it in, rerun main()
    fputs("[fut] waiting for changes...\n", stderr);
    waitChange();
    if ((c = load(loaded)).chunk) loaded = true, st = start(c);
  }
  fflush(stdout);
  return st ? 1 : 0;
}
