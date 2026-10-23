// Layer 1: the token stream (PLAN L3.1b). Tokens keep their trivia in raw
// file coordinates, so the stream rebuilds the input file byte for byte.

#ifndef CHIBICC_TOKEN_H
#define CHIBICC_TOKEN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <chibicc/context.h>

typedef struct chibicc_type ChibiccType;

// Private to preprocess.c / tokenize.c; a token only carries pointers.
typedef struct chibicc_hideset ChibiccHideset;
typedef struct chibicc_macro_prov ChibiccMacroProv;

// ChibiccToken
typedef enum {
  TK_IDENT,   // Identifiers
  TK_PUNCT,   // Punctuators
  TK_KEYWORD, // Keywords
  TK_STR,     // String literals
  TK_NUM,     // Numeric literals
  TK_PP_NUM,  // Preprocessing numbers
  TK_EOF,     // End-of-file markers
} ChibiccTokenKind;

typedef struct chibicc_file {
  char *name;
  int file_no;

  // 0 for a top-level file or a buffer; one more than the including
  // file for a file opened by #include.
  int include_depth;

  // `raw` is the text as read. `contents` is the buffer the lexer scans:
  // a derivative of `raw` with line breaks canonicalized to '\n', a
  // backslash before a line break removed together with it, universal
  // character names rewritten, a leading BOM skipped and a final newline
  // guaranteed (the rewrites of the T1-era pre-passes, kept so that every
  // consumer sees the text it always saw). `map` translates a scan offset
  // to an offset into `raw`, `lines` gives the line number of a raw
  // offset; both have one entry per byte plus a final one. For synthetic
  // buffers (paste, string tokens) the two texts coincide: `map` is the
  // identity and `lines` counts the newlines.
  char *contents;
  char *raw;
  int *map;
  int *lines;

  // For #line directive
  char *display_name;
  int line_delta;
} ChibiccFile;

// ChibiccToken type
typedef struct chibicc_token ChibiccToken;
struct chibicc_token {
  ChibiccTokenKind kind;   // ChibiccToken kind
  ChibiccToken *next;      // Next token
  int64_t val;      // If kind is TK_NUM, its value
  long double fval; // If kind is TK_NUM, its value
  char *loc;        // ChibiccToken location (in the file's scan buffer)
  int len;          // ChibiccToken length

  // The bytes between the previous token and this one that the lexer
  // skipped instead of tokenizing (whitespace, newlines, comments,
  // backslash-continuations) and the token's own spelling, both in raw
  // file coordinates: the trivia of the tokens plus their spellings cover
  // the raw text byte for byte, so the stream rebuilds the file (the
  // leading trivia of the first token includes the BOM, if any; the
  // final newline read_file synthesizes is not part of the file and does
  // not appear here). Tokens created outside tokenize()'s skip loop
  // (paste, tokenize_string_literal) carry the coordinates of the token
  // they are derived from.
  char *trivia;     // Start of the skipped bytes before this token
  int trivia_len;   // Number of skipped bytes before this token
  char *raw_loc;    // Start of the token's spelling in the raw text
  int raw_len;      // Length of the token's spelling in the raw text

  ChibiccType *ty;         // Used if TK_NUM or TK_STR
  char *str;        // String literal contents including terminating '\0'

  ChibiccFile *file;       // Source location
  char *filename;   // Filename
  int line_no;      // Line number
  int line_delta;   // Line number
  bool at_bol;      // True if this token is at beginning of line
  bool has_space;   // True if this token follows a space character
  ChibiccHideset *hideset; // For macro expansion
  ChibiccToken *origin;    // If this is expanded from a macro, the original token
  ChibiccMacroProv *prov;  // How this token came to be: the chain of macro
                    // expansions that produced it, innermost layer
                    // first, or NULL for a token read from the source
                    // (preprocess.c owns the chain; the accessors below
                    // are the public face)
};

CHIBICC_API ChibiccToken *chibicc_tokenize_file(ChibiccContext *ctx, char *path);
CHIBICC_API ChibiccToken *chibicc_tokenize_buffer(ChibiccContext *ctx, char *name, char *contents);

// Like chibicc_tokenize_buffer, but reads exactly `len` bytes. An
// embedded NUL is a diagnostic rather than a silent truncation.
CHIBICC_API ChibiccToken *chibicc_tokenize_buffer_len(ChibiccContext *ctx, char *name, char *contents, int len);

// Print the bytes the token stream was tokenized from: the trivia in
// front of every token plus the token spellings, in raw coordinates (the
// trailing trivia rides on EOF). On tokenize()'s direct output the two
// cover the file's raw text byte for byte, so this rebuilds the input
// text - with no exceptions: the rewrites that used to be pre-passes
// live in the scan buffer and the map now, not in the coordinates.
CHIBICC_API void chibicc_reconstruct(ChibiccContext *ctx, ChibiccToken *tok, FILE *out);

#endif
