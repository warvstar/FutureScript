// Types: optional annotations on parameters, results and declared fields (`fn f(n: num): str?`, `var hp: num = 10`).
// Code without them is as dynamic as ever. Where one is written it is checked at run time, at that boundary: an
// argument as the function is entered, a result as it returns, a field on every store. So a caller compiled
// against an older version of a function, or one typed at the REPL, still gets a clean error naming the
// boundary, rather than a wrong value travelling on.
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

bool typeAccepts(uint64_t t, V v) {
  if (v == NIL && (t & TY_OPT)) return true;
  switch (t & 15) {
    case TY_ANY: return true;
    case TY_NUM: return isNum(v);
    case TY_STR: return is(v, K::Str);
    case TY_BOOL: return v == TRUE_ || v == FALSE_;
    case TY_LIST: return is(v, K::List);
    case TY_MAP: return is(v, K::Map);
    case TY_FN: return is(v, K::Fn) || is(v, K::Clo);
    case TY_CLS: {
      if (!is(v, K::Inst)) return false;
      Global* g = findGlobal(int(t >> 8));
      Obj* want = g ? g->o : nullptr;
      for (Class* c = as<Inst>(v)->cls; c; c = c->super) if (c == want) return true;
      return false;
    }
  }
  return false;
}

static std::string describe(V v) {  // the value a check rejected, for its message
  if (isNum(v)) return "the number " + toStr(v);
  if (v == NIL) return "nil";
  if (v == TRUE_ || v == FALSE_) return toStr(v);
  if (is(v, K::Str)) {
    std::string s = toStr(v, true);
    return "the string " + (s.size() > 40 ? s.substr(0, 37) + "...\"" : s);
  }
  if (is(v, K::Inst)) return "an instance of " + symName((int)as<Inst>(v)->cls->name);
  if (is(v, K::Class)) return "the class " + symName((int)as<Class>(v)->name);
  return std::string("a ") + typeName(v);
}

void typeFail(int site, V v, void* ra) {
  TypeSite& s = sites[site];
  rtError(ra, "@type %s must be %s, got %s", s.what.c_str(), s.type.c_str(), describe(v).c_str());
}
static V rt_typechk(V v, uint64_t t, uint64_t site) {  // compiled code: checks v, which it then keeps
  if (!typeAccepts(t, v)) typeFail((int)site, v, RA());
  return v;
}
void typesInit() { vm->helpers[H_TYPECHK] = (void*)rt_typechk; }
