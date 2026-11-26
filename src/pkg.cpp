// Packages. A directory with a fut.pkg manifest is a package; its files are the .fut files below it (down to the
// next fut.pkg). Other packages use one by name, `import "auth"`, and reach only what its root file (auth.fut)
// makes `pub`, or re-exports with `pub import "./file.fut"`. Inside a package, `pub` makes a name visible to the
// package's other files. The manifest:
//
//   name auth                    // required: the package's name, how others import it
//   version 0.3.0                // major.minor.patch; `fut test` asks for a bump when the interface breaks
//   root auth.fut                // optional: the root file (default: <name>.fut)
//   dep geom ../geom             // a package this one imports, by name and directory
//   js jwt ./jwt.js              // foreign modules, bound with `extern jwt fn ...`: JavaScript (wasm target),
//   wasm zlib ./zlib.wasm        //   a WebAssembly module (wasm target),
//   c sqlite libsqlite3.dylib    //   a C library (native target). One name may have a js and a c version.
#include "fut.h"
#include <memory>
#include <unordered_map>

static std::unordered_map<std::string, std::unique_ptr<Package>> byDir;  // dir -> its manifest (null: none)
static std::unordered_map<std::string, bool> noManifest;
void packagesReset() { byDir.clear(), noManifest.clear(); }

static std::string dirOf(const std::string& p) { size_t s = p.rfind('/'); return s == std::string::npos ? "." : s ? p.substr(0, s) : "/"; }
static bool isName(const std::string& s) {
  if (s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
  for (char c : s) if (!(isalnum((unsigned char)c) || c == '_')) return false;
  return true;
}
bool parseVersion(const std::string& v, int out[3]) {
  return sscanf(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]) == 3 && out[0] >= 0 && out[1] >= 0 && out[2] >= 0;
}

static Package* load(const std::string& dir) {
  if (auto it = byDir.find(dir); it != byDir.end()) return it->second.get();
  if (noManifest.count(dir)) return nullptr;
  std::string path = dir + "/fut.pkg", text;
  if (!readFile(path, text)) return noManifest[dir] = true, nullptr;
  auto p = std::make_unique<Package>();
  p->dir = dir, p->manifest = path;
  auto bad = [&](int line, const std::string& m) { compileFail(path + ":" + std::to_string(line) + ": error: " + m + " [manifest]", false); };
  int ln = 0;
  for (size_t i = 0; i <= text.size();) {
    size_t j = text.find('\n', i);
    if (j == std::string::npos) j = text.size();
    std::string l = text.substr(i, j - i);
    i = j + 1, ln++;
    if (size_t c = l.find("//"); c != std::string::npos) l.resize(c);
    std::vector<std::string> w;
    for (size_t a = 0; a < l.size();) {
      while (a < l.size() && isspace((unsigned char)l[a])) a++;
      size_t b = a;
      while (b < l.size() && !isspace((unsigned char)l[b])) b++;
      if (b > a) w.push_back(l.substr(a, b - a));
      a = b;
    }
    if (w.empty()) continue;
    auto want = [&](size_t n, const char* form) { if (w.size() != n) bad(ln, std::string("expected: ") + form); };
    const std::string& k = w[0];
    if (k == "name") {
      want(2, "name <identifier>");
      if (!isName(w[1])) bad(ln, "a package name is an identifier: letters, digits and _");
      p->name = w[1];
    } else if (k == "version") {
      want(2, "version <major>.<minor>.<patch>");
      int v[3];
      if (!parseVersion(w[1], v)) bad(ln, "a version is major.minor.patch, such as 0.3.0");
      p->version = w[1];
    } else if (k == "root") {
      want(2, "root <file.fut>");
      p->root = w[1];
    } else if (k == "dep") {
      want(3, "dep <name> <directory>");
      if (!isName(w[1])) bad(ln, "a package name is an identifier: letters, digits and _");
      for (auto& d : p->deps) if (d.name == w[1]) bad(ln, "'" + w[1] + "' is already a dependency");
      p->deps.push_back({w[1], canonicalPath(w[2][0] == '/' ? w[2] : dir + "/" + w[2]), ln});
    } else if (k == "js" || k == "wasm" || k == "c") {
      bool main = k == "js" && w.size() == 4 && w[3] == "main";  // js dom ./dom.js main: it needs the page's thread
      if (!main) want(3, (k + " <name> <" + (k == "c" ? "library" : "file") + ">" + (k == "js" ? " [main]" : "")).c_str());
      if (!isName(w[1])) bad(ln, "a module name is an identifier: letters, digits and _");
      for (auto& m : p->mods)
        if (m.name == w[1] && (m.kind == k || (m.kind != "c") == (k != "c")))
          bad(ln, "'" + w[1] + "' is already a module" + (m.kind == k ? "" : " (one name can have a js or wasm version and a c version)"));
      std::string t = w[2];
      if (k != "c" || t.find('/') != std::string::npos) t = canonicalPath(t[0] == '/' ? t : dir + "/" + t);
      p->mods.push_back({k, w[1], t, ln, main});
    } else bad(ln, "unknown setting '" + k + "' (name, version, root, dep, js, wasm, c)");
  }
  if (p->name.empty()) bad(1, "a package needs a name: name <identifier>");
  if (p->version.empty()) p->version = "0.0.0";
  p->root = canonicalPath(dir + "/" + (p->root.empty() ? p->name + ".fut" : p->root));
  return (byDir[dir] = std::move(p)).get();
}

const Package* packageOf(const std::string& file) {
  if (file.empty() || file[0] == '<') return nullptr;
  for (std::string d = dirOf(file[0] == '/' ? file : canonicalPath(file));; d = dirOf(d)) {
    if (Package* p = load(d)) return p;
    if (d == "/" || d == ".") return nullptr;
  }
}
const Package* packageAt(const std::string& dir) { return load(dir); }
std::string stdRoot;
bool inStd(const std::string& path) { return !stdRoot.empty() && path.starts_with(stdRoot + "/"); }
const ForeignMod* Package::module(const std::string& n, bool native) const {
  const ForeignMod* any = nullptr;
  for (auto& m : mods)
    if (m.name == n) {
      if ((m.kind == "c") == native) return &m;
      any = &m;
    }
  return any;
}
