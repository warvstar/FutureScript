// Types: optional annotations on parameters, results, declared fields and vars (`fn f(n: num): str?`,
// `var hp: num = 10`). Code without them is as dynamic as ever. Where one is written it is checked at run time, at
// that boundary: an argument as the function is entered, a result as it returns, a field or var on every store. So a
// caller compiled against an older version of a function, or one typed at the REPL, still gets a clean error naming
// the boundary, rather than a wrong value travelling on.
//
// Besides the kinds (num str ... a class), a type can be compound: a union (`num | str`), a literal (`"GET"`), a shape
// (`{name: str, age: num?}`, which maps and instances match by their fields), or a list or map of an element type
// (`list<num>`, `map<str, User>`). A named type (`type User = {...}`) is looked up when it is checked, like a class, so
// editing it changes every check of it without recompiling them, and it may refer to itself.
#include "fut.h"
#include <unordered_map>

struct TypeSite { std::string what, type; };
static std::vector<TypeSite> sites;
static std::unordered_map<std::string, int> siteIds;  // compiling the same function again reuses its sites

int typeSite(const std::string& what, const std::string& type) {
  auto [it, fresh] = siteIds.try_emplace(what + '\0' + type, (int)sites.size());
  if (fresh) sites.push_back({what, type});
  return it->second;
}

// Compound types: a table that only grows, read by every isolate without a lock (an entry is complete before the
// count that publishes it).
static constexpr size_t MAXDESC = 1 << 16;
static TypeDesc* descs[MAXDESC];
static std::atomic<u32> ndesc{0};
static Mutex descMu;
static std::unordered_map<std::string, u32> descIds;

uint64_t typeIntern(const std::string& text, TypeDesc d) {
  Locked g(descMu);
  if (auto it = descIds.find(text); it != descIds.end()) return TY_EXT | uint64_t(it->second) << 8;
  u32 i = ndesc.load(std::memory_order_relaxed);
  if (i == MAXDESC) ERR("@limit too many distinct compound types");
  descs[i] = new TypeDesc(std::move(d));
  ndesc.store(i + 1, std::memory_order_release);
  return descIds[text] = i, TY_EXT | uint64_t(i) << 8;
}
uint64_t typeResolve(uint64_t t) {
  for (int n = 0; (t & 15) == TY_NAMED && n < 32; n++) {
    Global* g = findGlobal(int(t >> 8));
    if (!g || g->kind != 't' || !g->type) return t;
    t = g->type | (t & TY_OPT);
  }
  return t;
}
const TypeDesc* typeDesc(uint64_t t) {
  t = typeResolve(t);
  if ((t & 15) != TY_EXT) return nullptr;
  u32 i = u32(t >> 8);
  return i < ndesc.load(std::memory_order_acquire) ? descs[i] : nullptr;
}

static bool numEq(double a, double b) { return a == b; }
static bool isFnType(uint64_t t) { t = typeResolve(t) & ~TY_OPT; return t == TY_FN || t == TY_ANY; }
// A list or map that passes a check of its element type at run time keeps that type (the code of its list<T> or
// map<K, V>): later checks of the same type pass at once, and stores into it are checked (typeWrite). The ones a
// check passes are tagged once the whole check has: tags collects them. An alternative of a union tags nothing,
// since a value could pass more than one.
using Tags = std::vector<std::pair<Obj*, uint64_t>>;
// depth: compound types nest (and named ones recurse), so data nested deeper than this is taken on trust.
static bool accepts(uint64_t t, V v, int depth, Tags* tags = nullptr) {
  if (v == NIL && (t & TY_OPT)) return true;
  switch (t & 15) {
    case TY_ANY: return true;
    case TY_NUM: return isNum(v);
    case TY_STR: return is(v, K::Str);
    case TY_BOOL: return v == TRUE_ || v == FALSE_;
    case TY_LIST: return is(v, K::List);
    case TY_MAP: return is(v, K::Map);
    case TY_BUF: return is(v, K::Buf) && as<Buf>(v)->et == char(t >> 8) && (!(t >> 16 & 255) || as<Buf>(v)->n == (t >> 16 & 255));  // vec2f: an f32s of 2
    case TY_FN: return is(v, K::Fn) || is(v, K::Clo);
    case TY_CLS: {
      if (!is(v, K::Inst)) return false;
      Global* g = findGlobal(int(t >> 8));
      Obj* want = g ? g->o : nullptr;
      for (Class* c = as<Inst>(v)->cls; c; c = c->super) if (c == want) return true;
      return false;
    }
    case TY_NAMED: {
      uint64_t r = typeResolve(t);
      return (r & 15) != TY_NAMED && (depth > 64 || accepts(r, v, depth + 1, tags));
    }
    case TY_EXT: {
      const TypeDesc* d = typeDesc(t);
      if (!d) return false;
      if (depth > 64) return true;
      switch (d->k) {
        case 'u':
          for (uint64_t a : d->alts) if (accepts(a, v, depth + 1)) return true;
          return false;
        case 'l':
          if (d->lit == 's') return is(v, K::Str) && as<Str>(v)->sv() == d->s;
          if (d->lit == 'n') return isNum(v) && numEq(num(v), d->n);
          return v == d->v;
        case 's': {
          if (!is(v, K::Map) && !is(v, K::Inst)) return false;
          for (auto& [f, ft] : d->fields) {
            V x = NIL;
            bool m = false, has = fieldOf(v, f, &x, &m);
            if (m) { if (isFnType(ft)) continue; return false; }  // a method: what a fn (or any) field asks for
            if (!accepts(ft, has ? x : NIL, depth + 1, tags)) return false;
          }
          return true;
        }
        case 'L': {
          if (!is(v, K::List)) return false;
          List* l = as<List>(v);
          uint64_t code = typeResolve(t) & ~TY_OPT;
          if ((l->typed & 1) && l->etype == code) return true;
          for (V x : l->v) if (!accepts(d->elem, x, depth + 1, tags)) return false;
          if (tags && !(l->typed & (1 | FROZEN))) tags->push_back({l, code});  // (a frozen one never changes: nothing to check)
          return true;
        }
        case 'M': {
          if (!is(v, K::Map)) return false;
          Map* m = as<Map>(v);
          uint64_t code = typeResolve(t) & ~TY_OPT;
          if ((m->typed & 1) && m->etype == code) return true;
          for (auto& [k, x] : m->e)
            if (!accepts(d->key, k, depth + 1, tags) || !accepts(d->elem, x, depth + 1, tags)) return false;
          if (tags && !(m->typed & (1 | FROZEN))) tags->push_back({m, code});
          return true;
        }
      }
      return false;
    }
  }
  return false;
}
bool typeAccepts(uint64_t t, V v, int site) {
  if (site < 0) return accepts(t, v, 0);
  Tags tags;
  if (!accepts(t, v, 0, &tags)) return false;
  for (auto [o, code] : tags) {
    o->typed |= 1;
    if (o->kind == K::List) ((List*)o)->etype = code, ((List*)o)->esite = site;
    else ((Map*)o)->etype = code, ((Map*)o)->esite = site;
  }
  return true;
}

static std::string describe(V v) {  // the value a check rejected, for its message
  if (isNum(v)) return "the number " + toStr(v);
  if (v == NIL) return "nil";
  if (v == TRUE_ || v == FALSE_) return toStr(v);
  if (is(v, K::Str)) {
    std::string s = toStr(v, true);
    return "the string " + (s.size() > 40 ? s.substr(0, 37) + "...\"" : s);
  }
  if (is(v, K::Inst) && as<Inst>(v)->cls->sum == 2) {  // an enum's value: as made (Shape.Rect(3, 4))
    std::string s = toStr(v);
    return s.size() > 40 ? s.substr(0, 37) + "..." : s;
  }
  if (is(v, K::Inst)) return "an instance of " + symName((int)as<Inst>(v)->cls->name);
  if (is(v, K::Class)) return "the class " + symName((int)as<Class>(v)->name);
  return std::string("a ") + typeName(v);
}
static int distance(std::string_view a, std::string_view b) {  // edit distance, for "did you mean"
  std::vector<int> row(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++) row[j] = (int)j;
  for (size_t i = 1; i <= a.size(); i++) {
    int diag = row[0];
    row[0] = (int)i;
    for (size_t j = 1; j <= b.size(); j++) {
      int up = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] != b[j - 1])});
      diag = up;
    }
  }
  return row[b.size()];
}
// A type as written, from its code (for messages about the part of a compound type that failed).
std::string typeText(uint64_t t) {
  std::string q = t & TY_OPT ? "?" : "";
  switch (t & 15) {
    case TY_ANY: return "any";
    case TY_NUM: return "num" + q;
    case TY_STR: return "str" + q;
    case TY_BOOL: return "bool" + q;
    case TY_LIST: return "list" + q;
    case TY_MAP: return "map" + q;
    case TY_FN: return "fn" + q;
    case TY_CLS: case TY_NAMED: return symName(int(t >> 8)) + q;
    case TY_BUF: return ((t >> 16 & 255) ? "vec" + std::to_string(t >> 16 & 255) + "f" : std::string(bufName(char(t >> 8)))) + q;
  }
  const TypeDesc* d = typeDesc(t);
  if (!d) return "?";
  std::string o;
  switch (d->k) {
    case 'u': for (uint64_t a : d->alts) o += (o.empty() ? "" : " | ") + typeText(a); return q.empty() ? o : "(" + o + ")?";
    case 'l': return (d->lit == 's' ? "\"" + d->s + "\"" : d->lit == 'n' ? toStr(box(d->n)) : toStr(d->v)) + q;
    case 's': for (auto& [f, ft] : d->fields) o += (o.empty() ? "" : ", ") + symName(f) + ": " + typeText(ft); return "{" + o + "}" + q;
    case 'L': return "list<" + typeText(d->elem) + ">" + q;
    case 'M': return "map<" + ((d->key & 15) == TY_ANY ? "" : typeText(d->key) + ", ") + typeText(d->elem) + ">" + q;
  }
  return "?";
}
// Why v fails compound type t, for the message: where in v (".kids[0].value") and what is wrong there. "" when the
// whole value is simply not of t (the message says so then).
static std::string why(uint64_t t, V v, int depth, const std::string& at) {
  const TypeDesc* d = typeDesc(t);
  if (!d || depth > 8 || (v == NIL && (t & TY_OPT))) return "";
  auto inner = [&](uint64_t et, V x, const std::string& here) {
    std::string w = why(et, x, depth + 1, here);
    return w.empty() ? here + " must be " + typeText(et) + ", got " + describe(x) : w;
  };
  if (d->k == 's' && (is(v, K::Map) || is(v, K::Inst))) {
    for (auto& [f, ft] : d->fields) {
      V x = NIL;
      bool m = false, has = fieldOf(v, f, &x, &m);
      if (m && isFnType(ft)) continue;
      std::string here = at + "." + symName(f);
      if (m) return here + " is a method, but the type wants " + typeText(ft);
      if (accepts(ft, has ? x : NIL, 0)) continue;
      if (has) return inner(ft, x, here);
      std::string near;
      if (is(v, K::Map))
        for (auto& [k, _] : as<Map>(v)->e)
          if (is(k, K::Str) && distance(as<Str>(k)->sv(), symName(f)) <= 2) near = std::string(as<Str>(k)->sv());
      return here + " is missing (it must be " + typeText(ft) + ")" + (near.empty() ? "" : "; there is a '" + near + "': a misspelling?");
    }
  }
  if (d->k == 'L' && is(v, K::List)) {
    auto& l = as<List>(v)->v;
    for (size_t i = 0; i < l.size(); i++)
      if (!accepts(d->elem, l[i], 0)) return inner(d->elem, l[i], at + "[" + std::to_string(i) + "]");
  }
  if (d->k == 'M' && is(v, K::Map))
    for (auto& [k, x] : as<Map>(v)->e) {
      std::string here = at + "[" + toStr(k, true) + "]";
      if (!accepts(d->key, k, 0)) return "the key " + toStr(k, true) + (at.empty() ? "" : " of " + at) + " must be " + typeText(d->key);
      if (!accepts(d->elem, x, 0)) return inner(d->elem, x, here);
    }
  if (d->k == 'u' && !accepts(t, v, 0)) {  // the alternative of the value's own kind, when only one fits that kind
    const TypeDesc* only = nullptr;
    uint64_t which = 0;
    int n = 0;
    for (uint64_t a : d->alts) {
      const TypeDesc* ad = typeDesc(a);
      bool sameKind = ad && ((ad->k == 's' && (is(v, K::Map) || is(v, K::Inst))) || (ad->k == 'L' && is(v, K::List)) || (ad->k == 'M' && is(v, K::Map)));
      if (sameKind) only = ad, which = a, n++;
    }
    if (n == 1 && only) return why(which, v, depth + 1, at);
  }
  return "";
}

void typeFail(int site, V v, void* ra, uint64_t t) {
  TypeSite& s = sites[site];
  std::string w = t ? why(t, v, 0, "") : "";
  if (!w.empty()) rtError(ra, "@type %s must be %s: %s", s.what.c_str(), s.type.c_str(), w.c_str());
  std::string want = s.type;  // a named union of literals: say what they are
  if (const TypeDesc* d = t && (t & 15) == TY_NAMED ? typeDesc(t) : nullptr; d && d->k == 'u') {
    bool lits = true;
    for (uint64_t a : d->alts) lits = lits && typeDesc(a) && typeDesc(a)->k == 'l';
    if (lits) want += " (" + typeText(typeResolve(t) & ~TY_OPT) + ")";
  }
  rtError(ra, "@type %s must be %s, got %s", s.what.c_str(), want.c_str(), describe(v).c_str());
}
// A store into a list (key nil) or map that has its element type. The value stored gets tagged as a check would.
void typeWrite(Obj* o, V key, V v, void* ra) {
  bool list = o->kind == K::List;
  uint64_t code = list ? ((List*)o)->etype : ((Map*)o)->etype;
  int site = list ? ((List*)o)->esite : ((Map*)o)->esite;
  const TypeDesc* d = typeDesc(code);
  if (!d) return;
  auto since = [&] { return "it has been " + typeText(code) + " since it passed as " + sites[site].what; };
  if (!list && !accepts(d->key, key, 0))
    rtError(ra, "@type cannot use %s as a key of this map: %s, so its keys must be %s", describe(key).c_str(), since().c_str(),
            typeText(d->key).c_str());
  if (typeAccepts(d->elem, v, site)) return;
  std::string w = why(d->elem, v, 0, "");
  rtError(ra, "@type cannot store %s in this %s: %s, so what it holds must be %s%s", describe(v).c_str(), list ? "list" : "map",
          since().c_str(), typeText(d->elem).c_str(), (w.empty() ? "" : " (" + w + ")").c_str());
}
static V rt_typechk(V v, uint64_t t, uint64_t site) {  // compiled code: checks v, which it then keeps
  if (!typeAccepts(t, v, (int)site)) typeFail((int)site, v, RA(), t);
  return v;
}
void typesInit() { vm->helpers[H_TYPECHK] = (void*)rt_typechk; }
