// Lexer
// -----
//
// The lexer is the first real compiler phase. It turns raw source text into a
// flat token stream while preserving line/column locations for later errors.
// It does not try to understand grammar; for example ARR[INT] is emitted as
// ARR, [, INT, ] so the parser can decide what those tokens mean.

// ----------------------------- Lexer -----------------------------

typedef enum {
    TK_EOF,
    TK_NEWLINE,
    TK_NAME,
    TK_INT_LITERAL,
    TK_FLT_LITERAL,
    TK_STRING_LITERAL,
    TK_BOL_LITERAL,
    TK_TYPE,
    TK_MAP,
    TK_FN,
    TK_REF,
    TK_TSK,
    TK_INIT,
    TK_INT,
    TK_FLT,
    TK_BOL,
    TK_STR,
    TK_NUL_TYPE,
    TK_ARR,
    TK_RET,
    TK_IF,
    TK_ELX,
    TK_ELSE,
    TK_LOOP,
    TK_PASS,
    TK_THROW,
    TK_FREE,
    TK_HOLD,
    TK_AS,
    TK_IS,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_LBRACE,
    TK_RBRACE,
    TK_COLON,
    TK_EQUAL,
    TK_ARROW,
    TK_PLUSPLUS,
    TK_PLUSEQ,
    TK_MINUSEQ,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_PERCENT,
    TK_BANG,
    TK_LT,
    TK_LTE,
    TK_GT,
    TK_GTE,
    TK_EQEQ,
    TK_NEQ,
    TK_ANDAND,
    TK_OROR,
    TK_PIPE,
    TK_COMMA,
    TK_DOT
} TokenKind;

typedef struct {
    TokenKind kind;
    char *text;
    int line;
    int col;
} Token;

typedef struct {
    Token *items;
    int len;
    int cap;
} TokenVec;

static void tok_push(TokenVec *vec, Token tok) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 64;
        vec->items = xrealloc(vec->items, sizeof(Token) * (size_t)vec->cap);
    }
    vec->items[vec->len++] = tok;
}

static TokenKind keyword_kind(const char *s) {
    if (strcmp(s, "TYPE") == 0) return TK_TYPE;
    if (strcmp(s, "MAP") == 0) return TK_MAP;
    if (strcmp(s, "FN") == 0) return TK_FN;
    if (strcmp(s, "REF") == 0) return TK_REF;
    if (strcmp(s, "TSK") == 0) return TK_TSK;
    if (strcmp(s, "INIT") == 0) return TK_INIT;
    if (strcmp(s, "INT") == 0) return TK_INT;
    if (strcmp(s, "FLT") == 0) return TK_FLT;
    if (strcmp(s, "BOL") == 0) return TK_BOL;
    if (strcmp(s, "STR") == 0) return TK_STR;
    if (strcmp(s, "NUL") == 0) return TK_NUL_TYPE;
    if (strcmp(s, "ARR") == 0) return TK_ARR;
    if (strcmp(s, "ret") == 0) return TK_RET;
    if (strcmp(s, "if") == 0) return TK_IF;
    if (strcmp(s, "elx") == 0) return TK_ELX;
    if (strcmp(s, "else") == 0) return TK_ELSE;
    if (strcmp(s, "loop") == 0) return TK_LOOP;
    if (strcmp(s, "pass") == 0) return TK_PASS;
    if (strcmp(s, "throw") == 0) return TK_THROW;
    if (strcmp(s, "FREE") == 0) return TK_FREE;
    if (strcmp(s, "hold") == 0) return TK_HOLD;
    if (strcmp(s, "as") == 0) return TK_AS;
    if (strcmp(s, "IS") == 0) return TK_IS;
    if (strcmp(s, "true") == 0 || strcmp(s, "false") == 0) return TK_BOL_LITERAL;
    return TK_NAME;
}

static TokenVec lex_source(const char *src) {
    TokenVec toks = {0};
    int i = 0, line = 1, col = 1;
    while (src[i]) {
        char ch = src[i];
        if (ch == ' ' || ch == '\t') { i++; col++; continue; }
        if (ch == '\n' || ch == '\r') {
            int tl = line, tc = col;
            if (ch == '\r' && src[i + 1] == '\n') i += 2; else i++;
            tok_push(&toks, (Token){TK_NEWLINE, xstrdup("\n"), tl, tc});
            line++; col = 1; continue;
        }
        if (ch == '/' && src[i + 1] == '/') {
            i += 2; col += 2;
            while (src[i] && src[i] != '\n' && src[i] != '\r') { i++; col++; }
            continue;
        }
        if (ch == '/' && src[i + 1] == '*') {
            int tl = line, tc = col;
            i += 2; col += 2;
            while (src[i] && !(src[i] == '*' && src[i + 1] == '/')) {
                if (src[i] == '\n' || src[i] == '\r') {
                    if (src[i] == '\r' && src[i + 1] == '\n') i += 2; else i++;
                    line++; col = 1;
                } else {
                    i++; col++;
                }
            }
            if (!src[i]) die_at(tl, tc, "unterminated block comment");
            i += 2; col += 2;
            continue;
        }
        if (ch == '"') {
            int tl = line, tc = col;
            i++; col++;
            char *buf = NULL; int len = 0, cap = 0;
            while (src[i] && src[i] != '"') {
                if (src[i] == '\n' || src[i] == '\r') die_at(tl, tc, "unterminated string literal");
                char out = src[i];
                if (src[i] == '\\') {
                    char e = src[i + 1];
                    if (!e) die_at(line, col, "unterminated string escape");
                    if (e == '"') out = '"';
                    else if (e == '\\') out = '\\';
                    else if (e == 'n') out = '\n';
                    else if (e == 't') out = '\t';
                    else die_at(line, col, "unsupported string escape \\%c", e);
                    i += 2; col += 2;
                } else { i++; col++; }
                if (len + 1 >= cap) { cap = cap ? cap * 2 : 16; buf = xrealloc(buf, (size_t)cap); }
                buf[len++] = out;
            }
            if (!src[i]) die_at(tl, tc, "unterminated string literal");
            i++; col++;
            if (len + 1 >= cap) { cap = cap ? cap * 2 : 16; buf = xrealloc(buf, (size_t)cap); }
            buf[len] = '\0';
            tok_push(&toks, (Token){TK_STRING_LITERAL, buf, tl, tc});
            continue;
        }
        if (ch == '`') {
            int tl = line, tc = col;
            i++; col++;
            char *buf = NULL; int len = 0, cap = 0;
            while (src[i] && src[i] != '`') {
                char out = src[i];
                if (src[i] == '\n' || src[i] == '\r') {
                    if (len + 1 >= cap) { cap = cap ? cap * 2 : 16; buf = xrealloc(buf, (size_t)cap); }
                    buf[len++] = '\n';
                    if (src[i] == '\r' && src[i + 1] == '\n') i += 2; else i++;
                    line++; col = 1;
                    continue;
                }
                i++; col++;
                if (len + 1 >= cap) { cap = cap ? cap * 2 : 16; buf = xrealloc(buf, (size_t)cap); }
                buf[len++] = out;
            }
            if (!src[i]) die_at(tl, tc, "unterminated raw string literal");
            i++; col++;
            if (len + 1 >= cap) { cap = cap ? cap * 2 : 16; buf = xrealloc(buf, (size_t)cap); }
            buf[len] = '\0';
            tok_push(&toks, (Token){TK_STRING_LITERAL, buf, tl, tc});
            continue;
        }
        if (isdigit((unsigned char)ch)) {
            int tl = line, tc = col, start = i;
            while (isdigit((unsigned char)src[i])) { i++; col++; }
            bool is_float = false;
            if (src[i] == '.' && isdigit((unsigned char)src[i + 1])) {
                is_float = true;
                i++; col++;
                while (isdigit((unsigned char)src[i])) { i++; col++; }
            }
            if (src[i] == 'e' || src[i] == 'E') {
                is_float = true;
                i++; col++;
                if (src[i] == '+' || src[i] == '-') { i++; col++; }
                if (!isdigit((unsigned char)src[i])) die_at(line, col, "expected exponent digits");
                while (isdigit((unsigned char)src[i])) { i++; col++; }
            }
            tok_push(&toks, (Token){is_float ? TK_FLT_LITERAL : TK_INT_LITERAL, xstrndup(src + start, (size_t)(i - start)), tl, tc});
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_') {
            int tl = line, tc = col, start = i;
            if (ch == 'x' && i > 0 && isdigit((unsigned char)src[i - 1]) &&
                (isalnum((unsigned char)src[i + 1]) || src[i + 1] == '_' || src[i + 1] == '(')) {
                i++;
                col++;
                tok_push(&toks, (Token){TK_NAME, xstrdup("x"), tl, tc});
                continue;
            }
            while (isalnum((unsigned char)src[i]) || src[i] == '_') { i++; col++; }
            char *text = xstrndup(src + start, (size_t)(i - start));
            tok_push(&toks, (Token){keyword_kind(text), text, tl, tc});
            continue;
        }
        int tl = line, tc = col;
        if (ch == '-' && src[i + 1] == '>') { tok_push(&toks, (Token){TK_ARROW, xstrdup("->"), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '+' && src[i + 1] == '+') { tok_push(&toks, (Token){TK_PLUSPLUS, xstrdup("++"), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '+' && src[i + 1] == '=') { tok_push(&toks, (Token){TK_PLUSEQ, xstrdup("+="), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '-' && src[i + 1] == '=') { tok_push(&toks, (Token){TK_MINUSEQ, xstrdup("-="), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '=' && src[i + 1] == '=') { tok_push(&toks, (Token){TK_EQEQ, xstrdup("=="), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '!' && src[i + 1] == '=') { tok_push(&toks, (Token){TK_NEQ, xstrdup("!="), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '<' && src[i + 1] == '=') { tok_push(&toks, (Token){TK_LTE, xstrdup("<="), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '>' && src[i + 1] == '=') { tok_push(&toks, (Token){TK_GTE, xstrdup(">="), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '&' && src[i + 1] == '&') { tok_push(&toks, (Token){TK_ANDAND, xstrdup("&&"), tl, tc}); i += 2; col += 2; continue; }
        if (ch == '|' && src[i + 1] == '|') { tok_push(&toks, (Token){TK_OROR, xstrdup("||"), tl, tc}); i += 2; col += 2; continue; }
        TokenKind kind = TK_EOF;
        switch (ch) {
            case '(': kind = TK_LPAREN; break; case ')': kind = TK_RPAREN; break;
            case '[': kind = TK_LBRACKET; break; case ']': kind = TK_RBRACKET; break;
            case '{': kind = TK_LBRACE; break; case '}': kind = TK_RBRACE; break;
            case ':': kind = TK_COLON; break;
            case '=': kind = TK_EQUAL; break; case '+': kind = TK_PLUS; break;
            case '-': kind = TK_MINUS; break; case '*': kind = TK_STAR; break;
            case '/': kind = TK_SLASH; break; case '%': kind = TK_PERCENT; break;
            case '!': kind = TK_BANG; break; case '<': kind = TK_LT; break;
            case '>': kind = TK_GT; break; case '|': kind = TK_PIPE; break;
            case ',': kind = TK_COMMA; break;
            case '.': kind = TK_DOT; break;
            default: die_at(line, col, "unexpected character '%c'", ch);
        }
        tok_push(&toks, (Token){kind, xstrndup(src + i, 1), tl, tc});
        i++; col++;
    }
    tok_push(&toks, (Token){TK_EOF, xstrdup(""), line, col});
    return toks;
}
