#include "lexer.h"
#include "error.h"
#include "lib/alloc.h"
#include "stringview.h"
#include "token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lexer_t new_lexer(char *filename) {
  lexer_t res;
  res.col = 1;
  res.line = 1;
  res.cursor = 0;
  FILE *f = fopen(filename, "rb");
  if (f == NULL) {
    printf("Could not open file '%s': ", filename);
    fflush(stdout);
    perror("");
    exit(1);
  }
  res.filename = allocate_compiler_persistent(strlen(filename) + 1);
  strcpy(res.filename, filename);
  fseek(f, 0, SEEK_END);
  size_t file_size = ftell(f) + 1;
  fseek(f, 0, SEEK_SET);
  res.data = allocate_compiler_persistent(file_size);
  res.length = file_size;
  fread(res.data, 1, file_size, f);
  fclose(f);
  return res;
}

static char delimeters[] = "><;,:-+*/%&|!=(){}^. \n\'\"[]@";

int is_delimeter(char c) {
  for (unsigned int i = 0; i < sizeof(delimeters); i++)
    if (c == delimeters[i])
      return 1;
  return 0;
}

char lexer_peek(lexer_t l) {
  if (l.cursor < l.length)
    return l.data[l.cursor];
  else {
    printf("Problem !\n");
    exit(1);
    return 0;
  }
}

int is_whitespace(char c) { return c == ' ' || c == '\n' || c == '\t'; }

void lexer_consume(lexer_t *l) {
  char c = lexer_peek(*l);
  if (!c)
    return;
  if (c == '\n') {
    l->col = 1;
    l->line++;
  } else
    l->col++;
  l->cursor++;
}

int length_until_next_delimiter(lexer_t l) {
  int i = 0;
  while (!is_delimeter(lexer_peek(l))) {
    lexer_consume(&l);
    i++;
  }
  return i;
}

int length_of_delimiter(lexer_t l) {
  char c1 = lexer_peek(l);
  lexer_consume(&l);
  char c2 = lexer_peek(l);
  char str[3] = {c1, c2, 0};
  if (strcmp(str, "->") == 0)
    return 2;
  if (strcmp(str, "=>") == 0)
    return 2;
  if (strcmp(str, "&&") == 0)
    return 2;
  if (strcmp(str, "||") == 0)
    return 2;
  if (strcmp(str, ">=") == 0)
    return 2;
  if (strcmp(str, "<=") == 0)
    return 2;
  if (strcmp(str, "!=") == 0)
    return 2;
  if (strcmp(str, "::") == 0)
    return 2;
  if (strcmp(str, ":=") == 0)
    return 2;
  if (strcmp(str, "[]") == 0)
    return 2;
  if (c1 == ':' || c1 == ',' || c1 == '/' || c1 == '%' || c1 == '*' ||
      c1 == '+' || c1 == '-' || c1 == '^' || c1 == '{' || c1 == '}' ||
      c1 == '(' || c1 == ')' || c1 == '=' || c1 == '<' || c1 == '>' ||
      c1 == ';' || c1 == '.' || c1 == '[' || c1 == ']' )
    return 1;

  return -1;
}

int is_char_num(char c) { return c <= '9' && c >= '0'; }

int length_of_num_lit(lexer_t l) {
  int cter = 0;
  while (is_char_num(lexer_peek(l))) {
    cter++;
    lexer_consume(&l);
  }
  /* Optional fractional part: exactly one dot followed by at least one
   * digit. A trailing dot without a digit is left to the caller so we
   * do not eat record-field access like `arr.0` (Rock does not have
   * number.field semantics, but the principle stands). */
  if (lexer_peek(l) == '.') {
    lexer_t peek = l;
    lexer_consume(&peek);
    if (is_char_num(lexer_peek(peek))) {
      cter++;                 /* the '.' */
      lexer_consume(&l);
      while (is_char_num(lexer_peek(l))) {
        cter++;
        lexer_consume(&l);
      }
    }
  }
  return cter;
}

int length_of_delimited_literal(lexer_t l, char c) {
  int cursor = l.cursor;
  char c1 = lexer_peek(l);
  if (c1 != c)
    return 0;
  lexer_consume(&l);
  int pass = 0;
  while (lexer_peek(l) != c || pass) {
    if (pass)
      pass = 0;
    else if (lexer_peek(l) == '\\')
      pass = 1;
    lexer_consume(&l);
  }
  return l.cursor - cursor + 1;
}

string_view create_lexeme(lexer_t l, int length) {
  // char *s = allocate_compiler_persistent(length + 1);
  // if (s == NULL) {
  //   printf("Could not allocate memory\n");
  //   exit(1);
  // }
  // for (int i = 0; i < length; i++) {
  //   s[i] = l.data[l.cursor + i];
  // }
  // s[length] = 0;
  // return s;
  return sv_from_parts(l.data + l.cursor, length);
}
typedef enum comment_type_t { COM_SINGLE = 1, COM_MULTI } comment_type_t;

int is_comment(lexer_t l) {
  char c1 = lexer_peek(l);
  lexer_consume(&l);
  char c2 = lexer_peek(l);
  if (c1 != '/')
    return 0;
  if (c2 == '/')
    return COM_SINGLE;
  if (c2 == '*')
    return COM_MULTI;
  return 0;
}

int is_end_comment(lexer_t l, comment_type_t type) {
  if (type == 0)
    return 0;
  if (type == COM_SINGLE) {
    if (lexer_peek(l) == '\n')
      return 1;
  }

  else {
    char c1 = lexer_peek(l);
    lexer_consume(&l);
    char c2 = lexer_peek(l);
    return c1 == '*' && c2 == '/';
  }
  return 0;
}

void lexer_consume_n(lexer_t *l, int n) {
  for (int i = 0; i < n; i++)
    lexer_consume(l);
}

void skip_whitespace(lexer_t *l) {
  while (l->cursor < l->length && (l->data[l->cursor] == ' ' || l->data[l->cursor] == '\t'))
    l->cursor++;
}

void skip_newline(lexer_t *l) {
  if (l->cursor < l->length && l->data[l->cursor] == '\n') {
    l->cursor++;
    l->line++;
    l->col = 1;
  } else if (l->cursor < l->length && l->data[l->cursor] == '\r') {
    l->cursor++;
    if (l->cursor < l->length && l->data[l->cursor] == '\n')
      l->cursor++;
    l->line++;
    l->col = 1;
  }
}

void skip_whitespace_and_newline(lexer_t *l) {
  skip_whitespace(l);
  skip_newline(l);
}

int match_word_at_cursor(lexer_t l, const char *word) {
  int word_len = strlen(word);
  for (int i = 0; i < word_len; i++) {
    if (l.cursor + i >= l.length) return 0;
    if (l.data[l.cursor + i] != word[i]) return 0;
  }
  // Check that the character after the word is not alphanumeric
  if (l.cursor + word_len < l.length) {
    char next_char = l.data[l.cursor + word_len];
    if ((next_char >= 'a' && next_char <= 'z') ||
        (next_char >= 'A' && next_char <= 'Z') ||
        (next_char >= '0' && next_char <= '9') ||
        next_char == '_')
      return 0;
  }
  return 1;
}

string_view read_embed_body(lexer_t *l, const char *lang __attribute__((unused))) {
  int start_cursor = l->cursor;

  // Accumulate bytes until we find @end (optionally followed by lang)
  while (l->cursor < l->length) {
    // Check for @end
    if (l->data[l->cursor] == '@' && l->cursor + 3 < l->length &&
        match_word_at_cursor(*l, "@end")) {
      // Found @end, extract body without it
      int body_length = l->cursor - start_cursor;
      string_view body = sv_from_parts(l->data + start_cursor, body_length);
      return body;
    }
    lexer_consume(l);
  }

  // If we reach here, we didn't find @end - error will be handled by parser
  return sv_from_parts(l->data + start_cursor, l->cursor - start_cursor);
}

token_t step_lexer(lexer_t *l) {
  token_t res = {0, {0}, 0, 0, NULL, NULL, NULL};

  // Possible cases:
  // - we have a comment
  // - we have a literal
  // - we have a delimiter
  // - we have a keyword
  // - we have an identifier

  // First case: Comment
  while (is_comment(*l) || is_whitespace(lexer_peek(*l))) {
    int is_com_type = is_comment(*l);

    if (is_com_type) {
      lexer_consume_n(l, 2); // consume the initial comment 'declaration'
      while (!is_end_comment(*l, is_com_type)) {
        lexer_consume(l);
      }
      if (is_com_type == COM_MULTI)
        lexer_consume(l);
      lexer_consume(l); // consume the matching end;
    }
    while (is_whitespace(lexer_peek(*l)))
      lexer_consume(l);
  }
  if (lexer_peek(*l) == 0) {
    return (token_t){0};
  }
  res.col = l->col;
  res.line = l->line;
  res.filename = l->filename;
  res.embed_body = NULL;
  res.embed_lang = NULL;
  // The case must fall through
  // Second case: Num Literal
  if (is_char_num(lexer_peek(*l))) {
    int length = length_of_num_lit(*l);
    res.lexeme = create_lexeme(*l, length);
    res.type = TOK_NUM_LIT;
    res.embed_body = NULL;
    res.embed_lang = NULL;
    lexer_consume_n(l, length);
  }
  // Third case: Char Literal
  else if (lexer_peek(*l) == '\'') {
    int length = length_of_delimited_literal(*l, '\'');
    res.lexeme = create_lexeme(*l, length);
    res.type = TOK_CHR_LIT;
    res.embed_body = NULL;
    res.embed_lang = NULL;
    lexer_consume_n(l, length);
  }
  // Fourth case: String Literal
  else if (lexer_peek(*l) == '\"') {
    int length = length_of_delimited_literal(*l, '\"');
    res.lexeme = create_lexeme(*l, length);
    res.type = TOK_STR_LIT;
    res.embed_body = NULL;
    res.embed_lang = NULL;
    lexer_consume_n(l, length);
  }
  // Fifth case: Special handling for @embed and @end (MUST come before delimiter check)
  else if (lexer_peek(*l) == '@') {
    // Peek ahead to see if this is @embed or @end
    int start_cursor = l->cursor;
    lexer_consume(l);  // consume @

    if (!is_delimeter(lexer_peek(*l))) {
      // Next char is part of identifier
      int id_length = length_until_next_delimiter(*l);
      string_view next_id = create_lexeme(*l, id_length);

      if (svcmp(next_id, sv_from_cstr("embed")) == 0) {
        // Found @embed - extract language and body
        lexer_consume_n(l, id_length);
        res.lexeme = sv_from_cstr("@embed");
        res.type = TOK_EMBED;

        // Extract optional language ("c" or "asm"), default to "c"
        int temp_cursor = l->cursor;
        while (temp_cursor < l->length && (l->data[temp_cursor] == ' ' || l->data[temp_cursor] == '\t'))
          temp_cursor++;

        char *lang = "c";
        // Check if next token is "c" or "asm"
        if (temp_cursor < l->length && ((strncmp(&l->data[temp_cursor], "c\n", 2) == 0 ||
                                         strncmp(&l->data[temp_cursor], "c\r", 2) == 0 ||
                                         strncmp(&l->data[temp_cursor], "c ", 2) == 0 ||
                                         strncmp(&l->data[temp_cursor], "c\t", 2) == 0) ||
                                        (strncmp(&l->data[temp_cursor], "asm\n", 4) == 0 ||
                                         strncmp(&l->data[temp_cursor], "asm\r", 4) == 0 ||
                                         strncmp(&l->data[temp_cursor], "asm ", 4) == 0 ||
                                         strncmp(&l->data[temp_cursor], "asm\t", 4) == 0))) {
          // It's "c" or "asm" - consume it
          if (l->data[temp_cursor] == 'c' && (l->data[temp_cursor + 1] == '\n' || l->data[temp_cursor + 1] == '\r' ||
                                              l->data[temp_cursor + 1] == ' ' || l->data[temp_cursor + 1] == '\t')) {
            lang = allocate_compiler_persistent(2);
            strcpy(lang, "c");
            temp_cursor++;
          } else if (strncmp(&l->data[temp_cursor], "asm", 3) == 0 && (l->data[temp_cursor + 3] == '\n' ||
                                                                         l->data[temp_cursor + 3] == '\r' ||
                                                                         l->data[temp_cursor + 3] == ' ' ||
                                                                         l->data[temp_cursor + 3] == '\t')) {
            lang = allocate_compiler_persistent(4);
            strcpy(lang, "asm");
            temp_cursor += 3;
          }
        } else {
          lang = allocate_compiler_persistent(2);
          strcpy(lang, "c");
        }

        l->cursor = temp_cursor;
        skip_whitespace_and_newline(l);

        // Extract body until @end
        int body_start = l->cursor;
        while (l->cursor < l->length - 3) {
          if (l->data[l->cursor] == '@' && strncmp(&l->data[l->cursor], "@end", 4) == 0)
            break;
          lexer_consume(l);
        }

        int body_length = l->cursor - body_start;
        char *body = allocate_compiler_persistent(body_length + 1);
        if (body_length > 0) {
          strncpy(body, &l->data[body_start], body_length);
        }
        body[body_length] = 0;


        // Skip @end
        if (l->cursor < l->length - 3 && strncmp(&l->data[l->cursor], "@end", 4) == 0) {
          l->cursor += 4;
          l->col += 4;

          // Skip optional language name after @end
          skip_whitespace(l);
          while (l->cursor < l->length && !is_delimeter(l->data[l->cursor]))
            lexer_consume(l);
        }

        res.embed_body = body;
        res.embed_lang = lang;
      } else if (svcmp(next_id, sv_from_cstr("end")) == 0) {
        // Found @end - consume the "end" part and return TOK_END
        lexer_consume_n(l, id_length);
        res.lexeme = sv_from_cstr("@end");
        res.type = TOK_END;
        res.embed_body = NULL;
        res.embed_lang = NULL;
      } else {
        // Just a @ delimiter, reset and parse as normal
        l->cursor = start_cursor;
        int length = length_of_delimiter(*l);
        res.lexeme = create_lexeme(*l, length);
        res.type = type_of_lexeme(res.lexeme);
        lexer_consume_n(l, length);
        res.embed_body = NULL;
        res.embed_lang = NULL;
      }
    } else {
      // Just a @ delimiter
      l->cursor = start_cursor;
      int length = length_of_delimiter(*l);
      res.lexeme = create_lexeme(*l, length);
      res.type = type_of_lexeme(res.lexeme);
      lexer_consume_n(l, length);
      res.embed_body = NULL;
      res.embed_lang = NULL;
    }
  }
  // Sixth case: Delimeter
  else if (is_delimeter(lexer_peek(*l))) {
    int length = length_of_delimiter(*l);
    res.lexeme = create_lexeme(*l, length);
    res.type = type_of_lexeme(res.lexeme);
    res.embed_body = NULL;
    res.embed_lang = NULL;
    lexer_consume_n(l, length);
  }
  // Seventh case: Keyword or Identifier
  else {
    int length = length_until_next_delimiter(*l);
    res.lexeme = create_lexeme(*l, length);
    if (is_lexeme_keyword(res.lexeme))
      res.type = type_of_lexeme(res.lexeme);
    else
      res.type = TOK_IDENTIFIER;
    res.embed_body = NULL;
    res.embed_lang = NULL;
    lexer_consume_n(l, length);
  }
  if (res.type == TOK_IDENTIFIER && res.lexeme.length >= 9 &&
      strncmp(res.lexeme.data, "rock__tm_", 9) == 0) {
    error(res.filename, res.line, res.col,
          "identifiers beginning with 'rock__tm_' are reserved by the compiler");
    exit(1);
  }
  return res;
}

token_array_t lex_program(lexer_t *l) {
  if (l == NULL) // to make valgrind happy
    exit(1);
  token_array_t arr = new_token_array();
  while (lexer_peek(*l)) {
    token_t t = step_lexer(l);
    if (t.lexeme.data)
      token_array_push(&arr, t);
  }
  // Add EOF token to mark end of stream
  token_t eof_token = {
    .type = TOK_EOF,
    .lexeme = sv_from_cstr("<EOF>"),
    .filename = l->filename,
    .line = l->line,
    .col = l->col
  };
  token_array_push(&arr, eof_token);
  return arr;
}
