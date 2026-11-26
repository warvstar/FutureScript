// Views and state, for now as a rewrite of the source before it compiles. A file that declares `view` or `state` is
// rewritten into std/ui calls:
//
//   state n = 0                     var n = std/reactive signal(0); reading n is n.get(), n = v is n.set(v), n += v an update
//   class T { state done }          a field holding a signal, made in init; t.done reads it, t.done = v sets it
//   view Row(t) { ...body... }      fn Row(t), returning the nodes its body makes; mount(Page) shows Page
//
// A body is elements, one a line (`row .task .done: t.done { check "x" bind: t.done }`). Any expression in a view is a function of the state it reads: only that part of
// the page changes when the state does. An element's fixed part (tags, classes, literal attributes and text) becomes a
// template, made once and cloned (ui.tpl); only its live parts are wired. The rewrite keeps every line where it was, so
// errors point at the source.
#include "fut.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <tuple>
#include <map>

namespace {
bool idStart(char c) { return ((c | 32) >= 'a' && (c | 32) <= 'z') || c == '_'; }
bool idChar(char c) { return idStart(c) || (c >= '0' && c <= '9'); }

struct Tok {
  char k = 0;  // 'i' name, 'n' number, 's' string, 'p' punctuation, 'm' the no-rewrite mark, 0 the end
  size_t tb = 0, b = 0, e = 0;  // trivia (spaces, comments) from tb, the token itself [b, e)
  bool nl = false;              // a line break before it
  bool sp() const { return tb != b; }
};

// From a string's opening quote to just past its closing one, as compile.h's skipString: a quote inside an
// interpolation's braces starts a string of its own, which ends on its line; braces that don't pair up end the string
// at its next quote. npos: no closing quote.
size_t nestedEnd(const std::string& s, size_t i, bool inner) {
  int depth = 0;
  for (i++; i < s.size(); i++) {
    if (inner && s[i] == '\n') return std::string::npos;
    if (s[i] == '\\') { i++; continue; }
    if (s[i] == '{') depth++;
    else if (s[i] == '}' && depth) depth--;
    else if (s[i] == '"') {
      if (!depth) return i + 1;
      size_t e = nestedEnd(s, i, true);
      if (e == std::string::npos) return e;
      i = e - 1;
    }
  }
  return std::string::npos;
}
size_t stringEnd(const std::string& s, size_t i) {
  size_t e = nestedEnd(s, i, false);
  if (e != std::string::npos) return e;
  for (i++; i < s.size() && s[i] != '"'; i++) if (s[i] == '\\') i++;
  return std::min(i + 1, s.size());
}

Tok lexAt(const std::string& s, size_t i) {
  Tok t;
  t.tb = i;
  for (; i < s.size(); i++) {
    if (s[i] == '\n') t.nl = true;
    else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i + 1 < s.size() && s[i + 1] != '\n') i++; }
    else if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') break;
  }
  t.b = i;
  if (i >= s.size()) return t.e = i, t;
  char c = s[i];
  if (c == '\x01') return t.k = 'm', t.e = i + 1, t;
  if (idStart(c)) { while (i < s.size() && idChar(s[i])) i++; return t.k = 'i', t.e = i, t; }
  if (c >= '0' && c <= '9') {
    while (i < s.size() && (idChar(s[i]) || (s[i] == '.' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9'))) i++;
    return t.k = 'n', t.e = i, t;
  }
  if (c == '"') return t.k = 's', t.e = stringEnd(s, i), t;
  static const char* const two[] = {"==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "**", "?.", ".."};
  for (const char* o : two) if (s.compare(i, 2, o) == 0) return t.k = 'p', t.e = i + 2, t;
  return t.k = 'p', t.e = i + 1, t;
}
bool is(const std::string& s, const Tok& t, const char* text) { return t.k && s.compare(t.b, t.e - t.b, text) == 0 && strlen(text) == t.e - t.b; }
std::string textOf(const std::string& s, const Tok& t) { return s.substr(t.b, t.e - t.b); }
bool opener(const std::string& s, const Tok& t) { return t.k == 'p' && (is(s, t, "(") || is(s, t, "[") || is(s, t, "{")); }
bool closer(const std::string& s, const Tok& t) { return t.k == 'p' && (is(s, t, ")") || is(s, t, "]") || is(s, t, "}")); }
bool operandEnd(const std::string& s, const Tok& t) { return t.k == 'i' || t.k == 'n' || t.k == 's' || closer(s, t); }
bool assignOp(const std::string& s, const Tok& t) {
  for (const char* o : {"=", "+=", "-=", "*=", "/=", "%="}) if (is(s, t, o)) return true;
  return false;
}
std::string lines(const std::string& s) { return std::string(std::count(s.begin(), s.end(), '\n'), '\n'); }

// ---- pass 1: views into fut ----
struct Views {
  const std::string& s;
  size_t i = 0;  // what's read so far
  std::string out;
  Views(const std::string& src, const std::string& file) : s(src), path(file) {}

  Tok peek(size_t at) const { return lexAt(s, at); }
  Tok peek() const { return lexAt(s, i); }
  Tok take() { Tok t = peek(); i = t.e; return t; }
  const std::string& path;
  [[noreturn]] void fail(size_t at, const std::string& m) {
    at = std::min(at, s.size());
    size_t line = 1 + std::count(s.begin(), s.begin() + at, '\n');
    size_t ls = at ? s.rfind('\n', at - 1) : std::string::npos;
    compileFail(path + ":" + std::to_string(line) + ":" + std::to_string(at - (ls == std::string::npos ? 0 : ls + 1) + 1) +
                ": error: " + m + " [syntax]", false);
  }
  void expect(const char* p) {
    Tok t = take();
    if (!is(s, t, p)) fail(t.b, std::string("expected '") + p + "'");
    out += s.substr(t.tb, t.b - t.tb);
  }
  // The end of the expression from i: a line break, a bracket closing past it, a comma, a `{` (a block), a
  // `.name` after a space (a class), or a value right after a value (`"a" "b"`, `x label: y`: the next item).
  size_t exprEnd(size_t from, bool stopAtBrace = true, bool stopAtKey = false) {
    int depth = 0, body = -1;  // body: the depth a function literal's block opens at (its `{` doesn't end it)
    bool value = false;
    size_t at = from;
    for (bool first = true;; first = false) {
      Tok t = peek(at);
      if (!t.k) return at;
      if (is(s, t, "fn") && is(s, peek(t.e), "(") && body < 0) body = depth;
      if (depth == 0) {
        if (!first && t.nl) return at;
        if (closer(s, t) || is(s, t, ",")) return at;
        if (stopAtBrace && is(s, t, "{") && body != 0) return at;
        if (!first && is(s, t, ".") && t.sp()) return at;
        if (!first && value && (t.k == 'i' || t.k == 'n' || t.k == 's' || t.k == 'm')) return at;
        if (stopAtKey && t.k == 'i' && is(s, t, "key")) return at;
      }
      if (opener(s, t)) depth++;
      else if (closer(s, t)) {
        depth--;
        if (body == depth && is(s, t, "}")) body = -1;  // the function's block ends
      }
      value = depth == 0 && operandEnd(s, t);
      at = t.e;
    }
  }
  std::string expr(bool stopAtBrace = true, bool stopAtKey = false) {
    size_t e = exprEnd(i, stopAtBrace, stopAtKey);
    Tok f = peek();
    if (e <= f.b) fail(f.b, "expected an expression");
    out += s.substr(f.tb, f.b - f.tb);  // its leading trivia stays where it was
    std::string r = s.substr(f.b, e - f.b);
    i = e;
    return r;
  }
  static bool literal(const std::string& e) {  // shown as it is: no function, nothing to track
    if (e == "true" || e == "false" || e == "nil") return true;
    if (!e.empty() && e[0] >= '0' && e[0] <= '9' && e.find_first_of(" (") == std::string::npos) return true;
    if (e.size() < 2 || e[0] != '"' || e.back() != '"') return false;
    for (size_t k = 1; k + 1 < e.size(); k++) {
      if (e[k] == '\\') k++;
      else if (e[k] == '{' || e[k] == '"') return false;
    }
    return true;
  }
  static std::string live(const std::string& e) { return literal(e) ? e : "fn() { return " + e + " }"; }
  static std::string quote(const std::string& name) { return "\"" + name + "\""; }
  // A brace block's contents, raw (a handler's statements): from after `{` to its `}`, which is consumed.
  std::string braceBody() {
    size_t start = i;
    int depth = 1;
    for (;;) {
      Tok t = peek();
      if (!t.k) fail(start, "this block has no closing '}'");
      i = t.e;
      if (opener(s, t)) depth++;
      else if (closer(s, t) && --depth == 0) return s.substr(start, t.b - start);
    }
  }
  static std::string classProp(const std::vector<std::string>& fixed, const std::vector<std::pair<std::string, std::string>>& when) {
    std::string names;
    for (auto& c : fixed) names += (names.empty() ? "" : " ") + c;
    if (when.empty()) return "\"class\": " + quote(names);
    std::string l = "[" + quote(names) + ", true";
    for (auto& [c, e] : when) l += ", " + (c[0] == '=' ? c.substr(1) : quote(c)) + ", " + e;
    return "\"class\": fn() { return ui.classes(" + l + "]) }";
  }
  static std::string join(const std::vector<std::string>& v) {
    std::string r;
    for (auto& x : v) r += (r.empty() ? "" : ", ") + x;
    return r;
  }

  // Statements of a children block into pushes onto list k, up to its `}` (not consumed).
  void children(int n) {
    std::string k = "_k" + std::to_string(n);
    for (;;) {
      Tok t = peek();
      if (!t.k) fail(t.b, "this view's block has no closing '}'");
      if (is(s, t, "}")) return;
      out += s.substr(t.tb, t.b - t.tb), i = t.b;
      std::string word = t.k == 'i' ? textOf(s, t) : "";
      Tok n2 = peek(t.e);
      if (word == "if") { ifBlock(n); continue; }
      if (word == "for") { forBlock(n); continue; }
      if (word == "var" || (word == "state" && n2.k == 'i')) {  // a local: var x = e, state x = e
        take(), out += word;
        Tok name = take();
        out += s.substr(name.tb, name.e - name.tb);
        expect("=");
        out += "=";
        out += expr(false);
        continue;
      }
      if (t.k == 's') { out += k + ".push(" + live(expr()) + ")"; continue; }
      if (t.k == 'i' && word[0] >= 'A' && word[0] <= 'Z' && is(s, n2, "(") && !n2.sp()) {  // a view: Row(t)
        out += k + ".push(" + expr() + ")";
        continue;
      }
      if (t.k == 'i' && !assignOp(s, n2) && !((is(s, n2, "(") || is(s, n2, ".")) && !n2.sp())) { element(n); continue; }
      if (!assignOp(s, peek(exprEnd(i, false)))) { out += k + ".push(" + live(expr(false)) + ")"; continue; }  // a call: its result, kept current
      out += expr(false);  // an assignment: x = 1, t.done = true
      if (Tok a = peek(); assignOp(s, a)) { take(), out += s.substr(a.tb, a.e - a.tb) + expr(false); }
    }
  }
  std::string block(int n) {  // `{ children }` as a list: ui.kids(fn(_kn) { ... })
    Tok o = take();
    if (!is(s, o, "{")) fail(o.b, "expected '{'");
    std::string saved = std::exchange(out, "");
    out += "ui.kids(fn(_k" + std::to_string(n) + ") {";
    children(n);
    Tok c = take();
    out += s.substr(c.tb, c.b - c.tb) + "})";
    return std::exchange(out, saved);
  }
  void ifBlock(int n) { out += "_k" + std::to_string(n) + ".push(" + ifExpr(n) + ")"; }
  std::string ifExpr(int n) {  // if/else if/else children: a function giving the branch's list
    std::string r = "fn() {";
    for (bool first = true;; first = false) {
      take();  // if
      std::string saved = std::exchange(out, "");
      std::string cond = expr();
      std::string lead = std::exchange(out, saved);
      r += (first ? " if" : " else if") + lead + cond + " { return " + block(n + 1) + " }";
      Tok e = peek();
      if (!is(s, e, "else")) break;
      r += s.substr(e.tb, e.b - e.tb), take();
      if (Tok w2 = peek(); is(s, w2, "if")) { r += s.substr(w2.tb, w2.b - w2.tb), i = w2.b; continue; }
      r += " else { return " + block(n + 1) + " }";
      break;
    }
    return r + " return nil }";
  }
  void forBlock(int n) {
    std::string lead;
    std::string e = forExpr(n, lead);
    out += lead + "_k" + std::to_string(n) + ".push(" + e + ")";
  }
  std::string forExpr(int n, std::string& lead) {  // for x in list key k { children }: ui.each(...)
    take();  // for
    Tok v = take();
    if (v.k != 'i') fail(v.b, "expected a name after 'for'");
    std::string var = textOf(s, v);
    if (Tok in = take(); !is(s, in, "in")) fail(in.b, "expected 'in'");
    std::string saved = std::exchange(out, "");
    std::string list = expr(true, true), key = var;
    if (Tok kw = peek(); is(s, kw, "key")) take(), key = expr();
    lead = std::exchange(out, saved);
    return "ui.each(fn() { return " + list + " }, fn(" + var + ") { return " + key + " }, fn(" + var + ") { return " + block(n + 1) +
           " })";
  }
  // ---- an element as a tree, for a template (ui.tpl): its fixed part made once and cloned ----
  struct El;
  struct Kid {
    char k;            // 't' literal text, 's' a string with holes (code: a function), 'd' any other child (code), 'e' an element
    std::string code;
    std::shared_ptr<El> el;
    int ln = 0;        // the source line it starts on
  };
  struct El {
    std::string name, bind;
    std::vector<std::string> fixed;
    std::vector<std::pair<std::string, std::string>> when;
    std::vector<std::tuple<std::string, std::string, bool>> attrs;  // name, code, a handler's
    std::vector<int> attrLn;  // each attribute's source line
    std::vector<Kid> kids;
    int ln = 0;
  };
  // Lines: a template's wiring is one expression, but each live part's code goes on its own source line (padded with
  // newlines), so an error in a handler points at it. wl: the line the code being made is on; padded: newlines added.
  std::vector<size_t> nls;  // where the source's newlines are
  int wl = 0, padded = 0;
  int lineAt(size_t pos) {
    if (nls.empty()) { for (size_t q = 0; q < s.size(); q++) if (s[q] == '\n') nls.push_back(q); nls.push_back(SIZE_MAX); }
    return int(std::lower_bound(nls.begin(), nls.end(), pos) - nls.begin());
  }
  void pad(std::string& to, int ln) { while (wl < ln) to += '\n', wl++, padded++; }
  void leaf(std::string& to, int ln, const std::string& code) {  // code from the source, on its line
    pad(to, ln);
    to += code, wl += int(std::count(code.begin(), code.end(), '\n'));
  }
  Kid lnKid(Kid k, size_t pos) { return k.ln = lineAt(pos), k; }
  // tag .class .class: cond name: value on event { ... } content... { children }
  void element(int n) {
    size_t at = i;
    std::string was = out, nl;
    auto e = elTree(n, nl);
    if (!e) { i = at, out = was, elementMake(n); return; }  // a block with statements in it: as before
    wl = e->ln, padded = 0;
    std::string c = code(*e);
    out += "_k" + std::to_string(n) + ".push(" + c + ")" + std::string(std::max(0, int(nl.size()) - padded), '\n');
  }
  // The element from i as a tree, or nullptr when its block has statements (var, assignments). nl: its newlines.
  std::shared_ptr<El> elTree(int n, std::string& nl) {
    auto e = std::make_shared<El>();
    e->ln = lineAt(peek().b);
    e->name = textOf(s, take());
    std::string saved = std::exchange(out, "");
    for (;;) {
      Tok t = peek();
      if (!t.k || t.nl || is(s, t, "}")) break;
      Tok n2 = peek(t.e);
      if (is(s, t, ".") && n2.k == 'i' && !n2.sp()) {
        i = n2.b;
        std::string c = markName();
        if (Tok col = peek(); is(s, col, ":") && !col.sp()) take(), e->when.push_back({c, expr()});
        else e->fixed.push_back(c);
      } else if (t.k == 'i' && is(s, n2, ":") && !n2.sp()) {
        take(), take();
        std::string a = textOf(s, t);
        if (a == "class") e->when.push_back({"=" + expr(), "true"});
        else if (a == "bind") e->bind = expr();
        else e->attrLn.push_back(lineAt(t.b)), e->attrs.push_back({a, expr(), false});
      } else if (is(s, t, "on") && n2.k == 'i' && is(s, peek(n2.e), "{")) {
        take(), take(), take();
        e->attrLn.push_back(lineAt(t.b)), e->attrs.push_back({"on" + textOf(s, n2), "fn(e) {" + braceBody() + "}", true});
      } else if (is(s, t, "{")) {
        if (!kidsTree(n + 1, e->kids)) return out = saved, nullptr;
        break;
      } else e->kids.push_back(lnKid(kidOf(expr()), t.b));
    }
    nl += lines(std::exchange(out, saved));
    return e;
  }
  Kid kidOf(const std::string& x) { return literal(x) ? Kid{'t', x, nullptr} : x[0] == '"' ? Kid{'s', live(x), nullptr} : Kid{'d', live(x), nullptr}; }
  bool kidsTree(int n, std::vector<Kid>& kids) {  // { children } (consumed), or false: statements in it
    take();
    for (;;) {
      Tok t = peek();
      if (!t.k) fail(t.b, "this view's block has no closing '}'");
      if (is(s, t, "}")) { take(); return true; }
      out += s.substr(t.tb, t.b - t.tb), i = t.b;  // (only its newlines are kept)
      std::string word = t.k == 'i' ? textOf(s, t) : "";
      Tok n2 = peek(t.e);
      if (word == "if") { kids.push_back(lnKid({'d', ifExpr(n), nullptr}, t.b)); continue; }
      if (word == "for") { std::string lead; std::string e = forExpr(n, lead); out += lead, kids.push_back(lnKid({'d', e, nullptr}, t.b)); continue; }
      if (word == "var" || (word == "state" && n2.k == 'i')) return false;
      if (t.k == 's') { kids.push_back(lnKid(kidOf(expr()), t.b)); continue; }
      if (t.k == 'i' && word[0] >= 'A' && word[0] <= 'Z' && is(s, n2, "(") && !n2.sp()) { kids.push_back(lnKid({'d', expr(), nullptr}, t.b)); continue; }
      if (t.k == 'i' && !assignOp(s, n2) && !((is(s, n2, "(") || is(s, n2, ".")) && !n2.sp())) {
        std::string nl;
        auto c = elTree(n, nl);
        if (!c) return false;
        out += nl, kids.push_back(lnKid({'e', "", c}, t.b));
        continue;
      }
      if (!assignOp(s, peek(exprEnd(i, false)))) { kids.push_back(lnKid({'d', live(expr(false)), nullptr}, t.b)); continue; }
      return false;  // an assignment
    }
  }
  static bool templated(const El& e) {  // fut's own tags that do more than an element, binding and SVG: ui.make
    static const std::set<std::string> own = {"field", "choice", "area", "check", "dialog", "link", "svg", "g", "path", "line",
      "polyline", "polygon", "rect", "circle", "ellipse", "defs", "linearGradient", "stop", "title", "label", "tabs", "tab", "menu", "files"};
    if (e.name == "table") for (auto& a : e.attrs) if (std::get<0>(a) == "rows") return false;  // ui.table's
    return e.bind.empty() && !own.count(e.name);
  }
  std::string code(const El& e) {  // the element's code: a template's, else ui.make's
    if (!templated(e)) return makeCode(e);
    std::string wire;
    int at = 0;
    std::string desc = descOf(e, at, wire, true);
    uint64_t h = 1469598103934665603ull;
    for (char c : desc) h = (h ^ (unsigned char)c) * 1099511628211ull;
    char key[24];
    snprintf(key, sizeof key, "t%llx", (unsigned long long)h);
    return "ui.tpl(\"" + std::string(key) + "\", fn() { return " + desc + " }, fn(_n) {" + wire + " })";
  }
  std::string makeCode(const El& e) {
    std::string r = "ui.make(" + quote(e.name) + ", {";
    bool first = true;
    auto prop = [&](int ln, const std::string& code) { if (!first) r += ", "; first = false; leaf(r, ln, code); };
    if (!e.fixed.empty() || !e.when.empty()) prop(e.ln, classProp(e.fixed, e.when));
    if (!e.bind.empty()) prop(e.ln, "\"bind\": \x01" + e.bind);
    for (size_t q = 0; q < e.attrs.size(); q++) {
      auto& [a, c, h] = e.attrs[q];
      prop(e.attrLn[q], quote(a) + ": " + (h ? c : live(c)));
    }
    r += "}, [";
    first = true;
    for (auto& k : e.kids) {
      if (!first) r += ", ";
      first = false;
      if (k.k == 'e') pad(r, k.ln), r += code(*k.el);
      else leaf(r, k.ln, k.code);
    }
    return r + "])";
  }
  // e's part of a template's description (see dom.js's template), and the code wiring its live parts onto the
  // nodes an instance gives (_n[1], _n[2], ...: in this order; the root is _n[0]).
  std::string descOf(const El& e, int& at, std::string& wire, bool root) {
    std::string tag = e.name;
    std::vector<std::string> fixed = e.fixed, stat;
    std::vector<std::pair<int, std::string>> live_;  // (line, code)
    if (tag == "row" || tag == "column" || tag == "text") fixed.insert(fixed.begin(), tag), tag = "div";
    if (!fixed.empty() || !e.when.empty()) {
      if (e.when.empty()) stat.push_back(classProp(fixed, e.when));
      else live_.push_back({e.ln, classProp(fixed, e.when)});
    }
    for (size_t q = 0; q < e.attrs.size(); q++) {
      auto& [a, c, h] = e.attrs[q];
      bool fixedAttr = !h && literal(c) && a != "value" && a != "checked" && a != "disabled";
      if (fixedAttr) stat.push_back(quote(a) + ": " + c);
      else live_.push_back({e.attrLn[q], quote(a) + ": " + (h ? c : live(c))});
    }
    bool slot = e.kids.size() == 1 && e.kids[0].k == 's';  // one string with holes: a text node it keeps current
    int flags = (live_.empty() ? 0 : 1) | (slot ? 2 : 0);
    if (flags & 1) {
      wire += " ui.wire(_n[" + std::to_string(root ? 0 : ++at) + "], {";
      for (size_t q = 0; q < live_.size(); q++) {
        if (q) wire += ", ";
        leaf(wire, live_[q].first, live_[q].second);
      }
      wire += "})";
    }
    if (slot) wire += " ui.textAt(_n[" + std::to_string(++at) + "], ", leaf(wire, e.kids[0].ln, e.kids[0].code), wire += ")";
    std::string d = "[" + quote(tag) + ", {" + join(stat) + "}, " + std::to_string(flags);
    if (!slot)
      for (auto& k : e.kids) {
        if (k.k == 't') { if (k.code != "true" && k.code != "false" && k.code != "nil") d += ", " + k.code; }
        else if (k.k == 'e' && templated(*k.el)) d += ", " + descOf(*k.el, at, wire, false);
        else {
          d += ", nil", wire += " ui.hole(_n[" + std::to_string(++at) + "], ";
          if (k.k == 'e') pad(wire, k.ln), wire += code(*k.el);
          else leaf(wire, k.ln, k.code);
          wire += ")";
        }
      }
    return d + "]";
  }

  void elementMake(int n) {
    Tok tag = take();
    std::string name = textOf(s, tag);
    std::vector<std::string> props, kids, fixed;
    std::vector<std::pair<std::string, std::string>> when;
    std::string saved = std::exchange(out, "");
    for (;;) {
      Tok t = peek();
      if (!t.k || t.nl || is(s, t, "}")) break;
      Tok n2 = peek(t.e);
      if (is(s, t, ".") && n2.k == 'i' && !n2.sp()) {
        i = n2.b;
        std::string c = markName();
        if (Tok col = peek(); is(s, col, ":") && !col.sp()) take(), when.push_back({c, expr()});
        else fixed.push_back(c);
      } else if (t.k == 'i' && is(s, n2, ":") && !n2.sp()) {
        take(), take();
        std::string a = textOf(s, t);
        if (a == "class") when.push_back({"=" + expr(), "true"});  // class names from an expression
        else if (a == "bind") props.push_back("\"bind\": \x01" + expr());
        else props.push_back(quote(a) + ": " + live(expr()));
      } else if (is(s, t, "on") && n2.k == 'i' && is(s, peek(n2.e), "{")) {
        take(), take(), take();
        props.push_back(quote("on" + textOf(s, n2)) + ": fn(e) {" + braceBody() + "}");
      } else if (is(s, t, "{")) {
        kids.push_back(block(n + 1));
        break;
      } else kids.push_back(live(expr()));
    }
    std::string lead = std::exchange(out, saved);
    if (!fixed.empty() || !when.empty()) props.insert(props.begin(), classProp(fixed, when));
    out += "_k" + std::to_string(n) + ".push(ui.make(" + quote(name) + ", {" + join(props) + "}, [" + join(kids) + "]))" + lines(lead);
  }

  std::string markName() {  // a class name: dashes allowed (.bed-title)
    size_t b = i;
    while (i < s.size() && (idChar(s[i]) || s[i] == '-')) i++;
    if (b == i) fail(b, "expected a name");
    return s.substr(b, i - b);
  }

  // ---- the file ----
  std::string run() {
    bool ownMount = s.find("fn mount(") != std::string::npos;
    int depth = 0;
    Tok last;
    for (;;) {
      Tok t = peek();
      if (!t.k) { out += s.substr(t.tb); break; }
      if (depth == 0 && is(s, t, "view") && (t.nl || t.tb == 0) && peek(t.e).k == 'i') { view(t); last = Tok{}; continue; }
      if (depth == 0 && (t.nl || t.tb == 0) && is(s, t, "ui") && is(s, peek(t.e), ".") && is(s, peek(peek(t.e).e), "style")) {
        // a page's style sheet: a var's initializer, so an edit to it runs again on reload (a statement would not)
        // (named for its text: an edit anywhere in it makes a new var, and a reload runs a new var's initializer)
        size_t at = peek(peek(t.e).e).e, end = at;
        int depth = 0;
        for (Tok q = peek(at); q.k; q = peek(q.e)) {
          if (opener(s, q)) depth++;
          else if (closer(s, q) && --depth == 0) { end = q.e; break; }
        }
        uint64_t h = 1469598103934665603ull;
        for (size_t k = t.b; k < end; k++) h = (h ^ (unsigned char)s[k]) * 1099511628211ull;
        char name[32];
        snprintf(name, sizeof name, "_style_%llx", (unsigned long long)h);
        out += s.substr(t.tb, t.b - t.tb) + "var " + name + " = ";
        out += "ui", i = t.e, last = t;
        continue;
      }
      if (is(s, t, "mount") && !ownMount && !is(s, last, ".") && is(s, peek(t.e), "(")) {
        out += s.substr(t.tb, t.b - t.tb) + "ui.show", i = t.e, last = t;
        continue;
      }
      if (opener(s, t)) depth++;
      else if (closer(s, t)) depth--;
      out += s.substr(t.tb, t.e - t.tb), i = t.e, last = t;
    }
    return out;
  }
  bool views = false;
  void view(const Tok& kw) {
    views = true;
    out += s.substr(kw.tb, kw.b - kw.tb) + "fn", i = kw.e;
    Tok name = take();
    out += s.substr(name.tb, name.e - name.tb);
    if (Tok p = peek(); is(s, p, "(")) {
      take(), out += "(" + braceLess(")") + ")";
    } else out += "()";
    Tok o = take();
    if (!is(s, o, "{")) fail(o.b, "expected '{' to start the view");
    out += s.substr(o.tb, o.e - o.tb);
    out += " return ui.kids(fn(_k1) {";
    children(1);
    Tok c = take();
    out += s.substr(c.tb, c.b - c.tb) + "}) }";
  }
  std::string braceLess(const char* close) {  // a parameter list's text, up to `close` (consumed)
    size_t b = i;
    for (Tok t = peek(); t.k && !is(s, t, close); t = peek()) i = t.e;
    std::string r = s.substr(b, peek().b - b);
    take();
    return r;
  }
};

// ---- pass 2: state reads and writes ----
struct State {
  const std::string& s;
  std::set<std::string> tops, fields;
  std::vector<Tok> t;
  std::vector<size_t> match;  // each bracket's partner
  std::vector<bool> quiet;    // a fn's parameters, and names a local or parameter shadows: not the state
  std::vector<size_t> encl;   // each token's innermost enclosing `{`
  struct Local { std::string name; size_t from, to; };
  std::vector<Local> locals;  // state in a function or view: its name, to its block's end
  std::string out;
  struct ClassBody { std::vector<std::pair<std::string, std::string>> fields; bool hasInit = false; };
  std::map<size_t, ClassBody> classes;  // by the index of its `{`
  explicit State(const std::string& src) : s(src) {
    for (size_t at = 0;;) {
      Tok k = lexAt(s, at);
      t.push_back(k);
      if (!k.k) break;
      at = k.e;
    }
    match.assign(t.size(), 0), quiet.assign(t.size(), false);
    std::vector<size_t> open;
    for (size_t j = 0; j < t.size(); j++) {
      if (opener(s, t[j])) open.push_back(j);
      else if (closer(s, t[j]) && !open.empty()) match[open.back()] = j, match[j] = open.back(), open.pop_back();
    }
    encl.assign(t.size(), SIZE_MAX);
    for (size_t j = 0, cur = SIZE_MAX; j < t.size(); j++) {
      encl[j] = cur;
      if (is(s, t[j], "{")) cur = j;
      else if (is(s, t[j], "}") && match[j] < j) cur = encl[match[j]];
    }
    auto shade = [&](const std::string& name, size_t from, size_t to) {
      for (size_t q = from; q < to && q < t.size(); q++)  // the name itself, not a field of that name (self.name)
        if (t[q].k == 'i' && textOf(s, t[q]) == name && !(q && (is(s, t[q - 1], ".") || is(s, t[q - 1], "?.")))) quiet[q] = true;
    };
    for (size_t j = 0; j + 1 < t.size(); j++) {
      if (is(s, t[j], "fn")) {  // parameters: within the parameter list and the body
        size_t p = j + 1 + (t[j + 1].k == 'i');
        if (!is(s, t[p], "(")) continue;
        size_t body = match[p] + 1;
        while (body < t.size() && !is(s, t[body], "{")) body++;  // past a result type
        if (body >= t.size()) continue;
        for (size_t q = p + 1; q < match[p]; q++)
          if (t[q].k == 'i' && (is(s, t[q - 1], "(") || is(s, t[q - 1], ","))) shade(textOf(s, t[q]), p, match[body]);
      } else if ((is(s, t[j], "var") || is(s, t[j], "for")) && t[j + 1].k == 'i' && encl[j] != SIZE_MAX) {  // a local, to its block's end
        size_t end = match[encl[j]];
        if (is(s, t[j], "for")) {  // the loop's body
          size_t q = j;
          while (q + 1 < t.size() && !is(s, t[q], "{")) q++;
          end = match[q];
        }
        shade(textOf(s, t[j + 1]), j + 1, end);
      }
    }
  }
  std::string owner(size_t j) {  // the innermost named function (or view) around token j
    for (size_t b = encl[j]; b != SIZE_MAX; b = encl[b]) {
      size_t q = b;
      while (q > 0 && !is(s, t[q - 1], ")") && !is(s, t[q - 1], "{") && !is(s, t[q - 1], "}") && !t[q].nl) q--;  // past a result type
      if (q == 0 || !is(s, t[q - 1], ")")) continue;
      size_t p = match[q - 1];
      if (p >= 2 && t[p - 1].k == 'i' && is(s, t[p - 2], "fn")) return textOf(s, t[p - 1]);
    }
    return "top";
  }
  void scan() {  // state declarations: top level and in class bodies
    int depth = 0;
    std::vector<size_t> body;  // the open class bodies' `{`, with their depth
    bool classNext = false;
    for (size_t j = 0; j + 1 < t.size(); j++) {
      if (depth == 0 && is(s, t[j], "class")) classNext = true;
      if (is(s, t[j], "{")) {
        if (classNext && depth == 0) classes[j], body.push_back(j);
        classNext = false, depth++;
        continue;
      }
      if (is(s, t[j], "}")) { depth--; continue; }
      if (opener(s, t[j])) depth++;
      else if (closer(s, t[j])) depth--;
      if (!is(s, t[j], "state") || t[j + 1].k != 'i' || t[j + 1].nl) continue;
      if (depth == 0) tops.insert(textOf(s, t[j + 1]));
      else if (!(depth == 1 && !body.empty() && j > body.back() && j < match[body.back()])) {
        if (encl[j] != SIZE_MAX) locals.push_back({textOf(s, t[j + 1]), j + 1, match[encl[j]]});
      } else {
        std::string def = "nil";
        if (is(s, t[j + 2], "=")) def = textOf(s, t[j + 3]);
        classes[body.back()].fields.push_back({textOf(s, t[j + 1]), def});
        fields.insert(textOf(s, t[j + 1]));
      }
    }
    for (auto& [b, c] : classes)
      for (size_t j = b + 1; j < match[b]; j++) if (is(s, t[j], "fn") && is(s, t[j + 1], "init")) c.hasInit = true;
  }
  size_t exprEnd(size_t j) {  // as Views::exprEnd, in tokens
    int depth = 0;
    bool value = false;
    for (size_t at = j;; at++) {
      const Tok& k = t[at];
      if (!k.k) return at;
      if (depth == 0) {
        if (at > j && k.nl) return at;
        if (closer(s, k) || is(s, k, ",")) return at;
        if (at > j && value && (k.k == 'i' || k.k == 'n' || k.k == 's' || k.k == 'm')) return at;
      }
      if (opener(s, k)) depth++;
      else if (closer(s, k)) depth--;
      value = depth == 0 && operandEnd(s, k);
    }
  }
  bool localAt(const std::string& name, size_t j) const {
    for (auto& l : locals) if (l.name == name && j >= l.from && j < l.to) return true;
    return false;
  }
  const ClassBody* inClass(size_t j) const {
    for (auto& [b, c] : classes) if (j > b && j < match[b]) return &c;
    return nullptr;
  }
  std::string inits(const ClassBody& c) {
    std::string r;
    for (auto& [f, d] : c.fields) r += " self." + f + " = _state.cell(" + d + ")";
    return r;
  }
  // The first token of the value `.name` at j is read from: `a`, `a.b`, `f(x)[i]`, `(e)`.
  size_t receiver(size_t j) const {
    static const std::set<std::string> words = {"return", "if", "while", "for", "in", "else", "var", "fn", "throw",
                                                "yield", "spawn", "match", "and", "or", "not", "expect"};
    size_t q = j - 2;
    for (;;) {
      if (is(s, t[q], ")") || is(s, t[q], "]")) {
        q = match[q];
        if (q && !t[q].nl && !t[q].sp() && ((t[q - 1].k == 'i' && !words.count(textOf(s, t[q - 1]))) || is(s, t[q - 1], ")") ||
                                           is(s, t[q - 1], "]"))) { q--; continue; }
        return q;
      }
      if (is(s, t[q], "}")) return match[q];
      if (q >= 2 && (is(s, t[q - 1], ".") || is(s, t[q - 1], "?.")) && !t[q - 1].sp()) { q -= 2; continue; }
      return q;
    }
  }
  std::vector<size_t> at;  // where each token's text starts in out
  // Put text in out at a token's place (before what was emitted from there on), keeping the later places right.
  void insertAt(size_t tok, size_t upto, const std::string& text) {
    size_t p = at[tok];
    out.insert(p, text);
    for (size_t q = tok + 1; q <= upto; q++) if (at[q] >= p) at[q] += text.size();
  }
  void emit(size_t from, size_t to) {
    bool mute = false;  // after the mark: a bound signal, not its value
    if (at.size() < t.size()) at.assign(t.size(), 0);
    for (size_t j = from; j < to; j++) {
      const Tok& k = t[j];
      std::string pre = s.substr(k.tb, k.b - k.tb);
      at[j] = out.size() + pre.size();
      if (k.k == 'm') { out += pre, mute = true; continue; }
      if (mute && !(k.k == 'i' || is(s, k, "."))) mute = false;
      if (!k.k) { out += pre; continue; }
      if (is(s, k, "state") && j + 1 < t.size() && t[j + 1].k == 'i' && !t[j + 1].nl) {
        std::string name = textOf(s, t[j + 1]);
        const ClassBody* in = inClass(j);
        if (in && fields.count(name) && !localAt(name, j + 1)) {  // a field: made in init
          size_t e = j + 2 + (is(s, t[j + 2], "=") ? 2 : 0);
          std::string gone;
          for (size_t q = j + 1; q < e; q++) gone += s.substr(t[q].tb, t[q].b - t[q].tb);
          out += pre + lines(gone);
          j = e - 1;
          continue;
        }
        if ((!in && tops.count(name)) || localAt(name, j + 1)) {
          bool local = localAt(name, j + 1);  // a view's or function's own: kept across a reload (_state.local)
          out += pre + "var " + name + (local ? " = _state.local(\"" + owner(j) + "." + name + "\", " : " = _state.named(\"" + name + "\", ");
          size_t e = j + 2;
          if (is(s, t[e], "=")) { size_t x = exprEnd(e + 1); out += s.substr(t[e].tb, t[e].b - t[e].tb); emit(e + 1, x), e = x; }
          else out += "nil";
          out += ")";
          j = e - 1;
          continue;
        }
      }
      if (auto c = classes.find(j); c != classes.end()) {
        out += pre + "{";
        if (!c->second.hasInit && !c->second.fields.empty()) out += " fn init() {" + inits(c->second) + " }";
        continue;
      }
      if (is(s, k, "init") && j > 0 && is(s, t[j - 1], "fn") && is(s, t[j + 1], "(")) {
        if (const ClassBody* c = inClass(j)) {
          size_t open = match[j + 1] + 1;  // the body's `{`
          out += pre + "init";
          emit(j + 1, open + 1);
          out += inits(*c);
          j = open;
          continue;
        }
      }
      if (k.k == 'i' && !mute && !quiet[j]) {
        std::string name = textOf(s, k);
        const Tok& before = j ? t[j - 1] : t[j];
        bool member = j && (is(s, before, ".") || is(s, before, "?."));
        bool key = j && t[j + 1].k == 'p' && is(s, t[j + 1], ":") && !t[j + 1].sp() && (is(s, before, "{") || is(s, before, ","));
        bool decl = j && (is(s, before, "var") || is(s, before, "fn") || is(s, before, "for"));
        if (member && fields.count(name) && !key && !(is(s, t[j + 1], "(") && !t[j + 1].sp())) {
          size_t r = receiver(j);
          const ClassBody* c = inClass(j);
          bool own = false;
          if (c) for (auto& f : c->fields) own |= f.first == name;
          if (r == j - 2 && is(s, t[r], "self") && !own) { out += pre + name; continue; }  // a plain field of this class
          if (!(r == j - 2 && is(s, t[r], "self"))) {  // anything else's field of that name: a signal only if it holds one
            const Tok& op = t[j + 1];
            if (!assignOp(s, op)) {
              insertAt(r, j, "_state.val(");
              out += pre + name + ")";
              continue;
            }
            std::string lhs = out.substr(at[r]) + pre + name, o = textOf(s, op);
            size_t e = exprEnd(j + 2);
            out += pre + name + s.substr(op.tb, op.b - op.tb) + "= _state.store(" + lhs + ",";
            if (o != "=") out += " _state.val(" + lhs + ") " + o.substr(0, 1) + " (";
            emit(j + 2, e);
            out += o == "=" ? ")" : "))";
            j = e - 1;
            continue;
          }
        }
        if (((member && fields.count(name)) || (!member && (tops.count(name) || localAt(name, j)))) && !key && !decl) {
          out += pre + name;
          const Tok& op = t[j + 1];
          if (assignOp(s, op)) {
            size_t e = exprEnd(j + 2);
            std::string o = textOf(s, op);
            if (o == "=") out += ".set(";
            else out += ".update(fn(_v) { return _v " + o.substr(0, 1) + " (";
            emit(j + 2, e);
            out += o == "=" ? ")" : ") })";
            j = e - 1;
          } else out += ".get()";
          continue;
        }
      }
      if (k.k == 's') { out += pre + interpolate(textOf(s, k), j); continue; }
      out += pre + textOf(s, k);
    }
  }
  std::string interpolate(const std::string& str, size_t at) {  // state read in a string's {holes}
    std::string r;
    for (size_t j = 0; j < str.size(); j++) {
      if (str[j] == '\\') { r += str.substr(j, 2), j++; continue; }
      if (str[j] != '{') { r += str[j]; continue; }
      size_t e = j + 1;
      for (int depth = 1; e < str.size() && depth; e++) {
        if (str[e] == '"') { e = stringEnd(str, e) - 1; continue; }  // a string in the hole: its braces are its own
        depth += str[e] == '{' ? 1 : str[e] == '}' ? -1 : 0;
      }
      std::string inner = str.substr(j + 1, e - j - 2);
      State in(inner);
      in.tops = tops, in.fields = fields;
      for (auto& l : locals) if (at >= l.from && at < l.to) in.tops.insert(l.name);
      in.emit(0, in.t.size());
      r += "{" + in.out + "}";
      j = e - 1;
    }
    return r;
  }
};

bool wanted(const std::string& s) {  // a line starting `view Name` or `state name`
  for (size_t at = 0; at < s.size();) {
    size_t b = s.find_first_not_of(" \t", at);
    if (b == std::string::npos) break;
    for (const char* w : {"view ", "state "})
      if (s.compare(b, strlen(w), w) == 0 && b + strlen(w) < s.size() && idStart(s[b + strlen(w)])) return true;
    at = s.find('\n', b);
    if (at == std::string::npos) break;
    at++;
  }
  return false;
}
}  // namespace

// Rewrites src in place if it declares views or state (a malformed view fails the compile, at its place in path).
void viewsRewrite(std::string& src, const std::string& path) {
  if (!wanted(src)) return;
  Views v(src, path);
  std::string pass1 = v.run();
  State st(pass1);
  st.scan();
  st.emit(0, st.t.size());
  src = std::move(st.out);
  if (!st.tops.empty() || !st.fields.empty()) src = "import \"std/reactive\" as _state " + src;
  if (v.views && src.find("fn reload(") == std::string::npos) src = "fn reload() { ui.refresh() } " + src;  // a patched view shows
  if (v.views && src.find("import \"std/ui\"") == std::string::npos) src = "import \"std/ui\" " + src;
#ifndef __wasm__
  if (getenv("FUT_VIEWS")) fprintf(stderr, "%s\n", src.c_str());  // the rewrite, to see what a view became
#endif
}
