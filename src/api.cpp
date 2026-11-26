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

std::string interfaceOf(const std::string& path, const std::string& text) {
  const char* src = text.c_str();
  struct Item { std::string name, text; };
  std::vector<Item> items;
  Lexer L{src};
  auto next = [&] { Tok t = L.next(); return t.t == T_ERR ? Tok{T_EOF} : t; };
  auto isKw = [](const Tok& t, int k) { return t.t == T_ID && t.sym == k; };
  auto params = [&](Tok& t) {  // t is '(': through the matching ')'; returns just past it
    for (int d = 0; t.t != T_EOF; t = next()) if ((d += (t.t == '(') - (t.t == ')')) == 0) return t.p + 1;
    return t.p;
  };
  auto skipBody = [&](Tok& t) {  // t is '{': through the matching '}'
    for (int d = 0; t.t != T_EOF; t = next()) if ((d += (t.t == '{') - (t.t == '}')) == 0) return;
  };
  int depth = 0;
  for (Tok t = next(); t.t != T_EOF; t = next()) {
    if (t.t == '{') depth++;
    if (t.t == '}') depth--;
    if (depth || !isKw(t, S_pub)) continue;
    Lexer save = L;
    Tok k = next();
    if (k.t != T_ID || k.nl || !(k.sym == K_fn || k.sym == K_class || k.sym == K_var || k.sym == K_extern)) { L = save; continue; }
    Item it;
    std::string doc = docs(src, t.p, "");
    if (k.sym == K_var) {
      Tok n = next();
      it = {symName(n.sym), "pub var " + symName(n.sym)};
    } else if (k.sym == K_fn) {
      Tok n = next(), p = next();
      it = {symName(n.sym), "pub " + squash(k.p, params(p))};
    } else if (k.sym == K_extern) {
      Tok n = next();
      while (n.t != T_EOF && !isKw(n, K_fn)) n = next();
      Tok nm = next(), p = next();
      const char* end = params(p);
      Lexer s = L;
      if (s.next().t == ':') s.next(), end = s.p, L = s;  // its result type
      it = {symName(nm.sym), "pub " + squash(k.p, end)};
    } else {  // a class: its header, then its fields and methods (each sorted), with their comments
      Tok n = next(), b = next();
      while (b.t != '{' && b.t != T_EOF) b = next();
      it.name = symName(n.sym), it.text = "pub " + squash(k.p, b.p) + " {\n";
      std::vector<std::pair<std::string, std::string>> fields, methods;
      for (Tok m = next(); m.t != '}' && m.t != T_EOF; m = next()) {
        std::string d = docs(src, m.p, "  ");
        if (isKw(m, K_var)) {
          Tok f = next();
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
        const char* end = params(p);
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
  if (items.empty()) return "";
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return before(a.name, b.name); });
  std::string o = "// The public interface of " + base(path) + ", written by `fut api`. `fut test` fails while it is out of date.\n";
  for (auto& it : items) o += "\n" + it.text + "\n";
  return o;
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
    else if (l.starts_with("class ") && l.ends_with("{")) {
      l = squash(l.data(), l.data() + l.size() - 1), e.push_back(l);
      cls = l.substr(6, l.find(' ', 6) - 6);
    } else e.push_back(cls.empty() ? l : cls + ": " + l);
  }
  return std::sort(e.begin(), e.end()), e;
}

std::vector<ApiIssue> checkInterfaces(const std::vector<std::pair<std::string, std::string>>& sources) {
  std::vector<ApiIssue> out;
  for (auto& [file, text] : sources) {
    if (file.empty() || file[0] == '<') continue;  // the REPL
    std::string want = interfaceOf(file, text), have, api = apiPath(file);
    bool exists = readFile(api, have);
    if (exists ? have == want : want.empty()) continue;
    ApiIssue is{file, api, !exists ? "missing" : want.empty() ? "stale" : "changed", {}, {}, want};
    auto a = entries(have), b = entries(want);
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(is.removed));
    std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(is.added));
    out.push_back(std::move(is));
  }
  return out;
}

// A path as the reader knows it: relative to the program's directory when it is under it.
static std::string shown(const std::string& file, const std::string& program) {
  std::string dir = canonicalPath(program), f = file[0] == '/' ? file : canonicalPath(file);
  dir = dir.substr(0, dir.rfind('/') + 1);
  return f.starts_with(dir) ? f.substr(dir.size()) : file;
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
int writeInterfaces(const std::vector<std::pair<std::string, std::string>>& sources, const std::string& program) {  // fut api
  int n = 0;
  for (auto& is : checkInterfaces(sources)) {
    bool ok = saveFile(is.api, is.status == "stale" ? nullptr : &is.want);
    printf("%s %s\n", !ok ? "could not write" : is.status == "stale" ? "removed" : "wrote", shown(is.api, program).c_str());
    for (auto& r : is.removed) printf("  - %s\n", r.c_str());
    for (auto& a : is.added) printf("  + %s\n", a.c_str());
    n += ok ? 1 : 0;
  }
  if (!n) printf("every interface is up to date\n");
  return n;
}
