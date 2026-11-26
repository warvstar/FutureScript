#include "tree_sitter/parser.h"

enum Token {
  INLINE_PAREN,
  INLINE_BRACKET,
  INLINE_MINUS,
  INLINE_BRACE,
  INLINE_VALUE,
  PUBLIC_DECLARATION,
  GPU_DECLARATION,
  VIEW_END,
  VIEW_CLASS_NAME,
  VIEW_TAG,
};

void *tree_sitter_fut_external_scanner_create(void) { return NULL; }
void tree_sitter_fut_external_scanner_destroy(void *payload) {}
unsigned tree_sitter_fut_external_scanner_serialize(void *payload, char *buffer) { return 0; }
void tree_sitter_fut_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {}

static bool identifier_start(int32_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static bool digit(int32_t c) {
  return c >= '0' && c <= '9';
}

static bool horizontal(int32_t c) {
  return c == ' ' || c == '\t' || c == '\r';
}

static bool word_is(const char *word, unsigned length, const char *keyword) {
  for (unsigned i = 0; i < length; ++i) {
    if (word[i] != keyword[i]) return false;
  }
  return keyword[length] == '\0';
}

// After pub or gpu, look through horizontal whitespace for a declaration keyword.
// The marker is zero width, so the parser still reads that keyword itself.
static bool declaration_follows(TSLexer *lexer, bool gpu) {
  char word[6];
  unsigned length = 0;
  while (identifier_start(lexer->lookahead) || digit(lexer->lookahead)) {
    if (length == sizeof(word)) return false;
    word[length++] = (char)lexer->lookahead;
    lexer->advance(lexer, false);
  }
  if (gpu) return word_is(word, length, "fn");

  const char *keywords[] = {"var", "fn", "class", "extern", "effect", "enum", "import", "type", "gpu"};
  for (unsigned i = 0; i < sizeof(keywords) / sizeof(*keywords); ++i) {
    if (word_is(word, length, keywords[i])) return true;
  }
  return false;
}

static bool view_tag(TSLexer *lexer) {
  while (horizontal(lexer->lookahead) || lexer->lookahead == '\n') lexer->advance(lexer, true);
  if (!identifier_start(lexer->lookahead)) return false;

  char word[6];
  unsigned length = 0;
  do {
    if (length < sizeof(word)) word[length++] = (char)lexer->lookahead;
    lexer->advance(lexer, false);
  } while (identifier_start(lexer->lookahead) || digit(lexer->lookahead));
  lexer->mark_end(lexer);

  if (word_is(word, length, "if") || word_is(word, length, "else") ||
      word_is(word, length, "for") || word_is(word, length, "var")) return false;
  // Row(item), ui.make(...) and assignments are fut code in a children block.
  if (lexer->lookahead == '(' || lexer->lookahead == '.') return false;
  while (horizontal(lexer->lookahead)) lexer->advance(lexer, false);
  if (word_is(word, length, "state") && identifier_start(lexer->lookahead)) return false;
  int32_t op = lexer->lookahead;
  if (op == '=' || op == '+' || op == '-' || op == '*' || op == '/' || op == '%') {
    lexer->advance(lexer, false);
    if (op == '=' ? lexer->lookahead != '=' : lexer->lookahead == '=') return false;
  }
  return true;
}

bool tree_sitter_fut_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid) {
  bool space = false;
  while (horizontal(lexer->lookahead)) {
    lexer->advance(lexer, true);
    space = true;
  }

  int32_t c = lexer->lookahead;
  lexer->mark_end(lexer);

  // A view element without children ends on this line or at its parent's closing brace.
  if (valid[VIEW_END]) {
    if (c == '\n' || c == '}' || lexer->eof(lexer)) {
      lexer->result_symbol = VIEW_END;
      return true;
    }
    if (c == '/') {
      lexer->advance(lexer, false);
      if (lexer->lookahead != '/') return false;
      lexer->result_symbol = VIEW_END;
      return true;
    }
  }

  // In views a spaced .class starts a new part, while expr.field stays in the expression.
  if (valid[VIEW_CLASS_NAME] && space && c == '.') {
    lexer->advance(lexer, false);
    if (!identifier_start(lexer->lookahead)) return false;
    do {
      lexer->advance(lexer, false);
    } while (identifier_start(lexer->lookahead) || digit(lexer->lookahead) || lexer->lookahead == '-');
    lexer->mark_end(lexer);
    lexer->result_symbol = VIEW_CLASS_NAME;
    return true;
  }

  if (space && identifier_start(c) && (valid[PUBLIC_DECLARATION] || valid[GPU_DECLARATION])) {
    bool gpu = valid[GPU_DECLARATION];
    if (!declaration_follows(lexer, gpu)) return false;
    lexer->result_symbol = gpu ? GPU_DECLARATION : PUBLIC_DECLARATION;
    return true;
  }

  // These continuations can begin a new statement on the next line instead.
  if (valid[INLINE_VALUE] && (identifier_start(c) || digit(c) || c == '"' ||
      c == '(' || c == '[' || c == '{' || c == '-' || c == '!' || c == '~')) {
    lexer->result_symbol = INLINE_VALUE;
    return true;
  }

  if (valid[INLINE_PAREN] && c == '(') lexer->result_symbol = INLINE_PAREN;
  else if (valid[INLINE_BRACKET] && c == '[') lexer->result_symbol = INLINE_BRACKET;
  else if (valid[INLINE_BRACE] && c == '{') lexer->result_symbol = INLINE_BRACE;
  else if (valid[INLINE_MINUS] && c == '-') {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '=' || lexer->lookahead == '>') return false;
    lexer->mark_end(lexer);
    lexer->result_symbol = INLINE_MINUS;
    return true;
  } else {
    if (!valid[VIEW_TAG] || !view_tag(lexer)) return false;
    lexer->result_symbol = VIEW_TAG;
    return true;
  }

  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  return true;
}
