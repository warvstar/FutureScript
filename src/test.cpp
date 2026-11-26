// Tests: the expect@ native behind `expect`, and the runner behind `fut test` (shared by the native and wasm drivers).
#include "fut.h"

static std::vector<ExpectSite> sites;
int expectSite(ExpectSite s) { return sites.push_back(std::move(s)), (int)sites.size() - 1; }

// The first expect that failed in the running test. Kept even if the test catches the error expect throws.
struct Failure { bool set = false; int site = -1; std::string why, left, right, at, what; };  // at/what: where lists or maps differ

// expect's == compares lists and maps by contents (key order aside). On a mismatch inside them, `at` gets the
// path to the first difference and what differs there, e.g. `.items[1].qty: 3 vs 4`.
static std::string show(V v) {  // a value as source would spell it: strings quoted, newlines and tabs escaped
  std::string o;
  for (char c : toStr(v, true)) o += c == '\n' ? "\\n" : c == '\t' ? "\\t" : std::string(1, c);
  return o;
}
static std::string key(V k) {
  if (is(k, K::Str)) {
    auto s = as<Str>(k)->sv();
    bool name = !s.empty() && !(s[0] >= '0' && s[0] <= '9');
    for (char c : s) name &= (c | 32) >= 'a' && (c | 32) <= 'z' ? true : c == '_' || (c >= '0' && c <= '9');
    if (name) return "." + std::string(s);
  }
  return "[" + toStr(k, true) + "]";
}
static bool same(V a, V b, std::string& at, std::string& what) {
  if (isNum(a) && isNum(b)) return num(a) == num(b);
  if (is(a, K::Str) && is(b, K::Str)) return as<Str>(a)->sv() == as<Str>(b)->sv();
  auto differ = [&](std::string seg, V x, V y) {
    if (what.empty()) what = show(x) + " vs " + show(y);
    return at = seg + at, false;
  };
  if (is(a, K::List) && is(b, K::List)) {
    auto &x = as<List>(a)->v, &y = as<List>(b)->v;
    for (size_t i = 0; i < x.size() && i < y.size(); i++)
      if (!same(x[i], y[i], at, what)) return differ("[" + std::to_string(i) + "]", x[i], y[i]);
    if (x.size() != y.size()) return what = "length " + std::to_string(x.size()) + " vs " + std::to_string(y.size()), false;
    return true;
  }
  if (is(a, K::Map) && is(b, K::Map)) {
    auto &x = as<Map>(a)->e, &y = as<Map>(b)->e;
    auto find = [](auto& e, V k) -> V* {
      std::string i1, i2;
      for (auto& [k2, v] : e) if (same(k, k2, i1, i2)) return &v;
      return nullptr;
    };
    for (auto& [k, v] : x) {
      V* w = find(y, k);
      if (!w) return what = "only on the left", at = key(k), false;
      if (!same(v, *w, at, what)) return differ(key(k), v, *w);
    }
    for (auto& [k, v] : y) if (!find(x, k)) return what = "only on the right", at = key(k), false;
    return true;
  }
  return a == b;
}
static Failure failure;

static std::string oneLine(std::string s) {  // an expression's source on one line: a line break and its indent become one space
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\n' && s[i] != '\r') { o += s[i]; continue; }
    while (!o.empty() && (o.back() == ' ' || o.back() == '\t')) o.pop_back();
    while (i + 1 < s.size() && (s[i + 1] == ' ' || s[i + 1] == '\t' || s[i + 1] == '\n' || s[i + 1] == '\r')) i++;
    o += ' ';
  }
  return o;
}
// "x is 10" lines worth showing: a side whose source is just its value (a literal) says nothing new.
static std::vector<std::string> details(const ExpectSite& s, const Failure& f) {
  std::vector<std::string> d;
  if (!f.why.empty()) d.push_back(f.why);
  auto bare = [](const std::string& x) { std::string o; for (char c : x) if (c != ' ' && c != '\n' && c != '\t' && c != '"') o += c; return o; };
  auto side = [&](const std::string& src, const std::string& val) { if (!src.empty() && bare(src) != bare(val)) d.push_back(oneLine(src) + " is " + val); };
  bool whole = s.contract && s.op < 0 && s.left.find(' ') != std::string::npos;  // `a || b is false` says nothing new
  if (f.why.empty() && !whole) side(s.left, f.left);
  if (s.op >= 0 && f.why.empty()) side(s.right, f.right);
  if (!f.what.empty()) d.push_back(f.at.empty() ? f.what : "first difference at " + f.at + ": " + f.what);
  return d;
}

// expect@(site, left, right): checks left op right (or left's truthiness); a failure is recorded and raised.
static V expectAt(V site, V l, V r) {
  const ExpectSite& s = sites.at((size_t)num(site));
  Failure f;
  bool ok;
  if (s.contract) ok = false;  // compiled code found it false, and calls only to report it
  else if (s.throws && l == ABSENT) ok = false, f.why = "nothing was thrown";
  else if (s.op < 0) ok = s.throws || (l != NIL && l != FALSE_);
  else if (s.op == O_EQ || s.op == O_NE) {
    ok = same(l, r, f.at, f.what) == (s.op == O_EQ);
    if (ok || s.op == O_NE) f.at.clear(), f.what.clear();
  } else if (isNum(l) && isNum(r)) {  // compiled code compares numbers inline; binopValue expects not to see them
    double x = num(l), y = num(r);
    ok = s.op == O_LT ? x < y : s.op == O_LE ? x <= y : s.op == O_GT ? x > y : x >= y;
  } else { V c = binopValue(l, r, s.op); ok = c != NIL && c != FALSE_; }
  if (ok) return NIL;
  f.set = true, f.site = (int)num(site);
  if (l != ABSENT) f.left = show(l);
  if (s.op >= 0) f.right = show(r);
  std::string msg = (s.contract ? s.head : "expect failed: ") + oneLine(s.text) + s.tail;
  auto d = details(s, f);
  for (size_t i = 0; i < d.size(); i++) msg += (i ? ", " : " (") + d[i] + (i + 1 == d.size() ? ")" : "");
  if (!failure.set && !s.contract) failure = f;
  ERR("%s", msg.c_str());
}
// expect prints { ... }: print() output inside the block is collected (nested captures stack) and becomes the value.
struct Capture { std::string* prev; std::string buf; };
static std::vector<Capture*> captures;
static void endCaptures() {  // a test that returned or crashed out of a capture
  for (; !captures.empty(); captures.pop_back()) outSink = captures.back()->prev, delete captures.back();
}
static std::string popCapture() {
  if (captures.empty()) return "";
  Capture* c = captures.back();
  std::string s = std::move(c->buf);
  outSink = c->prev, captures.pop_back(), delete c;
  return s;
}
static V captureBegin() { captures.push_back(new Capture{outSink, {}}), outSink = &captures.back()->buf; return NIL; }
static V captureEnd() { return box(newStr(popCapture())); }
static V captureAbort(V e) { popCapture(), throwValue(e, RA()); }  // the block threw: stop capturing, rethrow

// run(file) or run({file, source, args, timeout}): run fut as a child process; {code, out, err, timedOut}.
// A relative file is found next to the program being run; source is written to a temporary file.
static V n_run(V o) {
  std::string file, src;
  bool hasSrc = false;
  std::vector<std::string> args;
  double timeout = 10;
  auto strOf = [](V v, const char* what) {
    if (!is(v, K::Str)) ERR("run(): %s must be a string, got %s", what, typeName(v));
    return std::string(as<Str>(v)->sv());
  };
  if (is(o, K::Str)) file = strOf(o, "the file");
  else if (is(o, K::Map)) {
    for (auto& [k, v] : as<Map>(o)->e) {
      std::string key = strOf(k, "an option name");
      if (key == "file") file = strOf(v, "file");
      else if (key == "source") src = strOf(v, "source"), hasSrc = true;
      else if (key == "timeout") timeout = isNum(v) && num(v) > 0 ? num(v) : (ERR("run(): timeout must be a positive number"), 0.0);
      else if (key == "args" && is(v, K::List)) for (V a : as<List>(v)->v) args.push_back(strOf(a, "each of args"));
      else ERR("run(): unknown option '%s' (file, source, args, timeout)", key.c_str());
    }
  } else ERR("run() expects a file name or {file, source, args, timeout}, got %s", typeName(o));
  if (hasSrc == !file.empty()) ERR("run(): give exactly one of file and source");
  if (!file.empty() && file[0] != '/') file = canonicalPath(mainFile.substr(0, mainFile.rfind('/') + 1) + file);
  ProcResult r;
  if (!runProcess(args, file, hasSrc ? &src : nullptr, timeout, r)) ERR("run(): could not start fut");
  Map* m = make<Map>();
  volatile V keep = box(m);  // on the stack while the strings below allocate
  (void)keep;
  mapPut(m, "code", box((double)r.code)), mapPut(m, "out", box(newStr(r.out))), mapPut(m, "err", box(newStr(r.err)));
  mapPut(m, "timedOut", boxb(r.timedOut));
  return box(m);
}
void testInit() {
  native<expectAt>("expect@"), native<captureBegin>("capture@begin"), native<captureEnd>("capture@end");
  native<captureAbort>("capture@abort"), native<n_run>("run");
}

// ---------------- the runner ----------------
static TestRun opts;
static int passed, failed, errored, pending;
static std::string out;  // the running test's print() output
static double t0, tAll;
static std::string json(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += (char)c;
    else if (c == '\n') o += "\\n";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
static std::string indent(const std::string& s, const char* pre) {
  std::string o;
  for (size_t i = 0; i < s.size();) {
    size_t j = s.find('\n', i);
    if (j == std::string::npos) j = s.size();
    o += pre + s.substr(i, j - i) + "\n", i = j + 1;
  }
  return o;
}
static std::string where(const TestCase& t) { return t.file.substr(t.file.rfind('/') + 1) + ":" + std::to_string(t.line); }
static void emit(const std::string& s) { fflush(stdout), fwrite(s.data(), 1, s.size(), stdout), fflush(stdout); }

void testsBegin(const TestRun& o) { opts = o, passed = failed = errored = pending = 0, tAll = now(); }

void* testStart(const TestCase& t) {
  if (!opts.filter.empty() && t.name.find(opts.filter) == std::string::npos) return nullptr;
  if (!t.pending.empty()) {
    pending++;
    if (opts.json) emit("{\"type\":\"test\",\"name\":" + json(t.name) + ",\"file\":" + json(t.file) + ",\"line\":" + std::to_string(t.line) +
                        ",\"status\":\"pending\",\"reason\":" + json(t.pending) + "}\n");
    else emit("pending " + t.name + " (" + where(t) + "): " + t.pending + "\n");
    return nullptr;
  }
  failure = {}, out.clear(), endCaptures(), outSink = &out;
  t0 = now();
  return t.code;
}

std::string testReport(const TestCase& t, bool err, const std::string& e, char& code) {
  outSink = nullptr, endCaptures();
  double ms = (now() - t0) * 1e3;
  const char* status = failure.set ? "fail" : err ? "error" : "pass";
  code = status[0];
  if (opts.json) {
    std::string j = "{\"type\":\"test\",\"name\":" + json(t.name) + ",\"file\":" + json(t.file) + ",\"line\":" + std::to_string(t.line) +
                    ",\"status\":\"" + status + "\"";
    char b[32];
    snprintf(b, sizeof b, "%.3f", ms), j += ",\"ms\":" + std::string(b);
    if (failure.set) {
      const ExpectSite& s = sites[failure.site];
      j += ",\"expect\":{\"file\":" + json(s.file) + ",\"line\":" + std::to_string(s.line) + ",\"expr\":" + json(oneLine(s.text));
      if (!failure.why.empty()) j += ",\"reason\":" + json(failure.why);
      if (!failure.left.empty()) j += ",\"left\":{\"expr\":" + json(oneLine(s.left)) + ",\"value\":" + json(failure.left) + "}";
      if (s.op >= 0) j += ",\"right\":{\"expr\":" + json(oneLine(s.right)) + ",\"value\":" + json(failure.right) + "}";
      if (!failure.what.empty()) j += ",\"diff\":{\"at\":" + json(failure.at) + ",\"what\":" + json(failure.what) + "}";
      j += "}";
    } else if (err) {
      size_t nl = e.find('\n');
      j += ",\"message\":" + json(e.substr(0, nl)) + ",\"trace\":" + json(nl == std::string::npos ? "" : e.substr(nl + 1));
    }
    return j + ",\"output\":" + json(out) + "}\n";
  }
  std::string s = std::string(failure.set ? "FAIL    " : err ? "ERROR   " : "ok      ") + t.name;
  if (failure.set) {
    const ExpectSite& x = sites[failure.site];
    s += " (" + where(t) + ")\n  " + x.file.substr(x.file.rfind('/') + 1) + ":" + std::to_string(x.line) + ": expect " + oneLine(x.text) + "\n";
    for (auto& d : details(x, failure)) s += "    " + d + "\n";
  } else if (err) s += " (" + where(t) + ")\n" + indent("runtime error: " + e.substr(0, e.find_last_not_of('\n') + 1), "  ");
  else s += "\n";
  if ((failure.set || err) && !out.empty()) s += "  output:\n" + indent(out.substr(0, out.size() - (out.back() == '\n')), "    ");
  return s;
}
void testRecord(char code, const std::string& text) { outSink = nullptr, (code == 'f' ? failed : code == 'e' ? errored : passed)++, emit(text); }
void testFinish(const TestCase& t, bool err, const std::string& e) {
  char code;
  std::string text = testReport(t, err, e, code);
  testRecord(code, text);
}

int testsEnd() {
  double ms = (now() - tAll) * 1e3;
  char b[256];
  if (opts.json) snprintf(b, sizeof b, "{\"type\":\"summary\",\"passed\":%d,\"failed\":%d,\"errors\":%d,\"pending\":%d,\"ms\":%.3f}\n",
                          passed, failed, errored, pending, ms);
  else {
    std::string parts;
    for (auto [n, w] : {std::pair{failed, "failed"}, {errored, errored == 1 ? "error" : "errors"}, {pending, "pending"}})
      if (n) parts += ", " + std::to_string(n) + " " + w;
    int total = passed + failed + errored + pending;
    snprintf(b, sizeof b, "%d test%s: %d passed%s (%.1f ms)\n", total, total == 1 ? "" : "s", passed, parts.c_str(), ms);
  }
  emit(b);
  return failed || errored || pending ? 1 : 0;
}

// "path:line:col: error: message\n  source\n  ^" -> its parts, for --json
void reportCompileError(const std::string& m) {
  if (!opts.json) return (void)(fflush(stdout), fprintf(stderr, "%s\n", m.c_str()));
  std::string head = m.substr(0, m.find('\n')), j = "{\"type\":\"compile_error\"";
  size_t e = head.find(": error: ");
  int line = 0, col = 0;
  if (e != std::string::npos) {
    size_t c2 = head.rfind(':', e - 1), c1 = c2 == std::string::npos || !c2 ? std::string::npos : head.rfind(':', c2 - 1);
    if (c1 != std::string::npos && sscanf(head.c_str() + c1, ":%d:%d", &line, &col) == 2)
      j += ",\"file\":" + json(head.substr(0, c1)) + ",\"line\":" + std::to_string(line) + ",\"col\":" + std::to_string(col);
    head = head.substr(e + 9);
  }
  emit(j + ",\"message\":" + json(head) + ",\"text\":" + json(m) + "}\n");
}
void reportTopError(const std::string& m) {
  if (!opts.json) return (void)(fflush(stdout), fprintf(stderr, "runtime error: %s", m.c_str()));
  size_t nl = m.find('\n');
  emit("{\"type\":\"error\",\"message\":" + json(m.substr(0, nl)) + ",\"trace\":" + json(nl == std::string::npos ? "" : m.substr(nl + 1)) + "}\n");
}
