// Isolates and channels (docs/isolates.md): `spawn f(args)` runs f in an isolate of its own, on a stack of its own,
// with a heap of its own and a copy of the spawner's globals; `task.join()` waits for its result. Isolates share no
// mutable objects: `channel(n)` makes a queue of up to n messages, and every value sent is copied into the
// receiver's heap. Channels and tasks are the shared things: each isolate holding one has a handle (a Chan or Task
// object in its heap) on state they all share.
//
// Every isolate belongs to a program: the root (main(), a test run, one of `--isolates N`) and what it spawned, and
// what those spawned. A program whose isolates are all waiting on each other can never go on: the last one to wait
// finds that, and each of them raises an error naming what every one waits for. When the root's program ends, it
// waits for the rest (isolatesFinish): an isolate that failed with nobody joining it is reported then, not lost.
//
// All channel and task state is guarded by one lock (chanMu). Messages are encoded before it is taken and decoded
// after it is released, so it is held only to move a message in or out of a queue.
//
// Scheduling. A spawned isolate runs on a stack of its own, and takes turns on threads: switching isolates is
// switching stacks (ctxSwap, as fibers do) and swapping every isolate-local variable (ISO_LOCAL). A program runs
// its isolates one of two ways, chosen at its first spawn:
// - the pool (the default): up to cores() threads. A spawned isolate is given to the thread with the fewest, and
//   stays on it: C++ frames on its stack may hold a thread_local's address (the compiler keeps those within a
//   function), so resuming one on another thread would be wrong. An isolate that waits lets its thread run its
//   others; one that runs for a whole slice while another of its thread's waits is asked to yield at its next poll
//   (vm->poll bit 8, set by a ticker thread). The root keeps the thread it started on (main(), say, may own the
//   window), and its waits block that thread.
// - seeded (fut test, --seed n): every isolate of the program runs on the root's thread, and at each channel
//   operation (and print) a generator seeded with n picks which runnable isolate goes on. The same seed replays the
//   same interleaving; between those points isolates share nothing, so nothing else can differ.
#include "fut.h"
#include <unordered_map>
#if !ONE_THREAD
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif
#if !__wasm__
#include <sys/mman.h>
#endif
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define FUT_TSAN 1  // TSan follows each isolate's stack as a fiber of its own
extern "C" void* __tsan_get_current_fiber();
extern "C" void* __tsan_create_fiber(unsigned);
extern "C" void __tsan_destroy_fiber(void*);
extern "C" void __tsan_switch_to_fiber(void*, unsigned);
#endif
#if __has_feature(address_sanitizer)
#define FUT_ASAN 1
extern "C" void __asan_unpoison_memory_region(void const volatile*, size_t);
extern "C" void __sanitizer_start_switch_fiber(void** fakeStackSave, const void* bottom, size_t size);
extern "C" void __sanitizer_finish_switch_fiber(void* fakeStackSave, const void** bottomOld, size_t* sizeOld);
#endif
#endif

#if __wasm__
#define WASM_OR(w, n) w
#else
#define WASM_OR(w, n) n
#endif
struct Iso;
struct Shared {
  std::atomic<int> rc{1};
  virtual ~Shared() = default;
};
static void release(Shared* s) { if (s && s->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) delete s; }

// A value, copied out of one heap to be rebuilt in another: a flat encoding (see Writer), and the channels and
// tasks it holds, on each of which it keeps a reference.
struct Msg {
  std::string b;
  std::vector<Shared*> refs;
  Msg() = default;
  Msg(Msg&& o) noexcept : b(std::move(o.b)), refs(std::move(o.refs)) {}
  Msg& operator=(Msg&& o) noexcept { std::swap(b, o.b), std::swap(refs, o.refs); return *this; }
  ~Msg() { for (Shared* s : refs) release(s); }
};
struct ChanState : Shared {
  std::deque<Msg> q;
  size_t cap = 0;         // 0: a rendezvous (send waits until its message is taken)
  bool closed = false;
  uint64_t sent = 0, taken = 0;
  std::vector<Iso*> recvq, sendq;  // waiting to receive, to send
};
struct TaskState : Shared {
  int id = 0;
  bool done = false, failed = false, joined = false;
  Msg start;              // the function, its arguments and the spawner's globals
  Msg result;             // its result, or the error it failed with
  std::string trace, at;  // the error's frames; where it was spawned
  uint64_t allowed = 0;   // the spawner's effect state: it may do what the spawner may
  u32 who = 0;
  std::string* out = nullptr;  // where its print() goes: the spawner's, outside any capture
  std::vector<Iso*> joiners;
};
struct Sched {             // where runnable isolates wait for a thread: a seeded program's, or a pool thread's
  bool det = false;        // seeded: all of a program's, on the root's thread
  uint64_t seed = 0, rng = 0;
  std::deque<Iso*> runq;
  // a pool thread's:
  int load = 0;            // isolates given to it, not yet ended
  Iso* cur = nullptr;      // the one it runs
#if !ONE_THREAD
  pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
#endif
};
struct Iso {
  int id;
  Iso* root;               // its program's first isolate (itself, for a root)
  TaskState* task = nullptr;
  Sched* sched = nullptr;  // seeded: its program's; pooled: its thread's (a root's: set at its program's first spawn)
  bool pinned = true;      // on a thread of its own, which its waits block (a root, unless seeded)
  enum State { RUNNING, RUNNABLE, PARKED } state = RUNNING;
  std::atomic<bool> onStack{true};  // a thread is on its stack, maybe still switching away: wait before resuming it
  std::atomic<double> since{0};     // when its turn on a thread began (the ticker reads it)
  bool exited = false;              // switched away from for the last time: whoever resumes next frees it
  // hot reload (reloadPoint):
  u32 rlEpoch = 0;                  // the last reload whose top level and reload() it ran
  bool rlParted = false;            // ran them, in a transaction still open: waits for the verdict
  bool rlParked = false;            // parked for the reload (not in any queue)
  int rlVerdict = 0;                // 1 commit, 2 roll back its transaction
  bool nudged = false;              // (a pinned root) a reload or a typed line waits for it
  void* sp = nullptr;      // while switched out
  char* stack = nullptr;   // its own (a root runs on its thread's)
  char* lo = nullptr;      // the stack's bounds, its own or its thread's (for ASan)
  size_t size = 0;
  char* saved = nullptr;   // its isolate-local variables while switched out (and the thread's own while in)
  void* tsan = nullptr;
#if !ONE_THREAD
  pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
#endif
  bool waiting = false, woken = false;
  bool deadlocked = false;        // a deadlock's error was raised in it: if it fails, that is why (not reported again)
  std::atomic<bool> stop{false};  // its program failed: raise at the next poll or wait
  const char* what = nullptr;     // what it waits in, and where (for a deadlock's report)
  void* ra = nullptr;
  VM* vmb = nullptr;
  std::string dead;               // set when woken because every isolate of its program waits: the report
  // a root's, for its program:
  int live = 1, blocked = 0, nextId = 2;
  std::vector<Iso*> exitq;              // waiting for an isolate to end (isolatesFinish)
  std::vector<TaskState*> failures;     // failed, and not joined yet (each holds a reference)
};

// ---- isolate-local variables (ISO_LOCAL): their place in each isolate's copy ----
static size_t isoLocalSize, isoLocalAlign = 16;
std::vector<IsoLocal>& isoLocals() {
  static auto* v = new std::vector<IsoLocal>;  // never destroyed: a pool thread may still switch while exit() runs
  return *v;
}
size_t isoLocalPlace(size_t size, size_t align) {
  isoLocalAlign = std::max(isoLocalAlign, align);
  size_t o = (isoLocalSize + align - 1) & ~(align - 1);
  return isoLocalSize = o + size, o;
}

namespace {
Mutex chanMu;
// Never destroyed, like the scheduler's state: pool threads go on running while exit() runs.
std::vector<Iso*>& isos = *new std::vector<Iso*>;  // every isolate (under chanMu): for deadlock reports, and to stop a program's
TLS Iso* me;
TLS std::unordered_map<Shared*, Obj*>* handles;  // this isolate's handle on each channel and task (one each)
TLS std::vector<V>* building;                    // objects a message is being rebuilt into (GC roots)
ISO_LOCAL(me);
ISO_LOCAL(handles);
ISO_LOCAL(building);
// fut.h's isolate-local variables
ISO_LOCAL(fiberCur);
ISO_LOCAL(fiberMainSp);
ISO_LOCAL(fiberMainTop);
ISO_LOCAL(stackTop);
ISO_LOCAL(vm);
ISO_LOCAL(errJmp);
ISO_LOCAL(outSink);
ISO_LOCAL(errSink);
ISO_LOCAL(errValue);
ISO_LOCAL(txnPollLater);

char* stateNew() {
  char* b = (char*)aligned_alloc(isoLocalAlign, (isoLocalSize + isoLocalAlign) & ~(isoLocalAlign - 1));
  for (auto& l : isoLocals()) l.init(b + l.off, l.def);
  return b;
}
void stateFree(char* b) {
  for (auto& l : isoLocals()) l.fini(b + l.off);
  free(b);
}
void stateSwap(char* b) { for (auto& l : isoLocals()) l.swap(b + l.off); }

#if __wasm__
// The shadow stack (C++ frames that need memory, and fut frames' slots); the host keeps each isolate's wasm stack.
constexpr size_t ISO_STACK = 256 << 10, ISO_GUARD = 16 << 10;  // as a fiber's
char* stackNew() { return (char*)aligned_alloc(16, ISO_STACK); }
void stackFree(char* s) { free(s); }
#else
constexpr size_t ISO_STACK = 8 << 20, ISO_GUARD = 256 << 10;  // as the main thread's; reserved, and pages are only touched as they are used
std::vector<char*>& spareStacks = *new std::vector<char*>;  // stacks of ended isolates, for the next spawns (under chanMu)
char* stackNew() {
  {
    Locked l(chanMu);
    if (!spareStacks.empty()) {
      char* s = spareStacks.back();
      return spareStacks.pop_back(), s;
    }
  }
  char* s = (char*)mmap(nullptr, ISO_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (s == MAP_FAILED) return nullptr;
  mprotect(s, 16 << 10, PROT_NONE);  // a guard page: overflowing C++ faults rather than writing below
  return s;
}
void stackFree(char* s) {
#ifdef FUT_ASAN
  __asan_unpoison_memory_region(s + (16 << 10), ISO_STACK - (16 << 10));  // its last frames never returned
#endif
  {
    Locked l(chanMu);
    if (spareStacks.size() < 64) return spareStacks.push_back(s);
  }
  munmap(s, ISO_STACK);
}
#endif
#if ONE_THREAD
#define THREAD  // one thread
#else
#define THREAD thread_local
constexpr double SLICE = 0.01;          // a pool isolate's turn, while others wait for a thread
Sched& pooled = *new Sched;               // a pool program's root has this (each spawned isolate gets a pool thread's)
std::vector<Sched*>& pool = *new std::vector<Sched*>;  // the pool's threads (under chanMu)
#endif
// The thread's own, not an isolate's (so plain thread_local, not ISO_LOCAL):
THREAD Iso* from;       // the isolate this thread just switched away from: the side switched to finishes it
THREAD Iso* running;    // the isolate this thread runs
THREAD void* loopSp;    // a pool thread's own context (its loop), while it runs an isolate
#if !__wasm__
[[maybe_unused]] thread_local char* loopLo;
[[maybe_unused]] thread_local size_t loopSize;
[[maybe_unused]] void threadStack(char*& lo, size_t& size) {
  pthread_t t = pthread_self();
  size = pthread_get_stacksize_np(t), lo = (char*)pthread_get_stackaddr_np(t) - size;
}
[[maybe_unused]] thread_local void* loopTsan;
#endif

void arrived() {  // just switched to: finish with the isolate switched away from
  Iso* f = std::exchange(from, nullptr);
  if (!f) return;
  if (!f->exited) return f->onStack.store(false, std::memory_order_release);
  ctxGone(f->sp), stackFree(f->stack), stateFree(f->saved);
#ifdef FUT_TSAN
  __tsan_destroy_fiber(f->tsan);
#endif
  delete f;
}
// Switch this thread from m (null: a pool thread's loop) to `to` (null: back to that loop), holding no lock: m's
// state already says what becomes of it. Returns when something switches back to m.
void switchTo(Iso* m, Iso* to) {
  if (to) {
#if !ONE_THREAD
    while (to->onStack.load(std::memory_order_acquire)) sched_yield();  // its last thread is still leaving it
#endif
    to->onStack.store(true, std::memory_order_relaxed), to->since.store(now(), std::memory_order_relaxed);
  }
  if (m) stateSwap(m->saved);  // m's variables out, the thread's own in
  if (to) stateSwap(to->saved);
  from = m, running = to;
#ifdef FUT_TSAN
  __tsan_switch_to_fiber(to ? to->tsan : loopTsan, 0);
#endif
#ifdef FUT_ASAN
  void* fake = nullptr;
  __sanitizer_start_switch_fiber(m && m->exited ? nullptr : &fake, to ? to->lo : loopLo, to ? to->size : loopSize);
#endif
  ctxSwap(m ? &m->sp : &loopSp, to ? to->sp : loopSp);
#ifdef FUT_ASAN
  __sanitizer_finish_switch_fiber(fake, nullptr, nullptr);
#endif
  arrived(), vmSwitched();
}
uint64_t draw(Sched* s) { return s->rng ^= s->rng << 13, s->rng ^= s->rng >> 7, s->rng ^= s->rng << 17; }
Iso* pick(Sched* s, size_t k) {  // take the k-th runnable isolate (chanMu held)
  Iso* i = s->runq[k];
  s->runq.erase(s->runq.begin() + (long)k), i->state = Iso::RUNNING;
  return i;
}
// ---- hot reload across isolates: the root reloads (main.cpp's hotReload); the rest stop, then each runs the new
// top level and reload() in a transaction of its own, then all commit or all roll back ----
enum { RL_NONE, RL_STOP, RL_PARTS };
struct Reload {
  int phase = RL_NONE;
  u32 epoch = 0;
  Iso* by = nullptr;      // the root reloading
  void *chunk = nullptr, *hook = nullptr;
  int pending = 0;        // isolates yet to run their part
  std::string err;        // the first part that failed
};
Reload& rl = *new Reload;
#if ONE_THREAD
void rlLeft() {}
void reloadPoint() {}  // (a reload waits until the program is down to one isolate: fut.js)
#else
pthread_cond_t rlCv = PTHREAD_COND_INITIALIZER;  // the reloader waits on it for the others to stop, or to report
void rlLeft() { if (rl.phase) pthread_cond_broadcast(&rlCv); }  // an isolate stopped running (chanMu held)
#endif
#if ONE_THREAD
Sched* poolThread() { return nullptr; }  // (never: one thread's programs are all seeded)
#else
void* ticker(void*) {  // ask isolates that had a whole slice to yield, while another of their thread's waits
  for (;;) {
    usleep((useconds_t)(SLICE * 1e6));
    Locked l(chanMu);
    double t = now();
    for (Sched* w : pool)
      if (Iso* i = w->cur; i && i->state == Iso::RUNNING && !w->runq.empty() && i->vmb &&
                           t - i->since.load(std::memory_order_relaxed) >= SLICE)
        __atomic_fetch_or(&i->vmb->poll, 8u, __ATOMIC_RELAXED);
  }
}
void detached(void* (*f)(void*), void* arg) {
  pthread_t th;
  pthread_attr_t a;
  pthread_attr_init(&a), pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if (int e = pthread_create(&th, &a, f, arg)) fprintf(stderr, "fut: cannot start a thread (%s)\n", strerror(e)), exit(1);
  pthread_attr_destroy(&a);
}
#endif
void ready(Iso* i) {  // runnable: queue it (chanMu held)
  i->state = Iso::RUNNABLE, i->sched->runq.push_back(i);
#if !ONE_THREAD
  if (!i->sched->det) pthread_cond_signal(&i->sched->cv);
#endif
}
#if !ONE_THREAD
void* worker(void* p) {  // a pool thread: runs the isolates given to it, each until it waits, yields or ends
  Sched* w = (Sched*)p;
#ifdef FUT_TSAN
  loopTsan = __tsan_get_current_fiber();
#endif
#ifdef FUT_ASAN
  threadStack(loopLo, loopSize);
#endif
  chanMu.lock();
  for (;;) {
    while (w->runq.empty() || rl.phase == RL_STOP) pthread_cond_wait(&w->cv, &chanMu.m);
    Iso* i = w->cur = pick(w, 0);
    chanMu.unlock();
    switchTo(nullptr, i);
    chanMu.lock();
    w->cur = nullptr;
  }
}
Sched* poolThread() {  // the pool thread a new isolate goes to: a new one while there are fewer than cores() (chanMu held)
  Sched* best = nullptr;
  for (Sched* w : pool) if (!best || w->load < best->load) best = w;
  size_t most = isolateThreads > 0 ? (size_t)isolateThreads : (size_t)WASM_OR(hostCores(), sysconf(_SC_NPROCESSORS_ONLN));
  if (!best || (best->load && pool.size() < most)) {
    if (pool.empty()) detached(ticker, nullptr);
    pool.push_back(best = new Sched);
    detached(worker, best);
  }
  return best->load++, best;
}
#endif
// m gives up its thread until woken (chanMu held, and held again when it returns): seeded, to the next isolate the
// generator picks; else back to the pool thread's loop.
void reloadPoint();
void park(Iso* m) {
  Sched* s = m->sched;
  m->state = Iso::PARKED, rlLeft();
  Iso* to = nullptr;
  if (s->det) {
    if (s->runq.empty()) fputs("internal: a seeded program has nothing to run\n", stderr), abort();
    to = pick(s, draw(s) % s->runq.size());
  }
  chanMu.unlock();
  switchTo(m, to);
  reloadPoint();  // (resumed, perhaps only to take part in a reload)
  chanMu.lock();
}
// (wasm: always seeded, by 1 unless --seed n: there is one thread, and the interleaving might as well be the same
// every run)
void schedule(Iso* r) {  // r's program spawns for the first time: how its isolates will run (chanMu held)
  if (r->sched) return;
#if ONE_THREAD
  uint64_t seed = isolateSeed ? isolateSeed : 1;
#else
  if (!isolateSeed) { r->sched = &pooled; return; }
  uint64_t seed = isolateSeed;
#if !__wasm__
  threadStack(r->lo, r->size);
#endif
#endif
  r->sched = new Sched{true, seed, seed};
  r->pinned = false, r->saved = stateNew();
#ifdef FUT_TSAN
  r->tsan = __tsan_get_current_fiber();
#endif
}

Iso* self() {
  if (!me) isolateRoot();
  return me;
}
void wake(Iso* i) {  // (chanMu held)
  if (!i->waiting || i->woken) return;
  i->woken = true, i->root->blocked--;
#if !ONE_THREAD
  if (i->pinned) pthread_cond_signal(&i->cv);
  else
#endif
  if (i->state == Iso::PARKED) ready(i);  // (else it is still running: it woke itself, finding a deadlock)
}
void wakeAll(std::vector<Iso*>& q) { for (Iso* i : q) wake(i); }  // each removes itself when it wakes
std::string describe(Iso* i) {
  std::string s = "isolate " + std::to_string(i->id) + (i == i->root ? " (main)" : "");
  if (i->task && !i->task->at.empty()) s += ", spawned at " + i->task->at;
  s += ", waits in " + std::string(i->what ? i->what : "?");
  if (i->ra) if (std::string w = codeWhere(i->ra); !w.empty()) s += " at " + w;
  return s;
}
void deadlock(Iso* r) {  // every isolate of r's program waits (chanMu held): wake each with the report
  std::string rep = "deadlock: every isolate of this program is waiting, so none can go on";
  std::vector<Iso*> stuck;
  for (Iso* i : isos)
    if (i->root == r && i->waiting && !i->woken) stuck.push_back(i), rep += "\n  " + describe(i);
  for (Iso* i : stuck) i->dead = rep, wake(i);
}
const char* STOPPED = "stopped: the program this isolate belongs to ended with an error";
#if __wasm__ && !ONE_THREAD
// A thread that is not the main one ends the process (a fatal error on a worker, which can't end it from there):
// the main thread does, at its next poll (bit 64) or wait.
std::atomic<int> exitAsked{-1};
pthread_t mainThread = pthread_self();  // (static initialization runs on it)
[[noreturn]] void exitNow() { chanMu.unlock(), exit(exitAsked.load()); }
#endif
// Wait (chanMu held) until woken; q: the list a waker finds this isolate in. "" or the error to raise.
std::string waitFor(std::vector<Iso*>* q, const char* what, void* ra) {
  Iso* m = self();
  if (m->stop) return STOPPED;
  if (q) q->push_back(m);
  m->waiting = true, m->woken = false, m->what = what, m->ra = ra;
  Iso* r = m->root;
  if (++r->blocked >= r->live) deadlock(r);
#if !ONE_THREAD
  if (m->pinned)
    while (!m->woken) {
#if __wasm__
      if (exitAsked.load() >= 0) exitNow();
      // The host's events (a changed file, a typed line, the page's) set poll bit 1, and can't signal the cv: look.
      if (m == r && (__atomic_load_n(&vm->poll, __ATOMIC_ACQUIRE) & 1)) m->nudged = true;  // (vm: m's, on its own thread)
#endif
      if (std::exchange(m->nudged, false) && pollHook) {  // a reload or a typed line: run it now, then wait again
        m->waiting = false, r->blocked--;
        if (q) std::erase(*q, m);
        chanMu.unlock();
        pollHook();  // (it may unwind past here, to restart main: nothing is left registered)
        chanMu.lock();
        return "";   // the caller looks again at what it waits for
      }
#if __wasm__
      timespec t;
      clock_gettime(CLOCK_REALTIME, &t);
      if ((t.tv_nsec += 20000000) >= 1000000000) t.tv_sec++, t.tv_nsec -= 1000000000;
      pthread_cond_timedwait(&m->cv, &chanMu.m, &t);  // (so it looks at the poll every 20 ms)
#else
      pthread_cond_wait(&m->cv, &chanMu.m);
#endif
    }
  else
#endif
  if (!m->pinned)
    while (!m->woken) park(m);
  m->waiting = false;
  if (q) std::erase(*q, m);
  if (m->stop) return STOPPED;
  if (!m->dead.empty()) m->deadlocked = true;
  return std::exchange(m->dead, {});
}
[[noreturn]] void raise(const std::string& msg, void* ra) { throwValue(box(newStr(msg)), ra); }

// ---- handles ----
V handle(Shared* s, K k) {  // this isolate's handle on s (made, and holding a reference, the first time)
  if (!handles) handles = new std::unordered_map<Shared*, Obj*>;
  if (auto it = handles->find(s); it != handles->end()) return box(it->second);
  s->rc.fetch_add(1, std::memory_order_relaxed);
  Obj* o;
  if (k == K::Chan) { Chan* c = make<Chan>(); c->s = s, o = c; }
  else { Task* t = make<Task>(); t->s = s, o = t; }
  return (*handles)[s] = o, box(o);
}
ChanState* chanOf(V v) { return (ChanState*)as<Chan>(v)->s; }
TaskState* taskOf(V v) { return (TaskState*)as<Task>(v)->s; }

// ---- copying values: Writer encodes a value graph, Reader rebuilds it in the running isolate's heap ----
// Each value is a tag and its contents. Every object encoded gets the next number, in the order both sides meet
// them, so 'r' (an object already met) keeps sharing and cycles. Compiled code's objects (functions, classes,
// constant strings) are in the image, which every isolate shares: they go as they are ('v'), like numbers.
constexpr int MAX_DEPTH = 10000;
struct Writer {
  Msg m;
  std::unordered_map<Obj*, u32> seen;
  u32 n = 0;
  int depth = 0;
  template <class T> void raw(T x) { m.b.append((const char*)&x, sizeof x); }
  void put(V v) {
    if (!isObj(v)) return m.b += 'v', raw(v);
    Obj* o = obj(v);
    if (auto it = seen.find(o); it != seen.end()) return m.b += 'r', raw(it->second);
    if (o->kind == K::Fn || o->kind == K::Class || (o->kind == K::Str && gcInImage(o))) {
      if (!gcInImage(o)) ERR("@send cannot send %s: it is not compiled code's", typeName(v));
      return m.b += 'v', raw(v);
    }
    if (o->kind == K::Fiber) ERR("@send cannot send a fiber: it runs on its own isolate's stacks");
    if (o->kind == K::Ref) ERR("@send cannot send a js value");
    if (++depth > MAX_DEPTH) ERR("@send cannot send a value nested more than %d deep", MAX_DEPTH);
    seen[o] = n++;
    switch (o->kind) {
      case K::Str: m.b += 's', raw(((Str*)o)->len), m.b.append(((Str*)o)->sv()); break;
      case K::List: {
        auto& l = ((List*)o)->v;
        m.b += 'l', raw((u32)l.size());
        for (size_t i = 0; i < l.size(); i++) put(l[i]);
        break;
      }
      case K::Map: {
        auto& e = ((Map*)o)->e;
        m.b += 'm', raw((u32)e.size());
        for (size_t i = 0; i < e.size(); i++) put(e[i].first), put(e[i].second);
        break;
      }
      case K::Inst: {
        Inst* in = (Inst*)o;
        m.b += 'i', raw(in->cls), raw(o->nslots);
        for (u32 k = 0; k < o->nslots; k++) put(in->slots[k]);
        break;
      }
      case K::Clo: {
        Clo* c = (Clo*)o;
        if (!gcInImage(c->fn)) ERR("@send cannot send this function");
        m.b += 'c', raw(c->fn), raw(c->n);
        for (u32 k = 0; k < c->n; k++) put(c->env()[k]);
        break;
      }
      case K::Chan: case K::Task: {
        Shared* s = ((Chan*)o)->s;
        s->rc.fetch_add(1, std::memory_order_relaxed), m.refs.push_back(s);
        m.b += o->kind == K::Chan ? 'C' : 'T', raw((u32)m.refs.size() - 1);
        break;
      }
      default: ERR("@send cannot send %s", typeName(v));
    }
    depth--;
  }
};
Msg encode(V v) { Writer w; w.put(v); return std::move(w.m); }

struct Reader {
  const Msg& m;
  size_t at = 0;
  std::vector<V> objs;
  explicit Reader(const Msg& m) : m(m) { building = &objs; }
  ~Reader() { building = nullptr; }
  template <class T> T raw() { T x; memcpy(&x, m.b.data() + at, sizeof x); at += sizeof x; return x; }
  V keep(Obj* o) { return objs.push_back(box(o)), box(o); }
  V get() {
    switch (m.b[at++]) {
      case 'v': return raw<V>();
      case 'r': return objs[raw<u32>()];
      case 's': {
        u32 len = raw<u32>();
        Str* s = newStr(std::string_view(m.b.data() + at, len));
        return at += len, keep(s);
      }
      case 'l': {
        u32 n = raw<u32>();
        List* l = make<List>();
        V r = keep(l);
        l->v.reserve(n);
        for (u32 i = 0; i < n; i++) { V x = get(); l->v.push_back(x); }
        return r;
      }
      case 'm': {
        u32 n = raw<u32>();
        Map* mp = make<Map>();
        V r = keep(mp);
        for (u32 i = 0; i < n; i++) { V k = get(), x = get(); mapSet(mp, k, x); }
        return r;
      }
      case 'i': {
        Class* c = raw<Class*>();
        uint16_t n = raw<uint16_t>();
        Inst* in = make<Inst>(n * 8);
        in->cls = c, in->cid = c->id, in->nslots = n, in->slots = in->inl();
        for (u32 k = 0; k < n; k++) in->slots[k] = NIL;
        V r = keep(in);
        for (u32 k = 0; k < n; k++) in->slots[k] = get();
        return r;
      }
      case 'c': {
        Fn* f = raw<Fn*>();
        u32 n = raw<u32>();
        Clo* c = make<Clo>(n * 8);
        c->fn = f, c->n = n;
        for (u32 k = 0; k < n; k++) c->env()[k] = NIL;
        V r = keep(c);
        for (u32 k = 0; k < n; k++) c->env()[k] = get();
        return r;
      }
      case 'C': case 'T': {
        char t = m.b[at - 1];
        V h = handle(m.refs[raw<u32>()], t == 'C' ? K::Chan : K::Task);
        return objs.push_back(h), h;
      }
      default: fputs("internal: bad message\n", stderr), abort();
    }
  }
};
V decode(const Msg& m) { codeSync(); Reader r(m); return r.get(); }  // (it may hold code compiled on another thread)

// ---- channels ----
std::string doSend(ChanState* c, Msg&& msg, void* ra) {  // "" or the error
  Locked l(chanMu);
  if (c->closed) return "send() on a closed channel";
  if (c->cap) {
    while (c->q.size() >= c->cap && !c->closed)
      if (std::string e = waitFor(&c->sendq, "send()", ra); !e.empty()) return e;
    if (c->closed) return "send() on a closed channel";
    c->q.push_back(std::move(msg)), c->sent++, wakeAll(c->recvq);
    return "";
  }
  c->q.push_back(std::move(msg));  // a rendezvous: queued, then wait until a receiver has taken it
  uint64_t mine = ++c->sent;
  wakeAll(c->recvq);
  while (c->taken < mine && !c->closed)
    if (std::string e = waitFor(&c->sendq, "send()", ra); !e.empty()) return e;
  return "";
}
struct Deferred { ChanState* c; Msg m; void* ra; };
V c_send(V ch, V v) {
  void* ra = RA();
  effRequire(EFF_CHAN, "send()", ra);
  isolatePoint();
  ChanState* c = chanOf(ch);
  Msg m = encode(v);
  if (vm->txn) {  // inside atomic { } (or a reload): sent when it commits, never if it rolls back
    c->rc.fetch_add(1, std::memory_order_relaxed);
    txnAfter(
        +[](void* p) {
          Deferred* d = (Deferred*)p;
          std::string e = doSend(d->c, std::move(d->m), d->ra);
          void* ra = d->ra;
          release(d->c), delete d;
          if (!e.empty()) raise(e, ra);
        },
        +[](void* p) { Deferred* d = (Deferred*)p; release(d->c), delete d; }, new Deferred{c, std::move(m), ra});
    return NIL;
  }
  if (std::string e = doSend(c, std::move(m), ra); !e.empty()) raise(e, ra);
  return NIL;
}
void noWaitInTxn(const char* what, void* ra) {
  if (vm->txn)
    rtError(ra, "@atomic %s inside atomic { }: waiting for another isolate would hold the block open (do it before or after the block)", what);
}
// The next message (waiting for one unless `wait` is false), or END once the channel is closed and drained.
V take(V ch, bool wait, const char* what, void* ra) {
  effRequire(EFF_CHAN, what, ra);
  if (wait) noWaitInTxn(what, ra);
  isolatePoint();
  ChanState* c = chanOf(ch);
  Msg m;
  std::string err;
  bool got = false;
  {
    Locked l(chanMu);
    while (wait && c->q.empty() && !c->closed)
      if (!(err = waitFor(&c->recvq, what, ra)).empty()) break;
    if (err.empty() && !c->q.empty()) {
      m = std::move(c->q.front()), c->q.pop_front(), c->taken++, got = true;
      wakeAll(c->sendq);
    }
  }
  if (!err.empty()) raise(err, ra);
  return got ? decode(m) : END;
}
V c_recv(V ch) {
  V v = take(ch, true, "recv()", RA());
  if (v == END) ERR("@channel recv() on a closed channel, and nothing is left in it");
  return v;
}
V c_tryRecv(V ch) { V v = take(ch, false, "tryRecv()", RA()); return v == END ? NIL : v; }
V c_close(V ch) {
  effRequire(EFF_CHAN, "close()", RA());
  isolatePoint();
  ChanState* c = chanOf(ch);
  bool was;
  {
    Locked l(chanMu);
    if (!(was = c->closed)) c->closed = true, wakeAll(c->recvq), wakeAll(c->sendq);
  }
  if (was) ERR("@channel close() of a channel already closed");
  return NIL;
}
V n_channel(V n) {
  if (!isNum(n) || num(n) < 0 || num(n) > 1e9 || num(n) != (double)(int64_t)num(n))
    ERR("@argument channel(n) expects how many values it may hold: a whole number, 0 or more (0: each send waits for a receiver)");
  ChanState* c = new ChanState;
  c->cap = (size_t)num(n);
  V h = handle(c, K::Chan);
  return release(c), h;  // the handle's reference is the only one
}

// ---- isolates ----
struct IsoRun { TaskState* t; Msg* result; };
void isoRun(void* p) {  // the spawned call: its globals and arguments rebuilt here, then the call; its result encoded
  TaskState* t = ((IsoRun*)p)->t;
  volatile V a[8] = {};  // on the stack, so the GC sees them
  bool clo;
  void* code;
  {
    Locked code(codeMu);  // compiled functions and classes, a reload's new ones too (the spawner's copy may predate it)
    codeSync();           // (threads) their slots, in this thread's table
    for (auto& g : globals) if (g.slot >= 0 && g.o) vm->globals[g.slot] = box(g.o);
  }
  {
    Reader r(t->start);  // the function, its arguments, then the spawner's globals (rebuilt before any code runs)
    V f = r.get(), args = r.get();
    u32 ng = r.raw<u32>();
    for (u32 i = 0; i < ng; i++) vm->globals[i] = r.get();
    auto& l = as<List>(args)->v;
    clo = is(f, K::Clo), code = clo ? as<Clo>(f)->fn->code : as<Fn>(f)->code;
    a[0] = f;
    for (size_t i = 0; i < l.size(); i++) a[i + clo] = l[i];
  }
  t->start = Msg();  // only this isolate reads it
  reloadPoint();     // spawned before a reload, and starting during it: its part first
  *((IsoRun*)p)->result = encode(vmEnter(code, (V*)a));
}
void isoEntry() {  // the bottom frame of every spawned isolate's stack
#ifdef FUT_ASAN
  __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
  arrived();
  Iso* m = me = running;
  TaskState* t = m->task;
  vm = vmNew(), vmSwitched();
  Heap* h = heapNew(vm->runs);
  heapUse(h);
  stackTop = (uintptr_t*)(m->stack + ISO_STACK), vm->limit = m->stack + ISO_GUARD;
  {
    Locked l(chanMu);
    m->vmb = vm;
    if (m->stop) __atomic_fetch_or(&vm->poll, 4u, __ATOMIC_RELAXED);
  }
  vm->allowed = t->allowed, vm->who = t->who;
  outSink = t->out;
  std::string trace;
  V err = NIL;
  Msg result;
  IsoRun run{t, &result};
#if __wasm__
  bool failed = false;  // fut's errors are wasm exceptions here, which only the host's try_call catches (wasm.cpp)
  if (catchFut(isoRun, &run, &err, &trace)) {
    failed = true;
    std::pair<V, Msg*> e{err, &result};
    if (catchFut([](void* p) { auto* e = (std::pair<V, Msg*>*)p; *e->second = encode(e->first); }, &e, nullptr, nullptr))
      result = encode(box(newStr(toStr(err))));  // its error could not be sent itself: its text then
  }
#else
  jmp_buf jb;
  errJmp = &jb, errValue = &err, errSink = &trace;
  volatile bool failed = false, again = false;
  if (setjmp(jb)) {
    failed = true;
    if (again) result = encode(box(newStr(toStr(err))));  // its error could not be sent itself: its text then
    else again = true, result = encode(err);
  } else isoRun(&run);
#endif
  errJmp = nullptr, errValue = nullptr, errSink = nullptr, outSink = nullptr;
  Iso* r = m->root;
  {
    Locked l(chanMu);
    t->result = std::move(result), t->failed = failed, t->trace = std::move(trace), t->done = true;
    m->vmb = nullptr;  // freed below: a stop must not poke it
    wakeAll(t->joiners);
    if (failed && !t->joined && !m->deadlocked && !m->stop) t->rc.fetch_add(1, std::memory_order_relaxed), r->failures.push_back(t);
  }
  heapFree(h), vmFree(vm), vm = nullptr;
  release(t);
  delete handles, handles = nullptr;
  Iso* to = nullptr;
  isolates--;
  {  // gone: its program may end now (exit() may run while this thread switches away: see isos)
    Locked l(chanMu);
    r->live--, std::erase(isos, m), wakeAll(r->exitq);
    if (r->blocked && r->blocked >= r->live) deadlock(r);
    m->exited = true;
    if (!m->sched->det) m->sched->load--, m->sched->cur = nullptr;
    if (rl.phase == RL_PARTS && m->rlEpoch != rl.epoch) rl.pending--;  // a part it will never run
    rlLeft();  // (the ticker looks at it; it is freed soon)
    if (Sched* s = m->sched; s->det) {  // never empty: the root waits for this program to end
      if (s->runq.empty()) fputs("internal: a seeded program has nothing to run\n", stderr), abort();
      to = pick(s, draw(s) % s->runq.size());
    }
  }
  switchTo(m, to);  // and whatever runs next frees this stack
  __builtin_unreachable();
}
V n_spawn(V f, V args) {
  void* ra = RA();
  effRequire(EFF_SPAWN, "spawn", ra);
  if (vm->txn) rtError(ra, "@atomic spawn inside atomic { }: an isolate, once started, can't be taken back (spawn after the block)");
  Fn* fn = is(f, K::Clo) ? as<Clo>(f)->fn : is(f, K::Fn) ? as<Fn>(f) : nullptr;
  if (!fn) rtError(ra, "@spawn spawn expects a function to call, got %s", typeName(f));
  if (fn->op || !fn->code) rtError(ra, "@spawn spawn: %s is a builtin; spawn a function that calls it", symName(fn->name).c_str());
  size_t n = as<List>(args)->v.size();
  int want = fn->arity - is(f, K::Clo);
  if (want != (int)n) rtError(ra, "@arity %s expects %d arguments, got %d", symName(fn->name).c_str(), want, (int)n);
  if (n > 7) rtError(ra, "@limit spawn passes at most 7 arguments (put more in a list)");
  Writer w;  // one message, so values the arguments and globals share stay shared
  w.put(f), w.put(args);
  int ng = globalCount();
  w.raw((u32)ng);
  for (int i = 0; i < ng; i++) w.put(vm->globals[i]);
  TaskState* t = new TaskState;  // one reference for the isolate, one for this handle
  t->start = std::move(w.m), t->at = WASM_OR(hereWhere(), codeWhere(ra)), t->allowed = vm->allowed, t->who = vm->who, t->out = outBase();
  V h = handle(t, K::Task);
  char* stack = stackNew();
  if (!stack) release(t), rtError(ra, "@memory spawn: out of memory for the isolate's stack");
  Iso* c = new Iso;
  c->stack = c->lo = stack, c->size = ISO_STACK, c->sp = ctxFrame((uintptr_t*)(stack + ISO_STACK), isoEntry), c->saved = stateNew(), c->task = t;
  c->pinned = false, c->onStack.store(false, std::memory_order_relaxed);
#ifdef FUT_TSAN
  c->tsan = __tsan_create_fiber(0);
#endif
  Iso* r = self()->root;
  isolates++;
  bool det;
  {
    Locked l(chanMu);
    schedule(r);
    c->id = t->id = r->nextId++, c->root = r, det = r->sched->det, c->sched = det ? r->sched : poolThread();
    isos.push_back(c), r->live++;
    ready(c);
  }
  // Seeded, the run is the same every time, so fut test --changed may reuse its result (if the seed is its default).
  if (traceOn && (!det || isolateSeed != 1)) traceImpure();
  isolatePoint();  // seeded: the new isolate may go first
  return h;
}
V t_join(V tv) {
  void* ra = RA();
  effRequire(EFF_CHAN, "join()", ra);
  noWaitInTxn("join()", ra);
  isolatePoint();
  TaskState* t = taskOf(tv);
  std::string err;
  {
    Locked l(chanMu);
    while (!t->done)
      if (!(err = waitFor(&t->joiners, "join()", ra)).empty()) break;
    if (err.empty() && !t->joined) {
      t->joined = true;
      auto& f = self()->root->failures;
      if (auto it = std::find(f.begin(), f.end(), t); it != f.end()) f.erase(it), release(t);
    }
  }
  if (!err.empty()) raise(err, ra);
  V v = decode(t->result);  // done: no one writes it again
  if (t->failed) traceAdd(t->trace + "  (in isolate " + std::to_string(t->id) + ", spawned at " + t->at + ")\n"), throwValue(v, ra);
  return v;
}
V t_done(V tv) {
  effRequire(EFF_CHAN, "done()", RA());
  isolatePoint();
  Locked l(chanMu);
  return boxb(taskOf(tv)->done);
}
V n_cores() {
#if ONE_THREAD
  return box(1);
#elif __wasm__
  return box(hostCores());
#else
  return box((double)sysconf(_SC_NPROCESSORS_ONLN));
#endif
}
#if !ONE_THREAD
// Run code as a driver would (main.cpp's run): unchecked effects, no handlers; an error is caught, its text in err.
#if __wasm__
int partRun(void* code, std::string& err) {  // (fut's errors are wasm exceptions here: catchFut catches them)
  void* handler = vm->handler;
  u32 txn = vm->txn, eff = effEnter();
  vm->handler = nullptr;
  V e = NIL;
  std::string trace;
  bool failed = catchFut([](void* c) { vmCall(c, {}); }, code, &e, &trace);
  if (failed) txnUnwind(txn), err = errorLine(e) + "\n" + trace;
  vm->handler = handler, effLeave(eff);
  return failed ? 1 : 0;
}
#else
int partRun(void* code, std::string& err) {
  jmp_buf jb, *outer = errJmp;
  std::string* sink = errSink;
  V* val = errValue;
  void* handler = vm->handler;
  u32 txn = vm->txn, eff = effEnter();
  int st = 0;
  errJmp = &jb, errSink = &err, errValue = nullptr, vm->handler = nullptr;
  if (int r = setjmp(jb)) st = r, txnUnwind(txn);
  else vmCall(code, {});
  errJmp = outer, errSink = sink, errValue = val, vm->handler = handler, effLeave(eff);
  return st;
}
#endif
void parkForReload(Iso* m) {  // (chanMu held, and held again on return)
  m->state = Iso::PARKED, m->rlParked = true, rlLeft();
  chanMu.unlock();
  switchTo(m, nullptr);
  chanMu.lock();
}
// Where a pool isolate meets a reload: at a poll (vm->poll bit 16), or when resumed. It stops, runs its part when
// asked, waits for the verdict, and applies it to its transaction. Inside an atomic block it goes on, and stops at
// the first poll after the block (the bit stays set).
void reloadPoint() {
  Iso* m = me;
  if (!m || m->pinned || m->sched->det) return;
  chanMu.lock();
  for (;;) {
    if (int v = std::exchange(m->rlVerdict, 0)) {
      chanMu.unlock();
      v == 1 ? txnCommit() : txnAbort();
      chanMu.lock();
      continue;
    }
    if (m->rlParted) { parkForReload(m); continue; }  // (resumed by a waker meanwhile)
    if (!rl.phase || vm->txn) break;
    if (rl.phase == RL_STOP || m->rlEpoch == rl.epoch) { parkForReload(m); continue; }
    m->rlEpoch = rl.epoch;
    void *chunk = rl.chunk, *hook = rl.hook;
    chanMu.unlock();
    std::string err;
    txnBegin();
    int st = partRun(chunk, err);
    if (!st && hook) st = partRun(hook, err);
    chanMu.lock();
    if (st && rl.err.empty()) rl.err = "isolate " + std::to_string(m->id) + ": " + err;
    m->rlParted = true, rl.pending--;
    parkForReload(m);
  }
  if (!rl.phase) __atomic_fetch_and(&vm->poll, ~16u, __ATOMIC_RELAXED);
  chanMu.unlock();
}
template <class F> void others(F f) {  // each isolate of the reloader's program but itself (chanMu held)
  for (Iso* i : isos) if (i->root == rl.by->root && i != rl.by) f(i);
}
#endif
}  // namespace

#if !ONE_THREAD
bool isolatesStop() {
  if (isolates <= 1) return false;
  Locked l(chanMu);
  rl.phase = RL_STOP, rl.by = self(), rl.err.clear();
  others([](Iso* i) { if (i->vmb) __atomic_fetch_or(&i->vmb->poll, 16u, __ATOMIC_RELAXED); });
  for (;;) {
    bool running = false;
    others([&](Iso* i) { running |= i->state == Iso::RUNNING; });
    if (!running) return true;
    pthread_cond_wait(&rlCv, &chanMu.m);
  }
}
void isolatesSync() {
  Locked code(codeMu);
  Locked l(chanMu);
  others([](Iso* i) {
    if (i->vmb) for (auto& g : globals) if (g.slot >= 0 && g.o) i->vmb->globals[g.slot] = box(g.o);
  });
}
std::string isolatesParts(void* chunk, void* hook) {
  Locked l(chanMu);
  rl.phase = RL_PARTS, rl.epoch++, rl.chunk = chunk, rl.hook = hook, rl.pending = 0;
  others([](Iso* i) {
    rl.pending++;
    if (i->state == Iso::PARKED) i->rlParked = false, ready(i);  // stopped, or waiting on a channel: resumed for its part
  });
  for (Sched* w : pool) pthread_cond_broadcast(&w->cv);
  while (rl.pending > 0) pthread_cond_wait(&rlCv, &chanMu.m);
  return rl.err;
}
void isolatesVerdict(bool commit) {
  Locked l(chanMu);
  others([&](Iso* i) {
    if (std::exchange(i->rlParted, false)) i->rlVerdict = commit ? 1 : 2;
    if (i->rlParked && i->state == Iso::PARKED) i->rlParked = false, ready(i);
  });
  rl.phase = RL_NONE, rl.by = nullptr;
  for (Sched* w : pool) pthread_cond_broadcast(&w->cv);
}
void isolateNudge() {
  Locked l(chanMu);
  for (Iso* i : isos)
    if (i == i->root && i->pinned && i->waiting && !i->woken) i->nudged = true, pthread_cond_signal(&i->cv);
}
#endif

void isolateExitPoint() {
#if __wasm__ && !ONE_THREAD
  if (exitAsked.load() >= 0 && isolateMainThread()) exit(exitAsked.load());  // (no lock: the worker may have died holding one)
#endif
}
#if __wasm__ && !ONE_THREAD
bool isolateMainThread() { return pthread_equal(pthread_self(), mainThread); }
void isolatesExit(int code) {
  int none = -1;
  exitAsked.compare_exchange_strong(none, code);
  eachVM([](VM* v, void*) { __atomic_fetch_or(&v->poll, 64u, __ATOMIC_RELAXED); }, nullptr);
  bool locked = false;  // (a thread that died holding it never lets go: give up after a while)
  for (int i = 0; i < 2000 && !(locked = !pthread_mutex_trylock(&chanMu.m)); i++) usleep(100);
  if (!locked) return;
  for (Iso* i : isos) if (i->pinned) pthread_cond_broadcast(&i->cv);
  pthread_mutex_unlock(&chanMu.m);
}
#endif
void isolateReloadPoint() {
#if !ONE_THREAD
  reloadPoint();
#endif
}

V chanNext(V ch, void* ra) { return take(ch, true, "for ... in a channel", ra); }
void sharedFree(Obj* o) {
  Shared* s = ((Chan*)o)->s;
  if (handles) handles->erase(s);
  release(s);
}
void isolatesMark(void (*mark)(V)) {
  if (building) for (V v : *building) mark(v);
}
bool isolateStopped() { return me && me->stop.load(std::memory_order_relaxed); }
void isolateYield() {
#if !ONE_THREAD
  __atomic_fetch_and(&vm->poll, ~8u, __ATOMIC_RELAXED);
  Iso* m = me;
  if (!m || m->pinned || m->sched->det || locksHeld) return;
  chanMu.lock();
  if (m->sched->runq.empty()) return chanMu.unlock();
  m->state = Iso::RUNNABLE, m->sched->runq.push_back(m);  // to the back; this thread's loop takes the front
  rlLeft();
  chanMu.unlock();
  switchTo(m, nullptr);
  reloadPoint();
#endif
}
void isolatePoint() {
  Iso* m = me;
  if (!m || !m->sched || !m->sched->det || locksHeld) return;
  Sched* s = m->sched;
  chanMu.lock();
  size_t n = s->runq.size(), k = n ? draw(s) % (n + 1) : n;
  if (k == n) return chanMu.unlock();  // m goes on
  Iso* to = pick(s, k);
  m->state = Iso::RUNNABLE, s->runq.push_back(m);
  chanMu.unlock();
  switchTo(m, to);
}
std::string isolateSeedNote() {
  Iso* r = me ? me->root : nullptr;
  if (r && r->sched && r->sched->det)
    return "its isolates ran under seed " + std::to_string(r->sched->seed) + ": --seed " + std::to_string(r->sched->seed) +
           " replays the same interleaving, and other seeds try others\n";
  return "";
}
void isolateRoot() {
  me = new Iso;
  me->id = 1, me->root = me;
  Locked l(chanMu);
  isos.push_back(me);
}
std::string isolatesFinish(bool failed) {
  Iso* r = self()->root;
  std::string report;
  std::vector<TaskState*> lost;
  {
    Locked l(chanMu);
    if (failed)  // stop the rest: each raises at its next poll or wait
      for (Iso* i : isos)
        if (i->root == r && i != r) {
          i->stop = true, wake(i);
          if (i->vmb) __atomic_fetch_or(&i->vmb->poll, 4u, __ATOMIC_RELAXED);
        }
    while (r->live > 1)
      if (std::string e = waitFor(&r->exitq, "the end of its program, for the isolates it started", nullptr); !e.empty() && report.empty())
        report = e;
    lost = std::move(r->failures), r->failures.clear();
  }
  if (report.empty() && !failed)
    for (TaskState* t : lost) {
      V e = decode(t->result);
      report += (report.empty() ? "" : "\n") + std::string("isolate ") + std::to_string(t->id) + ", spawned at " + t->at +
                ", failed and nothing joined it: " + toStr(e) + "\n" + t->trace;
      while (!report.empty() && report.back() == '\n') report.pop_back();
    }
  for (TaskState* t : lost) release(t);
  r->nextId = 2;
  return report;
}
void isolatesInit() {
  if (!me) isolateRoot();
  native<n_spawn>("spawn@"), native<n_channel>("channel"), native<n_cores>("cores");
  method<c_send>(K::Chan, S_send), method<c_recv>(K::Chan, intern("recv")), method<c_tryRecv>(K::Chan, intern("tryRecv"));
  method<c_close>(K::Chan, intern("close"));
  method<t_join>(K::Task, intern("join")), method<t_done>(K::Task, S_done);
}
