// The host, for programs that talk to the world (native build; all of it is the io effect):
//   fetch(req)        HTTP: req is a url or {url, method, headers, body, timeout}; -> {status, body, headers}, or
//                     {status: 0, error} when nothing came back (a failed request doesn't throw)
//   fetchAll(reqs)    the requests at once (one thread each); -> their results, in order
//   exec(cmd)         a process: cmd is [argv...] or {cmd: [argv...], cwd, input, timeout}; -> {code, out, err, timedOut}
//   execAll(cmds)     the processes at once; -> their results, in order
//   readFile(path)    its text, or nil if there's no such file;  writeFile(path, text) makes the directories it needs
//   env(name)         an environment variable, or nil
// (time(), json() and parseJson() are in rt.cpp: the wasm build has them too)
#include "fut.h"
#include <curl/curl.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

extern char** environ;
int reap(int pid, std::vector<std::pair<int, std::string*>> pipes, double timeout, bool& timedOut);

namespace {
std::string strArg(V v, const char* fn, const char* what) {
  if (!is(v, K::Str)) ERR("%s(): %s must be a string, got %s", fn, what, typeName(v));
  return std::string(as<Str>(v)->sv());
}
double numArg(V v, const char* fn, const char* what) {
  if (!isNum(v) || num(v) <= 0) ERR("%s(): %s must be a positive number", fn, what);
  return num(v);
}
V str(std::string_view s) { return box(newStr(s)); }

// ---- fetch ----
struct Req { std::string url, method = "GET", body; std::vector<std::string> headers; double timeout = 60; bool hasBody = false; };
struct Res { long status = 0; std::string body, error; std::vector<std::pair<std::string, std::string>> headers; };

Req reqOf(V v, const char* fn) {
  Req r;
  if (is(v, K::Str)) return r.url = strArg(v, fn, "the url"), r;
  if (!is(v, K::Map)) ERR("%s() expects a url or {url, method, headers, body, timeout}, got %s", fn, typeName(v));
  for (auto& [k, x] : as<Map>(v)->e) {
    std::string key = strArg(k, fn, "an option name");
    if (key == "url") r.url = strArg(x, fn, "url");
    else if (key == "method") r.method = strArg(x, fn, "method");
    else if (key == "body") r.body = strArg(x, fn, "body"), r.hasBody = true;
    else if (key == "timeout") r.timeout = numArg(x, fn, "timeout");
    else if (key == "headers") {
      if (!is(x, K::Map)) ERR("%s(): headers must be a map of strings", fn);
      for (auto& [hk, hv] : as<Map>(x)->e) r.headers.push_back(strArg(hk, fn, "a header name") + ": " + strArg(hv, fn, "a header value"));
    } else ERR("%s(): unknown option '%s' (url, method, headers, body, timeout)", fn, key.c_str());
  }
  if (r.url.empty()) ERR("%s(): no url", fn);
  return r;
}
size_t onBody(char* p, size_t n, size_t k, void* s) { return ((std::string*)s)->append(p, n * k), n * k; }
size_t onHeader(char* p, size_t n, size_t k, void* s) {
  std::string_view h(p, n * k);
  if (auto c = h.find(':'); c != std::string_view::npos) {
    std::string name(h.substr(0, c));
    for (char& ch : name) ch = (char)tolower((unsigned char)ch);
    std::string_view val = h.substr(c + 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.remove_suffix(1);
    ((Res*)s)->headers.push_back({name, std::string(val)});
  }
  return n * k;
}
void perform(const Req& q, Res& r) {  // on any thread: touches no fut values
  CURL* c = curl_easy_init();
  if (!c) return void(r.error = "could not start a request");
  curl_slist* hs = nullptr;
  for (auto& h : q.headers) hs = curl_slist_append(hs, h.c_str());
  char err[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(c, CURLOPT_URL, q.url.c_str());
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, q.method.c_str());
  if (q.hasBody) curl_easy_setopt(c, CURLOPT_POSTFIELDS, q.body.data()), curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)q.body.size());
  if (q.method == "HEAD") curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody), curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader), curl_easy_setopt(c, CURLOPT_HEADERDATA, &r);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L), curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(q.timeout * 1000)), curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  if (CURLcode rc = curl_easy_perform(c); rc != CURLE_OK) r.error = err[0] ? err : curl_easy_strerror(rc), r.headers.clear();
  else curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  curl_slist_free_all(hs), curl_easy_cleanup(c);
}
V resultOf(const Res& r) {
  Map* m = make<Map>();
  volatile V keep = box(m);
  (void)keep;
  mapPut(m, "status", box((double)r.status));
  if (!r.error.empty()) return mapPut(m, "error", str(r.error)), box(m);
  mapPut(m, "body", str(r.body));
  Map* h = make<Map>();
  mapPut(m, "headers", box(h));
  for (auto& [k, v] : r.headers) mapPut(h, k, str(v));  // a repeated header: the last one
  return box(m);
}
template <class Q, class R, class Parse, class Run, class Out>
V batch(V list, const char* fn, Parse parse, Run run, Out out) {  // each at once, on its own thread
  if (!is(list, K::List)) ERR("%s() expects a list, got %s", fn, typeName(list));
  std::vector<Q> qs;
  for (V v : as<List>(list)->v) qs.push_back(parse(v, fn));
  std::vector<R> rs(qs.size());
  std::vector<std::thread> ts;
  for (size_t i = 0; i < qs.size(); i++) ts.emplace_back([&, i] { run(qs[i], rs[i]); });
  for (auto& t : ts) t.join();
  List* l = make<List>();
  volatile V keep = box(l);
  (void)keep;
  for (auto& r : rs) { V x = out(r); l->v.push_back(x); }
  return box(l);
}
V n_fetch(V req) { Res r; perform(reqOf(req, "fetch"), r); return resultOf(r); }
V n_fetchAll(V reqs) { return batch<Req, Res>(reqs, "fetchAll", reqOf, perform, resultOf); }

// ---- exec ----
struct Cmd { std::vector<std::string> argv; std::string cwd, input; double timeout = 600; bool hasInput = false; };
struct Out { int code = -1; std::string out, err; bool timedOut = false; };

Cmd cmdOf(V v, const char* fn) {
  Cmd c;
  auto argv = [&](V l) {
    if (!is(l, K::List) || as<List>(l)->v.empty()) ERR("%s(): the command must be a non-empty list of strings", fn);
    for (V a : as<List>(l)->v) c.argv.push_back(strArg(a, fn, "each part of the command"));
  };
  if (is(v, K::List)) return argv(v), c;
  if (!is(v, K::Map)) ERR("%s() expects [argv...] or {cmd, cwd, input, timeout}, got %s", fn, typeName(v));
  for (auto& [k, x] : as<Map>(v)->e) {
    std::string key = strArg(k, fn, "an option name");
    if (key == "cmd") argv(x);
    else if (key == "cwd") c.cwd = strArg(x, fn, "cwd");
    else if (key == "input") c.input = strArg(x, fn, "input"), c.hasInput = true;
    else if (key == "timeout") c.timeout = numArg(x, fn, "timeout");
    else ERR("%s(): unknown option '%s' (cmd, cwd, input, timeout)", fn, key.c_str());
  }
  if (c.argv.empty()) ERR("%s(): no cmd", fn);
  return c;
}
void spawnOne(const Cmd& c, Out& o) {  // on any thread
  std::string in = "/dev/null", tmp;
  if (c.hasInput) {
    char t[] = "/tmp/fut-exec-XXXXXX";
    int fd = mkstemp(t);
    if (fd < 0) return void(o.err = "could not write the input");
    bool ok = write(fd, c.input.data(), c.input.size()) == (ssize_t)c.input.size();
    close(fd), in = tmp = t;
    if (!ok) return unlink(t), void(o.err = "could not write the input");
  }
  int po[2], pe[2];
  if (pipe(po)) return void(o.err = "no pipe");
  if (pipe(pe)) return close(po[0]), close(po[1]), void(o.err = "no pipe");
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, in.c_str(), O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, po[1], 1), posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
  for (int fd : {po[0], po[1], pe[0], pe[1]}) posix_spawn_file_actions_addclose(&fa, fd);
  if (!c.cwd.empty()) posix_spawn_file_actions_addchdir(&fa, c.cwd.c_str());
  std::vector<char*> av;
  for (auto& s : c.argv) av.push_back(const_cast<char*>(s.c_str()));
  av.push_back(nullptr);
  pid_t pid;
  int rc = posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(po[1]), close(pe[1]);
  if (rc) {
    close(po[0]), close(pe[0]), o.code = 127, o.err = "could not start '" + c.argv[0] + "': " + strerror(rc);
  } else {
    int st = reap(pid, {{po[0], &o.out}, {pe[0], &o.err}}, c.timeout, o.timedOut);
    o.code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  }
  if (!tmp.empty()) unlink(tmp.c_str());
}
V outOf(const Out& o) {
  Map* m = make<Map>();
  volatile V keep = box(m);
  (void)keep;
  mapPut(m, "code", box((double)o.code)), mapPut(m, "out", str(o.out)), mapPut(m, "err", str(o.err));
  mapPut(m, "timedOut", boxb(o.timedOut));
  return box(m);
}
V n_exec(V cmd) { Out o; spawnOne(cmdOf(cmd, "exec"), o); return outOf(o); }
V n_execAll(V cmds) { return batch<Cmd, Out>(cmds, "execAll", cmdOf, spawnOne, outOf); }

// ---- files, environment ----
V n_readFile(V path) {
  std::string p = strArg(path, "readFile", "the path"), s;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return NIL;
  char b[65536];
  for (size_t k; (k = fread(b, 1, sizeof b, f)) > 0;) s.append(b, k);
  fclose(f);
  return str(s);
}
V n_writeFile(V path, V text) {
  std::string p = strArg(path, "writeFile", "the path"), s = strArg(text, "writeFile", "the text");
  for (size_t i = 1; (i = p.find('/', i)) != std::string::npos; i++) mkdir(p.substr(0, i).c_str(), 0755);
  FILE* f = fopen(p.c_str(), "wb");
  if (!f || fwrite(s.data(), 1, s.size(), f) != s.size()) {
    if (f) fclose(f);
    ERR("writeFile(): could not write '%s': %s", p.c_str(), strerror(errno));
  }
  fclose(f);
  return NIL;
}
V n_env(V name) {
  const char* v = getenv(strArg(name, "env", "the name").c_str());
  return v ? str(v) : NIL;
}
}  // namespace

void sysInit() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  native<n_fetch>("fetch"), native<n_fetchAll>("fetchAll"), native<n_exec>("exec"), native<n_execAll>("execAll");
  native<n_readFile>("readFile"), native<n_writeFile>("writeFile"), native<n_env>("env");
}
