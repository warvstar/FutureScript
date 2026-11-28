// Public interfaces. A file's interface is its `pub` declarations, with the comments right above them, sorted by
// name: `fut api` writes it next to the file (geom.fut -> geom.api) and `fut test` fails while one is out of date.
// So a change to what other files may use is an edit of its own, made on purpose and reviewed, and two agents who
// change the same interface collide in that small file instead of somewhere downstream at run time. Agents working
// elsewhere can read the .api files instead of the sources.
#include "compile.h"
#include <iterator>

static std::string squash(const char* a, const char* b) {  // source text on one line: whitespace runs become one space
  std::string o;
  for (; a < b; a++) {
    bool ws = *a == ' ' || *a == '\t' || *a == '\n' || *a == '\r';
    if (!ws) o += *a;
    else if (!o.empty() && o.back() != ' ') o += ' ';
  }
  while (!o.empty() && o.back() == ' ') o.pop_back();
  return o;
}
static std::string docs(const char* src, const char* at, const char* indent) {  // the // lines right above `at`
  const char* ls = at;
  while (ls > src && ls[-1] != '\n') ls--;
  std::vector<std::string> lines;
  while (ls > src) {
    const char *pe = ls - 1, *ps = pe;
    while (ps > src && ps[-1] != '\n') ps--;
    const char* q = ps;
    while (q < pe && (*q == ' ' || *q == '\t')) q++;
    const char* e = pe > q && pe[-1] == '\r' ? pe - 1 : pe;
    if (e - q < 2 || q[0] != '/' || q[1] != '/') break;
    lines.push_back(std::string(q, e)), ls = ps;
  }
  std::string o;
  for (size_t i = lines.size(); i-- > 0;) o += indent + lines[i] + "\n";
  return o;
}
static std::string base(const std::string& p) { return p.substr(p.rfind('/') + 1); }
static bool before(const std::string& a, const std::string& b) {  // by name, ignoring case (then with it)
  auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
  std::string x = lower(a), y = lower(b);
  return x != y ? x < y : a < b;
}
std::string apiPath(const std::string& file) {
  size_t slash = file.rfind('/'), dot = file.rfind('.');
  return (dot == std::string::npos || (slash != std::string::npos && dot < slash) ? file : file.substr(0, dot)) + ".api";
}

// Reading declarations from tokens, without compiling.
struct Scan {
  Lexer L;
  Tok next() {
    Tok t = L.next();
    if (t.t == T_ERR) t.t = T_EOF;
    return t;
  }
  static bool isKw(const Tok& t, int k) { return t.t == T_ID && t.sym == k; }
  const char* params(Tok& t) {  // t is '(': through the matching ')'; returns just past it
    for (int d = 0; t.t != T_EOF; t = next()) if ((d += (t.t == '(') - (t.t == ')')) == 0) return t.p + 1;
    return t.p;
  }
  const char* typed(const char* end) {  // `: type` next: through it
    Lexer s = L;
    if (s.next().t != ':') return end;
    return L = s, type();
  }
  const char* type() {
    skipType(L, [](Lexer& at) {
      Tok t = at.next();
      return t.t == T_ERR ? Tok{T_EOF} : t;
    });
    return L.p;
  }
  const char* effects(const char* end) {  // `! io, geom.Log` next (or a bare `!`): through it
    Lexer s = L;
    if (s.next().t != '!') return end;
    L = s, end = s.p;
    for (Lexer d = L;; d = L) {
      Tok k = d.next();
      int t = k.t;
      if ((t != T_ID && t != ',' && t != '.') || k.nl) return end;  // (a contract's clauses start on the next line)
      L = d, end = d.p;
    }
  }
  int bodyEnd() {
    Tok t = next();
    if (t.t == '{') for (int depth = 1; depth > 0;) {
      t = next();
      if (t.t == T_EOF) break;
      depth += (t.t == '{') - (t.t == '}');
    }
    return t.line;
  }
  const char* clauseEnd() {  // after a clause's keyword: through its condition (the rest of the line, up to a '{' or '}')
    const char* e = L.p;
    Tok prev;
    for (int d = 0;;) {
      Lexer r = L;
      Tok t = r.next();
      if (t.t == T_EOF || t.t == T_ERR || (d == 0 && t.nl) || clauseEnds(d, prev, t)) return e;
      d += nesting(t);
      prev = t, L = r, e = r.p;
    }
  }
  std::vector<std::string> clauses() {  // a function's requires and ensures clauses next, each on one line
    std::vector<std::string> o;
    for (;;) {
      Lexer s = L;
      Tok k = next();
      if (!isKw(k, S_requires) && !isKw(k, S_ensures)) return L = s, o;
      o.push_back(squash(k.p, clauseEnd()));
    }
  }
};
// Every declaration in a file, read from tokens without compiling: what interfaces and `fut map` both show, so the
// two can't disagree. A top-level declaration, then its members (owner: the class or effect they are in), each with
// the // comments right above it and, for a function or method, its contract (an invariant is a member of its own).
std::vector<Decl> declsOf(const std::string& text) {
  std::vector<Decl> out;
  const char* src = text.c_str();
  Scan S{Lexer{src}};
  auto& L = S.L;
  auto isKw = Scan::isKw;
  int depth = 0;
  static const int gpuSym = intern("gpu"), viewSym = intern("view"), stateSym = intern("state");
  auto isGpu = [](const Tok& t) { return t.t == T_ID && t.sym == gpuSym; };
  auto add = [&](Decl d, const char* start, const char* name) {
    d.start = start - src, d.stop = L.p - src, d.nameAt = name - src;
    out.push_back(std::move(d));
  };
  for (Tok t = S.next(); t.t != T_EOF; t = S.next()) {
    if (t.t == '{') depth++;
    if (t.t == '}') depth--;
    if (depth || t.t != T_ID) continue;
    bool pub = false;
    Tok k = t;
    if (isKw(t, S_pub)) {
      Lexer save = L;
      Tok n = S.next();
      bool gpu = [&] { Lexer s = L; return isGpu(n) && isKw(s.next(), K_fn); }();
      if (!pubDecl(n) && !gpu) { L = save; continue; }
      pub = true, k = n;
    }
    const char* from = pub ? t.p : k.p;
    if (isGpu(k)) {  // gpu fn: a fn, which says so
      Lexer save = L;
      if (Tok f = S.next(); isKw(f, K_fn) && !f.nl) k = f;
      else L = save;
    }
    std::string doc = docs(src, t.p, "");
    if (isKw(k, K_fn)) {
      Tok n = S.next(), p = S.next();
      if (n.t != T_ID || p.t != '(') continue;
      const char* e = S.effects(S.typed(S.params(p)));
      auto cl = S.clauses();
      int end = S.bodyEnd();
      add({"fn", symName(n.sym), squash(from, e), "", "", pub, k.line, end, doc, cl}, from, n.p);
    } else if (isKw(k, viewSym) && (k.nl || out.empty())) {
      Lexer save = L;
      Tok n = S.next();
      const char* end = L.p;
      if (n.t != T_ID || n.nl) { L = save; continue; }
      if (Lexer l = L; l.next().t == '(') {
        Tok p = S.next();
        end = S.params(p);
      }
      Lexer body = L;
      int stopLine = body.next().t == '{' ? S.bodyEnd() : k.line;
      add({"view", symName(n.sym), squash(from, end), "", "", false, k.line, stopLine, doc, {}}, from, n.p);
    } else if (isKw(k, stateSym)) {
      Lexer save = L;
      Tok n = S.next();
      if (n.t != T_ID || n.nl) { L = save; continue; }
      add({"state", symName(n.sym), squash(from, L.p), "", "", false, k.line, k.line, doc, {}}, from, n.p);
    } else if (isKw(k, S_type)) {  // type Name = <type>
      Lexer save = L;
      Tok n = S.next(), eq = S.next();
      if (n.t != T_ID || n.nl || eq.t != '=') { L = save; continue; }
      add({"type", symName(n.sym), squash(from, S.type()), "", "", pub, k.line, L.line, doc, {}}, from, n.p);
    } else if (isKw(k, K_var)) {
      Tok n = S.next();
      if (n.t != T_ID) continue;
      add({"var", symName(n.sym), squash(from, S.typed(L.p)), "", "", pub, k.line, k.line, doc, {}}, from, n.p);
    } else if (isKw(k, K_import)) {
      Tok f = S.next();
      if (f.t != T_STR) continue;
      const char* e = f.p + f.len + 1;
      Lexer s = L;
      std::string alias;
      if (Tok a = s.next(); isKw(a, S_as)) if (Tok n = s.next(); n.t == T_ID) L = s, e = s.p, alias = symName(n.sym);
      add({"import", std::string(f.p, f.len), squash(from, e), "", "", pub, k.line, k.line, doc, {}}, from, f.p);
      out.back().alias = std::move(alias);
    } else if (isKw(k, K_extern)) {
      Tok n = S.next();
      std::string mod = n.t == T_ID && !isKw(n, K_fn) ? symName(n.sym) : n.t == T_STR ? std::string(n.p, n.len) : "";
      while (n.t != T_EOF && !isKw(n, K_fn)) n = S.next();
      Tok nm = S.next(), p = S.next();
      if (nm.t != T_ID || p.t != '(') continue;
      const char* e = S.params(p);
      if (Lexer s = L; s.next().t == ':') s.next(), e = s.p, L = s;  // its result type
      Lexer s = L;
      if (Tok b = s.next(); b.t == '!' && !b.nl) e = s.p, L = s;  // pure: a bare ! on its line
      std::string sig = squash(from, e);
      if (mod == "fut") if (size_t at = sig.find(" \"fut\""); at != std::string::npos) sig.erase(at, 6);
      add({"extern", symName(nm.sym), sig, "", mod, pub, k.line, k.line, doc, {}}, from, nm.p);
    } else if (isKw(k, S_effect) || isKw(k, K_class)) {
      bool eff = isKw(k, S_effect);
      Lexer save = L;
      Tok n = S.next(), b = S.next();
      if (n.t != T_ID || (eff && b.t != '{')) { L = save; continue; }
      while (b.t != '{' && b.t != T_EOF) b = S.next();
      size_t at = out.size();
      std::string owner = symName(n.sym);
      add({eff ? "effect" : "class", owner, squash(from, b.p), "", "", pub, k.line, k.line, doc, {}}, from, n.p);
      for (Tok m = S.next(); m.t != '}' && m.t != T_EOF; m = S.next()) {
        std::string md = docs(src, m.p, "");
        if (isKw(m, K_var) && !eff) {
          Tok f = S.next();
          if (f.t != T_ID) continue;
          S.typed(f.p);
          if (Lexer s = L; s.next().t == '=') {  // a scalar literal, [], or {}
            L = s;
            int t = S.next().t;
            if (t == '-' || t == '[' || t == '{') S.next();
          }
          add({"field", symName(f.sym), squash(m.p, L.p), owner, "", pub, m.line, m.line, md, {}}, m.p, f.p);
          continue;
        }
        if (isKw(m, S_invariant) && !eff) {
          add({"invariant", "", squash(m.p, S.clauseEnd()), owner, "", pub, m.line, m.line, md, {}}, m.p, m.p);
          continue;
        }
        Tok first = m;
        if (isKw(m, S_override)) m = S.next();
        if (!isKw(m, K_fn)) continue;
        Tok nm = S.next(), p = S.next();
        if (nm.t != T_ID || p.t != '(') continue;
        const char* e = S.effects(S.typed(S.params(p)));
        std::vector<std::string> cl;
        int end = m.line;
        if (!eff) {
          cl = S.clauses();
          end = S.bodyEnd();
        }
        add({eff ? "op" : "method", symName(nm.sym), squash(first.p, e), owner, "", pub, first.line, end, md, cl}, first.p, nm.p);
      }
      out[at].end = L.line, out[at].stop = L.p - src;
    } else if (isKw(k, S_enum)) {  // its variants, in order, each with its fields
      Lexer save = L;
      Tok n = S.next(), b = S.next();
      if (n.t != T_ID || b.t != '{') { L = save; continue; }
      size_t at = out.size();
      std::string owner = symName(n.sym);
      add({"enum", owner, squash(from, b.p), "", "", pub, k.line, k.line, doc, {}}, from, n.p);
      for (Tok v = S.next(); v.t != '}' && v.t != T_EOF; v = S.next()) {
        if (v.t != T_ID) continue;
        const char* e = v.p + symName(v.sym).size();
        if (Lexer r = L; r.next().t == '(') {
          for (int d = 0;;) {
            Tok t = S.next();
            if (t.t == T_EOF || (d += (t.t == '(') - (t.t == ')')) == 0) { e = L.p; break; }
          }
        }
        add({"variant", symName(v.sym), squash(v.p, e), owner, "", pub, v.line, v.line, docs(src, v.p, ""), {}}, v.p, v.p);
      }
      out[at].end = L.line, out[at].stop = L.p - src;
    } else if (isKw(k, S_test) && !pub) {
      Lexer save = L;
      Tok n = S.next();
      if (n.t != T_STR) { L = save; continue; }
      int end = S.bodyEnd();
      add({"test", std::string(n.p, n.len), squash(k.p, n.p + n.len + 1), "", "", false, k.line, end, doc, {}}, k.p, n.p);
    }
  }
  return out;
}

// ---- interfaces: a file's (or package's) pub declarations, rendered from declsOf ----
struct Item { std::string name, text; };
static std::string indented(const std::string& doc, const char* in) {  // each line of doc, indented
  std::string o;
  for (size_t i = 0; i < doc.size();) {
    size_t j = doc.find('\n', i);
    o += in + doc.substr(i, j - i + 1), i = j + 1;
  }
  return o;
}
static std::string contract(const std::vector<std::string>& clauses, const char* in) {
  std::string o;
  for (auto& c : clauses) o += in + c + "\n";
  return o;
}
static void itemsOf(const std::string& text, std::vector<Item>& items) {
  auto ds = declsOf(text);
  auto byName = [](auto& a, auto& b) { return before(a.first, b.first); };
  for (size_t i = 0; i < ds.size(); i++) {
    const Decl& d = ds[i];
    if (!d.owner.empty() || !d.pub || d.kind == "import" || d.kind == "test") continue;  // (members: with their owner)
    Item it{d.name, d.doc + d.sig};
    if (d.kind == "fn" && !d.clauses.empty()) {
      std::string c = contract(d.clauses, "  ");
      it.text += "\n" + c.substr(0, c.size() - 1);
    } else if (d.kind == "enum") {  // its variants, as declared (their order is the enum's)
      it.text += " {\n";
      for (size_t j = i + 1; j < ds.size() && ds[j].owner == d.name; j++) it.text += indented(ds[j].doc, "  ") + "  " + ds[j].sig + "\n";
      it.text += "}";
    } else if (d.kind == "class" || d.kind == "effect") {  // its invariants as written, then fields and methods, each sorted
      std::string invariants;
      std::vector<std::pair<std::string, std::string>> fields, methods;
      for (size_t j = i + 1; j < ds.size() && ds[j].owner == d.name; j++) {
        const Decl& m = ds[j];
        std::string line = indented(m.doc, "  ") + "  " + m.sig + "\n";
        if (m.kind == "invariant") invariants += line;
        else if (m.kind == "field") fields.push_back({m.name, line});
        else methods.push_back({m.name, line + contract(m.clauses, "    ")});
      }
      std::stable_sort(fields.begin(), fields.end(), byName), std::stable_sort(methods.begin(), methods.end(), byName);
      it.text += " {\n" + invariants;
      for (auto& [_, l] : fields) it.text += l;
      for (auto& [_, l] : methods) it.text += l;
      it.text += "}";
    }
    items.push_back(std::move(it));
  }
}
static std::string render(const std::string& what, std::vector<Item>& items) {
  if (items.empty()) return "";
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return before(a.name, b.name); });
  std::string o = "// The public interface of " + what + ", written by `fut api`. `fut test` fails while it is out of date.\n";
  for (auto& it : items) o += "\n" + it.text + "\n";
  return o;
}
std::string interfaceOf(const std::string& path, const std::string& text) {
  std::vector<Item> items;
  itemsOf(text, items);
  return render(base(path), items);
}
// A package's: its root file's pub declarations and those of the files it re-exports, under its name and version.
static std::string packageInterface(const ApiUnit& u, const Compiled& c) {
  std::vector<Item> items;
  for (auto& f : u.parts)
    for (auto& [file, text] : c.sources) if (file == f) itemsOf(text, items);
  return render("package " + u.pkg + " " + u.version + " (" + base(u.file) + ")", items);
}
static std::string versionIn(const std::string& api) {  // what an interface file says its package's version is
  size_t at = api.find("of package ");
  if (at == std::string::npos) return "";
  at = api.find(' ', at + 11);
  size_t end = api.find(' ', at + 1);
  return at == std::string::npos || end == std::string::npos ? "" : api.substr(at + 1, end - at - 1);
}

// The declarations an interface text lists, one per line ("fn area(r)", "Circle: var r = 1"), and its contracts, one
// clause per line ("Circle.grow() requires k > 0", "Circle invariant self.r >= 0"), sorted: what a change report
// compares. Comments are left out (a change to them alone is reported as such).
static std::vector<std::string> entries(const std::string& text) {
  std::vector<std::string> e;
  std::string cls, fn;  // the class (or effect, or enum) and the function the lines are in
  bool variants = false;  // cls is an enum: its lines are variants
  for (size_t i = 0; i < text.size();) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) j = text.size();
    std::string l = squash(text.data() + i, text.data() + j);
    i = j + 1;
    if (l.empty() || l.starts_with("//")) continue;
    if (l.starts_with("pub ")) l = l.substr(4);
    if (l == "}") cls.clear(), fn.clear(), variants = false;
    else if (variants) e.push_back(cls + " variant " + l);
    else if ((l.starts_with("class ") || l.starts_with("effect ") || l.starts_with("enum ")) && l.ends_with("{")) {
      variants = l.starts_with("enum ");
      l = squash(l.data(), l.data() + l.size() - 1), e.push_back(l);
      size_t at = l.find(' ') + 1;
      cls = l.substr(at, l.find(' ', at) - at), fn.clear();
    } else if (l.starts_with("requires ") || l.starts_with("ensures ")) e.push_back((cls.empty() ? "" : cls + ".") + fn + "() " + l);
    else if (l.starts_with("invariant ")) e.push_back(cls + " " + l);
    else {
      std::string d = l.starts_with("override ") ? l.substr(9) : l;
      if (d.starts_with("fn ")) fn = d.substr(3, d.find('(') - 3);
      e.push_back(cls.empty() ? l : cls + ": " + l);
    }
  }
  return std::sort(e.begin(), e.end()), e;
}
// A change callers may notice: a declaration gone (or changed), a requires added (callers must now meet it), an
// ensures or invariant gone (they may have relied on it), or a variant added (a match without else no longer covers
// the enum). An edited clause is one gone and one added. Adding an ensures or invariant, or dropping a requires, is
// compatible.
static bool breaks(const std::vector<std::string>& removed, const std::vector<std::string>& added) {
  auto has = [](const std::string& e, const char* w) { return e.find(w) != std::string::npos; };
  for (auto& r : removed) if (!has(r, "() requires ")) return true;  // a declaration, an ensures or an invariant
  for (auto& a : added) if (has(a, "() requires ") || has(a, " variant ")) return true;
  return false;
}

// A package whose interface lost (or changed) declarations must say so in its version: 0.x.y -> 0.(x+1).0, and
// from 1.0.0 on, x.y.z -> (x+1).0.0. "" if v is enough after `was`, else the least version that is.
static std::string bumpNeeded(const std::string& was, const std::string& v) {
  int a[3], b[3];
  if (!parseVersion(was, a) || !parseVersion(v, b)) return "";
  bool ok = a[0] == 0 ? b[0] > 0 || b[1] > a[1] : b[0] > a[0];
  return ok ? "" : a[0] == 0 ? "0." + std::to_string(a[1] + 1) + ".0" : std::to_string(a[0] + 1) + ".0.0";
}
std::vector<ApiIssue> checkInterfaces(const Compiled& c) {
  std::vector<ApiIssue> out;
  for (auto& u : c.apis) {
    std::string want, have;
    if (u.pkg.empty()) {
      for (auto& [file, text] : c.sources) if (file == u.file) want = interfaceOf(file, text);
    } else want = packageInterface(u, c);
    bool exists = readFile(u.api, have);
    if (exists ? have == want : want.empty()) continue;
    ApiIssue is{u.file, u.api, !exists ? "missing" : want.empty() ? "stale" : "changed", {}, {}, want};
    auto a = entries(have), b = entries(want);
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(is.removed));
    std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(is.added));
    if (std::string was = versionIn(have); exists && !u.pkg.empty() && breaks(is.removed, is.added) && !was.empty())
      if (std::string need = bumpNeeded(was, u.version); !need.empty()) is.status = "breaking", is.version = u.version, is.needs = need;
    out.push_back(std::move(is));
  }
  return out;
}

// A path as the reader knows it: relative to the program's directory
// (with ../ when it shares a directory with the program's, other than / itself), else as given.
static std::string shown(const std::string& file, const std::string& program) {
  std::string dir = canonicalPath(program), f = file[0] == '/' ? file : canonicalPath(file);
  dir = dir.substr(0, dir.rfind('/') + 1);
  if (f.starts_with(dir)) return f.substr(dir.size());
  size_t common = 0;  // the length of the shared directory, through its '/'
  for (size_t i = 0; i < dir.size() && i < f.size() && dir[i] == f[i]; i++) if (dir[i] == '/') common = i + 1;
  if (common <= 1) return file;
  std::string up;
  for (size_t i = common; i < dir.size(); i++) if (dir[i] == '/') up += "../";
  return up + f.substr(common);
}
// A declaration's version: a hash of its lines (line..end) as written. An agent that read it can check, before
// writing, that nobody changed it since (fut map --json: "hash").
static std::string spanHash(const std::string& src, int line, int end) {
  size_t a = 0;
  for (int l = 1; l < line && a != std::string::npos; l++) a = src.find('\n', a), a = a == std::string::npos ? a : a + 1;
  if (a == std::string::npos) a = src.size();
  size_t b = a;
  for (int l = line; l <= end && b < src.size(); l++) b = src.find('\n', b), b = b == std::string::npos ? src.size() : b + 1;
  uint64_t h = 1469598103934665603ull;
  for (size_t i = a; i < b; i++) h = (h ^ (unsigned char)src[i]) * 1099511628211ull;
  char o[17];
  snprintf(o, sizeof o, "%016llx", (unsigned long long)h);
  return std::string(o, 12);
}
int reportInterfaces(const std::vector<ApiIssue>& issues, bool asJson, const std::string& program) {
  for (auto& is : issues) {
    std::string s;
    if (asJson) {
      s = "{\"type\":\"api\",\"file\":" + quoteJson(is.file) + ",\"interface\":" + quoteJson(is.api) + ",\"status\":\"" + is.status + "\"";
      if (is.status == "breaking") s += ",\"version\":" + quoteJson(is.version) + ",\"needs\":" + quoteJson(is.needs);
      for (auto [key, list] : {std::pair{"removed", &is.removed}, {"added", &is.added}}) {
        s += std::string(",\"") + key + "\":[";
        for (size_t i = 0; i < list->size(); i++) s += (i ? "," : "") + quoteJson((*list)[i]);
        s += "]";
      }
      s += "}\n";
    } else {
      std::string fix = "`fut api " + program + "` ", api = base(is.api);
      s = "API     " + shown(is.file, program) + ": " +
          (is.status == "missing" ? "has pub declarations but no " + api + " yet; " + fix + "writes it\n"
           : is.status == "stale" ? "has no pub declarations, but " + api + " exists; " + fix + "removes it\n"
           : is.status == "breaking" ? "its package's interface lost declarations, strengthened a contract or added a variant, a breaking change, but its version is still " + is.version +
                                           ": set 'version " + is.needs + "' (or later) in its fut.pkg, then " + fix + "updates " + api + "\n"
                                  : "its public interface changed; " + fix + "updates " + api + "\n");
      if (is.status != "missing") {
        for (auto& r : is.removed) s += "  - " + r + "\n";
        for (auto& a : is.added) s += "  + " + a + "\n";
        if (is.removed.empty() && is.added.empty()) s += "  (only its comments or layout changed)\n";
      }
    }
    fflush(stdout), fwrite(s.data(), 1, s.size(), stdout), fflush(stdout);
  }
  return (int)issues.size();
}
int writeInterfaces(const Compiled& c, const std::string& program) {  // fut api
  int n = 0, held = 0;
  for (auto& is : checkInterfaces(c)) {
    if (is.status == "breaking") {
      printf("not writing %s: its package's interface lost declarations, strengthened a contract or added a variant, so its version must become at least %s (it is %s)\n", shown(is.api, program).c_str(), is.needs.c_str(), is.version.c_str());
      for (auto& r : is.removed) printf("  - %s\n", r.c_str());
      for (auto& a : is.added) printf("  + %s\n", a.c_str());
      held++;
      continue;
    }
    bool ok = saveFile(is.api, is.status == "stale" ? nullptr : &is.want);
    printf("%s %s\n", !ok ? "could not write" : is.status == "stale" ? "removed" : "wrote", shown(is.api, program).c_str());
    for (auto& r : is.removed) printf("  - %s\n", r.c_str());
    for (auto& a : is.added) printf("  + %s\n", a.c_str());
    n += ok ? 1 : 0;
  }
  if (!n && !held) printf("every interface is up to date\n");
  return held ? -held : n;
}

// ---- fut map: every package, file and top-level declaration the program uses, with line ranges ----
// Generated from the sources each time (never committed, so never stale), in fut's own syntax, for readers who
// want the layout of a program without reading it: agents deciding where to work, people new to it.
// What a function that declares no effects does, as fut map shows it: "does io + what f does", "does nothing",
// "does nothing; .get() may do clock".
static std::string doesText(const FnEffects& e) {
  std::string s, names;
  for (auto& d : e.does) s += (s.empty() ? "" : ", ") + d;
  for (size_t i = 0; i < e.params.size(); i++) names += (i ? i + 1 == e.params.size() ? " and " : ", " : "") + e.params[i];
  if (!names.empty()) s += (s.empty() ? "" : " + ") + ("what " + names + (e.params.size() > 1 ? " do" : " does"));
  if (e.unknown) s += (s.empty() ? "" : " + ") + std::string("what it calls through values");
  s = "does " + (s.empty() ? "nothing" : s);
  if (e.may.empty()) return s;
  std::string may, via;
  for (auto& m : e.may) may += (may.empty() ? "" : ", ") + m;
  for (size_t i = 0; i < e.via.size() && i < 3; i++) via += (i ? ", ." : ".") + e.via[i] + "()";
  if (e.via.size() > 3) via += ", ...";
  return s + "; " + via + " may do " + may;
}
// What fut map can match a name against, and how well: 3 the whole name (any case), 2 a part of it, 1 its letters in
// order from the start of a word, close together (rndr: render, hhero: hitHero), 0 not at all. A term with a * is a
// glob, and matches whole names only. A sentence (a test's name) has letters for any term: it only matches the first two ways.
static std::string lowered(std::string s) {
  for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
  return s;
}
static bool globMatch(const char* p, const char* s) {
  if (!*p) return !*s;
  if (*p == '*') return globMatch(p + 1, s) || (*s && globMatch(p, s + 1));
  return *s && *p == *s && globMatch(p + 1, s + 1);
}
static bool letters(const std::string& term, const std::string& name) {
  for (size_t s = 0; s < name.size(); s++) {
    bool word = s == 0 || strchr("_./- ", name[s - 1]) || (isupper((unsigned char)name[s]) && islower((unsigned char)name[s - 1]));
    if (!word || tolower((unsigned char)name[s]) != term[0]) continue;
    size_t i = 1, e = s + 1;
    for (; e < name.size() && i < term.size() && e - s < 3 * term.size(); e++) i += tolower((unsigned char)name[e]) == term[i];
    if (i == term.size()) return true;
  }
  return false;
}
static int matchTier(const std::string& term, const std::vector<std::string>& whole, const std::vector<std::string>& parts, bool sentence = false) {
  if (term.find('*') != std::string::npos) {
    for (auto* l : {&whole, &parts}) for (auto& n : *l) if (globMatch(term.c_str(), lowered(n).c_str())) return 3;
    return 0;
  }
  for (auto& n : whole) if (lowered(n) == term) return 3;
  for (auto& n : parts) if (lowered(n).find(term) != std::string::npos) return 2;
  if (!sentence) for (auto& n : parts) if (letters(term, n)) return 1;
  return 0;
}
// About how many tokens a model reads the map as: 4 bytes each (tokenizers make 4.6 of it, the alignment included).
static size_t tokensAbout(const std::string& s) { return (s.size() + 3) / 4; }
static std::string thousands(size_t n) { return n >= 1000 ? std::to_string((n + 500) / 1000) + "k" : std::to_string(n); }
static std::string counted(size_t n, const std::string& what) { return std::to_string(n) + " " + what + (n == 1 ? "" : "s"); }

int printMap(const Compiled& c, bool pubOnly, bool asJson, const std::string& query, const std::string& program, long budget) {
  std::map<std::pair<std::string, int>, const FnEffects*> does;  // by file and line
  for (auto& e : c.effects) does[{e.file, e.line}] = &e;
  auto doesOf = [&](const std::string& f, const Decl& d) -> const FnEffects* {
    if (d.kind != "fn" && d.kind != "method") return nullptr;
    auto it = does.find({f, d.line});
    return it != does.end() && it->second->name == d.name ? it->second : nullptr;
  };
  auto inInterface = [&](const Package* p, const std::string& f) {  // --pub: a package shows only its interface
    if (!p) return true;
    for (auto& u : c.apis) if (u.pkg == p->name) return std::find(u.parts.begin(), u.parts.end(), f) != u.parts.end();
    return false;
  };
  struct File {
    const std::string *path, *src;
    std::string rel;
    bool root = false, whole = false, show = false;
    std::vector<Decl> decls;
    std::vector<int> owner;           // a member's class, effect or enum (its index); -1 at the top level
    std::vector<char> visible, keep;  // shown at all (--pub, no contracts); matched
    size_t lines = 0, kept = 0;
  };
  struct Group { const Package* pkg; std::vector<File> files; bool whole = false; };
  std::vector<Group> groups{{nullptr, {}}};  // first: the files outside any package
  for (auto& d : c.packages) if (const Package* p = packageAt(d)) groups.push_back({p, {}});
  for (auto& src : c.sources) {
    if (src.first.empty() || src.first[0] == '<') continue;
    const Package* p = packageOf(src.first);
    if (pubOnly && !inInterface(p, src.first)) continue;
    for (auto& g : groups) if (g.pkg == p) { g.files.push_back({&src.first, &src.second}); break; }
  }
  auto relIn = [&](const Package* p, const std::string& f) {
    std::string dir = p ? p->dir + "/" : "";
    return p && f.starts_with(dir) ? f.substr(dir.size()) : shown(f, program);
  };
  for (auto& g : groups) {
    for (auto& f : g.files) {
      f.rel = relIn(g.pkg, *f.path), f.root = g.pkg && canonicalPath(*f.path) == g.pkg->root;
      f.decls = declsOf(*f.src);
      f.lines = std::count(f.src->begin(), f.src->end(), '\n') + (!f.src->empty() && f.src->back() != '\n');
      for (int i = 0, top = -1; i < (int)f.decls.size(); i++) {  // members come right after what they are in
        auto& d = f.decls[i];
        f.owner.push_back(d.owner.empty() ? (top = i, -1) : top);
        f.visible.push_back((!pubOnly || d.pub) && d.kind != "invariant");  // (contracts are in the source and the .api)
      }
      f.keep.assign(f.decls.size(), 0);
    }
    if (g.pkg) std::stable_partition(g.files.begin(), g.files.end(), [](const File& f) { return f.root; });
  }

  // What to show: everything, or what the terms name. A term's best matches win: a package, file or declaration
  // named exactly that, else every name it is part of, else every name with its letters in order.
  std::vector<std::string> terms, notes;
  for (size_t i = 0, j; i < query.size(); i = j) {
    i = query.find_first_not_of(" \t\n", i);
    if (i == std::string::npos) break;
    j = std::min(query.find_first_of(" \t\n", i), query.size());
    terms.push_back(query.substr(i, j - i));
  }
  auto keepDecl = [](File& f, int i) {
    f.keep[i] = 1;
    if (f.owner[i] >= 0) f.keep[f.owner[i]] = 1;  // a member, in its class
    else for (size_t j = i + 1; j < f.decls.size() && f.owner[j] == i; j++) f.keep[j] = 1;  // a class, with its members
  };
  if (terms.empty()) for (auto& g : groups) g.whole = true;
  struct Hit { int tier; Group* g; File* f; int decl; std::string name; };
  std::vector<std::pair<std::string, std::vector<Hit>>> partly;  // a term's matches that its exact ones hid
  for (auto& term : terms) {
    std::string t = lowered(term);
    std::vector<Hit> hits;
    for (auto& g : groups) {
      if (g.pkg) if (int k = matchTier(t, {g.pkg->name}, {g.pkg->name})) hits.push_back({k, &g, nullptr, -1, "pkg " + g.pkg->name});
      for (auto& f : g.files) {
        std::string base = f.rel.substr(f.rel.rfind('/') + 1), stem = base.ends_with(".fut") ? base.substr(0, base.size() - 4) : base;
        if (int k = matchTier(t, {f.rel, shown(*f.path, program), *f.path, base, stem}, {f.rel})) hits.push_back({k, &g, &f, -1, f.rel});
        for (int i = 0; i < (int)f.decls.size(); i++) {
          auto& d = f.decls[i];
          if (!f.visible[i] || d.kind == "import") continue;  // an import's name is the file it imports
          std::string qualified = d.owner.empty() ? d.name : d.owner + "." + d.name;
          if (int k = matchTier(t, {d.name, qualified}, {qualified}, d.kind == "test")) hits.push_back({k, &g, &f, i, d.kind == "test" ? "test \"" + d.name + "\"" : qualified});
        }
      }
    }
    int best = 0;
    for (auto& h : hits) best = std::max(best, h.tier);
    if (!best) { notes.push_back("nothing matches '" + term + "'"); continue; }
    if (best == 1) notes.push_back("no name contains '" + term + "': these have its letters in order");
    partly.push_back({term, {}});
    for (auto& h : hits) {
      if (h.tier != best) { if (h.tier == 2) partly.back().second.push_back(h); continue; }
      if (!h.f) h.g->whole = true;
      else if (h.decl < 0) h.f->whole = true;
      else keepDecl(*h.f, h.decl);
    }
  }
  for (auto& [term, hid] : partly) {  // exact matches hide the names a term is only part of: say which aren't shown anyway
    std::vector<std::string> also;
    for (auto& h : hid)
      if (!(h.g->whole || (h.f && (h.f->whole || (h.decl >= 0 && h.f->keep[h.decl])))) && std::find(also.begin(), also.end(), h.name) == also.end()) also.push_back(h.name);
    if (also.empty()) continue;
    std::string l;
    for (size_t i = 0; i < also.size() && i < 4; i++) l += (i ? ", " : "") + also[i];
    notes.push_back(counted(also.size(), "more name") + (also.size() == 1 ? " contains '" : " contain '") + term + "' (*" + term + "* shows them): " + l + (also.size() > 4 ? ", ..." : ""));
  }
  int n = 0;  // files and declarations shown
  for (auto& g : groups)
    for (auto& f : g.files) {
      if (g.whole) f.whole = true;
      if (f.whole) std::fill(f.keep.begin(), f.keep.end(), 1);
      for (size_t i = 0; i < f.decls.size(); i++) f.kept += f.visible[i] && f.keep[i];
      f.show = f.whole || std::count(f.keep.begin(), f.keep.end(), 1);
      if (f.show) n += 1 + (int)f.kept;
    }
  auto shownFiles = [](const Group& g) {
    std::vector<const File*> fs;
    for (auto& f : g.files) if (f.show) fs.push_back(&f);
    return fs;
  };

  // The map at a level of detail: 3 everything, 2 top-level declarations (members counted, no private imports), 1
// files, 0 packages.
  auto render = [&](int level) {
    std::string o;
    for (auto& g : groups) {
      const Package* p = g.pkg;
      auto fs = shownFiles(g);
      if (fs.empty() && !g.whole) continue;
      if (fs.empty() && !p) continue;
      size_t lines = 0, decls = 0;
      for (auto* f : fs) lines += f->lines, decls += f->kept;
      std::string sizes = counted(fs.size(), "file") + ", " + counted(lines, "line") + ", " + counted(decls, "declaration");
      if (asJson && p)
        o += "{\"type\":\"package\",\"name\":" + quoteJson(p->name) + ",\"version\":" + quoteJson(p->version) + ",\"dir\":" + quoteJson(shown(p->dir, program)) +
             ",\"root\":" + quoteJson(shown(p->root, program)) + "}\n";
      else if (p) o += "pkg " + p->name + " " + p->version + "  // " + shown(p->manifest, program) + (level == 0 ? ", " + sizes : "") + "\n";
      else if (level == 0) o += "// outside any package: " + sizes + "\n";
      if (level == 0) continue;
      if (p) {
        std::unordered_map<std::string, std::vector<std::string>> binds;  // module -> the extern fns bound to it
        for (auto* f : fs) for (size_t i = 0; i < f->decls.size(); i++) if (f->keep[i] && f->decls[i].kind == "extern" && !f->decls[i].mod.empty()) binds[f->decls[i].mod].push_back(f->decls[i].name);
        for (auto& m : p->mods) {
          std::string t = m.kind == "c" && m.target.find('/') == std::string::npos ? m.target : relIn(p, m.target), b;
          if (asJson) {
            for (auto& x : binds[m.name]) b += (b.empty() ? "" : ",") + quoteJson(x);
            o += "{\"type\":\"module\",\"package\":" + quoteJson(p->name) + ",\"kind\":\"" + m.kind + "\",\"name\":" + quoteJson(m.name) + ",\"target\":" + quoteJson(t) + ",\"binds\":[" + b + "]}\n";
          } else {
            for (auto& x : binds[m.name]) b += (b.empty() ? "" : ", ") + x;
            o += "  " + m.kind + " " + m.name + " " + t + (b.empty() ? "" : "  // " + b) + "\n";
          }
        }
      }
      std::string ind = p ? "  " : "", pkgField = p ? ",\"package\":" + quoteJson(p->name) : "";
      size_t fw = 0;  // files only: their notes in a column
      for (auto* f : fs) fw = std::max(fw, f->rel.size());
      for (auto* f : fs) {
        std::vector<int> ds;
        for (int i = 0; i < (int)f->decls.size(); i++)
          if (f->visible[i] && f->keep[i] && (level == 3 || (f->owner[i] < 0 && (f->decls[i].pub || f->decls[i].kind != "import")))) ds.push_back(i);
        if (asJson) {
          o += "{\"type\":\"file\"" + pkgField + ",\"file\":" + quoteJson(shown(*f->path, program)) + "}\n";
          for (int i : ds) {
            auto* d = &f->decls[i];
            o += "{\"type\":\"" + d->kind + "\"" + pkgField + ",\"file\":" + quoteJson(shown(*f->path, program)) + ",\"name\":" + quoteJson(d->name);
            if (!d->owner.empty()) o += ",\"in\":" + quoteJson(d->owner);
            if (!d->mod.empty()) o += ",\"module\":" + quoteJson(d->mod);
            o += std::string(",\"pub\":") + (d->pub ? "true" : "false") + ",\"sig\":" + quoteJson(d->sig) + ",\"line\":" + std::to_string(d->line) + ",\"end\":" + std::to_string(d->end) +
                 ",\"hash\":\"" + spanHash(*f->src, d->line, d->end) + "\"";
            if (const FnEffects* e = doesOf(*f->path, *d)) {  // what it does (it declares nothing)
              auto list = [&](const std::vector<std::string>& l) { std::string a; for (auto& x : l) a += (a.empty() ? "" : ",") + quoteJson(x); return "[" + a + "]"; };
              o += ",\"does\":" + list(e->does) + ",\"doesWhatParams\":" + list(e->params) + ",\"callsValues\":" + (e->unknown ? "true" : "false") +
                   ",\"mayDo\":" + list(e->may) + ",\"mayVia\":" + list(e->via);
            }
            o += "}\n";
          }
          continue;
        }
        std::string note = f->root ? "root" : "";
        if (level == 1) note += (note.empty() ? "" : ", ") + counted(f->lines, "line") + ", " + counted(f->kept, "declaration");
        o += ind + f->rel + (note.empty() ? "" : std::string(level == 1 ? fw - f->rel.size() + 2 : 2, ' ') + "// " + note) + "\n";
        if (level == 1) continue;
        size_t w = 0;
        for (int i : ds) w = std::max(w, f->decls[i].sig.size() + (f->decls[i].owner.empty() ? 0 : 2));
        w = std::min<size_t>(w, 56);
        for (int i : ds) {
          auto* d = &f->decls[i];
          std::string l = ind + "  " + (d->owner.empty() ? "" : "  ") + d->sig;
          size_t col = ind.size() + 2 + w + 2;
          l += std::string(l.size() < col ? col - l.size() : 2, ' ');
          l += "// " + std::to_string(d->line) + (d->end != d->line ? "-" + std::to_string(d->end) : "");
          if (level == 2) {  // its members, counted by kind
            std::vector<std::pair<std::string, size_t>> kinds;
            for (size_t j = i + 1; j < f->decls.size() && f->owner[j] == i; j++) {
              if (!f->visible[j] || !f->keep[j]) continue;
              auto k = std::find_if(kinds.begin(), kinds.end(), [&](auto& x) { return x.first == f->decls[j].kind; });
              if (k == kinds.end()) kinds.push_back({f->decls[j].kind, 1});
              else k->second++;
            }
            for (auto& [kind, count] : kinds) l += ", " + counted(count, kind);
          }
          if (const FnEffects* e = doesOf(*f->path, *d)) l += ", " + doesText(*e);
          o += l + "\n";
        }
      }
    }
    return o;
  };
  std::string out = render(3), head;
  if (asJson) {  // all of it: the budget is for readers, and a program filters what it reads
    for (auto& s : notes) if (s.starts_with("nothing")) fprintf(stderr, "%s\n", s.c_str());
    return fflush(stdout), fwrite(out.data(), 1, out.size(), stdout), fflush(stdout), n;
  }
  size_t full = tokensAbout(out);
  int level = 3;
  while (budget > 0 && level > 0 && tokensAbout(out) > (size_t)budget) out = render(--level);
  if (!n) return 0;
  for (auto& s : notes) head += "// " + s + "\n";
  if (level < 3) {  // say what was left out, and how to see it, with the biggest packages or files as the example
    std::vector<std::pair<size_t, std::string>> big;
    for (auto& g : groups) {
      size_t lines = 0;
      for (auto* f : shownFiles(g)) {
        lines += f->lines;
        if (level > 0) big.push_back({f->lines, f->rel.substr(f->rel.rfind('/') + 1)});
      }
      if (level == 0 && g.pkg) big.push_back({lines, g.pkg->name});
    }
    std::stable_sort(big.begin(), big.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string example;
    for (size_t i = 0; i < big.size() && i < 2; i++) example += " " + big[i].second;
    static const char* what[] = {"packages only", "files only", "top-level declarations, with members counted"};
    head += "// the whole map is about " + thousands(full) + " tokens, over the " + thousands(budget) + " budget, so this shows " + what[level] +
            (tokensAbout(out) > (size_t)budget ? " (and still doesn't fit)" : "") + ".\n// For more, name packages, files or declarations (fut map " +
            (pubOnly ? "--pub " : "") + program + example + "; parts of names and globs work too) or set --budget (0: no limit).\n";
  }
  out = head + out;
  fflush(stdout), fwrite(out.data(), 1, out.size(), stdout), fflush(stdout);
  return n;
}
