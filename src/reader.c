/* tsc
 * Copyright (c) 2021 Niels Sonnich Poulsen (http://nielssp.dk)
 * Licensed under the MIT license.
 * See the LICENSE file or http://opensource.org/licenses/MIT for more information.
 */

#include "reader.h"

#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define BUFFER_SIZE 32


#define SGR_RESET "\001\033[0m\002"
#define SGR_RED "\001\033[31m\002"
#define SGR_BOLD "\001\033[1m\002"

typedef struct ParenStack ParenStack;

struct ParenStack {
  ParenStack *next;
  uint8_t paren;
};

struct Reader {
  FILE *file;
  char *file_name;
  ParenStack *parens;
  int line;
  int column;
  int errors;
  int la;
  uint8_t buffer[3];
};

typedef struct {
  uint8_t *data;
  size_t capacity;
  size_t size;
} Buffer;

static char *copy_string(const char *src) {
  size_t l = strlen(src) + 1;
  char *dest = allocate(l);
  memcpy(dest, src, l);
  return dest;
}

Reader *open_reader(FILE *file, const char *file_name) {
  Reader *r = allocate(sizeof(Reader));
  r->file = file;
  r->file_name = copy_string(file_name);
  r->parens = NULL;
  r->line = 1;
  r->column = 1;
  r->la = 0;
  return r;
}

void close_reader(Reader *r) {
  free(r);
}

int reader_errors(Reader *r) {
  return r->errors;
}

static uint8_t get_top_paren(Reader *r) {
  if (r->parens) {
    return r->parens->paren;
  }
  return 0;
}

static uint8_t pop_paren(Reader *r) {
  uint8_t top = 0;
  if (r->parens) {
    ParenStack *p = r->parens;
    r->parens = p->next;
    top = p->paren;
    free(p);
  }
  return top;
}

static void push_paren(Reader *r, uint8_t paren) {
  ParenStack *p = allocate(sizeof(ParenStack));
  p->next = r->parens;
  p->paren = paren;
  r->parens = p;
}

static void clear_parens(Reader *r) {
  while (r->parens) {
    pop_paren(r);
  }
}

static void reader_error(Reader *r, const char *format, ...) {
  va_list va;
  fprintf(stderr, SGR_BOLD "%s:%d:%d: " SGR_RED "error: " SGR_RESET SGR_BOLD, r->file_name, r->line, r->column);
  va_start(va, format);
  vfprintf(stderr, format, va);
  va_end(va);
  fprintf(stderr, "\n" SGR_RESET);
}

static Buffer create_buffer() {
  return (Buffer) { .data = allocate(BUFFER_SIZE), .capacity = BUFFER_SIZE, .size = 0 };
}

static void delete_buffer(Buffer buffer) {
  free(buffer.data);
}

static void buffer_put(Buffer *buffer, uint8_t byte) {
  if (buffer->size >= buffer->capacity) {
    buffer->capacity += BUFFER_SIZE;
    buffer->data = reallocate(buffer->data, buffer->capacity);
  }
  buffer->data[buffer->size++] = byte;
}

static int peek_n(uint8_t n, Reader *r) {
  while (r->la < n) {
    int c = fgetc(r->file);
    if (c == EOF) {
      return EOF;
    }
    r->buffer[r->la] = (uint8_t)c;
    r->la++;
  }
  return r->buffer[n - 1];
}

static int peek(Reader *r) {
  return peek_n(1, r);
}

static int pop(Reader *r) {
  int c;
  if (r->la > 0) {
    c = r->buffer[0];
    r->la--;
    for (int i = 0; i < r->la; i++) {
      r->buffer[i] = r->buffer[i + 1];
    }
  } else {
    c = fgetc(r->file);
  }
  if (c == '\n') {
    r->line++;
    r->column = 1;
  } else {
    r->column++;
  }
  return c;
}

static Token *create_token(TokenType type, Reader *r) {
  Token *t = allocate(sizeof(Token));
  t->string_value = NULL;
  t->next = NULL;
  t->line = r->line;
  t->column = r->column;
  t->size = 0;
  t->type = type;
  t->error = 0;
  return t;
}

const char *token_name(Token *t) {
  switch (t->type) {
    case T_NAME: return "T_NAME";
    case T_KEYWORD: return "T_KEYWORD";
    case T_OPERATOR: return "T_OPERATOR";
    case T_STRING: return "T_STRING";
    case T_INT: return "T_INT";
    case T_FLOAT: return "T_FLOAT";
    case T_TEXT: return "T_TEXT";
    case T_LF: return "T_LF";
    case T_END_QUOTE: return "T_END_QUOTE";
    case T_START_QUOTE: return "T_START_QUOTE";
    case T_PUNCT: return "T_PUNCT";
    case T_EOF: return "T_EOF";
    default: return "unknwon";
  }
}

void delete_token(Token *t) {
  switch (t->type) {
    case T_NAME:
    case T_KEYWORD:
    case T_STRING:
      if (t->string_value) {
        free(t->string_value);
      }
      break;
    default:
      break;
  }
  free(t);
}

static int is_valid_name_char(int c) {
  return c == '_' || isalnum(c);
}

static Token *read_name(Reader *r) {
  Token *token = create_token(T_NAME, r);
  Buffer buffer = create_buffer();
  while (1) {
    int c = peek(r);
    if (c == EOF || !is_valid_name_char(c)) {
      if (!buffer.size) {
        reader_error(r, "invalid character: '%c'", c);
        token->error = 1;
      }
      break;
    }
    buffer_put(&buffer, c);
    pop(r);
  }
  token->size = buffer.size;
  buffer_put(&buffer, '\0');
  token->string_value = buffer.data;
  return token;
}

static int is_operator_char(int c) {
  return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '!' || c == '<'
    || c == '>' || c == '=' || c == '|' || c == '.' || c == ',' || c == ':';
}

static Token *read_operator(Reader *r) {
  Token *token = create_token(T_OPERATOR, r);
  token->operator_value[0] = (uint8_t) pop(r);
  token->operator_value[1] = '\0';
  switch (token->operator_value[0]) {
    case '-':
      if (peek(r) == '=' || peek(r) == '>') {
        token->operator_value[1] = (uint8_t) pop(r);
        token->operator_value[2] = '\0';
      }
      break;
    case '+':
    case '*':
    case '/':
    case '<':
    case '>':
    case '=':
    case '!':
      if (peek(r) == '=') {
        token->operator_value[1] = (uint8_t) pop(r);
        token->operator_value[2] = '\0';
      }
      break;
  }
  return token;
}

static int utf8_encode(uint32_t code_point, Buffer *buffer, Reader *r) {
  if (code_point < 0x80) {
    buffer_put(buffer, code_point);
    return 1;
  }
  if (code_point > 0x10FFFFFF) {
    reader_error(r, "unicode code point out of range");
    return 0;
  }
  uint8_t bytes[3];
  bytes[0] = 0x80 | (code_point & 0x3F);
  code_point >>= 6;
  if (code_point < 0x20) {
    buffer_put(buffer, 0xC0 | code_point);
  } else {
    bytes[1] = 0x80 | (code_point & 0x3F);
    code_point >>= 6;
    if (code_point < 0x10) {
      buffer_put(buffer, 0xE0 | code_point);
    } else {
      bytes[2] = 0x80 | (code_point & 0x3F);
      code_point >>= 6;
      buffer_put(buffer, 0xF0 | code_point);
      buffer_put(buffer, bytes[2]);
    }
    buffer_put(buffer, bytes[1]);
  }
  buffer_put(buffer, bytes[0]);
  return 1;
}

static uint8_t hex_to_dec(int c) {
  if (c >= 'a') {
    return 10 + (c - 'a');
  } else if (c >= 'A') {
    return 10 + (c - 'A');
  } else {
    return (c - '0');
  }
}

static int read_hex_code_point(Reader *r, uint32_t *code_point, int length) {
  *code_point = 0;
  for (int i = 0; i < length; i++) {
    *code_point <<= 4;
    int c = pop(r);
    if (!isxdigit(c)) {
      reader_error(r, "invalid hexadecimal escape sequence");
      return 0;
    }
    *code_point |= hex_to_dec(c);
  }
  return 1;
}

static int read_escape_sequence(Reader *r, Buffer *buffer, int double_quote) {
  uint32_t code_point;
  int c = pop(r);
  if (c == EOF) {
    reader_error(r, "unexpected end of input");
    return 0;
  }
  if (double_quote && (c == '{' || c == '}')) {
    buffer_put(buffer, pop(r));
    return 1;
  }
  switch (c) {
    case '"':
    case '\'':
    case '\\':
    case '/':
      buffer_put(buffer, c);
      break;
    case 'b':
      buffer_put(buffer, '\x08');
      break;
    case 'f':
      buffer_put(buffer, '\f');
      break;
    case 'n':
      buffer_put(buffer, '\n');
      break;
    case 'r':
      buffer_put(buffer, '\r');
      break;
    case 't':
      buffer_put(buffer, '\t');
      break;
    case 'x':
      if (!read_hex_code_point(r, &code_point, 2)) {
        return 0;
      }
      buffer_put(buffer, code_point);
      break;
    case 'u':
      if (!read_hex_code_point(r, &code_point, 4)) {
        return 0;
      }
      utf8_encode(code_point, buffer, r);
      break;
    case 'U':
      if (!read_hex_code_point(r, &code_point, 8)) {
        return 0;
      }
      utf8_encode(code_point, buffer, r);
      break;
  }
  return 1;
} 

static Token *read_string(Reader *r) {
  Token *token = create_token(T_STRING, r);
  Buffer buffer = create_buffer();
  pop(r);
  while (1) {
    int c = peek(r);
    if (c == EOF) {
      reader_error(r, "missing end of string literal, string literal started on line %d:%d", token->line, token->column);
      token->error = 1;
      break;
    } else if (c == '\'') {
      pop(r);
      break;
    } else if (c == '\\') {
      if (!read_escape_sequence(r, &buffer, 0)) {
        token->error = 1;
      }
    } else {
      buffer_put(&buffer, pop(r));
    }
  }
  token->size = buffer.size;
  buffer_put(&buffer, '\0');
  token->string_value = buffer.data;
  return token;
}

static Token *read_verbatim(Reader *r) {
  Token *token = create_token(T_STRING, r);
  Buffer buffer = create_buffer();
  pop(r);
  pop(r);
  pop(r);
  while (1) {
    int c = peek(r);
    if (c == EOF) {
      reader_error(r, "missing end of string literal, string literal started on line %d:%d", token->line, token->column);
      token->error = 1;
      break;
    } else if (c == '"' && peek_n(2, r) == '"' && peek_n(3, r) == '"') {
      pop(r);
      pop(r);
      pop(r);
      break;
    } else {
      buffer_put(&buffer, pop(r));
    }
  }
  token->size = buffer.size;
  buffer_put(&buffer, '\0');
  token->string_value = buffer.data;
  return token;
}

static Token *read_number(Reader *r) {
  int c;
  Token *token = create_token(T_INT, r);
  Buffer buffer = create_buffer();
  while (1) {
    c = peek(r);
    if (c == EOF || !isdigit(c)) {
      break;
    }
    buffer_put(&buffer, pop(r));
  }
  c = peek(r);
  if (c == '.' || c == 'e' || c == 'E') {
    token->type = T_FLOAT;
    if (c == '.') {
      buffer_put(&buffer, pop(r));
      while (1) {
        c = peek(r);
        if (c == EOF || !isdigit(c)) {
          break;
        }
        buffer_put(&buffer, pop(r));
      }
    }
    if (c == 'e' || c == 'E') {
      buffer_put(&buffer, pop(r));
      c = peek(r);
      if (c == '+' || c == '-') {
        buffer_put(&buffer, pop(r));
      }
      while (1) {
        c = peek(r);
        if (c == EOF || !isdigit(c)) {
          break;
        }
        buffer_put(&buffer, pop(r));
      }
    }
    buffer_put(&buffer, '\0');
    token->float_value = atof((char *) buffer.data);
  } else {
    buffer_put(&buffer, '\0');
    token->int_value = atol((char *) buffer.data);
  }
  delete_buffer(buffer);
  return token;
}

static void skip_ws(Reader *r, int skip_lf) {
  int c = peek(r);
  while (c == ' ' || c == '\t' || c == '\r' || (skip_lf && c == '\n')) {
    pop(r);
    c = peek(r);
  }
}

static Token *read_next_token(Reader *r) {
  if (peek(r) == EOF) {
    return create_token(T_EOF, r);
  }
  uint8_t top_paren = get_top_paren(r);
  if (!top_paren || top_paren == '"') {
    Token *token = create_token(T_TEXT, r);
    Buffer buffer = create_buffer();
    while (1) {
      int c = peek(r);
      if (c == EOF) {
        break;
      } else if (c == '{') {
        pop(r);
        if (peek(r) == '#') {
          // Comment
          pop(r);
          while (peek(r) != EOF) {
            if (pop(r) == '#') {
              if (peek(r) == '}') {
                pop(r);
                break;
              }
            }
          }
        } else {
          push_paren(r, '{');
        }
        break;
      } else if (top_paren == '"' && c == '\\') {
        pop(r);
        if (!read_escape_sequence(r, &buffer, 1)) {
          token->error = 1;
        }
      } else if (top_paren == '"' && c == '"') {
        // Don't pop char, will be picked up as T_END_QUOTE on next call
        pop_paren(r);
        push_paren(r, '$'); // end quote
        break;
      } else {
        buffer_put(&buffer, pop(r));
      }
    }
    token->size = buffer.size;
    buffer_put(&buffer, '\0');
    token->string_value = buffer.data;
    return token;
  } else {
    int is_command = (top_paren == '{' && (!r->parens->next || (r->parens->next && r->parens->next->paren == '"')));
    skip_ws(r, !!r->parens->next);
    int c = peek(r);
    if (c == '\n') {
      Token *token = create_token(T_LF, r);
      pop(r);
      return token;
    } else if (c == '}' && is_command) {
      pop(r);
      pop_paren(r);
      return read_next_token(r);
    } else if (c == '\'') {
      return read_string(r);
    } else if (c == '"' && top_paren == '$') {
      Token *token = create_token(T_END_QUOTE, r);
      pop(r);
      pop_paren(r);
      return token;
    } else if (c == '"') {
      if (peek_n(2, r) == '"' && peek_n(3, r) == '"') {
        return read_verbatim(r);
      } else {
        Token *token = create_token(T_START_QUOTE, r);
        pop(r);
        push_paren(r, '"');
        return token;
      }
    } else if (c == '(' || c == '[' || c == '{') {
      Token *token = create_token(T_PUNCT, r);
      token->punct_value = pop(r);
      if (c == '{' && peek(r) == '#') {
        // Comment
        pop(r);
        while (peek(r) != EOF) {
          if (pop(r) == '#') {
            if (peek(r) == '}') {
              pop(r);
              break;
            }
          }
        }
        return read_next_token(r);
      }
      push_paren(r, token->punct_value);
      return token;
    } else if (c == ')' || c == ']' || c == '}') {
      Token *token = create_token(T_PUNCT, r);
      token->punct_value = pop(r);
      if (!top_paren) {
        reader_error(r, "unexpected '%c'", c);
        token->error = 1;
      } else {
        uint8_t expected = pop_paren(r);
        switch (expected) {
          case '(': expected = ')'; break;
          case '[': expected = ']'; break;
          case '{': expected = '}'; break;
        }
        if (c != expected) {
          reader_error(r, "unexpected '%c', expected '%c'", c, expected);
          token->error = 1;
        }
      }
      return token;
    } else if (is_operator_char(c)) {
      return read_operator(r);
    } else if (isdigit(c)) {
      return read_number(r);
    } else {
      return read_name(r);
    }
  }
}

Token *read_all(Reader *r, int template) {
  Token *first = NULL, *last = NULL;
  r->errors = 0;
  clear_parens(r);
  if (!template) {
    push_paren(r, '{');
  }
  while (1) {
    Token *token = read_next_token(r);
    if (!first) {
      first = token;
    } else {
      last->next = token;
    }
    last = token;
    if (token->error) {
      r->errors++;
      if (r->errors > 20) {
        reader_error(r, "too many errors");
        break;
      }
    }
    if (token->type == T_EOF) {
      break;
    }
  }
  return first;
}
