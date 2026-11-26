// Format the trivia between tokens. The compiler's lexer keeps strings (including their holes) whole.
#include "fmt.h"
#include "compile.h"

#if !__wasm__
#include <cerrno>
#include <filesystem>
#include <map>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr int Comment = -1;
constexpr size_t NoToken = std::string_view::npos;
enum class Spacing { Normal, Block, TightBrace, Generic, Unary, Ternary, Optional, Slice, Inherits };

struct Piece {
  int kind;
  size_t begin, end;
  bool newline = false;  // the compiler sees a newline before this token, including across comments
  Spacing space = Spacing::Normal;
  size_t open = NoToken;
  bool classJoin = false;  // inside a view's dotted class name, where a hyphen is part of the name
  bool verbatim = false;   // the view rewriter interprets the original trivia before this token
  std::string_view text(std::string_view src) const { return src.substr(begin, end - begin); }
};

bool scan(const std::string& src, std::vector<Piece>& pieces, std::string& err) {
  if (src.find('\0') != std::string::npos) return err = "NUL byte in source", false;
  Lexer lex{src.c_str()};
  for (size_t prev = 0;;) {
    Tok tok = lex.next(false);
    size_t begin = tok.p - src.data() - (tok.t == T_STR);
    size_t end = lex.p - src.data();
    if (tok.t == T_ERR) return err = "line " + std::to_string(tok.line) + ": " + lex.err, false;
    if (tok.t < T_EOF && !std::strchr("(){}[]:;,.-+*/%<>?=~!|^&", tok.t))
      return err = "line " + std::to_string(tok.line) + ": unexpected character in source", false;
    auto trivia = std::string_view(src).substr(prev, begin - prev);
    for (size_t at = 0; (at = trivia.find("//", at)) != NoToken;) {
      size_t stop = std::min(trivia.find('\n', at), trivia.size());
      pieces.push_back({Comment, prev + at, prev + stop});
      at = stop;
    }
    if (tok.t == T_EOF) return true;
    pieces.push_back({tok.t, begin, end, tok.nl});
    prev = end;
  }
}

bool is(std::string_view word, std::initializer_list<std::string_view> words) {
  return std::find(words.begin(), words.end(), word) != words.end();
}
bool prefixWord(std::string_view word) {
  return is(word, {"if", "while", "for", "in", "return", "throw", "yield", "expect", "case", "match",
                   "requires", "ensures", "invariant", "when", "var", "state", "else"});
}
bool valueEnd(const Piece& p, std::string_view src) {
  return (p.kind == T_ID && !prefixWord(p.text(src))) || p.kind == T_NUM || p.kind == T_STR ||
         p.kind == ')' || p.kind == ']' || p.kind == '}' || p.space == Spacing::Generic || p.space == Spacing::Optional;
}
bool valueStart(int kind) {
  return kind == T_ID || kind == T_NUM || kind == T_STR || kind == '(' || kind == '[' || kind == '{' ||
         kind == '-' || kind == '!' || kind == '~';
}
bool binary(int kind) {
  return (kind >= T_EQ && kind <= T_POW && kind != T_DOTDOT) ||
         (kind > 0 && kind < T_EOF && std::strchr("=+-*/%<>|^&", kind));
}

int openerFor(int kind) {
  switch (kind) {
    case ')': return '(';
    case ']': return '[';
    case '}': return '{';
    default: return 0;
  }
}

// Classify the few tokens whose spacing depends on their role. Unbalanced delimiters can be present in an editor
// buffer, so only match a closer against the immediate enclosing group.
void classify(std::string_view src, std::vector<Piece>& pieces) {
  struct Group {
    size_t open;
    bool index = false;
    bool ternary = false;
    std::vector<size_t> questions, angles;
  };
  std::vector<Group> groups{{NoToken}};
  const Piece none{T_EOF, 0, 0};
  const Piece* prev = &none;
  const Piece* beforePrev = &none;
  for (size_t i = 0; i < pieces.size(); i++) {
    Piece& p = pieces[i];
    if (p.kind == Comment) continue;
    const Piece& next = i + 1 < pieces.size() ? pieces[i + 1] : none;
    const Piece& afterNext = i + 2 < pieces.size() ? pieces[i + 2] : none;
    int left = prev->kind, right = next.kind;
    auto word = prev->text(src);
    auto& group = groups.back();
    if (p.kind == '(' || p.kind == '[' || p.kind == '{') {
      if (p.kind == '{') {
        bool key = (right == T_ID || right == T_STR || right == T_NUM) && afterNext.kind == ':';
        bool expression = left == '=' || left == ':' || left == '(' || left == '[' || left == ',' ||
                          binary(left) || is(word, {"return", "yield", "in", "throw"});
        p.space = key || expression ? Spacing::TightBrace : Spacing::Block;
      }
      groups.push_back({i, p.kind == '[' && !p.newline && valueEnd(*prev, src)});
    } else if (int open = openerFor(p.kind); open && group.open != NoToken && pieces[group.open].kind == open) {
      p.open = group.open;
      p.space = pieces[p.open].space;
      groups.pop_back();
    } else if (p.kind == '?') {
      p.space = Spacing::Optional;
      if (!next.newline && valueStart(right) && !is(next.text(src), {"requires", "ensures"}))
        group.questions.push_back(i);
    } else if (p.kind == ':') {
      if (beforePrev->text(src) == "class") p.space = Spacing::Inherits;
      else if (!group.questions.empty()) {
        pieces[group.questions.back()].space = p.space = Spacing::Ternary;
        group.questions.pop_back();
        group.ternary = true;
      } else if (group.index) p.space = group.ternary ? Spacing::Ternary : Spacing::Slice;
    } else if (p.kind == '<' && is(word, {"list", "map"})) {
      group.angles.push_back(i);
    } else if ((p.kind == '>' || (p.kind == T_SHR && group.angles.size() >= 2)) && !group.angles.empty()) {
      for (int n = p.kind == T_SHR ? 2 : 1; n; n--) {
        pieces[group.angles.back()].space = p.space = Spacing::Generic;
        group.angles.pop_back();
      }
    } else if (p.kind == '~' || ((p.kind == '-' || p.kind == '!') && (p.newline || !valueEnd(*prev, src)))) {
      p.space = Spacing::Unary;
    }
    // A new statement or argument cannot finish the prior statement's optional type or comparison.
    if (p.kind == ';' || (p.newline && p.kind == T_ID && prefixWord(p.text(src)))) {
      groups.back().questions.clear();
      groups.back().angles.clear();
    }
    beforePrev = prev;
    prev = &p;
  }
}

bool usesViews(std::string_view src, const std::vector<Piece>& pieces) {
  return std::any_of(pieces.begin(), pieces.end(), [&](const Piece& p) {
    return p.kind == T_ID && is(p.text(src), {"view", "state"});
  });
}

void markViewClasses(const std::string& src, std::vector<Piece>& pieces) {
  for (size_t i = 0; i + 1 < pieces.size(); i++) {
    if (pieces[i].kind != '.' || pieces[i + 1].kind != T_ID || pieces[i].end != pieces[i + 1].begin) continue;
    // The rewriter reads these names directly from the source, across any number of lexer tokens.
    size_t end = pieces[++i].end;
    while (end < src.size() && (idc(src[end]) || dig(src[end]) || src[end] == '-')) end++;
    while (i + 1 < pieces.size() && pieces[i + 1].begin < end) pieces[++i].classJoin = true;
  }
}

void markViewNumbers(std::string_view src, std::vector<Piece>& pieces) {
  // The view rewriter uses the raw text of an expression starting with a digit to tell a fixed value from a
  // live one. Leave the gaps within a possible numeric expression as written. A bare number has no such gaps.
  int depth = 0;
  bool viewHeader = false, viewBody = false;
  size_t protectedUntil = 0;
  for (size_t i = 0; i < pieces.size(); i++) {
    auto& p = pieces[i];
    if (!depth && p.kind == T_ID && p.text(src) == "view" && (!i || p.newline)) viewHeader = true;
    if (p.kind == '{' && !depth && viewHeader) viewBody = true, viewHeader = false;
    if (p.kind == '(' || p.kind == '[' || p.kind == '{') depth++;
    if (p.open != NoToken && --depth == 0) viewBody = false;
    if (!viewBody || p.kind != T_NUM || i < protectedUntil) continue;

    bool value = true;
    int nested = 0;
    size_t j = i + 1;
    for (; j < pieces.size(); j++) {
      const auto& q = pieces[j];
      int k = q.kind;
      bool atom = k == T_ID || k == T_NUM || k == T_STR;
      if (!nested && (q.newline || k == Comment || k == ',' || k == ';' || k == '{' || openerFor(k) ||
                      (k == '.' && q.begin != pieces[j - 1].end) || (value && atom))) break;
      if (k == '(' || k == '[' || k == '{') nested++;
      if (openerFor(k)) nested--;
      value = !nested && (atom || openerFor(k));
    }
    for (size_t k = i + 1; k < j; k++) pieces[k].verbatim = true;
    protectedUntil = j;
  }
}

std::string_view gapBefore(std::string_view src, const Piece& left, const Piece& right, bool views) {
  auto gap = src.substr(left.end, right.begin - left.end);
  int a = left.kind, b = right.kind;
  auto lhs = left.text(src), rhs = right.text(src);
  if (b == Comment) return !gap.empty() && gap.find_first_not_of(' ') == NoToken ? gap : "  ";
  if (views) {
    // The view/state rewriter checks adjacency here. Its entry point and implicit import/reload also check raw text.
    if ((is(lhs, {"view", "state"}) && b == T_ID) || (lhs == "fn" && is(rhs, {"reload", "mount"})) ||
        (lhs == "import" && rhs == "\"std/ui\"")) return gap;
    if (right.classJoin) return "";
    if (left.classJoin && !gap.empty() && (b == T_ID || b == T_NUM || b == '-')) return " ";
    if ((b == '(' || b == '[' || b == ':') && (valueEnd(left, src) || a == T_ID))
      return gap.empty() ? "" : " ";
    if (a == '.' || b == '.' || a == T_OPT_DOT || b == T_OPT_DOT) return gap.empty() ? "" : " ";
  }
  if (a == '{' && b == '}') return left.space == Spacing::Block && !gap.empty() ? " " : "";
  if (b == ')' || b == ']' || b == ',' || b == ';' || b == '.' || b == T_OPT_DOT || b == T_DOTDOT) return "";
  if (a == '(' || a == '[' || a == '.' || a == T_OPT_DOT || a == T_DOTDOT) return "";
  if (b == ':' && (right.space == Spacing::Normal || right.space == Spacing::Slice)) return "";
  if ((a == ':' && left.space == Spacing::Slice) || right.space == Spacing::Optional) return "";
  if (b == '{' || b == '}' || a == '{') {
    if (a == '{') return left.space == Spacing::Block ? " " : "";
    if (b == '}') return right.space == Spacing::Block ? " " : "";
    return " ";
  }
  if (right.space == Spacing::Generic || (a == '<' && left.space == Spacing::Generic)) {
    // Two separate closing angles must stay separate tokens.
    return a == '>' && b == '>' ? " " : "";
  }
  if (a == '-' && b == '>') return "";  // expect grows { n -> ... }
  if (b == '(' && lhs == "yield") return gap.empty() ? "" : " ";  // yield (value) / with yield(v)
  if ((b == '(' || b == '[') && (valueEnd(left, src) || (b == '(' && lhs == "fn"))) return "";
  if (left.space == Spacing::Unary && (a != '-' || b != '-')) return "";
  if (gap.size() >= 2 && gap.find_first_not_of(' ') == NoToken &&
      valueEnd(left, src) && b == T_ID && !is(rhs, {"in", "key", "as"}) &&
      !is(lhs, {"fn", "class", "pub", "override", "shared", "parallel", "extern", "spawn", "with", "type", "enum", "effect"}))
    return gap;  // inline statements
  return " ";
}

size_t advanceColumn(size_t column, std::string_view text) {
  size_t newline = text.rfind('\n');
  return newline == NoToken ? column + text.size() : text.size() - newline - 1;
}

std::string format(std::string_view src, const std::vector<Piece>& pieces, bool views) {
  struct Indent {
    size_t open, base;
    size_t originalAlign = 0, align = 0;
  };
  std::vector<Indent> groups;
  size_t indent = 0, column = 0, oldColumn = 0;
  std::string out;
  out.reserve(src.size() + 1);
  const Piece* prev = nullptr;
  const Piece* prevCode = nullptr;
  for (size_t i = 0; i < pieces.size(); i++) {
    const Piece& p = pieces[i];
    size_t start = prev ? prev->end : 0;
    auto gap = src.substr(start, p.begin - start);
    size_t newline = gap.rfind('\n');
    oldColumn = advanceColumn(oldColumn, gap);
    bool inBlock = groups.empty() || pieces[groups.back().open].space == Spacing::Block;
    size_t base = groups.empty() ? 0 : groups.back().base + 2;
    if (p.open != NoToken) {
      base = groups.back().base;
      groups.pop_back();
    } else if (!groups.empty() && groups.back().originalAlign == oldColumn && groups.back().align) {
      base = groups.back().align;
    }
    if (p.verbatim && newline != NoToken) {
      out += gap;
      indent = column = oldColumn;
    } else if (!prev || newline != NoToken) {
      if (prev) {
        size_t first = gap.find('\n');
        auto addBreak = [&](size_t at) {
          if (at && gap[at - 1] == '\r') out += '\r';
          out += '\n';
        };
        addBreak(first);
        if (newline != first) addBreak(newline);
      }
      indent = base;
      if (p.kind == T_ID && is(p.text(src), {"requires", "ensures"})) indent += 2;
      else if (inBlock && p.open == NoToken && p.kind != Comment && prevCode && binary(prevCode->kind) &&
               prevCode->space == Spacing::Normal) indent += 2;
      out.append(indent, ' ');
      column = indent;
    } else {
      auto space = p.verbatim ? gap : gapBefore(src, *prev, p, views);
      out += space;
      column += space.size();
      if (!groups.empty() && groups.back().open + 1 == i && p.kind != Comment &&
          pieces[groups.back().open].space != Spacing::Block) {
        groups.back().originalAlign = oldColumn;
        groups.back().align = column;
      }
    }
    if (p.kind == '(' || p.kind == '[' || p.kind == '{') groups.push_back({i, indent});
    auto word = p.text(src);
    out += word;
    column = advanceColumn(column, word);
    oldColumn = advanceColumn(oldColumn, word);
    prev = &p;
    if (p.kind != Comment) prevCode = &p;
  }
  if (prev) {
    if (std::string_view tail = src.substr(prev->end); tail.starts_with("\r\n")) out += '\r';
    out += '\n';
  }
  return out;
}

bool sameTokens(std::string_view src, const std::vector<Piece>& before, const std::string& out) {
  std::string err;
  std::vector<Piece> after;
  if (!scan(out, after, err) || before.size() != after.size()) return false;
  bool first = true;
  for (size_t i = 0; i < before.size(); i++) {
    auto& a = before[i];
    auto& b = after[i];
    if (a.kind != b.kind || a.text(src) != b.text(out) || (!first && a.newline != b.newline)) return false;
    if (a.kind != Comment) first = false;
  }
  return true;
}
}  // namespace

std::string formatSource(std::string_view src, std::string* err) {
  std::string input(src), message;
  std::vector<Piece> pieces;
  if (err) err->clear();
  if (scan(input, pieces, message)) {
    classify(input, pieces);
    bool views = usesViews(input, pieces);
    if (views) {
      markViewClasses(input, pieces);
      markViewNumbers(input, pieces);
    }
    auto out = format(input, pieces, views);
    if (sameTokens(input, pieces, out)) return out;
    message = "formatting would change the token stream or a significant line break";
  }
  if (err) *err = std::move(message);
  return input;
}

#if !__wasm__
namespace {
namespace fs = std::filesystem;
constexpr const char* Usage = "usage: fut fmt [--check] [--stdin] [file.fut | dir ...]\n";

int report(const std::string& path, const std::string& err) {
  fprintf(stderr, "fut fmt: %s: %s\n", path.c_str(), err.c_str());
  return 2;
}

bool readSource(FILE* in, std::string& src) {
  char buf[8192];
  while (size_t n = fread(buf, 1, sizeof buf, in)) src.append(buf, n);
  return !ferror(in);
}
bool readSource(const fs::path& path, std::string& src) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = readSource(f, src);
  return fclose(f) == 0 && ok;
}

std::string replaceSource(const fs::path& path, const std::string& src, const std::string& out) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return strerror(errno);
  auto temp = (path.parent_path() / ".fut-fmt-XXXXXX").string();
  int fd = mkstemp(temp.data());
  if (fd < 0) return strerror(errno);
  std::string err;
  FILE* f = fdopen(fd, "wb");
  if (!f) {
    err = strerror(errno);
    close(fd);
  } else {
    if (fwrite(out.data(), 1, out.size(), f) != out.size() || fchmod(fd, st.st_mode & 0777) != 0)
      err = strerror(errno);
    if (fclose(f) != 0 && err.empty()) err = strerror(errno);
  }
  if (err.empty()) {
    std::string current;
    if (!readSource(path, current)) err = strerror(errno);
    else if (current != src) err = "file changed while formatting";
    else if (rename(temp.c_str(), path.c_str()) == 0) return {};
    else err = strerror(errno);
  }
  unlink(temp.c_str());
  return err;
}
}  // namespace

int fmtCommand(int argc, char** argv) {
  bool check = false, fromStdin = false, options = true;
  std::vector<fs::path> paths;
  for (int i = 0; i < argc; i++) {
    std::string_view arg = argv[i];
    if (options && arg == "--check") check = true;
    else if (options && arg == "--stdin") fromStdin = true;
    else if (options && arg == "--") options = false;
    else if (options && arg.starts_with('-')) return fputs(Usage, stderr), 2;
    else paths.emplace_back(arg);
  }
  if (fromStdin) {
    if (!paths.empty()) return fputs(Usage, stderr), 2;
    std::string src, err;
    if (!readSource(stdin, src)) return report("<stdin>", strerror(errno));
    auto out = formatSource(src, &err);
    if (!err.empty()) return report("<stdin>", err);
    if (check) {
      if (out == src) return 0;
      return puts("<stdin>"), 1;
    }
    if (fwrite(out.data(), 1, out.size(), stdout) != out.size() || fflush(stdout) != 0)
      return report("<stdout>", strerror(errno));
    return 0;
  }
  if (paths.empty()) paths.emplace_back(".");
  std::map<fs::path, fs::path> files;  // canonical target -> path as requested, in a stable order
  int status = 0;
  auto add = [&](const fs::path& path) {
    std::error_code ec;
    auto real = fs::canonical(path, ec);
    if (ec) status = report(path.string(), ec.message());
    else files.try_emplace(real, path.lexically_normal());
  };
  for (auto& path : paths) {
    std::error_code ec;
    auto info = fs::status(path, ec);
    if (ec) status = report(path.string(), ec.message());
    else if (fs::is_regular_file(info) && path.extension() == ".fut") add(path);
    else if (fs::is_directory(info)) {
      for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".fut" && fs::is_regular_file(it->symlink_status(ec))) add(it->path());
      }
      if (ec) status = report(path.string(), ec.message());
    } else status = report(path.string(), "expected a .fut file or directory");
  }
  for (const auto& [real, path] : files) {
    std::string src, err;
    if (!readSource(real, src)) { status = report(path.string(), strerror(errno)); continue; }
    auto out = formatSource(src, &err);
    if (!err.empty()) { status = report(path.string(), err); continue; }
    if (out == src) continue;
    if (check) puts(path.c_str()), status = std::max(status, 1);
    else if (err = replaceSource(real, src, out); !err.empty()) status = report(path.string(), err);
  }
  return status;
}
#endif
