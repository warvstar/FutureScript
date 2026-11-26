// Views and state, for now as a rewrite of the source before it compiles (two syntaxes on trial; one will stay and be
// compiled directly). A file that declares `view` or `state` is rewritten into std/ui calls:
//
//   state n = 0                     var n = std/reactive signal(0); reading n is n.get(), n = v is n.set(v), n += v an update
//   class T { state done }          a field holding a signal, made in init; t.done reads it, t.done = v sets it
//   view Row(t) { ...body... }      fn Row(t), returning the nodes its body makes; mount(Page) shows Page
//
// A body is blocks (`row .task .done: t.done { check "x" bind: t.done }`) or markup (`<div .task .done={t.done}>`),
// chosen by its first character. Any expression in a view is a function of the state it reads: only that part of
// the page changes when the state does. The rewrite keeps every line where it was, so errors point at the source.
#include "fut.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
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
  if (c == '"') {
    for (i++; i < s.size() && s[i] != '"'; i++) if (s[i] == '\\') i++;
    return t.k = 's', t.e = std::min(i + 1, s.size()), t;
  }
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
  std::map<std::string, std::vector<std::string>> params;  // each view's parameters, for <Row t={t}/>
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
    int depth = 0;
    bool value = false;
    size_t at = from;
    for (bool first = true;; first = false) {
      Tok t = peek(at);
      if (!t.k) return at;
      if (depth == 0) {
        if (!first && t.nl) return at;
        if (closer(s, t) || is(s, t, ",")) return at;
        if (stopAtBrace && is(s, t, "{")) return at;
        if (!first && is(s, t, ".") && t.sp()) return at;
        if (!first && value && (t.k == 'i' || t.k == 'n' || t.k == 's' || t.k == 'm')) return at;
        if (stopAtKey && t.k == 'i' && is(s, t, "key")) return at;
      }
      if (opener(s, t)) depth++;
      else if (closer(s, t)) depth--;
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

  // ---- blocks ----
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
      if (word == "var") {  // a local: var x = e
        take(), out += "var";
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
      out += expr(false);  // a statement: x = 1, f(x)
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
  void ifBlock(int n) {
    std::string k = "_k" + std::to_string(n), r = k + ".push(fn() {";
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
    out += r + " return nil })";
  }
  void forBlock(int n) {
    take();  // for
    Tok v = take();
    if (v.k != 'i') fail(v.b, "expected a name after 'for'");
    std::string var = textOf(s, v);
    if (Tok in = take(); !is(s, in, "in")) fail(in.b, "expected 'in'");
    std::string saved = std::exchange(out, "");
    std::string list = expr(true, true), key = var;
    if (Tok kw = peek(); is(s, kw, "key")) take(), key = expr();
    std::string lead = std::exchange(out, saved);
    out += lead + "_k" + std::to_string(n) + ".push(ui.each(fn() { return " + list + " }, fn(" + var + ") { return " + key +
           " }, fn(" + var + ") { return " + block(n + 1) + " }))";
  }
  // tag .class .class: cond name: value on event { ... } content... { children }
  void element(int n) {
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
        std::string c = markName();  // a class: dashes allowed (.bed-title)
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

  // ---- markup ----
  void ws(std::string& debt) {  // spaces and line breaks between markup parts: the breaks are kept for after it
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) debt += s[i++] == '\n' ? "\n" : "";
  }
  std::string markName() {
    size_t b = i;
    while (i < s.size() && (idChar(s[i]) || s[i] == '-')) i++;
    if (b == i) fail(b, "expected a name");
    return s.substr(b, i - b);
  }
  std::string braced(std::string& debt) {  // {expr}: its text, the braces consumed
    if (s[i] != '{') fail(i, "expected '{'");
    i++;
    std::string body = braceBody();
    debt += lines(body);
    for (char& c : body) if (c == '\n') c = ' ';
    return body;
  }
  std::string markup(std::string& debt) {  // at '<'
    size_t at = i++;
    std::string tag = markName();
    std::vector<std::string> props, fixed, args;
    std::vector<std::pair<std::string, std::string>> when;
    bool view = tag[0] >= 'A' && tag[0] <= 'Z';
    std::map<std::string, std::string> named;
    for (;;) {
      ws(debt);
      if (i >= s.size()) fail(at, "<" + tag + "> is not closed");
      if (s.compare(i, 2, "/>") == 0) { i += 2; return finish(tag, props, fixed, when, named, {}, at); }
      if (s[i] == '>') { i++; break; }
      if (s[i] == '.') {
        i++;
        std::string c = markName();
        if (i < s.size() && s[i] == '=') i++, when.push_back({c, braced(debt)});
        else fixed.push_back(c);
        continue;
      }
      std::string a = markName();
      if (a == "on" && i < s.size() && s[i] == ':') i++, a = "on" + markName();
      if (i >= s.size() || s[i] != '=') fail(i, "expected '=' after " + a);
      i++;
      std::string v;
      if (s[i] == '"') { Tok t = lexAt(s, i); v = textOf(s, t), i = t.e; }
      else v = braced(debt);
      if (view) named[a] = v;
      else if (a == "class") when.push_back({"=" + v, "true"});
      else if (a == "bind") props.push_back("\"bind\": \x01" + v);
      else if (a.compare(0, 2, "on") == 0 && a.size() > 2) props.push_back(quote(a) + ": fn(e) { " + v + " }");
      else props.push_back(quote(a) + ": " + live(v));
    }
    std::vector<std::string> kids = markKids(debt, "</");
    i += 2;
    if (std::string close = markName(); close != tag) fail(i, "<" + tag + "> is closed by </" + close + ">");
    ws(debt);
    if (s[i] != '>') fail(i, "expected '>'");
    i++;
    return finish(tag, props, fixed, when, named, kids, at);
  }
  std::string finish(const std::string& tag, std::vector<std::string>& props, const std::vector<std::string>& fixed,
                     const std::vector<std::pair<std::string, std::string>>& when, std::map<std::string, std::string>& named,
                     const std::vector<std::string>& kids, size_t at) {
    if (tag[0] >= 'A' && tag[0] <= 'Z') {
      std::vector<std::string> args;
      if (auto p = params.find(tag); p != params.end()) {
        for (auto& n : p->second) args.push_back(named.count(n) ? named[n] : "nil"), named.erase(n);
        if (!named.empty()) fail(at, tag + " has no parameter '" + named.begin()->first + "'");
      } else for (auto& [_, v] : named) args.push_back(v);
      return tag + "(" + join(args) + ")";
    }
    if (!fixed.empty() || !when.empty()) props.insert(props.begin(), classProp(fixed, when));
    return "ui.make(" + quote(tag) + ", {" + join(props) + "}, [" + join(kids) + "])";
  }
  // Children up to `until` ("</" or "}"), not consumed: text, {holes} and elements. Text and simple holes next to
  // each other make one interpolated string; whitespace with a line break in it is one space, or none at the ends.
  std::vector<std::string> markKids(std::string& debt, const char* until) {
    std::vector<std::string> kids;
    std::string run;
    bool hole = false, space = false;
    auto flush = [&] {
      if (!run.empty()) kids.push_back(hole ? "fn() { return \"" + run + "\" }" : "\"" + run + "\"");
      run.clear(), hole = space = false;
    };
    auto put = [&](const std::string& x) { if (space && !run.empty()) run += ' '; space = false, run += x; };
    for (;;) {
      if (i >= s.size()) fail(i, std::string("expected ") + until);
      if (s.compare(i, strlen(until), until) == 0) { flush(); return kids; }
      char c = s[i];
      if (c == '<') { flush(), kids.push_back(markup(debt)); continue; }
      if (c == '{') {
        size_t j = i + 1;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\n' || s[j] == '\t')) j++;
        Tok w = lexAt(s, j);
        if (is(s, w, "if") || is(s, w, "for") || s[j] == '<') {
          flush();
          i = j;
          kids.push_back(is(s, w, "if") ? markIf(debt) : is(s, w, "for") ? markFor(debt) : markup(debt));
          ws(debt);
          if (s[i] != '}') fail(i, "expected '}'");
          i++;
          continue;
        }
        std::string e = braced(debt);
        if (e.find('"') != std::string::npos) flush(), kids.push_back("fn() { return " + e + " }");
        else put("{" + e + "}"), hole = true;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        size_t b = i;
        std::string nl;
        ws(nl);
        debt += nl;
        if (!nl.empty()) space = true;
        else if (!run.empty()) put(s.substr(b, i - b));
        continue;
      }
      put(c == '"' ? "\\\"" : c == '\\' ? "\\\\" : std::string(1, c)), i++;
    }
  }
  std::string markBranch(std::string& debt) {  // { markup... } as a list
    ws(debt);
    if (s[i] != '{') fail(i, "expected '{'");
    i++;
    std::vector<std::string> kids = markKids(debt, "}");
    i++;
    return "[" + join(kids) + "]";
  }
  std::string markCond(std::string& debt, bool stopAtKey) {
    std::string saved = std::exchange(out, "");
    std::string e = expr(true, stopAtKey);
    debt += lines(std::exchange(out, saved)) + lines(e);
    for (char& c : e) if (c == '\n') c = ' ';
    return e;
  }
  std::string markIf(std::string& debt) {
    std::string r = "fn() {";
    for (bool first = true;; first = false) {
      i = take().e;  // if
      std::string cond = markCond(debt, false);
      r += std::string(first ? " if " : " else if ") + cond + " { return " + markBranch(debt) + " }";
      Tok e = peek();
      if (!is(s, e, "else")) break;
      debt += lines(s.substr(e.tb, e.b - e.tb)), i = e.e;
      if (Tok w = peek(); is(s, w, "if")) { debt += lines(s.substr(w.tb, w.b - w.tb)), i = w.b; continue; }
      r += " else { return " + markBranch(debt) + " }";
      break;
    }
    return r + " return nil }";
  }
  std::string markFor(std::string& debt) {
    take();
    Tok v = take();
    if (v.k != 'i') fail(v.b, "expected a name after 'for'");
    std::string var = textOf(s, v);
    if (Tok in = take(); !is(s, in, "in")) fail(in.b, "expected 'in'");
    std::string list = markCond(debt, true), key = var;
    if (Tok kw = peek(); is(s, kw, "key")) take(), key = markCond(debt, false);
    return "ui.each(fn() { return " + list + " }, fn(" + var + ") { return " + key + " }, fn(" + var + ") { return " +
           markBranch(debt) + " })";
  }

  // ---- the file ----
  std::string run() {
    for (size_t at = 0;;) {  // the views' parameters first, for <View a={x}/> anywhere
      Tok t = peek(at);
      if (!t.k) break;
      if (is(s, t, "view") && (t.nl || t.b == 0 || t.tb == 0)) {
        Tok n = peek(t.e), p = peek(n.e);
        if (n.k == 'i' && is(s, p, "(")) {
          auto& ps = params[textOf(s, n)];
          for (Tok a = peek(p.e); a.k && !is(s, a, ")"); a = peek(a.e)) if (a.k == 'i') ps.push_back(textOf(s, a));
        }
      }
      at = t.e;
    }
    bool ownMount = s.find("fn mount(") != std::string::npos;
    int depth = 0;
    Tok last;
    for (;;) {
      Tok t = peek();
      if (!t.k) { out += s.substr(t.tb); break; }
      if (depth == 0 && is(s, t, "view") && (t.nl || t.tb == 0) && peek(t.e).k == 'i') { view(t); last = Tok{}; continue; }
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
    Tok first = peek();
    if (is(s, first, "<")) {
      out += s.substr(first.tb, first.b - first.tb) + " return ", i = first.b;
      std::string debt, m = markup(debt);
      out += m + debt;
      Tok c = take();
      if (!is(s, c, "}")) fail(c.b, "a view in markup is one element: wrap several in one");
      out += s.substr(c.tb, c.e - c.tb);
      return;
    }
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
    std::vector<size_t> encl(t.size(), SIZE_MAX);  // each token's innermost enclosing `{`
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
      else if (depth == 1 && !body.empty() && j > body.back() && j < match[body.back()]) {
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
  const ClassBody* inClass(size_t j) const {
    for (auto& [b, c] : classes) if (j > b && j < match[b]) return &c;
    return nullptr;
  }
  std::string inits(const ClassBody& c) {
    std::string r;
    for (auto& [f, d] : c.fields) r += " self." + f + " = _state.signal(" + d + ")";
    return r;
  }
  void emit(size_t from, size_t to) {
    bool mute = false;  // after the mark: a bound signal, not its value
    for (size_t j = from; j < to; j++) {
      const Tok& k = t[j];
      std::string pre = s.substr(k.tb, k.b - k.tb);
      if (k.k == 'm') { out += pre, mute = true; continue; }
      if (mute && !(k.k == 'i' || is(s, k, "."))) mute = false;
      if (!k.k) { out += pre; continue; }
      if (is(s, k, "state") && j + 1 < t.size() && t[j + 1].k == 'i' && !t[j + 1].nl) {
        std::string name = textOf(s, t[j + 1]);
        const ClassBody* in = inClass(j);
        if (in && fields.count(name)) {  // a field: made in init
          size_t e = j + 2 + (is(s, t[j + 2], "=") ? 2 : 0);
          std::string gone;
          for (size_t q = j + 1; q < e; q++) gone += s.substr(t[q].tb, t[q].b - t[q].tb);
          out += pre + lines(gone);
          j = e - 1;
          continue;
        }
        if (!in && tops.count(name)) {
          out += pre + "var " + name + " = _state.signal(";
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
        if (((member && fields.count(name)) || (!member && tops.count(name))) && !key && !decl) {
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
      if (k.k == 's') { out += pre + interpolate(textOf(s, k)); continue; }
      out += pre + textOf(s, k);
    }
  }
  std::string interpolate(const std::string& str) {  // state read in a string's {holes}
    std::string r;
    for (size_t j = 0; j < str.size(); j++) {
      if (str[j] == '\\') { r += str.substr(j, 2), j++; continue; }
      if (str[j] != '{') { r += str[j]; continue; }
      size_t e = j + 1;
      for (int depth = 1; e < str.size() && depth; e++) depth += str[e] == '{' ? 1 : str[e] == '}' ? -1 : 0;
      std::string inner = str.substr(j + 1, e - j - 2);
      State in(inner);
      in.tops = tops, in.fields = fields;
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
  if (v.views && src.find("import \"std/ui\"") == std::string::npos) src = "import \"std/ui\" " + src;
#ifndef __wasm__
  if (getenv("FUT_VIEWS")) fprintf(stderr, "%s\n", src.c_str());  // the rewrite, to see what a view became
#endif
}
