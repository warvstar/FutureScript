// Record and replay (fut --record log file.fut, fut --replay log file.fut). A run can't repeat by itself only what it
// asks the outside world: the clock, input, files, the environment, an unseeded random(). --record writes down what
// each of those builtins returned, in order; --replay answers them from the log instead, so the run repeats exactly
// (numbers give the same bits on every platform, so a log recorded on one replays on another). Sleeps are skipped,
// so a replay runs as fast as it can. Every print is checked against the recording, and the first operation that
// differs from it raises [replay]: the program changed, or something it did wasn't in the log (isolates' timing).
//
// The log is text, one operation per line: its name, a space, then what it returned as JSON ("clock 0.25",
// "key true", "print \"score 3\""), or "!" if it raised (a replay runs it again, to raise the same way).
#include "fut.h"
#include <cstring>
#ifndef __wasm__
#include <fstream>
#endif

namespace {
const char* const NAMES[] = {"clock", "time", "random", "key", "mousex", "mousey", "mousedown", "readFile", "env",
                             "exec", "execAll", "fetch", "fetchAll", "sleep", "print"};
constexpr int N = sizeof NAMES / sizeof *NAMES;
struct Wrapped { const char* name; void* orig; int arity; };
Wrapped wrapped[N];
bool recording, replaying;
std::vector<std::string> lines;  // recording: what to write; replaying: what was read
size_t at;                       // replaying: the next line
std::string pending;             // recording: the operation whose result isn't back yet ("!" if it never comes)

std::string textOf(V v) { return is(v, K::Str) ? std::string(as<Str>(v)->sv()) : toStr(v); }
std::string quoted(const std::string& s) { return toJs(box(newStr(s))); }

bool divergedOnce;
[[noreturn]] void diverged(const std::string& what) {
  divergedOnce = true;
  ERR("@replay this run differs from the recording at operation %zu: %s", at + 1, what.c_str());
}
V through(int i, V* a) {
  Wrapped& w = wrapped[i];
  bool print = !strcmp(w.name, "print"), sleep = !strcmp(w.name, "sleep");
  if (replaying) {
    if (at >= lines.size()) diverged(std::string("it calls ") + w.name + "(), and the recording had ended");
    const std::string& l = lines[at];
    size_t sp = l.find(' ');
    std::string op = l.substr(0, sp), rest = sp == std::string::npos ? "" : l.substr(sp + 1);
    if (op != w.name) diverged(std::string("it calls ") + w.name + "(), the recording " + op + "()");
    at++;
    if (print) {
      std::string now = quoted(textOf(a[0]));
      if (now != rest) diverged("it prints " + now + ", the recording printed " + rest);
    } else if (sleep) return NIL;
    else if (rest != "!") return fromJs(rest);
    return vmEnter(w.orig, a);  // print, or an operation that raised when recorded
  }
  lines.push_back(std::string(w.name) + " !");  // if it raises, it stays "!"
  size_t me = lines.size() - 1;
  V r = vmEnter(w.orig, a);
  lines[me] = std::string(w.name) + " " + (print ? quoted(textOf(a[0])) : sleep ? "null" : toJs(r));
  return r;
}
template <int I> V w0() { V a[8] = {}; return through(I, a); }
template <int I> V w1(V x) { V a[8] = {x}; return through(I, a); }
template <int I> V w2(V x, V y) { V a[8] = {x, y}; return through(I, a); }
template <int I> void* wrapperFor(int arity) {
  return arity == 0 ? callable<w0<I>>() : arity == 1 ? callable<w1<I>>() : arity == 2 ? callable<w2<I>>() : nullptr;
}
template <size_t... I> void* wrapper(int i, int arity, std::index_sequence<I...>) {
  void* r = nullptr;
  ((i == (int)I ? (void)(r = wrapperFor<I>(arity)) : void()), ...);
  return r;
}
}  // namespace

std::string replayLog() {
  std::string o = "# fut replay log: name, then the result as JSON (! = it raised)\n";
  for (auto& l : lines) o += l + "\n";
  return o;
}
void replayBegin(const std::string& log, bool record) {
  recording = record, replaying = !record, lines.clear(), at = 0;
  for (size_t b = 0, e; replaying && b < log.size(); b = e + 1) {
    e = log.find('\n', b);
    if (e == std::string::npos) e = log.size();
    if (e > b && log[b] != '#') lines.push_back(log.substr(b, e - b));
  }
  for (int i = 0; i < N; i++) {
    Global* g = findGlobal(intern(NAMES[i]));
    if (!g || !g->o || g->o->kind != K::Fn || wrapped[i].name) continue;  // not in this build (wasm has no exec)
    Fn* f = (Fn*)g->o;
    if (void* w = wrapper(i, f->arity, std::make_index_sequence<N>{})) wrapped[i] = {NAMES[i], f->code, f->arity}, f->code = w;
  }
}
bool replayEnded() { return !replaying || at >= lines.size() || divergedOnce; }

#ifndef __wasm__  // the wasm host reads and writes the log itself (fut_replay_begin, fut_replay_log)
static std::string logPath;
bool replayStart(const char* path, bool record) {
  logPath = path;
  std::string log;
  if (!record) {
    std::ifstream in(path);
    if (!in) return fprintf(stderr, "fut: can't read the replay log %s\n", path), false;
    log.assign(std::istreambuf_iterator<char>(in), {});
  }
  replayBegin(log, record);
  atexit([] {  // also after an uncaught error, which exits
    if (recording) std::ofstream(logPath) << replayLog();
    else if (!replayEnded()) fprintf(stderr, "fut: the replay ended before the recording did (%zu of %zu operations)\n", at, lines.size());
  });
  return true;
}
#endif
