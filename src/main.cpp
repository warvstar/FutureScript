// Driver: run a file, hot-patch it on save (-w), dump its machine code (--asm), or start a REPL.
#include "compile.h"
#include "fmt.h"
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
extern char** environ;
int lspCommand();

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
// (leaked: the watcher threads still read them while exit() runs destructors)
static std::vector<std::string>& watched = *new std::vector<std::string>;  // the program file and everything it imports
static std::vector<std::string>& inbox = *new std::vector<std::string>;    // lines typed into a running program (both guarded by watchMu)
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
// The program ended (st: 0, or 1 after an uncaught error): wait for the isolates it started, stopping them if it
// failed. A deadlock among them, or one that failed with nothing joining it, fails the program.
static int finish(int st, std::string* to = nullptr) {
  std::string r = isolatesFinish(st != 0);
  if (r.empty() || st) return st;
  if (to) *to = r + "\n";
  else fflush(stdout), fprintf(stderr, "runtime error: %s\n", r.c_str());
  return 1;
}
static int start(Compiled& c) {  // run the top level, then main() (rerun when a reload changes it)
  int st = run(c.chunk);
  jitRetire((u32*)c.chunk), inMain = true;
  for (Global* m; !st && (m = fnNamed(S_main)) && (st = run(m->stub)) == 2; st = 0) fputs("[fut] main() changed: restarting it\n", stderr);
  inMain = false;
  return finish(st);
}
// ---- --isolates N: the same code in N isolates at once, each on a thread of its own ----
// Isolates share no mutable state, so each must behave exactly as it would alone: the same output, the same end.
struct IsoJob { std::vector<void*> code; std::string out, err; int st = 0; const TestCase* test = nullptr; char status; std::string report; };
static void* isolateMain(void* p) {
  IsoJob& j = *(IsoJob*)p;
  vm = vmNew();
  Heap* h = heapNew(vm->runs);
  heapUse(h), threadInit(), isolateRoot();
  for (auto& g : globals) if (g.slot >= 0 && g.o) vm->globals[g.slot] = box(g.o);  // its functions and classes
  outSink = &j.out, errSink = &j.err;
  for (void* c : j.code) if ((j.st = run(c))) break;
  j.st = finish(j.st, &j.err);
  if (j.test) j.report = testReport(*j.test, j.st, j.err, j.status, &j.out);
  outSink = errSink = nullptr;
  heapFree(h), vmFree(vm), vm = nullptr;
  return nullptr;
}
static std::vector<IsoJob> inIsolates(int n, const std::vector<void*>& code, const TestCase* test = nullptr) {
  std::vector<IsoJob> jobs(n, IsoJob{code});
  jobs.front().test = test;
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

static Compiled compileStopped(const std::string& name, const std::string* text) {
  bool stopped = isolatesStop();  // other isolates run this code: none may run while it is patched
  try {
    Compiled c = compile(name, false, true, text);
    if (stopped) isolatesSync(), isolatesVerdict(true);
    return c;
  } catch (...) {
    if (stopped) isolatesVerdict(true);
    throw;
  }
}
static void eval(bool more) {  // compile and run replBuf, unless it is unfinished and `more` may follow
  try {
    Compiled c = compileStopped("<repl>", &replBuf);  // (then run with the others going again: it may wait on them)
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
// Runs at a safepoint (loop back-edge) on the main thread when a watched file changed or a line was typed, or while
// it waits on a channel or a task (isolateNudge). Inside a transaction (atomic { }) both wait for it to end.
// With isolates running, a reload stops them first (each at a poll or where it waits), so no code runs while it is
// patched; each then runs the new top level and reload() in a transaction of its own, and all of
// them commit, or all roll back.
static void hotReload() {
  if (vm != mainVM) return;  // an isolate it spawned: the main thread reloads it (isolatesParts)
  if (!(__atomic_fetch_and(&vm->poll, ~1u, __ATOMIC_ACQUIRE) & 1)) return;
  if (vm->txn) return void(txnPollLater = true);
  evalInbox();
  if (!__atomic_exchange_n(&fileChanged, 0, __ATOMIC_ACQUIRE)) return;
  bool stopped = isolatesStop();
  Compiled c = load(true);
  if (!c.chunk) {  // compile error: keep running the old code
    if (stopped) isolatesVerdict(true);
    return;
  }
  if (stopped) isolatesSync();
  // The new top level and the reload() hook (which migrates state) run as one transaction. If either fails, the
  // reload is undone: the data they changed, then the code. The program goes on as if the file had not changed.
  txnBegin();
  int st = run(c.chunk);
  Global* r = fnNamed(S_reload);
  void* hook = r && ((Fn*)r->o)->arity == 0 ? r->stub : nullptr;
  if (!st && hook) st = run(hook);
  std::string failed;
  if (!st && stopped) failed = isolatesParts(c.chunk, hook);
  jitRetire((u32*)c.chunk);
  if (st || !failed.empty()) {
    if (!failed.empty()) fprintf(stderr, "[fut] reload failed in %s\n", failed.c_str());
    rollbackReload(c.newVars);  // aborts the transaction, reverts the code, says how it went
    if (stopped) isolatesSync(), isolatesVerdict(false);
    return;
  }
  txnCommit();
  if (stopped) isolatesVerdict(true);
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

// Each test has its own process, deadline and report pipe. Programs with C bindings normally exec per test:
// system frameworks like Metal must be initialized in a fresh process. Suites with inert library constructors
// can opt to inherit a clean JIT image and exec only when the test reaches a foreign call or host I/O.
static bool preflight, bindOnly;
static int childTest = -1, childFd = -1;
static TestRun childOpts;
static int jobOverride;
struct TestSettings {
  int jobs = 1;
  bool deferred = false;
};
static int positiveCount(const std::string& s) {
  char* end;
  long n = strtol(s.c_str(), &end, 10);
  return !*end && n > 0 && n <= INT_MAX ? (int)n : 0;
}
static TestSettings testSettings() {
  TestSettings settings;
  const Package* pkg = packageOf(path);
  std::string config;
  if (pkg && readFile(pkg->dir + "/fut.test", config)) {
    std::istringstream lines(config);
    std::string line;
    for (int n = 1; std::getline(lines, line); n++) {
      std::istringstream fields(line);
      std::string key, value, extra;
      if (!(fields >> key)) continue;
      if ((fields >> value) && !(fields >> extra)) {
        if (key == "jobs") {
          settings.jobs = value == "auto" ? std::max(1L, sysconf(_SC_NPROCESSORS_ONLN)) : positiveCount(value);
          if (settings.jobs) continue;
        } else if (key == "native-libraries" && value == "on-call") { settings.deferred = true; continue; }
      }
      compileFail(pkg->dir + "/fut.test:" + std::to_string(n) + ": error: expected jobs auto, jobs <positive count>, or native-libraries on-call", false);
    }
  }
  if (jobOverride) settings.jobs = jobOverride;
  return settings;
}
static void sendReport(int fd, const TestCase& t, int st, const std::string& err) {
  char status;
  std::string text = testReport(t, st != 0, err, status);
  text.insert(text.begin(), status);
  for (size_t o = 0; o < text.size();) {
    ssize_t n = write(fd, text.data() + o, text.size() - o);
    if (n <= 0) break;
    o += n;
  }
}
// Build the exec argument vector before forking. An isolate can request the native process too.
struct NativeTestCommand {
  char exe[4096];
  std::string which, seed;
  char* args[12]{};
  NativeTestCommand(int index, int fd) : which(std::to_string(index) + ":" + std::to_string(fd)), seed(std::to_string(isolateSeed)) {
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n)) exe[0] = 0;
    size_t i = 0;
    args[i++] = exe, args[i++] = (char*)"test";
    if (index >= 0) args[i++] = (char*)"--test-child", args[i++] = which.data();
    else args[i++] = (char*)(index == -1 ? "--test-top" : "--test-bind");
    args[i++] = (char*)"--seed", args[i++] = seed.data();
    if (childOpts.json) args[i++] = (char*)"--json";
    args[i++] = path.data();
  }
};
static NativeTestCommand* nativeCommand;
[[noreturn]] static void execNativeTest() {
  execv(nativeCommand->exe, nativeCommand->args);
  _exit(127);
}
static int prepareNativeTests(bool runTop) {
  NativeTestCommand cmd(runTop ? -1 : -2, -1);
  fflush(nullptr);
  pid_t pid;
  if (posix_spawn(&pid, cmd.exe, nullptr, nullptr, cmd.args, environ))
    return fputs("fut test: cannot start the native test process\n", stderr), 2;
  int st;
  while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return 2;
  if (!WIFEXITED(st)) return fputs("fut test: native initialization crashed\n", stderr), 2;
  return WEXITSTATUS(st);
}
struct TestProcess {
  size_t index;
  pid_t pid;
  int fd;
  double deadline;
};
struct TestResult {
  std::string report, error;
  bool done = false;
};
// Only the parent launches jobs. A forked test inherits the compiled program and top level as before.
static void runProcesses(const std::vector<TestCase>& tests, int limit, const TestRun& opts, bool fresh, void* nativeTop) {
  std::vector<TestProcess> active;
  std::vector<TestResult> results(tests.size());
  size_t next = 0, printed = 0;
  fflush(nullptr);
  while (printed < tests.size()) {
    while (next < tests.size() && active.size() < (size_t)limit) {
      size_t i = next++;
      const auto& t = tests[i];
      auto& result = results[i];
      result.done = true;
      if (!t.pending.empty() || t.name.find(opts.filter) == std::string::npos || traceCached(t)) continue;
      int p[2];
      if (pipe(p)) { result.error = "cannot create a pipe for the test process\n"; continue; }
      // Siblings have no write ends to each other's report pipes. Exec closes inherited read ends.
      fcntl(p[0], F_SETFD, FD_CLOEXEC);
      NativeTestCommand cmd((int)i, p[1]);
      pid_t pid;
      if (fresh) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        if (posix_spawn(&pid, cmd.exe, &fa, nullptr, cmd.args, environ)) pid = -1;
        posix_spawn_file_actions_destroy(&fa);
      } else pid = fork();
      if (pid < 0) { close(p[0]), close(p[1]); result.error = "cannot start the test process\n"; continue; }
      if (!pid) {
        close(p[0]);
        for (auto& sibling : active) close(sibling.fd);
        std::string err;
        errSink = &err;
        int st = 0;
        if (nativeTop) {
          // The preflight showed top-level output once. Every native test executes its own top level.
          int sink = open("/dev/null", O_WRONLY);
          if (sink < 0 || dup2(sink, 1) < 0 || dup2(sink, 2) < 0) _exit(127);
          close(sink);
          nativeCommand = &cmd;
          enterNativeTest = execNativeTest;
        }
        jitForked();
        if (nativeTop) st = finish(run(nativeTop), &err);
        void* code = testStart(t);
        if (st) err = "the top level failed in the test's process: " + err;
        else st = finish(run(code), &err);
        sendReport(p[1], t, st, err);
        _exit(0);
      }
      close(p[1]);
      result.done = false;
      active.push_back({i, pid, p[0], now() + opts.timeout});
    }
    // Reports, pending tests and cache updates remain in declaration order, whatever order the children finish.
    while (printed < next && results[printed].done) {
      auto& r = results[printed];
      auto& t = tests[printed++];
      if (!testStart(t)) continue;
      if (!r.error.empty()) testFinish(t, true, r.error);
      else testRecord(r.report[0], r.report.substr(1));
      r = {};
    }
    if (active.empty()) continue;
    std::vector<pollfd> fds;
    double deadline = active[0].deadline;
    for (auto& j : active) {
      fds.push_back({j.fd, POLLIN, 0});
      deadline = std::min(deadline, j.deadline);
    }
    int ready = poll(fds.data(), fds.size(), (int)(std::max(0., deadline - now()) * 1000) + 1);
    if (ready < 0 && errno == EINTR) continue;
    for (size_t i = active.size(); i-- > 0;) {
      auto& j = active[i];
      auto& r = results[j.index];
      bool timeout = now() >= j.deadline;
      if (timeout) kill(j.pid, SIGKILL);
      else if (fds[i].revents) {
        char buf[8192];
        ssize_t n = read(j.fd, buf, sizeof buf);
        if (n > 0) { r.report.append(buf, n); continue; }
        if (n < 0 && errno == EINTR) continue;
      } else continue;
      close(j.fd);
      int st = 0;
      while (waitpid(j.pid, &st, 0) < 0 && errno == EINTR) {}
      char why[128] = {};
      if (timeout) snprintf(why, sizeof why, "timed out after %g s\n", opts.timeout);
      else if (WIFSIGNALED(st)) snprintf(why, sizeof why, "the test process crashed: %s\n", strsignal(WTERMSIG(st)));
      else if (r.report.empty() || WEXITSTATUS(st)) snprintf(why, sizeof why, "the test process exited (status %d) without a report\n", WEXITSTATUS(st));
      r.error = why, r.done = true;
      active.erase(active.begin() + i);
    }
  }
}
// fut test: the top level runs once, then each `test` block in order (main() is not called), each in its own process.
static int runTests(const TestRun& o) {
  testsBegin(o);
  // Keep native libraries and their constructors out of the forking process. A fresh exec validates
  // every binding and runs the real top level; a test that touches the host execs before doing so.
  TestSettings settings;
  Compiled c;
  try {
    if (childTest < 0 && !preflight && !o.isolates) settings = testSettings();
    deferTestLibraries = settings.deferred;
    c = compile(path, false, false, nullptr, true);
  } catch (std::exception& e) { return reportCompileError(e.what()), 2; }
  if (preflight) {
    if (bindOnly) return 0;
    std::string err;
    errSink = &err;
    int st = finish(run(c.chunk), &err);
    errSink = nullptr;
    if (st) reportTopError(err);
    return st;
  }
  if (childTest >= 0) {  // spawned for one test: the top level, then it, reported to the parent
    if (childTest >= (int)c.tests.size()) return 2;
    std::string err;
    errSink = &err;
    const TestCase& t = c.tests[childTest];
    int st = finish(run(c.chunk), &err);
    void* code = st ? nullptr : testStart(t);
    if (st) testStart(t), err = "the top level failed in the test's process: " + err;
    else if (code) st = finish(run(code), &err);
    sendReport(childFd, t, st, err);
    fflush(nullptr);
    _exit(0);
  }
  childOpts = o;
  if (o.isolates) {  // each test in o.isolates isolates at once, each running the top level first
    for (auto& t : c.tests) {
      if (!testStart(t)) continue;
      auto jobs = inIsolates(o.isolates, {c.chunk, t.code}, &t);
      if (std::string why = differs(jobs); !why.empty()) testFinish(t, true, why);
      else testRecord(jobs[0].status, jobs[0].report);
    }
    jitRetire((u32*)c.chunk);
    return std::max(testsEnd(), reportInterfaces(checkInterfaces(c), o.json, path) ? 1 : 0);
  }
  std::string err;
  errSink = &err;
  bool runTop = testsToRun(c.tests) != 0;
  int st = hasTestLibraries ? prepareNativeTests(runTop) : runTop ? finish(run(c.chunk), &err) : 0;
  auto api = [&](int st) { return reportInterfaces(checkInterfaces(c), o.json, path) ? std::max(st, 1) : st; };
  if (st) {
    errSink = nullptr;
    if (!hasTestLibraries) reportTopError(err);
    return api(st);
  }
  bool fresh = false;
  if (!settings.deferred)
    for (auto& d : c.packages)
      if (const Package* p = packageAt(d))
        for (auto& m : p->mods) fresh |= m.kind == "c";
  runProcesses(c.tests, settings.jobs, o, fresh, hasTestLibraries ? c.chunk : nullptr);
  jitRetire((u32*)c.chunk);
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
// fut init [dir]: a new package in dir (default: here), named after it: fut.pkg, <name>.fut with a pub function, main
// and a test, its .api, and .fut-cache/ in .gitignore. Nothing is overwritten: if a file it would write exists, it
// writes none. (.gitignore is appended to.)
static int initCommand(int argc, char** argv) {
  if (argc > 3 || (argc == 3 && argv[2][0] == '-')) return fputs("usage: fut init [dir]\n", stderr), 2;
  std::string dir = argc == 3 ? argv[2] : ".";
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  for (size_t i = 1; i <= dir.size(); i++)  // make dir and its parents
    if ((i == dir.size() || dir[i] == '/') && mkdir(dir.substr(0, i).c_str(), 0755) && errno != EEXIST)
      return fprintf(stderr, "fut init: could not make %s: %s\n", dir.substr(0, i).c_str(), strerror(errno)), 1;
  std::string abs = canonicalPath(dir), name = abs.substr(abs.rfind('/') + 1);
  for (char& c : name) if (!isalnum((unsigned char)c) && c != '_') c = '_';  // my-app -> my_app: a name is an identifier
  if (name.empty() || isdigit((unsigned char)name[0])) name = "_" + name;
  std::string at = dir == "." ? "" : dir + "/", root = at + name + ".fut";
  for (std::string f : {at + "fut.pkg", root, at + name + ".api"})
    if (struct stat st; !stat(f.c_str(), &st)) return fprintf(stderr, "fut init: %s already exists, so nothing was written\n", f.c_str()), 1;
  std::string pkg = "name " + name + "\nversion 0.1.0\n";
  std::string src = "// " + name + ": `fut " + name + ".fut` runs main, `fut -w " + name + ".fut` reloads it on save, `fut test " + name +
                    ".fut` runs the tests.\n// pub names are the package's interface, kept in " + name + ".api: `fut api " + name +
                    ".fut` updates it when they change.\n\n"
                    "pub fn greet(who: str): str { return \"hello, {who}\" }\n\n"
                    "fn main() {\n  print(greet(\"world\"))\n}\n\n"
                    "test \"greet\" {\n  expect greet(\"fut\") == \"hello, fut\"\n}\n";
  if (!saveFile(at + "fut.pkg", &pkg) || !saveFile(root, &src)) return fprintf(stderr, "fut init: could not write in %s: %s\n", abs.c_str(), strerror(errno)), 1;
  printf("created %sfut.pkg\ncreated %s\n", at.c_str(), root.c_str());
  std::string ignore, gi = at + ".gitignore";
  bool had = readFile(gi, ignore);
  if (ignore.find(".fut-cache") == std::string::npos) {  // `fut test --changed` keeps its cache there
    ignore += (ignore.empty() || ignore.back() == '\n' ? "" : "\n") + std::string(".fut-cache/\n");
    if (saveFile(gi, &ignore)) printf("%s %s\n", had ? "added .fut-cache/ to" : "created", gi.c_str());
  }
  path = root;
  rtInit();
  try {
    for (auto& is : checkInterfaces(compile(path, false, false)))
      if (saveFile(is.api, &is.want)) printf("created %s%s\n", at.c_str(), is.api.substr(is.api.rfind('/') + 1).c_str());
  } catch (std::exception& e) { return fprintf(stderr, "%s\n", e.what()), 1; }
  printf("\n%s  fut %s.fut         run it\n  fut test %s.fut    test it\n", dir == "." ? "" : ("  cd " + dir + "\n").c_str(), name.c_str(), name.c_str());
  return 0;
}

// fut map [--pub] [--json] file.fut [package | file]: the packages, files and declarations the program uses
static int mapCommand(bool pubOnly, bool json, const std::string& only) {
  Compiled c;
  inferEffectsWanted = true;  // and what each function does
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

// fut worker ...: Cloudflare Worker bundles, made by web/worker.mjs (Node) next to this binary's build/ directory
static int workerCommand(int argc, char** argv) {
  char exe[4096], real[4096];
  uint32_t n = sizeof exe;
  if (_NSGetExecutablePath(exe, &n) || !realpath(exe, real)) return fputs("fut worker: cannot find the fut binary\n", stderr), 2;
  std::string script = real;
  for (int up = 0; up < 2; up++) script = script.substr(0, script.rfind('/'));  // build/fut -> the repository
  script += "/web/worker.mjs";
  std::vector<char*> av{(char*)"node", script.data()};
  for (int i = 2; i < argc; i++) av.push_back(argv[i]);
  av.push_back(nullptr);
  execvp("node", av.data());
  return fprintf(stderr, "fut worker: cannot run node (%s): it runs %s\n", strerror(errno), script.c_str()), 2;
}

// A crash still writes what the program printed (stdout is fully buffered, for speed): flush, unless another thread
// holds stdout (waiting could hang instead of crashing), then die of the same signal.
static void flushAndDie(int sig) {
  if (ftrylockfile(stdout) == 0) fflush(stdout), funlockfile(stdout);
  signal(sig, SIG_DFL), raise(sig);
}
static void onCrash() {
  static char alt[1 << 16];  // a stack overflow leaves no stack to run the handler on
  stack_t ss = {};
  ss.ss_sp = alt, ss.ss_size = sizeof alt;
  sigaltstack(&ss, nullptr);
  struct sigaction sa = {};
  sa.sa_handler = flushAndDie, sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
  for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

// Both commands analyze the entry and its imports without executing the program.
static int inspectCommand(int argc, char** argv, bool refs) {
  bool writes = false;
  std::vector<std::string> args;
  for (int i = 2; i < argc; i++) {
    if (refs && std::string_view(argv[i]) == "--writes") writes = true;
    else if (argv[i][0] != '-') args.emplace_back(argv[i]);
    else return fprintf(stderr, "unknown option: %s\n", argv[i]), 2;
  }
  if (args.size() != (refs ? 2u : 1u))
    return fputs(refs ? "usage: fut refs file.fut Class.field [--writes]\n" : "usage: fut check file.fut\n", stderr), 2;
  rtInit();
  CompileCheck::Buffers buffers;
  CompileCheck check{buffers};
  check.referenceQuery = refs;
  auto diagnostics = compileDiagnostics(args[0], check);
  for (const auto& d : diagnostics)
    fprintf(stderr, "%s:%d:%d: error: %s [%s]\n", d.file.c_str(), d.line, d.column, d.message.c_str(), d.code.c_str());
  if (!diagnostics.empty()) return 1;
  if (!refs) return 0;
  const auto& index = check.symbols;
  const auto& query = args[1];
  int target = -1;
  for (size_t i = 0; i < index.declarations.size(); i++) {
    const auto& d = index.declarations[i];
    std::string qualified = d.container + "." + d.name;
    if (d.kind != "field" || (qualified != query && !qualified.ends_with("." + query))) continue;
    if (query.find('.') == std::string::npos) break;
    if (target >= 0) return fprintf(stderr, "ambiguous field: %s (include the module name)\n", query.c_str()), 2;
    target = int(i);
  }
  if (target < 0) return fprintf(stderr, "unknown field: %s\n", query.c_str()), 2;
  std::map<CompileSymbols::Site, std::string> results;
  for (auto& u : index.unresolved)
    if (u.name == index.declarations[target].name && (!writes || u.role == CompileSymbols::Write))
      results[u.site] = u.role == CompileSymbols::Write ? "possible write" : "possible read";
  for (auto& u : index.occurrences)
    if (u.declaration == target && (!writes || u.role == CompileSymbols::Write))
      results[u.site] = u.role == CompileSymbols::Decl ? "declaration" : u.role == CompileSymbols::Write ? "write" : "read";

  for (auto& [site, role] : results) {
    const auto& lines = index.lineStarts[site.file];
    auto next = std::upper_bound(lines.begin(), lines.end(), size_t(site.offset));
    printf("%s:%td:%zu: %s %s\n", index.files[site.file].c_str(), next - lines.begin(), site.offset - next[-1] + 1,
           role.c_str(), query.c_str());
  }
  return 0;
}

int main(int argc, char** argv) {
  onCrash();
  findStd();
  if (argc > 1 && (std::string_view(argv[1]) == "check" || std::string_view(argv[1]) == "refs"))
    return inspectCommand(argc, argv, std::string_view(argv[1]) == "refs");
  if (argc > 1 && std::string(argv[1]) == "explain") return explain(argc > 2 ? argv[2] : nullptr);  // an error code's rule and fix
  if (argc > 1 && std::string(argv[1]) == "worker") return workerCommand(argc, argv);
  if (argc > 1 && std::string(argv[1]) == "init") return initCommand(argc, argv);
  bool watch = false, asmMode = false, testMode = argc > 1 && std::string(argv[1]) == "test";
  bool apiMode = argc > 1 && std::string(argv[1]) == "api", apiCheck = false;
  bool mapMode = argc > 1 && std::string(argv[1]) == "map", mapPub = false, mapJson = false;
  std::string mapOnly;
  TestRun topts;
  std::vector<std::string> testFiles;
  std::string replayLog;
  bool record = false;
  const char* usage = "usage: fut [-w | -v | --asm | --isolates n | --seed n | --threads n | --record log | --replay log] [file.fut]   (no file: REPL)\n"
                      "       fut test [--changed] [--json] [--filter text] [--timeout s] [--jobs n] [--isolates n] [--seed n] file.fut...\n"
                      "       fut check file.fut\n       fut refs file.fut Class.field [--writes]\n       fut init [dir]   (a new package)\n       fut api [--check] file.fut\n       fut map [--pub] [--json] file.fut [package | file]\n       fut explain [code]\n"
                      "       fut fmt [--check] [--stdin] [file.fut | dir ...]\n"
                      "       fut lsp   (language server over stdio)\n       fut worker build ...   (Cloudflare Workers: node web/worker.mjs)\n";
  if (argc > 1 && std::string_view(argv[1]) == "fmt") return fmtCommand(argc - 2, argv + 2);
  if (argc > 1 && std::string_view(argv[1]) == "lsp") return argc == 2 ? lspCommand() : (fputs(usage, stderr), 2);
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
    else if (a == "--seed" && i + 1 < argc && strtoull(argv[i + 1], nullptr, 10) > 0) isolateSeed = strtoull(argv[++i], nullptr, 10);
    else if (a == "--threads" && i + 1 < argc && atoi(argv[i + 1]) > 0) isolateThreads = atoi(argv[++i]);
    else if (testMode && a == "--jobs" && i + 1 < argc) {
      if (!(jobOverride = positiveCount(argv[++i]))) return fputs(usage, stderr), 2;
    } else if (testMode && a == "--json") topts.json = true;
    else if (testMode && a == "--filter" && i + 1 < argc) topts.filter = argv[++i];
    else if (testMode && a == "--changed") topts.changed = true;
    else if (testMode && (a == "--test-top" || a == "--test-bind")) preflight = true, bindOnly = a == "--test-bind";
    else if (testMode && a == "--test-child" && i + 1 < argc && sscanf(argv[i + 1], "%d:%d", &childTest, &childFd) == 2) i++;
    else if (testMode && a == "--timeout" && i + 1 < argc && atof(argv[i + 1]) > 0) topts.timeout = atof(argv[++i]);
    else if ((a == "--record" || a == "--replay") && i + 1 < argc && !testMode) record = a == "--record", replayLog = argv[++i];
    else if (a == "-w" && !testMode) watch = verbose = true;
    else if (a == "-v") verbose = true;
    else if (a == "--asm") asmMode = true;
    else if (a[0] == '-') return fputs(usage, stderr), 2;
    else {
      path = a;
      if (testMode) testFiles.push_back(path);
    }
  }
  if (testMode && !isolateSeed) isolateSeed = 1;
  if (testFiles.size() > 1) {
    int result = 0;
    for (auto& file : testFiles) {
      fflush(nullptr);
      pid_t child = fork();
      if (child < 0) return perror("fut test: fork"), 2;
      if (!child) {
        path = file;
        rtInit();
        int status = runTests(topts);
        fflush(nullptr);
        _exit(status);
      }
      int status;
      while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return perror("fut test: waitpid"), 2;
      result = std::max(result, WIFEXITED(status) ? WEXITSTATUS(status) : 2);
    }
    return result;
  }
  if (watch) isolateSeed = 0;  // -w: a reload stops pool isolates at their polls (seeded ones share the main thread)  // a test's isolates interleave the same way every run
  rtInit();
  if ((testMode || apiMode || mapMode) && path.empty()) return fputs(usage, stderr), 2;
  if (mapMode) return mapCommand(mapPub, mapJson, mapOnly);
  if (testMode) return runTests(topts);
  if (apiMode) return apiCommand(apiCheck);
  if (path.empty()) return repl();
  if (!replayLog.empty() && (watch || topts.isolates)) return fputs("fut: --record and --replay run one isolate, without -w\n", stderr), 2;
  if (!replayLog.empty() && !replayStart(replayLog.c_str(), record)) return 2;
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
        if ((n = stamp()) != t) usleep(30000), t = stamp(), __atomic_store_n(&fileChanged, 1, __ATOMIC_RELAXED),
                                    __atomic_fetch_or(&mainVM->poll, 1u, __ATOMIC_RELEASE), isolateNudge();
    }, nullptr);
    pthread_detach(th);
    pthread_create(&th, nullptr, [](void*) -> void* {  // a REPL into the running program: lines run at the next safepoint
      char* line = nullptr;
      size_t cap = 0;
      for (ssize_t n; (n = getline(&line, &cap, stdin)) > 0; __atomic_fetch_or(&mainVM->poll, 1u, __ATOMIC_RELEASE), isolateNudge())
        pthread_mutex_lock(&watchMu), inbox.emplace_back(line, n), pthread_mutex_unlock(&watchMu);
      return nullptr;
    }, nullptr);
    pthread_detach(th);
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
