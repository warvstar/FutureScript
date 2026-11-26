// Isolates and channels (docs/isolates.md): `spawn f(args)` runs f in an isolate of its own, on a thread of its own,
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
#include "fut.h"
#include <unordered_map>
#if !__wasm__
#include <pthread.h>
#include <unistd.h>
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
struct Iso {
  int id;
  Iso* root;               // its program's first isolate (itself, for a root)
  TaskState* task = nullptr;
#if !__wasm__
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

namespace {
Mutex chanMu;
std::vector<Iso*> isos;  // every isolate (under chanMu): for deadlock reports, and to stop a program's
TLS Iso* me;
TLS std::unordered_map<Shared*, Obj*>* handles;  // this isolate's handle on each channel and task (one each)
TLS std::vector<V>* building;                    // objects a message is being rebuilt into (GC roots)

Iso* self() {
  if (!me) isolateRoot();
  return me;
}
void wake(Iso* i) {  // (chanMu held)
  if (!i->waiting || i->woken) return;
  i->woken = true, i->root->blocked--;
#if !__wasm__
  pthread_cond_signal(&i->cv);
#endif
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
// Wait (chanMu held) until woken; q: the list a waker finds this isolate in. "" or the error to raise.
std::string waitFor(std::vector<Iso*>* q, const char* what, void* ra) {
  Iso* m = self();
  if (m->stop) return STOPPED;
  if (q) q->push_back(m);
  m->waiting = true, m->woken = false, m->what = what, m->ra = ra;
  Iso* r = m->root;
  if (++r->blocked >= r->live) deadlock(r);
#if !__wasm__
  while (!m->woken) pthread_cond_wait(&m->cv, &chanMu.m);
#endif
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
      if (!WASM && !gcInImage(o)) ERR("cannot send %s: it is not compiled code's", typeName(v));
      return m.b += 'v', raw(v);
    }
    if (o->kind == K::Fiber) ERR("cannot send a fiber: it runs on its own isolate's stacks");
    if (o->kind == K::Ref) ERR("cannot send a js value");
    if (++depth > MAX_DEPTH) ERR("cannot send a value nested more than %d deep", MAX_DEPTH);
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
        if (!WASM && !gcInImage(c->fn)) ERR("cannot send this function");
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
      default: ERR("cannot send %s", typeName(v));
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
V decode(const Msg& m) { Reader r(m); return r.get(); }

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
    rtError(ra, "%s inside atomic { }: waiting for another isolate would hold the block open (do it before or after the block)", what);
}
// The next message (waiting for one unless `wait` is false), or END once the channel is closed and drained.
V take(V ch, bool wait, const char* what, void* ra) {
  effRequire(EFF_CHAN, what, ra);
  if (wait) noWaitInTxn(what, ra);
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
  if (v == END) ERR("recv() on a closed channel, and nothing is left in it");
  return v;
}
V c_tryRecv(V ch) { V v = take(ch, false, "tryRecv()", RA()); return v == END ? NIL : v; }
V c_close(V ch) {
  effRequire(EFF_CHAN, "close()", RA());
  ChanState* c = chanOf(ch);
  bool was;
  {
    Locked l(chanMu);
    if (!(was = c->closed)) c->closed = true, wakeAll(c->recvq), wakeAll(c->sendq);
  }
  if (was) ERR("close() of a channel already closed");
  return NIL;
}
V n_channel(V n) {
  if (!isNum(n) || num(n) < 0 || num(n) > 1e9 || num(n) != (double)(int64_t)num(n))
    ERR("channel(n) expects how many values it may hold: a whole number, 0 or more (0: each send waits for a receiver)");
  ChanState* c = new ChanState;
  c->cap = (size_t)num(n);
  V h = handle(c, K::Chan);
  return release(c), h;  // the handle's reference is the only one
}

// ---- isolates ----
#if !__wasm__
void* isoMain(void* p) {
  Iso* m = me = (Iso*)p;
  TaskState* t = m->task;
  vm = vmNew();
  Heap* h = heapNew(vm->runs);
  heapUse(h), threadInit();
  {
    Locked l(chanMu);
    m->vmb = vm;
    if (m->stop) vm->poll |= 4;
  }
  vm->allowed = t->allowed, vm->who = t->who;
  outSink = t->out;
  std::string trace;
  V err = NIL;
  jmp_buf jb;
  errJmp = &jb, errValue = &err, errSink = &trace;
  Msg result;
  volatile bool failed = false, again = false;
  if (setjmp(jb)) {
    failed = true;
    if (again) result = encode(box(newStr(toStr(err))));  // its error could not be sent itself: its text then
    else again = true, result = encode(err);
  } else {
    volatile V a[8] = {};  // on the stack, so the GC sees them
    bool clo;
    void* code;
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
    result = encode(vmEnter(code, (V*)a));
  }
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
  {  // gone: its program may end now (so nothing after this touches what exiting destroys)
    Locked l(chanMu);
    r->live--, std::erase(isos, m), wakeAll(r->exitq);
    if (r->blocked && r->blocked >= r->live) deadlock(r);
  }
  isolates--;
  pthread_cond_destroy(&m->cv);
  delete m;
  return nullptr;
}
#endif
V n_spawn(V f, V args) {
  void* ra = RA();
  effRequire(EFF_SPAWN, "spawn", ra);
  if (traceOn) traceImpure();  // how isolates interleave isn't deterministic: fut test --changed always reruns the test
  if (vm->txn) rtError(ra, "spawn inside atomic { }: an isolate, once started, can't be taken back (spawn after the block)");
  if (WASM) rtError(ra, "spawn needs the native build for now (in wasm, isolates will be web workers)");
  Fn* fn = is(f, K::Clo) ? as<Clo>(f)->fn : is(f, K::Fn) ? as<Fn>(f) : nullptr;
  if (!fn) rtError(ra, "spawn expects a function to call, got %s", typeName(f));
  if (fn->op || !fn->code) rtError(ra, "spawn: %s is a builtin; spawn a function that calls it", symName(fn->name).c_str());
  size_t n = as<List>(args)->v.size();
  int want = fn->arity - is(f, K::Clo);
  if (want != (int)n) rtError(ra, "%s expects %d arguments, got %d", symName(fn->name).c_str(), want, (int)n);
  if (n > 7) rtError(ra, "spawn passes at most 7 arguments (put more in a list)");
  Writer w;  // one message, so values the arguments and globals share stay shared
  w.put(f), w.put(args);
  int ng = globalCount();
  w.raw((u32)ng);
  for (int i = 0; i < ng; i++) w.put(vm->globals[i]);
  TaskState* t = new TaskState;  // one reference for the isolate, one for this handle
  t->start = std::move(w.m), t->at = codeWhere(ra), t->allowed = vm->allowed, t->who = vm->who, t->out = outBase();
  V h = handle(t, K::Task);
#if !__wasm__
  Iso* c = new Iso;
  Iso* r = self()->root;
  {
    Locked l(chanMu);
    c->id = t->id = r->nextId++, c->root = r, c->task = t;
    isos.push_back(c), r->live++;
  }
  isolates++;
  pthread_attr_t a;
  pthread_attr_init(&a), pthread_attr_setstacksize(&a, 64 << 20), pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_t th;
  int e = pthread_create(&th, &a, isoMain, c);
  pthread_attr_destroy(&a);
  if (e) {
    { Locked l(chanMu); r->live--, std::erase(isos, c); }
    isolates--, delete c, release(t);
    rtError(ra, "spawn: could not start a thread (%s)", strerror(e));
  }
#endif
  return h;
}
V t_join(V tv) {
  void* ra = RA();
  effRequire(EFF_CHAN, "join()", ra);
  noWaitInTxn("join()", ra);
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
  Locked l(chanMu);
  return boxb(taskOf(tv)->done);
}
V n_cores() {
#if __wasm__
  return box(1);
#else
  return box((double)sysconf(_SC_NPROCESSORS_ONLN));
#endif
}
}  // namespace

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
