// External scanner for tree-sitter-gcon (from tree-sitter-icon): semicolon insertion.
//
// Icon's lexer (icon/src/common/yylex.h) inserts a semicolon between two
// tokens on different lines when the first can end an expression and the
// second can begin one (flags in icon/src/common/lextab.h). The grammar
// only allows an automatic semicolon directly after a complete expression,
// and every complete Icon expression ends in a token that can end one, so
// here it remains to check that a newline follows and that the next token
// can begin an expression.

#include "tree_sitter/parser.h"
#include <string.h>

enum TokenType { AUTOMATIC_SEMICOLON };

void *tree_sitter_gcon_external_scanner_create(void) { return NULL; }
void tree_sitter_gcon_external_scanner_destroy(void *p) {}
unsigned tree_sitter_gcon_external_scanner_serialize(void *p, char *b) { return 0; }
void tree_sitter_gcon_external_scanner_deserialize(void *p, const char *b, unsigned n) {}

static void skip(TSLexer *lexer) { lexer->advance(lexer, true); }
static void next(TSLexer *lexer) { lexer->advance(lexer, false); }

static bool is_space(int32_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

static bool is_letter(int32_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(int32_t c) { return c >= '0' && c <= '9'; }

// Reserved words that cannot begin an expression.
static const char *non_beginning_words[] = {
  "by", "do", "else", "global", "invocable", "link", "of", "procedure",
  "record", "then", "to", NULL,
};

// Operators (longest first within each prefix family is not needed: we
// look for the longest operator that is a prefix of the input) and whether
// each can begin an expression.
static const struct { const char *op; bool begins; } operators[] = {
  {"!", true}, {"%", false}, {"%:=", false}, {"&", true}, {"&:=", false},
  {"*", true}, {"*:=", false}, {"**", true}, {"**:=", false},
  {"+", true}, {"+:=", false}, {"++", true}, {"++:=", false}, {"+:", false},
  {"-", true}, {"-:=", false}, {"--", true}, {"--:=", false}, {"-:", false},
  {".", true}, {"/", true}, {"/:=", false},
  {":=", false}, {":=:", false}, {":", false},
  {"<", false}, {"<:=", false}, {"<-", false}, {"<->", false},
  {"<<", false}, {"<<:=", false}, {"<<=", false}, {"<<=:=", false},
  {"<=", false}, {"<=:=", false},
  {"=", true}, {"=:=", false}, {"==", true}, {"==:=", false},
  {"===", true}, {"===:=", false},
  {">", false}, {">:=", false}, {">=", false}, {">=:=", false},
  {">>", false}, {">>:=", false}, {">>=", false}, {">>=:=", false},
  {"?", true}, {"?:=", false}, {"@", true}, {"@:=", false},
  {"\\", true}, {"^", true}, {"^:=", false},
  {"|", true}, {"||", true}, {"||:=", false}, {"|||", true}, {"|||:=", false},
  {"~", true}, {"~=", true}, {"~=:=", false}, {"~==", true}, {"~==:=", false},
  {"~===", true}, {"~===:=", false},
  {NULL, false},
};

static bool is_op_char(int32_t c) {
  return c && strchr("!%&*+-./:<=>?@\\^|~", (int)c) != NULL;
}

// begins_expression reports whether the token starting at the lexer's
// position can begin an expression. It advances the lexer (the caller has
// already marked the end of the zero-width semicolon token).
static bool begins_expression(TSLexer *lexer) {
  int32_t c = lexer->lookahead;
  if (is_letter(c)) {
    char word[16];
    unsigned n = 0;
    while (is_letter(lexer->lookahead) || is_digit(lexer->lookahead)) {
      if (n < sizeof word - 1) word[n++] = (char)lexer->lookahead;
      else n = sizeof word; // too long to be a reserved word
      next(lexer);
    }
    if (n >= sizeof word) return true;
    word[n] = 0;
    for (const char **w = non_beginning_words; *w; w++) {
      if (strcmp(word, *w) == 0) return false;
    }
    return true;
  }
  if (is_digit(c) || c == '"' || c == '\'' || c == '(' || c == '[' || c == '{') {
    return true;
  }
  if (c == '$') { // digraphs $( $< begin; $) $> do not
    next(lexer);
    return lexer->lookahead == '(' || lexer->lookahead == '<';
  }
  if (!is_op_char(c)) return false;
  // Longest operator that is a prefix of the input, without backtracking
  // (as icont's getopr).
  char buf[8];
  unsigned n = 0;
  while (n < sizeof buf - 1 && is_op_char(lexer->lookahead)) {
    buf[n++] = (char)lexer->lookahead;
    buf[n] = 0;
    bool prefix = false;
    for (int i = 0; operators[i].op; i++) {
      if (strncmp(operators[i].op, buf, n) == 0) { prefix = true; break; }
    }
    if (!prefix) { n--; break; }
    next(lexer);
  }
  buf[n] = 0;
  for (int i = 0; operators[i].op; i++) {
    if (strcmp(operators[i].op, buf) == 0) return operators[i].begins;
  }
  return false;
}

bool tree_sitter_gcon_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
  if (!valid_symbols[AUTOMATIC_SEMICOLON]) return false;
  lexer->result_symbol = AUTOMATIC_SEMICOLON;
  lexer->mark_end(lexer); // the semicolon is zero-width, here

  bool newline = false;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (lexer->eof(lexer)) return false;
    if (c == '\n') {
      newline = true;
      skip(lexer);
      // A preprocessor directive line counts as a newline (ipp replaces it
      // with one).
      while (is_space(lexer->lookahead)) skip(lexer);
      if (lexer->lookahead == '$') {
        skip(lexer);
        while (is_space(lexer->lookahead)) skip(lexer);
        if (is_letter(lexer->lookahead)) {
          while (lexer->lookahead && lexer->lookahead != '\n' && !lexer->eof(lexer)) skip(lexer);
          continue;
        }
        // A digraph: "$(" or "$<" begins an expression.
        if (!newline) return false;
        return lexer->lookahead == '(' || lexer->lookahead == '<';
      }
      continue;
    }
    if (is_space(c)) {
      skip(lexer);
      continue;
    }
    if (c == '#') {
      while (lexer->lookahead != '\n' && !lexer->eof(lexer)) skip(lexer);
      continue;
    }
    break;
  }
  if (!newline) return false;
  return begins_expression(lexer);
}
