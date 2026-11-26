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

// Declarations read from tokens, without compiling: what interfaces and `fut map` show.
struct Scan {
  Lexer L;
  Tok next() { Tok t = L.next(); return t.t == T_ERR ? Tok{T_EOF} : t; }
  static bool isKw(const Tok& t, int k) { return t.t == T_ID && t.sym == k; }
  const char* params(Tok& t) {  // t is '(': through the matching ')'; returns just past it
    for (int d = 0; t.t != T_EOF; t = next()) if ((d += (t.t == '(') - (t.t == ')')) == 0) return t.p + 1;
    return t.p;
  }
  const char* typed(const char* end) {  // `: type` next (a name, maybe module.name, maybe ?): through it
    Lexer s = L;
    if (s.next().t != ':' || s.next().t != T_ID) return end;
    L = s, end = s.p;
    if (Lexer d = L; d.next().t == '.' && d.next().t == T_ID) L = d, end = d.p;
    if (Lexer q = L; q.next().t == '?') L = q, end = q.p;
    return end;
  }
  const char* effects(const char* end) {  // `! io, geom.Log` next (or a bare `!`): through it
    Lexer s = L;
    if (s.next().t != '!') return end;
    L = s, end = s.p;
    for (Lexer d = L;; d = L) {
      int t = d.next().t;
      if (t != T_ID && t != ',' && t != '.') return end;
      L = d, end = d.p;
    }
  }
  void skipBody(Tok& t) {  // t is '{': through the matching '}'
    for (int d = 0; t.t != T_EOF; t = next()) if ((d += (t.t == '{') - (t.t == '}')) == 0) return;
  }
};
struct Item { std::string name, text; };
static void itemsOf(const std::string& text, std::vector<Item>& items) {
  const char* src = text.c_str();
  Scan S{Lexer{src}};
  Lexer& L = S.L;
  auto next = [&] { return S.next(); };
  auto isKw = Scan::isKw;
  auto params = [&](Tok& t) { return S.params(t); };
  auto typed = [&](const char* e) { return S.typed(e); };
  auto effects = [&](const char* e) { return S.effects(e); };
  auto skipBody = [&](Tok& t) { S.skipBody(t); };
  int depth = 0;
  for (Tok t = next(); t.t != T_EOF; t = next()) {
    if (t.t == '{') depth++;
    if (t.t == '}') depth--;
    if (depth || !isKw(t, S_pub)) continue;
    Lexer save = L;
    Tok k = next();
    if (k.t != T_ID || k.nl || !(k.sym == K_fn || k.sym == K_class || k.sym == K_var || k.sym == K_extern || k.sym == S_effect)) { L = save; continue; }
    Item it;
    std::string doc = docs(src, t.p, "");
    if (k.sym == K_var) {
      Tok n = next();
      it = {symName(n.sym), "pub var " + symName(n.sym)};
    } else if (k.sym == K_fn) {
      Tok n = next(), p = next();
      const char* end = params(p);
      it = {symName(n.sym), "pub " + squash(k.p, effects(typed(end)))};
    } else if (k.sym == K_extern) {
      Tok n = next();
      while (n.t != T_EOF && !isKw(n, K_fn)) n = next();
      Tok nm = next(), p = next();
      const char* end = params(p);
      Lexer s = L;
      if (s.next().t == ':') s.next(), end = s.p, L = s;  // its result type
      it = {symName(nm.sym), "pub " + squash(k.p, end)};
    } else if (k.sym == S_effect) {  // an effect: its operations, sorted, with their comments
      Tok n = next(), b = next();
      while (b.t != '{' && b.t != T_EOF) b = next();
      it.name = symName(n.sym), it.text = "pub effect " + symName(n.sym) + " {\n";
      std::vector<std::pair<std::string, std::string>> opl;
      for (Tok m = next(); m.t != '}' && m.t != T_EOF; m = next()) {
        if (!isKw(m, K_fn)) continue;
        std::string d = docs(src, m.p, "  ");
        Tok nm = next(), p = next();
        opl.push_back({symName(nm.sym), d + "  " + squash(m.p, typed(params(p))) + "\n"});
      }
      std::stable_sort(opl.begin(), opl.end(), [](auto& a, auto& b) { return before(a.first, b.first); });
      for (auto& [_, s] : opl) it.text += s;
      it.text += "}";
    } else {  // a class: its header, then its fields and methods (each sorted), with their comments
      Tok n = next(), b = next();
      while (b.t != '{' && b.t != T_EOF) b = next();
      it.name = symName(n.sym), it.text = "pub " + squash(k.p, b.p) + " {\n";
      std::vector<std::pair<std::string, std::string>> fields, methods;
      for (Tok m = next(); m.t != '}' && m.t != T_EOF; m = next()) {
        std::string d = docs(src, m.p, "  ");
        if (isKw(m, K_var)) {
          Tok f = next();
          typed(f.p);
          Lexer s = L;
          if (s.next().t == '=') {  // its default, a constant: one token, maybe negated
            L = s;
            if (next().t == '-') next();
          }
          fields.push_back({symName(f.sym), d + "  " + squash(m.p, L.p) + "\n"});
          continue;
        }
        Tok first = m;
        if (isKw(m, S_override)) m = next();
        if (!isKw(m, K_fn)) continue;
        Tok nm = next(), p = next();
        const char* end = effects(typed(params(p)));
        methods.push_back({symName(nm.sym), d + "  " + squash(first.p, end) + "\n"});
        Tok body = next();
        skipBody(body);
      }
      auto byName = [](auto& a, auto& b) { return before(a.first, b.first); };
      std::stable_sort(fields.begin(), fields.end(), byName), std::stable_sort(methods.begin(), methods.end(), byName);
      for (auto& [_, s] : fields) it.text += s;
      for (auto& [_, s] : methods) it.text += s;
      it.text += "}";
    }
    it.text = doc + it.text;
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

// The declarations an interface text lists, one per line ("fn area(r)", "Circle: var r = 1"), sorted: what a
// change report compares. Comments are left out (a change to them alone is reported as such).
static std::vector<std::string> entries(const std::string& text) {
  std::vector<std::string> e;
  std::string cls;
  for (size_t i = 0; i < text.size();) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) j = text.size();
    std::string l = squash(text.data() + i, text.data() + j);
    i = j + 1;
    if (l.empty() || l.starts_with("//")) continue;
    if (l.starts_with("pub ")) l = l.substr(4);
    if (l == "}") cls.clear();
    else if ((l.starts_with("class ") || l.starts_with("effect ")) && l.ends_with("{")) {
      l = squash(l.data(), l.data() + l.size() - 1), e.push_back(l);
      size_t at = l.find(' ') + 1;
      cls = l.substr(at, l.find(' ', at) - at);
    } else e.push_back(cls.empty() ? l : cls + ": " + l);
  }
  return std::sort(e.begin(), e.end()), e;
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
    if (std::string was = versionIn(have); exists && !u.pkg.empty() && !is.removed.empty() && !was.empty())
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
static std::string json(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += (char)c;
    else if (c == '\n') o += "\\n";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
int reportInterfaces(const std::vector<ApiIssue>& issues, bool asJson, const std::string& program) {
  for (auto& is : issues) {
    std::string s;
    if (asJson) {
      s = "{\"type\":\"api\",\"file\":" + json(is.file) + ",\"interface\":" + json(is.api) + ",\"status\":\"" + is.status + "\"";
      if (is.status == "breaking") s += ",\"version\":" + json(is.version) + ",\"needs\":" + json(is.needs);
      for (auto [key, list] : {std::pair{"removed", &is.removed}, {"added", &is.added}}) {
        s += std::string(",\"") + key + "\":[";
        for (size_t i = 0; i < list->size(); i++) s += (i ? "," : "") + json((*list)[i]);
        s += "]";
      }
      s += "}\n";
    } else {
      std::string fix = "`fut api " + program + "` ", api = base(is.api);
      s = "API     " + shown(is.file, program) + ": " +
          (is.status == "missing" ? "has pub declarations but no " + api + " yet; " + fix + "writes it\n"
           : is.status == "stale" ? "has no pub declarations, but " + api + " exists; " + fix + "removes it\n"
           : is.status == "breaking" ? "its package's interface lost declarations, a breaking change, but its version is still " + is.version +
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
      printf("not writing %s: its package's interface lost declarations, so its version must become at least %s (it is %s)\n", shown(is.api, program).c_str(), is.needs.c_str(), is.version.c_str());
      for (auto& r : is.removed) printf("  - %s\n", r.c_str());
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
struct Decl {
  std::string kind, name, sig, owner, mod;  // owner: the class or effect a member is in; mod: an extern's module
  bool pub;
  int line, end;
};
static std::vector<Decl> declsOf(const std::string& text) {
  std::vector<Decl> out;
  Scan S{Lexer{text.c_str()}};
  auto& L = S.L;
  auto isKw = Scan::isKw;
  int depth = 0;
  for (Tok t = S.next(); t.t != T_EOF; t = S.next()) {
    if (t.t == '{') depth++;
    if (t.t == '}') depth--;
    if (depth || t.t != T_ID) continue;
    bool pub = false;
    Tok k = t;
    if (isKw(t, S_pub)) {
      Lexer save = L;
      Tok n = S.next();
      if (n.t != T_ID || n.nl || !(n.sym == K_fn || n.sym == K_class || n.sym == K_var || n.sym == K_extern || n.sym == S_effect || n.sym == K_import)) { L = save; continue; }
      pub = true, k = n;
    }
    const char* from = pub ? t.p : k.p;
    if (isKw(k, K_fn)) {
      Tok n = S.next(), p = S.next();
      if (n.t != T_ID || p.t != '(') continue;
      const char* e = S.effects(S.typed(S.params(p)));
      Tok b = S.next();
      int end = b.line;
      if (b.t == '{') S.skipBody(b), end = b.line;
      out.push_back({"fn", symName(n.sym), squash(from, e), "", "", pub, k.line, end});
    } else if (isKw(k, K_var)) {
      Tok n = S.next();
      if (n.t != T_ID) continue;
      out.push_back({"var", symName(n.sym), squash(from, S.typed(L.p)), "", "", pub, k.line, k.line});
    } else if (isKw(k, K_import)) {
      Tok f = S.next();
      if (f.t != T_STR) continue;
      const char* e = f.p + f.len + 1;
      Lexer s = L;
      if (Tok a = s.next(); isKw(a, S_as)) if (Tok n = s.next(); n.t == T_ID) L = s, e = s.p;
      out.push_back({"import", std::string(f.p, f.len), squash(from, e), "", "", pub, k.line, k.line});
    } else if (isKw(k, K_extern)) {
      Tok n = S.next();
      std::string mod = n.t == T_ID && !isKw(n, K_fn) ? symName(n.sym) : n.t == T_STR ? std::string(n.p, n.len) : "";
      while (n.t != T_EOF && !isKw(n, K_fn)) n = S.next();
      Tok nm = S.next(), p = S.next();
      const char* e = S.params(p);
      if (Lexer s = L; s.next().t == ':') s.next(), e = s.p, L = s;
      out.push_back({"extern", symName(nm.sym), squash(from, e), "", mod, pub, k.line, k.line});
    } else if (isKw(k, S_effect) || isKw(k, K_class)) {
      bool eff = isKw(k, S_effect);
      Lexer save = L;
      Tok n = S.next(), b = S.next();
      if (n.t != T_ID || (eff && b.t != '{')) { L = save; continue; }
      while (b.t != '{' && b.t != T_EOF) b = S.next();
      size_t at = out.size();
      out.push_back({eff ? "effect" : "class", symName(n.sym), squash(from, b.p), "", "", pub, k.line, k.line});
      for (Tok m = S.next(); m.t != '}' && m.t != T_EOF; m = S.next()) {
        if (isKw(m, K_var) && !eff) {
          Tok f = S.next();
          S.typed(f.p);
          if (Lexer s = L; s.next().t == '=') {
            L = s;
            if (S.next().t == '-') S.next();
          }
          out.push_back({"field", symName(f.sym), squash(m.p, L.p), symName(n.sym), "", pub, m.line, m.line});
          continue;
        }
        Tok first = m;
        if (isKw(m, S_override)) m = S.next();
        if (!isKw(m, K_fn)) continue;
        Tok nm = S.next(), p = S.next();
        const char* e = S.effects(S.typed(S.params(p)));
        int end = m.line;
        if (!eff) {
          Tok body = S.next();
          if (body.t == '{') S.skipBody(body), end = body.line;
        }
        out.push_back({eff ? "op" : "method", symName(nm.sym), squash(first.p, e), symName(n.sym), "", pub, first.line, end});
      }
      out[at].end = L.line;
    } else if (isKw(k, S_test) && !pub) {
      Lexer save = L;
      Tok n = S.next();
      if (n.t != T_STR) { L = save; continue; }
      Tok b = S.next();
      int end = b.line;
      if (b.t == '{') S.skipBody(b), end = b.line;
      out.push_back({"test", std::string(n.p, n.len), squash(k.p, n.p + n.len + 1), "", "", false, k.line, end});
    }
  }
  return out;
}
int printMap(const Compiled& c, bool pubOnly, bool asJson, const std::string& only, const std::string& program) {
  struct Group { const Package* pkg; std::vector<const std::pair<std::string, std::string>*> files; };
  std::vector<Group> groups{{nullptr, {}}};  // first: the files outside any package
  for (auto& d : c.packages) if (const Package* p = packageAt(d)) groups.push_back({p, {}});
  for (auto& src : c.sources) {
    if (src.first.empty() || src.first[0] == '<') continue;
    const Package* p = packageOf(src.first);
    for (auto& g : groups) if (g.pkg == p) g.files.push_back(&src);
  }
  auto inInterface = [&](const Package* p, const std::string& f) {  // --pub: a package shows only its interface
    if (!p) return true;
    for (auto& u : c.apis) if (u.pkg == p->name) return std::find(u.parts.begin(), u.parts.end(), f) != u.parts.end();
    return false;
  };
  int n = 0;
  std::string o;
  auto emit = [&] { fflush(stdout), fwrite(o.data(), 1, o.size(), stdout), o.clear(); };
  for (auto& g : groups) {
    const Package* p = g.pkg;
    std::string dir = p ? p->dir + "/" : "";
    auto rel = [&](const std::string& f) { return p && f.starts_with(dir) ? f.substr(dir.size()) : shown(f, program); };
    std::vector<const std::pair<std::string, std::string>*> files;
    for (auto* f : g.files)
      if ((only.empty() || (p && only == p->name) || rel(f->first) == only || shown(f->first, program) == only || f->first == only) &&
          (!pubOnly || inInterface(p, f->first)))
        files.push_back(f);
    if (files.empty() && !(p && only == p->name)) continue;
    if (p) std::stable_partition(files.begin(), files.end(), [&](auto* f) { return canonicalPath(f->first) == p->root; });
    std::vector<std::vector<Decl>> decls;
    std::unordered_map<std::string, std::vector<std::string>> binds;  // module -> the extern fns bound to it
    for (auto* f : files) {
      decls.push_back(declsOf(f->second));
      for (auto& d : decls.back()) if (d.kind == "extern" && !d.mod.empty()) binds[d.mod].push_back(d.name);
    }
    if (p) {
      if (asJson) o += "{\"type\":\"package\",\"name\":" + json(p->name) + ",\"version\":" + json(p->version) + ",\"dir\":" + json(shown(p->dir, program)) +
                       ",\"root\":" + json(shown(p->root, program)) + "}\n";
      else o += "pkg " + p->name + " " + p->version + "  // " + shown(p->manifest, program) + "\n";
      for (auto& m : p->mods) {
        std::string t = m.kind == "c" && m.target.find('/') == std::string::npos ? m.target : rel(m.target), b;
        if (asJson) {
          for (auto& x : binds[m.name]) b += (b.empty() ? "" : ",") + json(x);
          o += "{\"type\":\"module\",\"package\":" + json(p->name) + ",\"kind\":\"" + m.kind + "\",\"name\":" + json(m.name) + ",\"target\":" + json(t) + ",\"binds\":[" + b + "]}\n";
        } else {
          for (auto& x : binds[m.name]) b += (b.empty() ? "" : ", ") + x;
          o += "  " + m.kind + " " + m.name + " " + t + (b.empty() ? "" : "  // " + b) + "\n";
        }
      }
    }
    std::string ind = p ? "  " : "";
    for (size_t i = 0; i < files.size(); i++) {
      const std::string& f = files[i]->first;
      std::vector<const Decl*> shownDecls;
      for (auto& d : decls[i]) if (!pubOnly || d.pub) shownDecls.push_back(&d);
      if (asJson) {
        o += "{\"type\":\"file\"" + (p ? ",\"package\":" + json(p->name) : "") + ",\"file\":" + json(shown(f, program)) + "}\n";
        for (auto* d : shownDecls) {
          o += "{\"type\":\"" + d->kind + "\"" + (p ? ",\"package\":" + json(p->name) : "") + ",\"file\":" + json(shown(f, program)) + ",\"name\":" + json(d->name);
          if (!d->owner.empty()) o += ",\"in\":" + json(d->owner);
          if (!d->mod.empty()) o += ",\"module\":" + json(d->mod);
          o += std::string(",\"pub\":") + (d->pub ? "true" : "false") + ",\"sig\":" + json(d->sig) + ",\"line\":" + std::to_string(d->line) + ",\"end\":" + std::to_string(d->end) + "}\n";
          n++;
        }
        emit();
        continue;
      }
      o += ind + rel(f) + (p && canonicalPath(f) == p->root ? "  // root" : "") + "\n";
      size_t w = 0;
      for (auto* d : shownDecls) w = std::max(w, d->sig.size() + (d->owner.empty() ? 0 : 2));
      w = std::min<size_t>(w, 56);
      for (auto* d : shownDecls) {
        std::string l = ind + "  " + (d->owner.empty() ? "" : "  ") + d->sig;
        size_t col = ind.size() + 2 + w + 2;
        l += std::string(l.size() < col ? col - l.size() : 2, ' ');
        l += "// " + std::to_string(d->line) + (d->end != d->line ? "-" + std::to_string(d->end) : "") + "\n";
        o += l, n++;
      }
      emit();
    }
    emit();
  }
  return n;
}
