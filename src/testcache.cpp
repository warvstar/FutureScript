// fut test --changed: each test's trace (what it executed and resolved, with their hashes) and the cache of results.
//
// Soundness: a test that is deterministic (nothing it ran did io, read the clock or drew a random number) does the same
// thing again if everything it used is as it was: the function bodies it entered (a lambda counts as its enclosing
// function), the method lookups it resolved (receiver class, name -> the method found, or none: a new override changes
// the answer), the top-level vars and classes its code names, and every file's other top-level code (imports, externs,
// effects, statements: conservatively, all of it). New code is reached only through one of those. The top level runs
// once, before the tests are forked, so its trace is part of every test's. A cached pass is reported only if every
// hash still matches, and only for the binary that recorded it.
#include "fut.h"
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#if !__wasm__
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr uint64_t FNV = 1469598103934665603ull, PRIME = 1099511628211ull;
uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * PRIME; }
uint64_t fnvStr(const std::string& s) {
  uint64_t h = FNV;
  for (unsigned char c : s) h = (h ^ c) * PRIME;
  return h;
}

// ---- recording ----
std::deque<uint64_t> marks;  // stable addresses: JIT code stores to them
std::vector<std::string> markName;
std::unordered_map<std::string, size_t> markOf;
std::unordered_map<std::string, std::set<int>> refs;
Mutex lookMu;  // isolates a test starts resolve methods too
std::set<std::pair<int64_t, int>> lookups;
std::atomic<bool> impure{false};

// ---- the program's hashes (the compiler's) ----
std::unordered_map<std::string, uint64_t> defs;
struct ClassInfo { std::string super; uint64_t shape; };
std::unordered_map<std::string, ClassInfo> classes;
std::unordered_map<std::string, std::string> methodClass;  // "C.m" -> "C"

uint64_t chain(std::string c) {  // a class's shape with its superclasses'
  uint64_t h = FNV;
  for (int n = 0; !c.empty() && n < 256; n++) {
    auto it = classes.find(c);
    if (it == classes.end()) return mix(h, 0x9e3779b97f4a7c15ull);
    h = mix(h, it->second.shape), c = it->second.super;
  }
  return h;
}
Class* classNamed(const std::string& c) {
  Global* g = findGlobal(intern(c));
  return g && g->o && g->o->kind == K::Class ? (Class*)g->o : nullptr;
}
// An item's hash in the program as compiled now; false if it no longer exists.
bool itemHash(const std::string& item, uint64_t& h) {
  if (item.size() < 2 || item[1] != ':') return false;
  std::string n = item.substr(2);
  switch (item[0]) {
    case 'f': case 'v': case 't': {
      auto it = defs.find(item);
      if (it == defs.end()) return false;
      h = it->second;
      if (auto m = methodClass.find(n); item[0] == 'f' && m != methodClass.end()) h = mix(h, chain(m->second));
      return true;
    }
    case 'c': return classes.count(n) && (h = chain(n), true);
    case 'l': {  // "l:C m": method m looked up on an instance of C
      size_t sp = n.rfind(' ');
      if (sp == std::string::npos || !classes.count(n.substr(0, sp))) return false;
      Class* c = classNamed(n.substr(0, sp));
      if (!c) return false;
      int m = intern(n.substr(sp + 1));
      std::string owner;
      for (Class* k = c; k && owner.empty(); k = k->super)
        for (auto& [s, f] : k->methods) if (s == m) { owner = symName((int)k->name) + "." + symName(m); break; }
      h = mix(fnvStr(owner), chain(n.substr(0, sp)));
      return true;
    }
  }
  return false;
}

// ---- the cache ----
struct Entry { uint64_t body = 0; char code = 'e'; bool pure = false; std::vector<std::pair<std::string, uint64_t>> items; };
TestRun opts;
std::string cachePath, binId;
std::map<std::string, Entry> old, fresh;
std::unordered_map<const TestCase*, std::string> keyOf;
std::unordered_set<const TestCase*> cached;
std::vector<std::string> keys;  // every test's, in order
const TestCase* current;
[[maybe_unused]] int parent;
bool icsCleared;

[[maybe_unused]] std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) o += c == '\\' ? "\\\\" : c == '\t' ? "\\t" : c == '\n' ? "\\n" : std::string(1, c);
  return o;
}
std::string unesc(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++)
    if (s[i] != '\\' || i + 1 == s.size()) o += s[i];
    else o += s[++i] == 't' ? '\t' : s[i] == 'n' ? '\n' : s[i];
  return o;
}
std::string hex(uint64_t v) { char b[20]; snprintf(b, sizeof b, "%llx", (unsigned long long)v); return b; }
uint64_t unhex(const std::string& s) { return strtoull(s.c_str(), nullptr, 16); }

#if !__wasm__
std::string binaryId() {  // the fut that recorded a trace: another build may run the same code differently
  char path[4096];
  uint32_t n = sizeof path;
  struct stat st;
  if (_NSGetExecutablePath(path, &n) || stat(path, &st)) return "";
  return hex((uint64_t)st.st_size) + "-" + hex((uint64_t)st.st_mtimespec.tv_sec * 1000000000ull + st.st_mtimespec.tv_nsec) + "-" + hex((uint64_t)st.st_ino);
}
#else
std::string binaryId() { return ""; }
#endif

void load() {
  std::string text;
  if (cachePath.empty() || !readFile(cachePath, text)) return;
  size_t at = 0;
  auto line = [&](std::string& l) {
    if (at >= text.size()) return false;
    size_t e = text.find('\n', at);
    if (e == std::string::npos) e = text.size();
    return l = text.substr(at, e - at), at = e + 1, true;
  };
  std::string l;
  if (!line(l) || l != "fut tests 1 " + binId) return;  // another fut (or format) recorded it: nothing holds
  Entry* e = nullptr;
  while (line(l)) {
    size_t t1 = l.find('\t');
    if (t1 == std::string::npos) continue;
    if (l.starts_with("T ")) {  // T key \t body \t code \t pure
      size_t t2 = l.find('\t', t1 + 1), t3 = t2 == std::string::npos ? t2 : l.find('\t', t2 + 1);
      if (t3 == std::string::npos) { e = nullptr; continue; }
      e = &old[unesc(l.substr(2, t1 - 2))];
      *e = {unhex(l.substr(t1 + 1, t2 - t1 - 1)), l[t2 + 1], l.substr(t3 + 1) == "1", {}};
    } else if (e) e->items.push_back({unesc(l.substr(0, t1)), unhex(l.substr(t1 + 1))});
  }
}
bool holds(const TestCase& t, const Entry& e) {
  if (e.code != 'p' || !e.pure || e.body != t.hash || e.items.empty()) return false;
  for (auto& [item, h] : e.items) {
    uint64_t now;
    if (!itemHash(item, now) || now != h) return false;
  }
  return true;
}
}  // namespace

uint64_t* traceMark(const std::string& owner) {
  auto [it, added] = markOf.try_emplace(owner, marks.size());
  if (added) marks.push_back(0), markName.push_back(owner);
  return &marks[it->second];
}
void traceRef(const std::string& owner, int sym) { refs[owner].insert(sym); }
void traceLookup(int64_t cls, int sym) { Locked l(lookMu); lookups.insert({cls, sym}); }
void traceImpure() { impure.store(true, std::memory_order_relaxed); }
void traceDefine(const std::string& item, uint64_t hash) { defs[item] = hash; }
void traceClass(const std::string& cls, const std::string& super, uint64_t shape) { classes[cls] = {super, shape}; }
void traceMethod(const std::string& q, const std::string& cls) { methodClass[q] = cls; }
std::string traceTestRefs(uint64_t testHash) { return "T:" + hex(testHash); }

void traceBegin(const TestRun& o) {
  opts = o, traceOn = !WASM && !o.isolates;
#if !__wasm__
  parent = getpid();
#endif
}
int testsToRun(const std::vector<TestCase>& tests) {
  if (!traceOn) return 1;
  std::string main = canonicalPath(mainFile);
  size_t slash = main.rfind('/');
  std::string dir = main.substr(0, slash + 1), stem = main.substr(slash + 1);
  cachePath = dir + ".fut-cache/" + stem.substr(0, stem.rfind('.')) + ".tests", binId = binaryId();
  if (binId.empty()) cachePath.clear();
  load();
  std::unordered_map<std::string, int> seen;
  int run = 0;
  for (auto& t : tests) {
    std::string k = canonicalPath(t.file) + "\t" + t.name;
    if (int n = seen[k]++) k += "#" + std::to_string(n + 1);
    keyOf[&t] = k, keys.push_back(k);
    if (!t.pending.empty() || (!opts.filter.empty() && t.name.find(opts.filter) == std::string::npos)) continue;
    auto it = old.find(k);
    if (opts.changed && it != old.end() && holds(t, it->second)) cached.insert(&t);
    else run++;
  }
  return opts.changed ? run : 1;
}
bool traceCached(const TestCase& t) { return cached.count(&t); }
void traceStart(const TestCase& t) {
  current = &t;
  // The top level has run (its trace is every test's). Its method caches go, so each test's lookups miss once and are seen.
  if (traceOn && !icsCleared) clearICs(), icsCleared = true;
}
std::string traceReport(const TestCase& t) {
#if __wasm__
  return "";
#else
  if (!traceOn || getpid() == parent) return "";
  std::set<std::string> items;
  auto named = [&](const std::string& owner) {
    auto it = refs.find(owner);
    if (it == refs.end()) return;
    for (int s : it->second)
      if (Global* g = findGlobal(s)) {
        if (g->kind == 'v') items.insert("v:" + symName(s));
        else if (g->kind == 'c') items.insert("c:" + symName(s));
      }
  };
  for (size_t i = 0; i < marks.size(); i++)
    if (marks[i]) items.insert("f:" + markName[i]), named(markName[i]);
  {
    Locked l(lookMu);
    for (auto [c, m] : lookups) items.insert("l:" + symName((int)c) + " " + symName(m));
  }
  named("<top>"), named(traceTestRefs(t.hash));
  for (auto& [item, h] : defs) if (item.starts_with("t:")) items.insert(item);
  std::string o(1, '\0');
  o += impure.load(std::memory_order_relaxed) ? 'i' : 'p';
  for (auto& item : items) {
    uint64_t h;
    if (!itemHash(item, h)) return std::string(1, '\0') + 'i';  // something it used has no hash: never cached
    o += esc(item) + "\t" + hex(h) + "\n";
  }
  return o;
#endif
}
void traceResult(char code, const std::string& trace) {
  if (!traceOn || !current || !keyOf.count(current)) return;
  Entry& e = fresh[keyOf[current]];
  e = {current->hash, code, !trace.empty() && trace[0] == 'p', {}};
  for (size_t at = 1; at < trace.size();) {
    size_t nl = trace.find('\n', at), tab = trace.find('\t', at);
    if (nl == std::string::npos || tab == std::string::npos || tab > nl) break;
    e.items.push_back({unesc(trace.substr(at, tab - at)), unhex(trace.substr(tab + 1, nl - tab - 1))});
    at = nl + 1;
  }
  current = nullptr;
}
void traceEnd() {
#if !__wasm__
  if (!traceOn || cachePath.empty()) return;
  std::string o = "fut tests 1 " + binId + "\n";
  for (auto& k : keys) {
    auto it = fresh.find(k);
    if (it == fresh.end() && (it = old.find(k)) == old.end()) continue;  // (tests that are gone are dropped)
    Entry& e = it->second;
    o += "T " + esc(k) + "\t" + hex(e.body) + "\t" + e.code + "\t" + (e.pure ? "1" : "0") + "\n";
    for (auto& [item, h] : e.items) o += esc(item) + "\t" + hex(h) + "\n";
  }
  std::string dir = cachePath.substr(0, cachePath.rfind('/')), tmp = cachePath + "." + std::to_string(getpid());
  mkdir(dir.c_str(), 0755);
  if (FILE* f = fopen(tmp.c_str(), "w")) {  // written whole, then renamed over: a concurrent run reads one or the other
    bool ok = fwrite(o.data(), 1, o.size(), f) == o.size();
    if (fclose(f) || !ok || rename(tmp.c_str(), cachePath.c_str())) unlink(tmp.c_str());
  }
#endif
}
