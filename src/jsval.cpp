// Values crossing between fut and JavaScript (extern fns of js modules, `any` and `fn` types). Both sides speak
// JSON text: numbers, strings, bools, nil (null), lists (arrays) and maps with string keys (objects) are copied.
// What is not data crosses by reference, as an object with the key "\u0000" (no JSON data has it):
//   {"\u0000":"fn","id":N}  a fut function JavaScript may call: pinned here (a GC root) until JavaScript drops it
//   {"\u0000":"js","id":N}  a JavaScript value (an object with methods, a DOM node, a JS function): a Ref here,
//                           the same Ref for the same value, released to JavaScript when the GC finds it dead
//   {"\u0000":"num","v":"NaN"}  NaN and the infinities, which JSON has no numbers for
#include "fut.h"
#include <cmath>
#include <unordered_map>

static std::vector<V> pins;          // by id; NIL: free
static std::vector<int> freePins;
static std::unordered_map<u32, Ref*> refs;  // id -> its Ref, while one is alive
static std::vector<V> building;      // what fromJs made so far: roots until it returns
void (*refRelease)(u32 id);

void jsMark(void (*mark)(V)) {
  for (V v : pins) mark(v);
  for (V v : building) mark(v);
}
V jsPinned(int id) { return id >= 0 && (size_t)id < pins.size() ? pins[id] : NIL; }
void jsUnpin(int id) {
  if (id >= 0 && (size_t)id < pins.size() && pins[id] != NIL) pins[id] = NIL, freePins.push_back(id);
}
static int pin(V f) {
  int id;
  if (!freePins.empty()) id = freePins.back(), freePins.pop_back(), pins[id] = f;
  else id = (int)pins.size(), pins.push_back(f);
  return id;
}
void refFree(Ref* r) {
  if (auto it = refs.find(r->id); it != refs.end() && it->second == r) refs.erase(it);
  if (refRelease) refRelease(r->id);
}

static void quote(std::string_view s, std::string& o) {
  o += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += (char)c;
    else if (c == '\n') o += "\\n";
    else if (c == '\t') o += "\\t";
    else if (c == '\r') o += "\\r";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  o += '"';
}
static void enc(V v, std::string& o, int depth) {
  if (depth > 256) ERR("a value nested too deeply (or containing itself) to pass to JavaScript");
  if (isNum(v)) {
    double d = num(v);
    if (std::isnan(d)) o += "{\"\\u0000\":\"num\",\"v\":\"NaN\"}";
    else if (std::isinf(d)) o += d > 0 ? "{\"\\u0000\":\"num\",\"v\":\"Infinity\"}" : "{\"\\u0000\":\"num\",\"v\":\"-Infinity\"}";
    else {
      char b[32];
      snprintf(b, sizeof b, d == (double)(long long)d && std::fabs(d) < 1e15 ? "%.0f" : "%.17g", d);
      o += b;
    }
    return;
  }
  if (!isObj(v)) { o += v == NIL ? "null" : v == TRUE_ ? "true" : "false"; return; }
  Obj* x = obj(v);
  switch (x->kind) {
    case K::Str: return quote(((Str*)x)->sv(), o);
    case K::List: {
      o += '[';
      bool first = true;
      for (V e : ((List*)x)->v) o += first ? "" : ",", first = false, enc(e, o, depth + 1);
      o += ']';
      return;
    }
    case K::Map: {
      o += '{';
      bool first = true;
      for (auto& [k, e] : ((Map*)x)->e) {
        if (!is(k, K::Str)) ERR("a map passed to JavaScript must have string keys (this one has a %s key)", typeName(k));
        o += first ? "" : ",", first = false, quote(as<Str>(k)->sv(), o), o += ':', enc(e, o, depth + 1);
      }
      o += '}';
      return;
    }
    case K::Fn: case K::Clo: o += "{\"\\u0000\":\"fn\",\"id\":" + std::to_string(pin(v)) + "}"; return;
    case K::Ref: o += "{\"\\u0000\":\"js\",\"id\":" + std::to_string(((Ref*)x)->id) + "}"; return;
    case K::Inst: ERR("an instance of %s can't pass to JavaScript (pass a map of what it needs)", symName(((Inst*)x)->cls->name).c_str());
    default: ERR("a %s can't pass to JavaScript", typeName(v));
  }
}
std::string toJs(V v) {
  std::string o;
  enc(v, o, 0);
  return o;
}

namespace {
struct Parser {
  const char *p, *e;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  [[noreturn]] void bad() { building.clear(), ERR("JavaScript gave fut a value it can't read"); }
  bool lit(const char* w) {
    size_t n = strlen(w);
    if ((size_t)(e - p) < n || memcmp(p, w, n)) return false;
    return p += n, true;
  }
  static void utf8(std::string& o, u32 c) {
    if (c < 0x80) o += (char)c;
    else if (c < 0x800) o += (char)(0xc0 | c >> 6), o += (char)(0x80 | (c & 63));
    else if (c < 0x10000) o += (char)(0xe0 | c >> 12), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
    else o += (char)(0xf0 | c >> 18), o += (char)(0x80 | (c >> 12 & 63)), o += (char)(0x80 | (c >> 6 & 63)), o += (char)(0x80 | (c & 63));
  }
  u32 hex4() {
    if (e - p < 4) bad();
    u32 c = 0;
    for (int i = 0; i < 4; i++, p++) c = c * 16 + (u32)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
    return c;
  }
  std::string str() {  // after the opening quote
    std::string o;
    while (p < e && *p != '"') {
      if (*p != '\\') { o += *p++; continue; }
      if (++p >= e) bad();
      char c = *p++;
      switch (c) {
        case 'n': o += '\n'; break;
        case 't': o += '\t'; break;
        case 'r': o += '\r'; break;
        case 'b': o += '\b'; break;
        case 'f': o += '\f'; break;
        case 'u': {
          u32 u = hex4();
          if (u >= 0xd800 && u < 0xdc00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
            p += 2;
            u32 lo = hex4();
            u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
          }
          utf8(o, u);
          break;
        }
        default: o += c;
      }
    }
    if (p >= e) bad();
    p++;
    return o;
  }
  V keep(V v) { return building.push_back(v), v; }
  V value() {
    ws();
    if (p >= e) bad();
    if (lit("null")) return NIL;
    if (lit("true")) return TRUE_;
    if (lit("false")) return FALSE_;
    if (*p == '"') return p++, keep(box(newStr(str())));
    if (*p == '[') {
      p++;
      List* l = make<List>();
      keep(box(l));
      ws();
      if (p < e && *p == ']') return p++, box(l);
      for (;;) {
        l->v.push_back(value());
        ws();
        if (p < e && *p == ',') { p++; continue; }
        if (p < e && *p == ']') return p++, box(l);
        bad();
      }
    }
    if (*p == '{') {
      p++, ws();
      if (e - p > 8 && !memcmp(p, "\"\\u0000\"", 8)) return marker();
      Map* m = make<Map>();
      keep(box(m));
      if (p < e && *p == '}') return p++, box(m);
      for (;;) {
        ws();
        if (p >= e || *p != '"') bad();
        p++;
        std::string k = str();
        ws();
        if (p >= e || *p++ != ':') bad();
        mapPut(m, k, value());
        ws();
        if (p < e && *p == ',') { p++; continue; }
        if (p < e && *p == '}') return p++, box(m);
        bad();
      }
    }
    char* end;
    double d = strtod(p, &end);
    if (end == p) bad();
    return p = end, box(d);
  }
  V marker() {  // {"\u0000":"fn"|"js"|"num", ...}: written by fut.js, so in its order
    p += 8, ws();
    if (p >= e || *p++ != ':') bad();
    ws();
    if (p >= e || *p++ != '"') bad();
    std::string kind = str();
    ws();
    if (p >= e || *p++ != ',') bad();
    ws();
    if (p >= e || *p++ != '"') bad();
    std::string key = str();
    ws();
    if (p >= e || *p++ != ':') bad();
    ws();
    V r = NIL;
    if (kind == "num") {
      if (p >= e || *p++ != '"') bad();
      std::string v = str();
      r = box(v == "NaN" ? NAN : v == "Infinity" ? INFINITY : -INFINITY);
    } else {
      char* end;
      long id = strtol(p, &end, 10);
      p = end;
      if (kind == "fn") r = jsPinned((int)id);
      else {
        Ref*& ref = refs[(u32)id];
        if (!ref) ref = make<Ref>(), ref->id = (u32)id;
        r = keep(box(ref));
      }
    }
    ws();
    if (p >= e || *p++ != '}') bad();
    return r;
  }
};
}  // namespace
V fromJs(const std::string& json) {
  Parser ps{json.data(), json.data() + json.size()};
  V v = ps.value();
  building.clear();
  return v;
}
