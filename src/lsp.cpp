// The native stdio language server. All source analysis runs in a disposable child of the protocol process.
#include "compile.h"
#include "fmt.h"
#include <charconv>
#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>

namespace {
constexpr size_t MAX_HEADER = 8192, MAX_MESSAGE = 32 << 20;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if ((c | 32) >= 'a' && (c | 32) <= 'f') return (c | 32) - 'a' + 10;
  return -1;
}
bool digit(char c) { return c >= '0' && c <= '9'; }

struct Json {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  std::string string;
  std::string_view raw;
  std::vector<Json> list;
  std::vector<std::pair<std::string, Json>> fields;

  const Json& get(std::string_view key) const {
    for (auto& [name, value] : fields) if (name == key) return value;
    static const Json missing;
    return missing;
  }
  int integer(int fallback = -1) const {
    if (kind != Number) return fallback;
    int n;
    auto [p, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), n);
    return ec == std::errc{} && p == raw.data() + raw.size() ? n : fallback;
  }
  bool isTrue() const { return kind == Bool && raw == "true"; }
};

struct BadJson {};
class JsonParser {
  std::string_view text;
  size_t pos = 0;

  char peek() const { return pos < text.size() ? text[pos] : '\0'; }
  bool take(char c) { if (peek() != c) return false; pos++; return true; }
  void space() { while (peek() == ' ' || peek() == '\r' || peek() == '\n' || peek() == '\t') pos++; }
  void expect(char c) { if (!take(c)) throw BadJson{}; }
  void literal(std::string_view s) {
    if (!text.substr(pos).starts_with(s)) throw BadJson{};
    pos += s.size();
  }
  unsigned hex4() {
    unsigned n = 0;
    for (int i = 0; i < 4; i++) {
      int h = hex(peek());
      if (h < 0) throw BadJson{};
      n = (n << 4) + h;
      pos++;
    }
    return n;
  }
  void unicode(std::string& out) {
    unsigned n = hex4();
    if (n >= 0xd800 && n <= 0xdbff) {
      literal("\\u");
      unsigned lo = hex4();
      if (lo < 0xdc00 || lo > 0xdfff) throw BadJson{};
      n = 0x10000 + ((n - 0xd800) << 10) + lo - 0xdc00;
    } else if (n >= 0xdc00 && n <= 0xdfff) throw BadJson{};
    if (n < 0x80) { out += char(n); return; }
    int trailing = n < 0x800 ? 1 : n < 0x10000 ? 2 : 3;
    out += char((0x100 - (0x80 >> trailing)) | (n >> (6 * trailing)));
    while (trailing--) out += char(0x80 | ((n >> (6 * trailing)) & 0x3f));
  }
  std::string string() {
    expect('"');
    std::string out;
    while (!take('"')) {
      unsigned char c = peek();
      if (c < 0x20) throw BadJson{};
      pos++;
      if (c != '\\') { out += char(c); continue; }
      switch (peek()) {
        case '"': case '\\': case '/': out += peek(); pos++; break;
        case 'b': out += '\b'; pos++; break;
        case 'f': out += '\f'; pos++; break;
        case 'n': out += '\n'; pos++; break;
        case 'r': out += '\r'; pos++; break;
        case 't': out += '\t'; pos++; break;
        case 'u': pos++; unicode(out); break;
        default: throw BadJson{};
      }
    }
    return out;
  }
  void digits() {
    if (!digit(peek())) throw BadJson{};
    while (digit(peek())) pos++;
  }
  Json value(int depth) {
    if (depth > 64) throw BadJson{};
    space();
    size_t start = pos;
    Json j;
    switch (peek()) {
      case 'n': literal("null"); break;
      case 't': literal("true"); j.kind = Json::Bool; break;
      case 'f': literal("false"); j.kind = Json::Bool; break;
      case '"': j.kind = Json::String; j.string = string(); break;
      case '[':
        pos++; j.kind = Json::Array; space();
        if (take(']')) break;
        do { j.list.push_back(value(depth + 1)); space(); } while (take(','));
        expect(']');
        break;
      case '{':
        pos++; j.kind = Json::Object; space();
        if (take('}')) break;
        do {
          space();
          std::string key = string();
          space(); expect(':');
          j.fields.emplace_back(std::move(key), value(depth + 1));
          space();
        } while (take(','));
        expect('}');
        break;
      default:
        j.kind = Json::Number;
        take('-');
        if (!take('0')) digits();
        if (take('.')) digits();
        if (take('e') || take('E')) {
          if (!take('+')) take('-');
          digits();
        }
    }
    j.raw = text.substr(start, pos - start);
    return j;
  }

public:
  explicit JsonParser(std::string_view input) : text(input) {}
  Json parse() {
    Json j = value(0);
    space();
    if (pos != text.size()) throw BadJson{};
    return j;
  }
};

struct Rows {
  std::string text = "[";
  void add(std::string_view row) {
    if (text.size() > 1) text += ',';
    text += row;
  }
  std::string end() const { return text + ']'; }
};

std::string normalize(const std::string& path) {
  std::error_code err;
  auto p = std::filesystem::weakly_canonical(path, err);
  return err ? std::filesystem::path(path).lexically_normal().string() : p.string();
}
std::string fromUri(std::string_view uri) {
  if (!uri.starts_with("file:")) return "";
  uri.remove_prefix(5);
  if (uri.starts_with("//")) {
    uri.remove_prefix(2);
    if (uri.starts_with("localhost/")) uri.remove_prefix(9);
  }
  if (!uri.starts_with('/')) return "";
  std::string path;
  for (size_t i = 0; i < uri.size(); i++) {
    char c = uri[i];
    if (c == '%') {
      if (uri.size() - i < 3) return "";
      int high = hex(uri[i + 1]), low = hex(uri[i + 2]);
      if (high < 0 || low < 0) return "";
      c = char(high * 16 + low);
      i += 2;
    }
    if (!c) return "";
    path += c;
  }
  return normalize(path);
}

struct Position {
  int line, character;
  auto operator<=>(const Position&) const = default;
  std::string json() const {
    return "{\"line\":" + std::to_string(line) + ",\"character\":" + std::to_string(character) + "}";
  }
};
struct Range {
  Position start, end;
  std::string json() const { return "{\"start\":" + start.json() + ",\"end\":" + end.json() + "}"; }
  bool contains(Position p) const { return p >= start && (p < end || (p == start && start == end)); }
};
// The compiler and scanner count bytes. LSP clients use UTF-16 code units by default.
class Lines {
  std::string_view text;
  std::vector<size_t> starts{0};

  size_t end(size_t line) const {
    size_t e = line + 1 < starts.size() ? starts[line + 1] - 1 : text.size();
    return e > starts[line] && text[e - 1] == '\r' ? e - 1 : e;
  }
  size_t next(size_t at, size_t stop) const {
    at++;
    while (at < stop && (uint8_t(text[at]) & 0xc0) == 0x80) at++;
    return at;
  }
  int units(size_t at) const { return uint8_t(text[at]) >= 0xf0 ? 2 : 1; }

public:
  explicit Lines(std::string_view s) : text(s) {
    for (size_t i = 0; i < s.size(); i++) if (s[i] == '\n') starts.push_back(i + 1);
  }
  Position at(size_t byte) const {
    byte = std::min(byte, text.size());
    size_t line = std::upper_bound(starts.begin(), starts.end(), byte) - starts.begin() - 1;
    byte = std::min(byte, end(line));
    int character = 0;
    for (size_t i = starts[line]; i < byte; i = next(i, byte)) character += units(i);
    return {int(line), character};
  }
  std::optional<size_t> offset(Position p) const {
    if (p.line < 0 || size_t(p.line) >= starts.size() || p.character < 0) return {};
    size_t i = starts[p.line], stop = end(p.line);
    for (int col = 0; i < stop && col < p.character; i = next(i, stop)) col += units(i);
    return i;
  }
  Range token(int line, int column) const {
    size_t l = std::min(size_t(std::max(1, line) - 1), starts.size() - 1);
    size_t stop = end(l), b = std::min(starts[l] + size_t(std::max(1, column) - 1), stop), e = b;
    if (b < stop && idc(text[b])) {
      while (e < stop && (idc(text[e]) || digit(text[e]))) e++;
    } else if (b < stop) e = next(b, stop);
    return {at(b), at(e)};
  }
};

using Buffers = std::vector<std::pair<std::string, std::string_view>>;
bool source(const std::string& path, const Buffers& buffers, std::string& text) {
  for (auto& [file, contents] : buffers) if (file == path) { text = contents; return true; }
  return readFile(path, text);
}
std::string base(std::string_view path) { return std::string(path.substr(path.rfind('/') + 1)); }

struct Kinds { int symbol, completion; };
Kinds kinds(const Decl& d) {
  if (d.kind == "fn" || d.kind == "extern" || d.kind == "test") return {12, 3};
  if (d.kind == "method" || d.kind == "op") return d.name == "init" ? Kinds{9, 4} : Kinds{6, 2};
  if (d.kind == "class") return {5, 7};
  if (d.kind == "effect") return {11, 8};
  if (d.kind == "enum") return {10, 13};
  if (d.kind == "variant") return {22, 20};
  if (d.kind == "field") return {8, 5};
  if (d.kind == "import") return {2, 9};
  if (d.kind == "type") return {26, 25};
  return {13, 6};  // var
}

Range nameRange(const Decl& d, const Lines& lines) {
  return {lines.at(d.nameAt), lines.at(d.nameAt + d.name.size())};
}
std::string symbols(const std::string& text, const std::vector<Decl>& decls, bool hierarchical, std::string_view uri) {
  Lines lines(text);
  Rows symbols;
  auto entry = [&](const Decl& d, const std::string& children) {
    std::string out = "{\"name\":" + quoteJson(d.name) + ",\"kind\":" + std::to_string(kinds(d).symbol);
    Range selection = nameRange(d, lines), range{lines.at(d.start), lines.at(d.stop)};
    if (hierarchical)
      out += ",\"detail\":" + quoteJson(d.sig) + ",\"range\":" + range.json() + ",\"selectionRange\":" + selection.json() +
             (children == "[]" ? "" : ",\"children\":" + children);
    else {
      out += ",\"location\":{\"uri\":" + quoteJson(uri) + ",\"range\":" + range.json() + "}";
      if (!d.owner.empty()) out += ",\"containerName\":" + quoteJson(d.owner);
    }
    return out + "}";
  };
  for (size_t i = 0; i < decls.size();) {
    const Decl& d = decls[i++];
    if (d.name.empty()) continue;
    Rows children;
    if (hierarchical) while (i < decls.size() && decls[i].owner == d.name) {
      const Decl& child = decls[i++];
      if (!child.name.empty()) children.add(entry(child, "[]"));
    }
    symbols.add(entry(d, children.end()));
  }
  return symbols.end();
}
std::string hover(const Range& range, std::string_view text) {
  return "{\"contents\":{\"kind\":\"plaintext\",\"value\":" + quoteJson(text) + "},\"range\":" + range.json() + "}";
}

struct Target { std::string path, alias; bool package = false; };
// Same file/package distinction as the compiler. This lookup also works while an importer's body is incomplete.
Target completionTarget(const Decl& d, const std::string& from) {
  if (d.name.empty()) return {};
  if (d.name.ends_with(".fut") || (d.name.contains('/') && !d.name.starts_with("std/"))) {
    std::string path = normalize((std::filesystem::path(from).parent_path() / d.name).string());
    return packageOf(path) == packageOf(from) ? Target{std::move(path), d.alias} : Target{};
  }
  const Package* p = nullptr;
  std::string package = d.name;
  if (d.name.starts_with("std/")) {
    package = d.name.substr(4);
    if (!stdRoot.empty() && !package.empty() && !package.contains('/')) p = packageAt(stdRoot + "/" + package);
  } else if (const Package* here = packageOf(from))
    for (auto& dep : here->deps) if (dep.name == d.name) { p = packageAt(dep.dir); break; }
  return p && p->name == package ? Target{normalize(p->root), d.alias.empty() ? p->name : d.alias, true} : Target{};
}
void publicDecls(const Target& target, const Buffers& buffers, std::vector<Decl>& out) {
  std::vector<std::string> pending{target.path};
  std::set<std::string> seen;
  for (size_t i = 0; i < pending.size(); i++) {
    std::string file = pending[i], text;
    if (!seen.insert(file).second || !source(file, buffers, text)) continue;
    for (auto& d : declsOf(text)) {
      if (d.kind != "import") {
        if (d.pub && d.owner.empty()) out.push_back(std::move(d));
      } else if (target.package ? d.pub : target.alias.empty()) {
        Target next = completionTarget(d, file);
        if (!next.path.empty() && !next.package && next.alias.empty()) pending.push_back(std::move(next.path));
      }
    }
  }
}

std::string receiverClass(const std::string& text, const std::vector<Decl>& decls, std::string_view receiver, size_t before) {
  if (receiver == "self")
    for (const Decl& d : decls) if (d.kind == "class" && d.start <= before && before < d.stop) return d.name;
  for (const Decl& d : decls)
    if ((d.kind == "class" || d.kind == "enum" || d.kind == "effect") && d.name == receiver) return d.name;
  if (receiver.empty()) return {};
  std::string type;
  Lexer lex{text.c_str()};
  for (Tok t = lex.next(); t.t != T_EOF && size_t(t.p - text.c_str()) < before; t = lex.next()) {
    if (t.t != T_ID || t.sym != K_var) continue;
    Lexer look = lex;
    Tok name = look.next(), op = look.next(), cls = look.next();
    if (name.t != T_ID || symName(name.sym) != receiver || cls.t != T_ID) continue;
    if (op.t == ':' || (op.t == '=' && look.next().t == '(')) type = symName(cls.sym);
  }
  return type;
}
std::string completion(const std::string& path, const std::string& text, const std::vector<Decl>& decls,
                       const Buffers& buffers, size_t at) {
  auto wordStart = [&](size_t i) {
    while (i && (idc(text[i - 1]) || digit(text[i - 1]))) i--;
    return i;
  };
  size_t start = wordStart(at);
  bool member = start && text[start - 1] == '.';
  std::string_view receiver;
  if (member) {
    size_t end = start - 1;
    if (end && text[end - 1] == '?') end--;
    size_t begin = wordStart(end);
    receiver = std::string_view(text).substr(begin, end - begin);
  }
  Rows items;
  std::set<std::string> seen;
  auto add = [&](const std::string& name, int kind, std::string_view detail = {}) {
    if (name.empty() || !seen.insert(name).second) return;
    items.add("{\"label\":" + quoteJson(name) + ",\"kind\":" + std::to_string(kind) +
              (detail.empty() ? "" : ",\"detail\":" + quoteJson(detail)) + "}");
  };
  auto addDecl = [&](const Decl& d) { add(d.name, kinds(d).completion, d.sig); };
  for (const Decl& d : decls)
    if (!member && d.owner.empty() && d.kind != "test" && d.kind != "import") addDecl(d);

  bool alias = false;
  for (const Decl& d : decls) {
    if (d.kind != "import") continue;
    try {
      Target target = completionTarget(d, path);
      if (!member && !target.alias.empty()) add(target.alias, 9, d.sig);
      if (target.path.empty() || (member ? target.alias != receiver || receiver.empty() : !target.alias.empty())) continue;
      alias |= member;
      std::vector<Decl> imported;
      publicDecls(target, buffers, imported);
      for (auto& item : imported) addDecl(item);
    } catch (const CompileError&) {}  // malformed manifest: other completions are still useful
  }
  if (member && !alias) {
    std::string cls = receiverClass(text, decls, receiver, at);
    if (!cls.empty()) for (const Decl& d : decls) if (d.owner == cls) addDecl(d);
  }
  if (!member) {
    for (const char* k : {KEYWORDS(SYM_STR)}) add(k, 14);
    int sym = S_FIRST + 1;
    for (const char* k : {EXTRA_SYMS(SYM_STR)}) {
      int kind = sym >= S_as ? 14 : sym == S_main || sym == S_reload ? 3 : 2;
      add(k, kind);
      sym++;
    }
    add("gpu", 14);
  }
  return items.end();
}

bool writeAll(int fd, std::string_view s) {
  while (!s.empty()) {
    ssize_t n = write(fd, s.data(), s.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    s.remove_prefix(n);
  }
  return true;
}

template <class Work>
std::optional<std::string> isolated(Work work) {
  int fds[2];
  if (pipe(fds) < 0) { perror("fut lsp: pipe"); return {}; }
  pid_t pid = fork();
  if (!pid) {
    close(fds[0]);
    close(STDIN_FILENO);
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) _exit(1);
    try { _exit(writeAll(fds[1], work()) ? 0 : 1); }
    catch (const std::exception& e) { fprintf(stderr, "fut lsp: %s\n", e.what()); }
    _exit(1);
  }
  close(fds[1]);
  if (pid < 0) { perror("fut lsp: fork"); close(fds[0]); return {}; }
  bool timedOut = false;
  std::string result;
  int status = reap(pid, {{fds[0], &result}}, 2, timedOut);
  if (!timedOut && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !result.empty()) return result;
  if (timedOut) fputs("fut lsp: source analysis timed out\n", stderr);
  else if (WIFSIGNALED(status)) fprintf(stderr, "fut lsp: analysis process ended on signal %d\n", WTERMSIG(status));
  else fputs("fut lsp: analysis process failed\n", stderr);
  return {};
}

std::string checkSource(const std::string& path, const Buffers& buffers) {
  rtInit();
  CompileCheck check{buffers, normalize, {}};
  std::vector<CompileDiagnostic> diagnostics;
  try { compile(path, false, false, nullptr, false, &check); }
  catch (const CompileError& e) { diagnostics.push_back(compileDiagnostic(e.what())); }
  Rows errors, imports;
  for (auto& d : diagnostics)
    errors.add("{\"file\":" + quoteJson(d.file) + ",\"message\":" + quoteJson(d.message) + ",\"code\":" + quoteJson(d.code) +
               ",\"line\":" + std::to_string(d.line) + ",\"column\":" + std::to_string(d.column) + "}");
  for (auto& i : check.imports)
    imports.add("{\"from\":" + quoteJson(i.from) + ",\"to\":" + quoteJson(i.to) +
                ",\"line\":" + std::to_string(i.line) + ",\"column\":" + std::to_string(i.column) + "}");
  return "{\"errors\":" + errors.end() + ",\"imports\":" + imports.end() + "}";
}

struct Document {
  std::string path, text;
  int version = 0;
  Clock::time_point due{};
  std::vector<CompileDiagnostic> errors;
  std::vector<CompileCheck::Import> imports;
  std::string published;
};
struct Finding {
  Range range;
  std::string message, code, related;
};

class Server {
  std::map<std::string, Document> documents;
  bool initialized = false, stopped = false, exiting = false, connected = true, hierarchical = false, relatedInfo = false;

  void send(std::string body) {
    if (!connected) return;
    std::string header = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    connected = writeAll(STDOUT_FILENO, header) && writeAll(STDOUT_FILENO, body);
  }
  void respond(std::string_view id, std::string result) {
    if (!id.empty()) send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id) + ",\"result\":" + result + "}");
  }
  void error(std::string_view id, int code, std::string_view message) {
    send("{\"jsonrpc\":\"2.0\",\"id\":" + std::string(id.empty() ? "null" : id) + ",\"error\":{\"code\":" +
         std::to_string(code) + ",\"message\":" + quoteJson(message) + "}}");
  }
  Buffers buffers() const {
    Buffers out;
    for (auto& [_, doc] : documents) out.emplace_back(doc.path, doc.text);
    return out;
  }
  void schedule(const std::string& path, Clock::time_point when) {
    for (auto& [_, d] : documents)
      if (d.path == path || std::any_of(d.imports.begin(), d.imports.end(), [&](auto& i) { return i.to == path; })) {
        d.due = when;
        d.errors.clear();
      }
  }
  std::vector<Finding> findings(const Document& doc) const {
    Lines lines(doc.text);
    std::vector<Finding> out;
    std::set<std::tuple<Position, std::string, std::string>> seen;
    auto add = [&](int line, int column, std::string message, const std::string& code, const std::string& related = "") {
      Range range = lines.token(line, column);
      if (seen.emplace(range.start, message, code).second) out.push_back({range, std::move(message), code, related});
    };
    for (auto& [_, root] : documents) for (const auto& d : root.errors) {
      if (d.file == doc.path) {
        add(d.line, d.column, d.message, d.code);
        continue;
      }
      if (&root != &doc) continue;
      std::string message = base(d.file) + ":" + std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message;
      std::string related;
      if (relatedInfo) for (auto& [uri, other] : documents) if (other.path == d.file) {
        related = "[{\"location\":{\"uri\":" + quoteJson(uri) + ",\"range\":" + Lines(other.text).token(d.line, d.column).json() +
                  "},\"message\":" + quoteJson(d.message) + "}]";
        break;
      }
      // Walk backwards from the error through the import graph, then mark the root's import site(s).
      std::set<std::string> reaches{d.file};
      std::vector<std::string> pending{d.file};
      std::multimap<std::string_view, std::string_view> parents;
      for (auto& i : root.imports) if (!i.to.empty()) parents.emplace(i.to, i.from);
      for (size_t i = 0; i < pending.size(); i++) {
        if (pending[i] == root.path) continue;
        auto [first, last] = parents.equal_range(pending[i]);
        for (; first != last; ++first) if (reaches.emplace(first->second).second) pending.emplace_back(first->second);
      }
      bool placed = false;
      for (auto& i : root.imports)
        if (i.from == root.path && reaches.contains(i.to)) {
          add(i.line, i.column, message, d.code, related);
          placed = true;
        }
      if (!placed) add(1, 1, std::move(message), d.code, related);
    }
    return out;
  }
  void publish(const std::string& uri, const std::string& rows, std::optional<int> version) {
    send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":" + quoteJson(uri) +
         (version ? ",\"version\":" + std::to_string(*version) : "") + ",\"diagnostics\":" + rows + "}}");
  }
  void publishAll() {
    for (auto& [uri, d] : documents) if (d.due == Clock::time_point{}) {
      Rows rows;
      for (auto& f : findings(d)) {
        std::string info = errorExplanation(f.code);
        rows.add("{\"range\":" + f.range.json() + ",\"severity\":1,\"source\":\"fut\",\"message\":" + quoteJson(f.message) +
                 (f.code.empty() ? "" : ",\"code\":" + quoteJson(f.code)) +
                 (info.empty() ? "" : ",\"data\":{\"explanation\":" + quoteJson(info) + "}") +
                 (f.related.empty() ? "" : ",\"relatedInformation\":" + f.related) + "}");
      }
      std::string result = rows.end();
      std::string snapshot = std::to_string(d.version) + result;
      if (snapshot == d.published) continue;
      d.published = std::move(snapshot);
      publish(uri, result, d.version);
    }
  }
  void check(Document& doc) {
    doc.due = {};
    doc.errors.clear();
    auto result = isolated([&] { return checkSource(doc.path, buffers()); });
    if (!result) {
      doc.errors.push_back({doc.path, "The compiler process failed while checking this file. See the language server log."});
      return;
    }
    Json report = JsonParser(*result).parse();
    doc.imports.clear();
    for (auto& i : report.get("imports").list) {
      std::string to = i.get("to").string;
      doc.imports.push_back({normalize(i.get("from").string), to.empty() ? "" : normalize(to),
                             i.get("line").integer(1), i.get("column").integer(1)});
    }
    for (auto& d : report.get("errors").list) {
      auto& file = d.get("file").string;
      CompileDiagnostic result{file.empty() ? doc.path : normalize(file), d.get("message").string, d.get("code").string,
                               d.get("line").integer(1), d.get("column").integer(1)};
      if (result.code == "manifest" && !doc.imports.empty() && doc.imports.back().to.empty()) doc.imports.back().to = result.file;
      doc.errors.push_back(std::move(result));
    }
  }

  void changed(const std::string& method, const Json& params) {
    const Json& info = params.get("textDocument");
    const std::string& uri = info.get("uri").string;
    auto it = documents.find(uri);
    auto time = Clock::now();
    if (method == "textDocument/didOpen") {
      std::string path = fromUri(uri);
      const Json& text = info.get("text");
      if (path.empty() || text.kind != Json::String) return;
      Document& d = documents[uri];
      d.path = std::move(path);
      d.text = text.string;
      d.version = info.get("version").integer(0);
      schedule(d.path, time);
    } else if (it != documents.end()) {
      Document& d = it->second;
      if (method == "textDocument/didClose") {
        std::string path = d.path;
        documents.erase(it);
        publish(uri, "[]", {});
        schedule(path, time);
        publishAll();
      } else if (method == "textDocument/didSave") {
        if (const Json& t = params.get("text"); t.kind == Json::String) d.text = t.string;
        d.published.clear();  // publish again even when the file's version and diagnostics are unchanged
        schedule(d.path, time);
      } else if (method == "textDocument/didChange") {
        for (auto& c : params.get("contentChanges").list) {
          const Json& t = c.get("text");
          if (t.kind == Json::String && c.get("range").kind == Json::Null) d.text = t.string;
        }
        d.version = info.get("version").integer(d.version);
        schedule(d.path, time + 25ms);
      }
    }
  }
  void feature(const std::string& method, const Json& params, std::string_view id) {
    const std::string& uri = params.get("textDocument").get("uri").string;
    auto it = documents.find(uri);
    std::string path = it != documents.end() ? it->second.path : fromUri(uri);
    bool hovering = method == "textDocument/hover";
    if (path.empty()) { respond(id, hovering ? "null" : "[]"); return; }
    const Json& pos = params.get("position");
    Position p{pos.get("line").integer(), pos.get("character").integer()};
    if (hovering && it != documents.end()) for (auto& f : findings(it->second)) if (f.range.contains(p)) {
      std::string message = f.message, info = errorExplanation(f.code);
      if (!f.code.empty()) message += " [" + f.code + "]";
      if (!info.empty()) message += "\n\n" + info;
      respond(id, hover(f.range, message));
      return;
    }
    auto result = isolated([&]() -> std::string {
      std::string text;
      Buffers open = buffers();
      if (!source(path, open, text)) return hovering ? "null" : "[]";
      for (const char* word : {KEYWORDS(SYM_STR) EXTRA_SYMS(SYM_STR)}) intern(word);
      auto decls = declsOf(text);
      if (method == "textDocument/documentSymbol") return symbols(text, decls, hierarchical, uri);
      Lines lines(text);
      if (hovering) {
        for (auto& d : decls) if (!d.name.empty() && nameRange(d, lines).contains(p)) return hover(nameRange(d, lines), d.sig);
        return "null";
      }
      auto offset = lines.offset(p);
      return offset ? completion(path, text, decls, open, *offset) : "[]";
    });
    if (result) respond(id, std::move(*result));
    else error(id, -32603, "fut source analysis failed");
  }
  // textDocument/formatting: the whole buffer through fut fmt, as one edit (none when it's formatted or doesn't lex)
  void formatting(const Json& params, std::string_view id) {
    auto it = documents.find(params.get("textDocument").get("uri").string);
    if (it == documents.end()) { respond(id, "[]"); return; }
    const std::string& text = it->second.text;
    auto result = isolated([&]() -> std::string {
      std::string err, out = formatSource(text, &err);
      if (!err.empty() || out == text) return "[]";
      return "[{\"range\":" + Range{{0, 0}, Lines(text).at(text.size())}.json() + ",\"newText\":" + quoteJson(out) + "}]";
    });
    if (result) respond(id, std::move(*result));
    else error(id, -32603, "fut fmt failed");
  }
  void dispatch(const Json& message) {
    const Json &method = message.get("method"), &id = message.get("id"), &params = message.get("params");
    std::string_view request = id.kind == Json::String || id.kind == Json::Number || id.kind == Json::Null ? id.raw : "";
    if (message.kind != Json::Object || message.get("jsonrpc").string != "2.0" ||
        (!id.raw.empty() && request.empty()) || (method.kind != Json::String && !method.raw.empty())) {
      error(request, -32600, "Invalid Request");
      return;
    }
    if (method.kind != Json::String) {
      if (request.empty() || (message.get("result").raw.empty() && message.get("error").raw.empty()))
        error(request, -32600, "Invalid Request");
      return;
    }
    const std::string& name = method.string;
    if (name == "exit") exiting = true;
    else if (name == "initialize" && !initialized && !request.empty()) {
      initialized = true;
      const Json& caps = params.get("capabilities").get("textDocument");
      hierarchical = caps.get("documentSymbol").get("hierarchicalDocumentSymbolSupport").isTrue();
      relatedInfo = caps.get("publishDiagnostics").get("relatedInformation").isTrue();
      respond(request, "{\"serverInfo\":{\"name\":\"fut\"},\"capabilities\":{\"positionEncoding\":\"utf-16\","
                       "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":true}},"
                       "\"completionProvider\":{\"triggerCharacters\":[\".\"]},\"hoverProvider\":true,\"documentSymbolProvider\":true,\"documentFormattingProvider\":true}}");
    } else if (!initialized) {
      if (!request.empty()) error(request, -32002, "Server not initialized");
    } else if (stopped || name == "initialize") {
      if (!request.empty()) error(request, -32600, "Invalid Request");
    } else if (!request.empty()) {
      if (name == "shutdown") { stopped = true; respond(request, "null"); }
      else if (name == "textDocument/documentSymbol" || name == "textDocument/hover" || name == "textDocument/completion")
        feature(name, params, request);
      else if (name == "textDocument/formatting") formatting(params, request);
      else error(request, -32601, "Method not found");
    } else if (name == "textDocument/didOpen" || name == "textDocument/didChange" ||
               name == "textDocument/didSave" || name == "textDocument/didClose")
      changed(name, params);
  }

  static std::optional<size_t> contentLength(std::string_view header) {
    std::optional<size_t> length;
    while (!header.empty()) {
      size_t end = header.find("\r\n");
      std::string_view field = header.substr(0, end);
      size_t colon = field.find(':');
      if (colon == std::string_view::npos) return {};
      constexpr std::string_view key = "content-length";
      if (colon == key.size() && std::equal(key.begin(), key.end(), field.begin(),
                                            [](char a, char b) { return a == tolower(uint8_t(b)); })) {
        if (length) return {};
        field.remove_prefix(colon + 1);
        while (!field.empty() && (field.front() == ' ' || field.front() == '\t')) field.remove_prefix(1);
        while (!field.empty() && (field.back() == ' ' || field.back() == '\t')) field.remove_suffix(1);
        if (field.empty()) return {};
        size_t n;
        auto [p, ec] = std::from_chars(field.data(), field.data() + field.size(), n);
        if (ec != std::errc{} || p != field.data() + field.size() || n > MAX_MESSAGE) return {};
        length = n;
      }
      if (end == std::string_view::npos) break;
      header.remove_prefix(end + 2);
    }
    return length;
  }

public:
  int run() {
    std::string input;
    std::optional<size_t> length;
    while (connected && !exiting) {
      int wait = -1;
      if (!stopped) for (auto& [_, d] : documents) if (d.due != Clock::time_point{}) {
        int ms = std::max(0, int(std::chrono::ceil<std::chrono::milliseconds>(d.due - Clock::now()).count()));
        wait = wait < 0 ? ms : std::min(wait, ms);
      }
      pollfd fd{STDIN_FILENO, POLLIN, 0};
      int ready = poll(&fd, 1, wait);
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) { perror("fut lsp: poll"); return 1; }
      if (ready) {
        char chunk[8192];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        input.append(chunk, size_t(n));
        size_t used = 0;
        while (connected && !exiting) {
          if (!length) {
            size_t end = input.find("\r\n\r\n", used);
            size_t bytes = end == std::string::npos ? input.size() - used : end - used;
            if (bytes > MAX_HEADER) return fputs("fut lsp: oversized message header\n", stderr), 1;
            if (end == std::string::npos) break;
            length = contentLength(std::string_view(input).substr(used, bytes));
            if (!length) return fputs("fut lsp: invalid Content-Length header\n", stderr), 1;
            used = end + 4;
          }
          if (input.size() - used < *length) break;
          try { dispatch(JsonParser(std::string_view(input).substr(used, *length)).parse()); }
          catch (const BadJson&) { error("null", -32700, "Parse error"); }
          used += *length;
          length.reset();
        }
        input.erase(0, used);
      }
      if (!stopped && !exiting && connected) {
        bool checked = false;
        auto now = Clock::now();
        for (auto& [_, d] : documents) if (d.due != Clock::time_point{} && d.due <= now) {
          check(d);
          checked = true;
        }
        if (checked) publishAll();
      }
    }
    return stopped ? 0 : 1;
  }
};
}  // namespace

int lspCommand() {
  signal(SIGPIPE, SIG_IGN);
  return Server{}.run();
}
