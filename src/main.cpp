// Driver: run a file, hot-patch it on save (-w), dump its machine code (--asm), or start a REPL.
#include "fut.h"
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static std::string path;
static pthread_mutex_t watchMu = PTHREAD_MUTEX_INITIALIZER;
static std::vector<std::string> watched;  // the program file and everything it imports
static std::vector<std::string> inbox;    // lines typed into a running program (both guarded by watchMu)
static std::string replBuf;               // REPL input so far (more lines may follow)
static bool inMain;
static int fileChanged;

static Compiled load(bool reload) {
  timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  try {
    Compiled c = compile(path, reload, false);
    pthread_mutex_lock(&watchMu), watched = c.files, pthread_mutex_unlock(&watchMu);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (reload) fprintf(stderr, "[fut] %s (%.2f ms, %zu KB code)\n", c.log.c_str(),
                        (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6, jitLive() >> 10);
    return c;
  } catch (std::exception& e) {
    fflush(stdout);
    fprintf(stderr, "%s\n", e.what());
    return {};
  }
}
static int run(void* code) {  // 0 ok, 1 runtime error, 2 restart requested
  jmp_buf jb, *outer = errJmp;
  void* handler = vm.handler;  // REPL lines typed into a running program must not land in its try blocks
  int st = 0;
  errJmp = &jb, vm.handler = nullptr;
  if (int r = setjmp(jb)) st = r;
  else vmCall(code);
  errJmp = outer, vm.handler = handler;
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
// Runs at a safepoint (loop back-edge) on the main thread when a watched file changed or a line was typed.
static void hotReload() {
  vm.poll = 0;
  evalInbox();
  if (!__atomic_exchange_n(&fileChanged, 0, __ATOMIC_ACQUIRE)) return;
  Compiled c = load(true);
  if (!c.chunk) return;  // compile error: keep running the old code
  vmCall(c.chunk), jitRetire((u32*)c.chunk);
  if (Global* r = fnNamed(S_reload); r && ((Fn*)r->o)->arity == 0) vmCall(r->stub);  // user hook: migrate state
  if (c.mainChanged && inMain) c = {}, longjmp(*errJmp, 2);                       // unwind and rerun main()
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
    if (__atomic_exchange_n(&vm.poll, 0, __ATOMIC_ACQUIRE) && (evalInbox(), __atomic_exchange_n(&fileChanged, 0, __ATOMIC_ACQUIRE))) return;
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
  bool watch = false, asmMode = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-w") watch = verbose = true;
    else if (a == "-v") verbose = true;
    else if (a == "--asm") asmMode = true;
    else if (a[0] == '-') return fputs("usage: fut [-w | -v | --asm] [file.fut]   (no file: REPL)\n", stderr), 2;
    else path = a;
  }
  rtInit();
  if (path.empty()) return repl();
  if (asmMode) return dumpAsm();
  watched = {path};
  Compiled c = load(false);
  if (watch) {  // started after the first load, so the baseline covers every imported file
    // The watcher only raises a flag; the main thread recompiles at its next safepoint.
    pollHook = hotReload;
    pthread_t th;
    pthread_create(&th, nullptr, [](void*) -> void* {
      for (long long t = stamp(), n;; usleep(100000))
        if ((n = stamp()) != t) usleep(30000), t = stamp(), __atomic_store_n(&fileChanged, 1, __ATOMIC_RELAXED), __atomic_store_n(&vm.poll, 1, __ATOMIC_RELEASE);
    }, nullptr);
    pthread_create(&th, nullptr, [](void*) -> void* {  // a REPL into the running program: lines run at the next safepoint
      char* line = nullptr;
      size_t cap = 0;
      for (ssize_t n; (n = getline(&line, &cap, stdin)) > 0; __atomic_store_n(&vm.poll, 1, __ATOMIC_RELEASE))
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
