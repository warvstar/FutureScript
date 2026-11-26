// The host, for programs that talk to the world (native build; all of it is the io effect):
//   fetch(req)        HTTP: req is a url or {url, method, headers, body, timeout, idleTimeout}; -> {status, body,
//                     headers}, or {status: 0, error} when nothing came back (a failed request doesn't throw)
//   fetchAll(reqs)    the requests at once (one thread each); -> their results, in order
//   exec(cmd)         a process: cmd is [argv...] or {cmd: [argv...], cwd, input, timeout}; -> {code, out, err, timedOut}
//   execAll(cmds)     the processes at once; -> their results, in order
//   readFile(path)    its text, or nil if there's no such file;  writeFile(path, text) makes the directories it needs
//   env(name)         an environment variable, or nil
// (time(), json() and parseJson() are in rt.cpp: the wasm build has them too)
#include "fut.h"
#include <curl/curl.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

extern char** environ;
int reap(int pid, std::vector<std::pair<int, std::string*>> pipes, double timeout, bool& timedOut);

namespace {
std::string strArg(V v, const char* fn, const char* what) { return std::string(strViewArg(v, fn, what)); }
V str(std::string_view s) { return box(newStr(s)); }

// ---- fetch ----
struct Res {
  long status = 0;
  std::string body, error;
  std::vector<std::pair<std::string, std::string>> headers;
  double last = 0;    // when the transfer last moved a byte (request bytes sent, a header or body byte received)
  curl_off_t up = 0;  // request bytes sent so far
};

size_t onBody(char* p, size_t n, size_t k, void* s) { return ((Res*)s)->body.append(p, n * k), ((Res*)s)->last = now(), n * k; }
size_t onHeader(char* p, size_t n, size_t k, void* s) {
  ((Res*)s)->last = now();
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
// idleTimeout: libcurl calls this about once a second while it waits, and more often while data moves. Any byte
// counts as progress, in either direction, headers included; so a server that sends nothing until its whole answer
// is ready looks idle meanwhile (that is what the option measures: pick a limit above its think time, or stream).
// CURLOPT_LOW_SPEED_LIMIT/TIME measures much the same, but averages the speed over a few seconds and takes whole
// seconds.
struct Idle { Res* r; double limit; bool hit = false; };
int onProgress(void* p, curl_off_t, curl_off_t, curl_off_t, curl_off_t ulnow) {
  Idle& i = *(Idle*)p;
  double t = now();
  if (ulnow != i.r->up) i.r->up = ulnow, i.r->last = t;
  return t - i.r->last > i.limit ? (i.hit = true, 1) : 0;
}
// Dead peers (a host gone, a NAT entry dropped): TCP keepalive on every connection. The peer's kernel answers the
// probes however long its server takes, so this never cuts off a slow answer, only a connection nobody holds: idle
// 30 s, then 3 probes 10 s apart, about a minute in all. (libcurl sends no HTTP/2 PING during a transfer; and a
// proxy that holds the connection while its upstream is gone answers both, which only idleTimeout can catch.)
int onSocket(void*, curl_socket_t fd, curlsocktype) {
#ifdef TCP_KEEPCNT  // CURLOPT_TCP_KEEPCNT is libcurl 8.9+
  int n = 3;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &n, sizeof n);
#endif
  return CURL_SOCKOPT_OK;
}
void perform(const FetchRequest& q, Res& r) {  // on any thread: reads the calling isolate's request while it waits
  CURL* c = curl_easy_init();
  if (!c) return void(r.error = "could not start a request");
  curl_slist* hs = nullptr;
  if (q.headers) for (const auto& [k, v] : q.headers->e) {
    std::string h = std::string(as<Str>(k)->sv()) + ": ";
    h += as<Str>(v)->sv();
    hs = curl_slist_append(hs, h.c_str());
  }
  char err[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(c, CURLOPT_URL, q.url.data());
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, q.method.data());
  if (q.body.data()) curl_easy_setopt(c, CURLOPT_POSTFIELDS, q.body.data()), curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)q.body.size());
  if (q.method == "HEAD") curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onBody), curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader), curl_easy_setopt(c, CURLOPT_HEADERDATA, &r);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L), curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(q.timeout * 1000)), curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L), curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE, 30L), curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL, 10L);
  curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, onSocket);
  Idle idle{&r, q.idleTimeout};
  if (q.idleTimeout > 0) {
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress), curl_easy_setopt(c, CURLOPT_XFERINFODATA, &idle);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  }
  r.last = now();
  if (CURLcode rc = curl_easy_perform(c); rc != CURLE_OK) {
    if (idle.hit) snprintf(err, sizeof err, "idle timeout: no data for %g s (idleTimeout)", q.idleTimeout);
    r.error = err[0] ? err : curl_easy_strerror(rc), r.headers.clear();
  }
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
  if (!is(list, K::List)) ERR("@argument %s() expects a list, got %s", fn, typeName(list));
  volatile V requests = list;  // parsed requests can borrow strings from the list
  (void)requests;
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
V n_fetch(V req) {
  Res r;
  perform(fetchRequest(req, "fetch"), r);
  return resultOf(r);
}
V n_fetchAll(V reqs) { return batch<FetchRequest, Res>(reqs, "fetchAll", fetchRequest, perform, resultOf); }

// ---- exec ----
struct Cmd { std::vector<std::string> argv; std::string cwd, input; double timeout = 600; bool hasInput = false; };

Cmd cmdOf(V v, const char* fn) {
  Cmd c;
  auto argv = [&](V l) {
    if (!is(l, K::List) || as<List>(l)->v.empty()) ERR("@argument %s(): the command must be a non-empty list of strings", fn);
    for (V a : as<List>(l)->v) c.argv.push_back(strArg(a, fn, "each part of the command"));
  };
  if (is(v, K::List)) return argv(v), c;
  if (!is(v, K::Map)) ERR("@argument %s() expects [argv...] or {cmd, cwd, input, timeout}, got %s", fn, typeName(v));
  for (auto& [k, x] : as<Map>(v)->e) {
    std::string key = strArg(k, fn, "an option name");
    if (key == "cmd") argv(x);
    else if (key == "cwd") c.cwd = strArg(x, fn, "cwd");
    else if (key == "input") c.input = strArg(x, fn, "input"), c.hasInput = true;
    else if (key == "timeout") c.timeout = numArg(x, fn, "timeout");
    else ERR("@argument %s(): unknown option '%s' (cmd, cwd, input, timeout)", fn, key.c_str());
  }
  if (c.argv.empty()) ERR("@argument %s(): no cmd", fn);
  return c;
}
bool spawnOne(const Cmd& c, ProcResult& o, bool searchPath = true) {  // on any thread
  o.code = -1;
  std::string in = "/dev/null", tmp;
  if (c.hasInput) {
    char t[] = "/tmp/fut-exec-XXXXXX";
    int fd = mkstemp(t);
    if (fd < 0) return o.err = "could not write the input", false;
    bool ok = write(fd, c.input.data(), c.input.size()) == (ssize_t)c.input.size();
    close(fd), in = tmp = t;
    if (!ok) return unlink(t), o.err = "could not write the input", false;
  }
  int po[2], pe[2];
  if (pipe(po)) {
    if (!tmp.empty()) unlink(tmp.c_str());
    return o.err = "no pipe", false;
  }
  if (pipe(pe)) {
    if (!tmp.empty()) unlink(tmp.c_str());
    return close(po[0]), close(po[1]), o.err = "no pipe", false;
  }
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
  auto spawn = searchPath ? posix_spawnp : posix_spawn;
  int rc = spawn(&pid, av[0], &fa, nullptr, av.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(po[1]), close(pe[1]);
  if (rc) {
    close(po[0]), close(pe[0]), o.code = 127, o.err = "could not start '" + c.argv[0] + "': " + strerror(rc);
  } else {
    int st = reap(pid, {{po[0], &o.out}, {pe[0], &o.err}}, c.timeout, o.timedOut);
    o.code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  }
  if (!tmp.empty()) unlink(tmp.c_str());
  return rc == 0;
}
V outOf(const ProcResult& o) {
  Map* m = make<Map>();
  volatile V keep = box(m);
  (void)keep;
  mapPut(m, "code", box((double)o.code)), mapPut(m, "out", str(o.out)), mapPut(m, "err", str(o.err));
  mapPut(m, "timedOut", boxb(o.timedOut));
  return box(m);
}
V n_exec(V cmd) { ProcResult o; spawnOne(cmdOf(cmd, "exec"), o); return outOf(o); }
V n_execAll(V cmds) { return batch<Cmd, ProcResult>(cmds, "execAll", cmdOf, [](const Cmd& c, ProcResult& o) { spawnOne(c, o); }, outOf); }

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
    ERR("@host writeFile(): could not write '%s': %s", p.c_str(), strerror(errno));
  }
  fclose(f);
  return NIL;
}
V n_env(V name) {
  const char* v = getenv(strArg(name, "env", "the name").c_str());
  return v ? str(v) : NIL;
}
}  // namespace

bool runProcess(const std::vector<std::string>& args, const std::string& file, const std::string* source, double timeout, ProcResult& r) {
  char exe[4096];
  u32 n = sizeof exe;
  if (_NSGetExecutablePath(exe, &n)) return false;
  std::string path = file, tmp;
  if (source) {
    char t[] = "/tmp/fut-run-XXXXXX.fut";
    int fd = mkstemps(t, 4);
    if (fd < 0) return false;
    bool ok = write(fd, source->data(), source->size()) == (ssize_t)source->size();
    close(fd), path = tmp = t;
    if (!ok) return unlink(t), false;
  }
  Cmd cmd{{exe}, {}, {}, timeout};
  cmd.argv.insert(cmd.argv.end(), args.begin(), args.end());
  cmd.argv.push_back(path);
  bool ok = spawnOne(cmd, r, false);
  if (!tmp.empty()) unlink(tmp.c_str());
  return ok;
}

void sysInit() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  native<n_fetch>("fetch"), native<n_fetchAll>("fetchAll"), native<n_exec>("exec"), native<n_execAll>("execAll");
  native<n_readFile>("readFile"), native<n_writeFile>("writeFile"), native<n_env>("env");
}
