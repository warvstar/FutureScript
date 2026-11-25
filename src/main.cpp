// Driver: run a file, hot-patch it on save (-w), dump its machine code (--asm), or start a REPL.
#include "fut.h"
#include <chrono>
#include <iostream>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using namespace std::chrono;

static std::string path;
static std::mutex watchMu;
static std::vector<std::string> watched;  // the program file and everything it imports
static bool inMain;

static Compiled load(bool reload) {
  auto t0 = steady_clock::now();
  try {
    Compiled c = compile(path, reload, false);
    { std::lock_guard l(watchMu); watched = c.files; }
    if (reload) fprintf(stderr, "[fut] %s (%.2f ms, %zu KB code)\n", c.log.c_str(),
                        duration<double, std::milli>(steady_clock::now() - t0).count(), jitLive() >> 10);
    return c;
  } catch (std::exception& e) {
    fflush(stdout);
    fprintf(stderr, "%s\n", e.what());
    return {};
  }
}
static int run(void* code) {  // 0 ok, 1 runtime error, 2 restart requested
  jmp_buf jb, *outer = errJmp;
  int st = 0;
  errJmp = &jb;
  if (int r = setjmp(jb)) st = r;
  else vmCall(code);
  errJmp = outer;
  return st;
}
static Global* fnNamed(int sym) {
  auto it = globals.find(sym);
  return it != globals.end() && it->second.kind == 'f' && it->second.o ? &it->second : nullptr;
}
static int start(Compiled& c) {  // run the top level, then main() (rerun when a reload changes it)
  int st = run(c.chunk);
  jitRetire((u32*)c.chunk), inMain = true;
  for (Global* m; !st && (m = fnNamed(S_main)) && (st = run(m->stub)) == 2; st = 0) fputs("[fut] main() changed: restarting it\n", stderr);
  inMain = false;
  return st;
}
// Runs at a safepoint (loop back-edge) on the main thread when a watched file changed.
static void hotReload() {
  vm.poll = 0;
  Compiled c = load(true);
  if (!c.chunk) return;  // compile error: keep running the old code
  vmCall(c.chunk), jitRetire((u32*)c.chunk);
  if (Global* r = fnNamed(S_reload); r && ((Fn*)r->o)->arity == 0) vmCall(r->stub);  // user hook: migrate state
  if (c.mainChanged && inMain) c = {}, longjmp(*errJmp, 2);                       // unwind and rerun main()
}
static long long stamp() {
  std::lock_guard l(watchMu);
  long long s = 0;
  for (auto& f : watched) {
    struct stat st;
    if (!stat(f.c_str(), &st)) s = s * 31 + st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
  }
  return s;
}
static void waitChange() {
  while (!__atomic_exchange_n(&vm.poll, 0, __ATOMIC_ACQUIRE)) std::this_thread::sleep_for(50ms);
}

static int repl() {
  std::string buf, line;
  for (;;) {
    fputs(buf.empty() ? "> " : ". ", stdout), fflush(stdout);
    if (!std::getline(std::cin, line)) return puts(""), 0;
    buf += line + "\n";
    try {
      Compiled c = compile("<repl>", false, true, &buf);
      run(c.chunk), jitRetire((u32*)c.chunk);
    } catch (CompileError& e) {
      if (e.atEnd && !line.empty()) continue;  // unfinished input: keep reading (an empty line gives up)
      fprintf(stderr, "%s\n", e.what());
    } catch (std::exception& e) {
      fprintf(stderr, "%s\n", e.what());
    }
    buf.clear(), fflush(stdout);
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
    if (a == "-w") watch = true;
    else if (a == "--asm") asmMode = true;
    else if (a[0] == '-') return fputs("usage: fut [-w | --asm] [file.fut]   (no file: REPL)\n", stderr), 2;
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
    std::thread([] {
      for (long long t = stamp(), n;; std::this_thread::sleep_for(100ms))
        if ((n = stamp()) != t) std::this_thread::sleep_for(30ms), t = stamp(), __atomic_store_n(&vm.poll, 1, __ATOMIC_RELEASE);
    }).detach();
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
