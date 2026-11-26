// Shared vars: `shared var hits = 0` is one variable for every isolate of the program, not a copy per isolate as
// other globals are. It holds frozen values only (assigning freezes a copy, so no isolate can change what another
// reads), and it is assigned only inside atomic { }, which makes the blocks that use shared vars transactions across
// isolates (software transactional memory). docs/isolates.md, phase 7.
//
// Each var keeps its committed versions, numbered by a clock that each commit that writes advances. A block reads
// as of a snapshot: the clock when it first touched a shared var. So what it reads is always one consistent state,
// however others commit meanwhile, and nothing it does depends on a state that never existed. Its writes wait in
// a write set. At its end it commits if no var it read has a version newer than its snapshot; otherwise it rolls
// back (txn.cpp) and runs again, as the compiler arranges (atomicStmt). Old versions go once no running block's
// snapshot can need them. Outside atomic, a read gives the latest committed value. All under frozenMu (FrozenLock),
// which also keeps a value read from being freed before the reader holds it.
#include "fut.h"
#include <set>
#include <unordered_map>

namespace {
struct Version { uint64_t at; V v; };
struct Cell {
  std::string name;
  bool set = false;
  std::vector<Version> vs;  // oldest first; each holds a reference on its value
};
constexpr int MAXCELLS = 1 << 16;
Cell* cells[MAXCELLS];  // (made at compile time, never freed: compiled code names them by index)
int ncells;
std::unordered_map<std::string, int>& byName = *new std::unordered_map<std::string, int>;
uint64_t versions;  // (frozenMu) the clock: commits that wrote so far
std::multiset<uint64_t>& snapshots = *new std::multiset<uint64_t>;  // (frozenMu) those of running blocks

struct Tx {  // the running isolate's outermost atomic block, once it touches a shared var
  bool on = false;
  uint64_t at = 0;
  std::vector<int> reads;
  std::vector<std::pair<int, V>> writes;  // each holds a reference on its value
};
TLS Tx tx;
ISO_LOCAL(tx);

Cell& cell(V i) { return *cells[(int)num(i)]; }
void prune(FrozenLock& l, Cell& c) {  // the versions no snapshot needs: all before the newest at or before the oldest
  uint64_t oldest = snapshots.empty() ? UINT64_MAX : *snapshots.begin();
  size_t keep = c.vs.size() - 1;
  while (keep > 0 && c.vs[keep].at > oldest) keep--;
  if (!keep) return;
  for (size_t i = 0; i < keep; i++) l.drop(c.vs[i].v);
  c.vs.erase(c.vs.begin(), c.vs.begin() + keep);
}
void end(FrozenLock& l) {  // (frozenMu) the block is done with its snapshot and write set
  if (!tx.on) return;
  for (auto& [i, v] : tx.writes) l.drop(v);
  snapshots.erase(snapshots.find(tx.at));
  tx = Tx{};
}
void parallelCheck(const char* what) {
  if (txnInParallel()) ERR("@parallel a parallel for's body can't %s a shared var: its result would depend on when each iteration ran", what);
}

V n_get(V i) {  // shared@get(cell): its value
  parallelCheck("read");
  Cell& c = cell(i);
  if (!txnAtomic()) {
    FrozenLock l;
    V v = c.vs.back().v;
    return l.hold(v), v;
  }
  for (auto& [j, v] : tx.writes) if (j == (int)num(i)) return v;
  FrozenLock l;
  if (!tx.on) tx.on = true, tx.at = versions, snapshots.insert(tx.at);
  V v = c.vs.front().v;
  for (auto& x : c.vs) if (x.at <= tx.at) v = x.v;
  tx.reads.push_back((int)num(i));
  return l.hold(v), v;
}
V n_set(V i, V v) {  // shared@set(cell, value): inside atomic { }, a frozen copy of value, committed with the block
  parallelCheck("assign");
  Cell& c = cell(i);
  if (!txnAtomic())
    ERR("@atomic shared var '%s' can be assigned only inside atomic { }: every isolate shares it, so each change must be all or nothing", c.name.c_str());
  V f = freezeValue(v);
  FrozenLock l;
  if (!tx.on) tx.on = true, tx.at = versions, snapshots.insert(tx.at);
  l.ref(f), l.hold(f);
  for (auto& [j, w] : tx.writes)
    if (j == (int)num(i)) return l.drop(w), w = f, f;
  tx.writes.push_back({(int)num(i), f});
  return f;
}
V n_init(V i, V v) {  // shared@init(cell, value): its declaration. Only the first one sets it: a reload keeps it
  Cell& c = cell(i);
  V f = freezeValue(v);
  FrozenLock l;
  if (!c.set) l.ref(f), c.vs = {{0, f}}, c.set = true;
  return NIL;
}
}  // namespace

int sharedCell(const std::string& name) {
  FrozenLock l;
  if (auto it = byName.find(name); it != byName.end()) return it->second;
  if (ncells == MAXCELLS) return -1;
  cells[ncells] = new Cell{name, false, {{0, NIL}}};
  return byName[name] = ncells++;
}
bool sharedCommit() {
  if (!tx.on) return true;
  FrozenLock l;
  bool ok = true;
  for (int i : tx.reads)
    if (!tx.writes.empty() && cells[i]->vs.back().at > tx.at) ok = false;  // (a block that only reads saw one state)
  std::vector<int> wrote;
  if (ok && !tx.writes.empty()) {
    uint64_t at = ++versions;
    for (auto& [i, v] : tx.writes) cells[i]->vs.push_back({at, v}), wrote.push_back(i);  // (its reference goes with it)
    tx.writes.clear();
  }
  end(l);
  for (int i : wrote) prune(l, *cells[i]);
  return ok;
}
void sharedAbort() {
  if (!tx.on) return;
  FrozenLock l;
  end(l);
}
void sharedInit() { native<n_get>("shared@get"), native<n_set>("shared@set"), native<n_init>("shared@init"); }
