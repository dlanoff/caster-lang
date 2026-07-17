// Parser
// ------
//
// Statements are parsed with normal recursive descent: parse a function, parse
// a block, then parse statements inside that block.
//
// Expressions use a Pratt-style precedence loop so `1 + 2 * 3` becomes
// `1 + (2 * 3)` without needing a separate function for every precedence
// level.

// ----------------------------- Parser -----------------------------

typedef struct { Token *toks; int pos; bool allow_newline_ops; } Parser;
static Token *cur(Parser *p) { return &p->toks[p->pos]; }
static Token *peek(Parser *p, int offset) { return &p->toks[p->pos + offset]; }
static Token *prev(Parser *p) { return &p->toks[p->pos - 1]; }
static bool match(Parser *p, TokenKind k) { if (cur(p)->kind == k) { p->pos++; return true; } return false; }
static Token expect(Parser *p, TokenKind k, const char *msg) { if (cur(p)->kind != k) die_at(cur(p)->line, cur(p)->col, "%s", msg); return p->toks[p->pos++]; }
static void skip_newlines(Parser *p) { while (match(p, TK_NEWLINE)) {} }

typedef struct { char *type; Token token; } ParsedType;

static char *normalize_source_type_name(const char *name) {
    if (strcmp(name, "Req") == 0) return xstrdup("HttpReq");
    if (strcmp(name, "Res") == 0) return xstrdup("HttpRes");
    return xstrdup(name);
}

static Node *parse_expression(Parser *p, int min_prec);
static Node *parse_statement(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_fn_decl(Parser *p);

static ParsedType parse_type(Parser *p) {
    Token tok = *cur(p);
    if (match(p, TK_ARR)) {
        expect(p, TK_LBRACKET, "expected '[' after ARR");
        ParsedType elem = parse_type(p);
        expect(p, TK_RBRACKET, "expected ']' after ARR element type");
        return (ParsedType){array_type(elem.type), tok};
    }
    if (match(p, TK_MAP)) {
        expect(p, TK_LBRACKET, "expected '[' after MAP");
        ParsedType key = parse_type(p);
        expect(p, TK_COMMA, "expected ',' after MAP key type");
        ParsedType value = parse_type(p);
        expect(p, TK_RBRACKET, "expected ']' after MAP value type");
        return (ParsedType){map_type(key.type, value.type), tok};
    }
    if (match(p, TK_REF)) {
        expect(p, TK_LBRACKET, "expected '[' after REF");
        ParsedType target = parse_type(p);
        expect(p, TK_RBRACKET, "expected ']' after REF target type");
        return (ParsedType){ref_type(target.type), tok};
    }
    if (match(p, TK_TSK)) {
        expect(p, TK_LBRACKET, "expected '[' after TSK");
        ParsedType target = parse_type(p);
        expect(p, TK_RBRACKET, "expected ']' after TSK target type");
        return (ParsedType){task_type(target.type), tok};
    }
    if (match(p, TK_FN)) {
        expect(p, TK_LBRACKET, "expected '[' after FN");
        ParsedType input = parse_type(p);
        expect(p, TK_ARROW, "expected '->' in FN type");
        ParsedType output = parse_type(p);
        expect(p, TK_RBRACKET, "expected ']' after FN return type");
        return (ParsedType){fn_type(input.type, output.type), tok};
    }
    if (match(p, TK_INT) || match(p, TK_FLT) || match(p, TK_BOL) || match(p, TK_STR) || match(p, TK_NUL_TYPE)) {
        return (ParsedType){xstrdup(tok.text), tok};
    }
    if (match(p, TK_NAME)) {
        if (match(p, TK_DOT)) {
            Token member = expect(p, TK_NAME, "expected type name after module '.'");
            if (strcmp(tok.text, "WEB") == 0) return (ParsedType){normalize_source_type_name(member.text), tok};
            if (strcmp(tok.text, "BUF") == 0 && strcmp(member.text, "Buffer") == 0) return (ParsedType){xstrdup("Buffer"), tok};
            return (ParsedType){strf("%s_%s", tok.text, member.text), tok};
        }
        return (ParsedType){normalize_source_type_name(tok.text), tok};
    }
    die_at(tok.line, tok.col, "expected type");
    return (ParsedType){0};
}

static bool is_collection_body_method(const char *name) {
    return strcmp(name, "upd") == 0 || strcmp(name, "filt") == 0 || strcmp(name, "agg") == 0;
}

static bool match_method_name(Parser *p, Token *name) {
    if (cur(p)->kind == TK_NAME || cur(p)->kind == TK_ARR || cur(p)->kind == TK_STR || cur(p)->kind == TK_INIT) {
        *name = *cur(p);
        p->pos++;
        return true;
    }
    return false;
}

static bool starts_method_body(Parser *p, const char *name, bool had_leading_newline) {
    if (!is_collection_body_method(name) || cur(p)->kind == TK_RPAREN) return false;
    (void)had_leading_newline;
    if (strcmp(name, "agg") == 0 && (cur(p)->kind == TK_PLUSEQ || cur(p)->kind == TK_MINUS)) return true;
    if (cur(p)->kind == TK_RET || cur(p)->kind == TK_IF || cur(p)->kind == TK_LOOP || cur(p)->kind == TK_PASS || cur(p)->kind == TK_THROW) return true;
    if (cur(p)->kind == TK_NAME) {
        TokenKind next = peek(p, 1)->kind;
        return next == TK_EQUAL || next == TK_PLUSEQ || next == TK_PLUSPLUS;
    }
    return false;
}

static Node *parse_paren_value_expr(Parser *p, const char *message);

static Node *parse_agg_method_body(Parser *p, Token open) {
    Node *b = node_new(NK_BLOCK, open);
    skip_newlines(p);
    while (cur(p)->kind != TK_RPAREN) {
        if (cur(p)->kind == TK_EOF) die_at(cur(p)->line, cur(p)->col, "unterminated agg body");

        Token tok = *cur(p);
        Node *step = node_new(NK_AGG_STEP, tok);
        if (match(p, TK_PLUSEQ)) {
            step->op = xstrdup("+=");
        } else if (match(p, TK_MINUS)) {
            step->op = xstrdup("-");
        } else {
            die_at(tok.line, tok.col, ".agg() accumulator body expects '+= expression' or '- expression'");
        }

        skip_newlines(p);
        bool old_allow_newline_ops = p->allow_newline_ops;
        p->allow_newline_ops = false;
        if (cur(p)->kind == TK_IF) {
            Token if_tok = *cur(p);
            p->pos++;
            expect(p, TK_LPAREN, "expected '(' after if");
            Node *condition = parse_expression(p, 0);
            expect(p, TK_RPAREN, "expected ')' after if condition");
            Node *then_value = parse_paren_value_expr(p, "expected '(' after if expression condition");
            skip_newlines(p);

            Node *fallback = NULL;
            if (match(p, TK_ELSE)) {
                fallback = parse_paren_value_expr(p, "expected '(' after if expression fallback");
            }

            Node *expr = node_new(NK_IF_EXPR, if_tok);
            expr->condition = condition;
            expr->value = then_value;
            expr->right = fallback;
            step->value = expr;
        } else {
            step->value = parse_expression(p, 0);
        }
        if (match(p, TK_IF)) {
            Token if_tok = *prev(p);
            expect(p, TK_LPAREN, "expected '(' after agg step conditional if");
            Node *condition = parse_expression(p, 0);
            expect(p, TK_RPAREN, "expected ')' after agg step conditional if");
            Node *fallback = NULL;
            if (match(p, TK_ELSE)) {
                fallback = parse_expression(p, 0);
            }
            Node *expr = node_new(NK_IF_EXPR, if_tok);
            expr->condition = condition;
            expr->value = step->value;
            expr->right = fallback;
            step->value = expr;
        }
        p->allow_newline_ops = old_allow_newline_ops;
        vec_push(&b->statements, step);
        skip_newlines(p);
    }
    return b;
}

static Node *parse_method_body(Parser *p, Token open) {
    Node *b = node_new(NK_BLOCK, open);
    skip_newlines(p);
    while (cur(p)->kind != TK_RPAREN) {
        if (cur(p)->kind == TK_EOF) die_at(cur(p)->line, cur(p)->col, "unterminated method body");
        vec_push(&b->statements, parse_statement(p));
        skip_newlines(p);
    }
    return b;
}

static Node *parse_method_argument(Parser *p, const char *method_name) {
    bool old_allow_newline_ops = p->allow_newline_ops;
    if (is_collection_body_method(method_name) || strcmp(method_name, "group") == 0 || strcmp(method_name, "sort") == 0) {
        p->allow_newline_ops = true;
    }
    Node *arg = parse_expression(p, 0);
    p->allow_newline_ops = old_allow_newline_ops;
    if (strcmp(method_name, "agg") == 0 && match(p, TK_IF)) {
        Token if_tok = *prev(p);
        expect(p, TK_LPAREN, "expected '(' after agg conditional if");
        Node *condition = parse_expression(p, 0);
        expect(p, TK_RPAREN, "expected ')' after agg conditional if");
        Node *expr = node_new(NK_IF_EXPR, if_tok);
        expr->value = arg;
        expr->condition = condition;
        return expr;
    }
    return arg;
}

static bool parse_sort_args(Parser *p, Node *method) {
    if (strcmp(method->name, "sort") != 0) return false;

    vec_push(&method->args, parse_expression(p, 0));
    skip_newlines(p);
    if (cur(p)->kind == TK_NAME &&
        (strcmp(cur(p)->text, "ASC") == 0 || strcmp(cur(p)->text, "DESC") == 0)) {
        method->sort_dir = xstrdup(cur(p)->text);
        p->pos++;
    } else {
        method->sort_dir = xstrdup("ASC");
    }
    skip_newlines(p);
    if (cur(p)->kind == TK_COMMA) die_at(cur(p)->line, cur(p)->col, ".sort() expects one key expression and optional ASC or DESC");
    return true;
}

static bool starts_inline_array_map(Parser *p) {
    return cur(p)->kind == TK_ARR &&
        peek(p, 1)->kind == TK_LBRACKET &&
        peek(p, 2)->kind == TK_MAP &&
        peek(p, 3)->kind == TK_RBRACKET;
}

static Node *parse_paren_value_expr(Parser *p, const char *message) {
    expect(p, TK_LPAREN, message);
    bool old_allow_newline_ops = p->allow_newline_ops;
    p->allow_newline_ops = true;
    skip_newlines(p);
    Node *expr = parse_expression(p, 0);
    skip_newlines(p);
    p->allow_newline_ops = old_allow_newline_ops;
    expect(p, TK_RPAREN, "expected ')' after value expression");
    return expr;
}

static Node *parse_if_value_expr(Parser *p, Token if_tok) {
    expect(p, TK_LPAREN, "expected '(' after if");
    Node *condition = parse_expression(p, 0);
    expect(p, TK_RPAREN, "expected ')' after if condition");
    Node *then_value = parse_paren_value_expr(p, "expected '(' after if expression condition");
    skip_newlines(p);

    expect(p, TK_ELSE, "if expression needs else(value) fallback");
    Node *else_value = parse_paren_value_expr(p, "expected '(' after if expression fallback");

    Node *expr = node_new(NK_IF_EXPR, if_tok);
    expr->condition = condition;
    expr->value = then_value;
    expr->right = else_value;
    return expr;
}

static int precedence(TokenKind k, const char **op) {
    switch (k) {
        case TK_OROR: *op = "||"; return 1; case TK_ANDAND: *op = "&&"; return 2;
        case TK_EQEQ: *op = "=="; return 3; case TK_NEQ: *op = "!="; return 3;
        case TK_LT: *op = "<"; return 4; case TK_LTE: *op = "<="; return 4;
        case TK_GT: *op = ">"; return 4; case TK_GTE: *op = ">="; return 4;
        case TK_PIPE: *op = "|"; return 5;
        case TK_PLUS: *op = "+"; return 5; case TK_MINUS: *op = "-"; return 5;
        case TK_STAR: *op = "*"; return 6; case TK_SLASH: *op = "/"; return 6; case TK_PERCENT: *op = "%"; return 6;
        default: return 0;
    }
}

static bool is_is_check_op(TokenKind k) {
    return k == TK_LT || k == TK_LTE || k == TK_GT || k == TK_GTE || k == TK_EQEQ || k == TK_NEQ;
}

static const char *token_binary_op(TokenKind k) {
    const char *op = NULL;
    precedence(k, &op);
    return op;
}

static Node *parse_is_piece(Parser *p, Node *subject) {
    Token tok = *cur(p);
    const char *op = "==";

    if (is_is_check_op(tok.kind)) {
        op = token_binary_op(tok.kind);
        p->pos++;
    }

    Node *right = parse_expression(p, 5);
    Node *cmp = node_new(NK_BINARY, tok);
    cmp->op = xstrdup(op);
    cmp->left = subject;
    cmp->right = right;
    return cmp;
}

static Node *parse_is_check(Parser *p, Token is_tok, Node *subject) {
    Node *expr = parse_is_piece(p, subject);
    int pieces = 1;

    while (cur(p)->kind == TK_ANDAND || cur(p)->kind == TK_OROR) {
        Token tok = *cur(p);
        p->pos++;

        Node *right = parse_is_piece(p, subject);
        Node *bin = node_new(NK_BINARY, tok);
        bin->op = xstrdup(token_binary_op(tok.kind));
        bin->left = expr;
        bin->right = right;
        expr = bin;
        pieces++;
    }

    if (pieces < 2) {
        die_at(is_tok.line, is_tok.col, "IS expects at least two comparison clauses; use an ordinary comparison for one clause");
    }

    return expr;
}

static Node *parse_postfix(Parser *p, Node *expr) {
    for (;;) {
        int newline_save = p->pos;
        skip_newlines(p);
        if (cur(p)->kind != TK_DOT) p->pos = newline_save;

        if (match(p, TK_LPAREN)) {
            Token open = *prev(p); Node *call = node_new(NK_CALL, open); call->callee = expr;
            if (cur(p)->kind != TK_RPAREN) {
                for (;;) { vec_push(&call->args, parse_expression(p, 0)); if (!match(p, TK_COMMA)) break; }
            }
            expect(p, TK_RPAREN, "expected ')' after call arguments"); expr = call; continue;
        }
        if (match(p, TK_DOT)) {
            Token dot = *prev(p); Node *d = node_new(NK_DOT, dot); d->object = expr;
            if (match(p, TK_INT_LITERAL)) { Token it = *prev(p); Node *idx = node_new(NK_INT, it); idx->int_value = strtoll(it.text, NULL, 10); d->index = idx; }
            else if (match(p, TK_LBRACKET)) { d->index = parse_expression(p, 0); expect(p, TK_RBRACKET, "expected ']' after computed dot expression"); }
            else if (match(p, TK_LPAREN)) { die_at(prev(p)->line, prev(p)->col, "computed dot access uses .[expression], not .(...)"); }
            else {
                Token name;
                if (!match_method_name(p, &name)) {
                    die_at(cur(p)->line, cur(p)->col, "expected name, numeric index, or [expression] after '.'");
                }
                if (cur(p)->kind == TK_LPAREN) {
                    Token open = expect(p, TK_LPAREN, "expected '(' after method name");
                    Node *m = node_new(NK_METHOD_CALL, dot);
                    m->object = expr;
                    m->name = xstrdup(name.text);
                    bool had_leading_newline = cur(p)->kind == TK_NEWLINE;
                    skip_newlines(p);
                    if (cur(p)->kind != TK_RPAREN) {
                        if (starts_method_body(p, m->name, had_leading_newline)) {
                            if (strcmp(m->name, "agg") == 0 && (cur(p)->kind == TK_PLUSEQ || cur(p)->kind == TK_MINUS)) {
                                m->body = parse_agg_method_body(p, open);
                            } else {
                                m->body = parse_method_body(p, open);
                            }
                        } else if (parse_sort_args(p, m)) {
                            /* parsed by helper */
                        } else {
                            if (strcmp(m->name, "agg") == 0) {
                                die_at(cur(p)->line, cur(p)->col, ".agg() expects accumulator steps starting with += or -");
                            }
                            for (;;) {
                                vec_push(&m->args, parse_method_argument(p, m->name));
                                skip_newlines(p);
                                if (!match(p, TK_COMMA)) break;
                                skip_newlines(p);
                            }
                        }
                    }
                    expect(p, TK_RPAREN, "expected ')' after method arguments");
                    if (strcmp(m->name, "tap") == 0 && cur(p)->kind == TK_LPAREN) {
                        m->body = parse_block(p);
                    }
                    expr = m;
                    continue;
                }
                d->name = xstrdup(name.text);
            }
            expr = d; continue;
        }
        break;
    }
    return expr;
}

static bool is_shape_separator(Parser *p) {
    return cur(p)->kind == TK_NAME && strcmp(cur(p)->text, "x") == 0;
}

static Node *parse_array_literal(Parser *p, Token open) {
    Node *arr = node_new(NK_ARRAY, open);
    skip_newlines(p);
    if (cur(p)->kind != TK_RBRACKET) {
        Node *first = parse_expression(p, 0);
        skip_newlines(p);

        if (is_shape_separator(p)) {
            Node *shape = node_new(NK_SHAPE, open);
            vec_push(&shape->elements, first);
            while (is_shape_separator(p)) {
                p->pos++;
                skip_newlines(p);
                if (cur(p)->kind == TK_RBRACKET || cur(p)->kind == TK_COMMA) {
                    die_at(cur(p)->line, cur(p)->col, "expected dimension expression after shape separator");
                }
                vec_push(&shape->elements, parse_expression(p, 0));
                skip_newlines(p);
            }
            expect(p, TK_RBRACKET, "expected ']' after shape literal");
            return shape;
        }

        vec_push(&arr->elements, first);
        while (cur(p)->kind != TK_RBRACKET) {
            if (!match(p, TK_COMMA)) break;
            skip_newlines(p);
            if (cur(p)->kind == TK_RBRACKET) break;
            vec_push(&arr->elements, parse_expression(p, 0));
            skip_newlines(p);
        }
    }
    expect(p, TK_RBRACKET, "expected ']' after array literal");
    return arr;
}

static Node *parse_map_literal_after_open(Parser *p, Token open) {
    Node *map = node_new(NK_MAP_LITERAL, open);
    skip_newlines(p);
    while (cur(p)->kind != TK_RBRACE) {
        if (cur(p)->kind == TK_EOF) die_at(cur(p)->line, cur(p)->col, "unterminated map literal");

        Node *key = NULL;
        if (match(p, TK_LBRACKET)) {
            key = parse_expression(p, 0);
            expect(p, TK_RBRACKET, "expected ']' after computed map key");
        } else {
            key = parse_expression(p, 0);
        }

        Node *entry = node_new(NK_FIELD, (Token){TK_NAME, xstrdup("field"), key->line, key->col});
        if (key->kind == NK_STR) entry->text = xstrdup(key->text);
        entry->target = key;
        expect(p, TK_EQUAL, "expected '=' after map literal key");
        entry->value = parse_expression(p, 0);
        vec_push(&map->fields, entry);

        skip_newlines(p);
        match(p, TK_COMMA);
        skip_newlines(p);
    }
    expect(p, TK_RBRACE, "expected '}' after map literal");
    return map;
}

static Node *parse_record_literal_until(Parser *p, Token open, TokenKind close_kind, const char *close_message) {
    Node *record = node_new(NK_RECORD_LITERAL, open);
    skip_newlines(p);
    while (cur(p)->kind != close_kind) {
        if (cur(p)->kind == TK_EOF) die_at(cur(p)->line, cur(p)->col, "unterminated fixed-map literal");
        Token name = expect(p, TK_NAME, "expected fixed-map field name");
        Node *field = node_new(NK_FIELD, name);
        field->name = xstrdup(name.text);
        expect(p, TK_EQUAL, "expected '=' after fixed-map field name");
        field->value = parse_expression(p, 0);
        vec_push(&record->fields, field);
        skip_newlines(p);
        match(p, TK_COMMA);
        skip_newlines(p);
    }
    expect(p, close_kind, close_message);
    return record;
}

static Node *parse_brace_literal(Parser *p, Token open) {
    skip_newlines(p);
    if (cur(p)->kind == TK_NAME && peek(p, 1)->kind == TK_EQUAL) {
        return parse_record_literal_until(p, open, TK_RBRACE, "expected '}' after fixed-map literal");
    }

    if (cur(p)->kind != TK_RBRACE) {
        int save_pos = p->pos;
        bool save_newline_ops = p->allow_newline_ops;
        Node *wrapped = parse_expression(p, 0);
        skip_newlines(p);
        if (match(p, TK_RBRACE)) return wrapped;
        p->pos = save_pos;
        p->allow_newline_ops = save_newline_ops;
    }

    return parse_map_literal_after_open(p, open);
}

static Node *parse_prefix(Parser *p) {
    Token tok = *cur(p);
    if (match(p, TK_INT_LITERAL)) { Node *n = node_new(NK_INT, tok); n->int_value = strtoll(tok.text, NULL, 10); return n; }
    if (match(p, TK_FLT_LITERAL)) { Node *n = node_new(NK_FLT, tok); n->float_value = strtod(tok.text, NULL); return n; }
    if (match(p, TK_BOL_LITERAL)) { Node *n = node_new(NK_BOL, tok); n->bool_value = strcmp(tok.text, "true") == 0; return n; }
    if (match(p, TK_STRING_LITERAL)) { Node *n = node_new(NK_STR, tok); n->text = xstrdup(tok.text); return parse_postfix(p, n); }
    if (match(p, TK_NUL_TYPE)) return node_new(NK_NIL, tok);
    if (match(p, TK_INIT)) return node_new(NK_INIT, tok);
    if (tok.kind == TK_DOT) {
        Node *n = node_new(NK_NAME, tok);
        n->name = xstrdup("e");
        return parse_postfix(p, n);
    }
    if (match(p, TK_IF)) return parse_postfix(p, parse_if_value_expr(p, tok));
    if (match(p, TK_INT) || match(p, TK_FLT) || match(p, TK_STR) || match(p, TK_ARR)) {
        Node *n = node_new(NK_NAME, tok);
        n->name = xstrdup(tok.text);
        return parse_postfix(p, n);
    }
    if (match(p, TK_NAME)) { Node *n = node_new(NK_NAME, tok); n->name = xstrdup(tok.text); return parse_postfix(p, n); }
    if (match(p, TK_LBRACKET)) return parse_postfix(p, parse_array_literal(p, tok));
    if (match(p, TK_LBRACE)) return parse_postfix(p, parse_brace_literal(p, tok));
    if (match(p, TK_STAR)) {
        Node *n = node_new(NK_UNARY, tok);
        n->op = xstrdup(tok.text);
        n->expr = parse_expression(p, 7);

        if (n->expr && n->expr->kind == NK_DOT) {
            Node *dot = n->expr;
            Node *cursor = dot;
            while (cursor->object && cursor->object->kind == NK_DOT) cursor = cursor->object;
            n->expr = cursor->object;
            cursor->object = n;
            return parse_postfix(p, dot);
        }

        return parse_postfix(p, n);
    }
    if (match(p, TK_MINUS) || match(p, TK_BANG)) { Node *n = node_new(NK_UNARY, tok); n->op = xstrdup(tok.text); n->expr = parse_expression(p, 7); return parse_postfix(p, n); }
    if (match(p, TK_REF) || match(p, TK_HOLD)) { Node *n = node_new(NK_UNARY, tok); n->op = xstrdup(tok.text); n->expr = parse_expression(p, 7); return parse_postfix(p, n); }
    if (match(p, TK_LPAREN)) {
        Token open = tok;
        skip_newlines(p);
        if (cur(p)->kind == TK_NAME && peek(p, 1)->kind == TK_EQUAL) {
            die_at(open.line, open.col, "fixed-map literals use '{...}', not '(...)'");
        }
        Node *n = parse_expression(p, 0);
        expect(p, TK_RPAREN, "expected ')' after grouped expression");
        return parse_postfix(p, n);
    }
    die_at(tok.line, tok.col, "expected expression"); return NULL;
}

static Node *parse_expression(Parser *p, int min_prec) {
    Node *left = parse_prefix(p);
    const char *op = NULL; int prec = 0;
    while (true) {
        if (p->allow_newline_ops) skip_newlines(p);

        if (cur(p)->kind == TK_ELSE) {
            if (min_prec > 0) break;
            Token tok = *cur(p);
            p->pos++;
            Node *right = parse_expression(p, 1);
            Node *bin = node_new(NK_BINARY, tok);
            bin->op = xstrdup("else");
            bin->left = left;
            bin->right = right;
            left = bin;
            continue;
        }

        if (cur(p)->kind == TK_IS) {
            if (min_prec > 0) break;
            Token tok = *cur(p);
            p->pos++;
            left = parse_is_check(p, tok, left);
            continue;
        }

        if (cur(p)->kind == TK_ARROW) {
            die_at(cur(p)->line, cur(p)->col, "'->' is only valid in function signatures and FN[...] types");
        }

        prec = precedence(cur(p)->kind, &op);
        if (prec < min_prec || prec <= 0) break;
        Token tok = *cur(p); p->pos++;
        Node *right = parse_expression(p, prec + 1);
        Node *bin = node_new(NK_BINARY, tok); bin->op = xstrdup(op); bin->left = left; bin->right = right; left = bin;
    }
    return left;
}

static Node *parse_statement(Parser *p);
static Node *parse_block(Parser *p) {
    Token open = expect(p, TK_LPAREN, "expected '(' to start block");
    Node *b = node_new(NK_BLOCK, open); skip_newlines(p);
    while (cur(p)->kind != TK_RPAREN) { if (cur(p)->kind == TK_EOF) die_at(cur(p)->line, cur(p)->col, "unterminated block"); vec_push(&b->statements, parse_statement(p)); skip_newlines(p); }
    expect(p, TK_RPAREN, "expected ')' to end block"); return b;
}

static Node *parse_if(Parser *p) {
    Token tok = expect(p, TK_IF, "expected if"); Node *n = node_new(NK_IF, tok);
    expect(p, TK_LPAREN, "expected '(' after if"); n->condition = parse_expression(p, 0); expect(p, TK_RPAREN, "expected ')' after if condition");
    n->then_block = parse_block(p); skip_newlines(p);
    while (match(p, TK_ELX)) { Token et = *prev(p); Node *e = node_new(NK_ELX, et); expect(p, TK_LPAREN, "expected '(' after elx"); e->condition = parse_expression(p, 0); expect(p, TK_RPAREN, "expected ')' after elx condition"); e->body = parse_block(p); vec_push(&n->elx_branches, e); skip_newlines(p); }
    if (match(p, TK_ELSE)) n->else_block = parse_block(p);
    return n;
}

static bool loop_wrapper_inner_can_be_target(Node *inner) {
    return inner && (inner->kind == NK_NAME || inner->kind == NK_DOT || inner->kind == NK_CALL || inner->kind == NK_METHOD_CALL);
}

static bool try_parse_loop_wrapped_arg(Parser *p, TokenKind open_kind, TokenKind close_kind, const char *hint, Node **out) {
    int save = p->pos;
    if (!match(p, open_kind)) return false;

    skip_newlines(p);
    if (cur(p)->kind == close_kind) {
        p->pos = save;
        return false;
    }

    Node *inner = parse_expression(p, 0);
    skip_newlines(p);
    if (cur(p)->kind != close_kind || !loop_wrapper_inner_can_be_target(inner)) {
        p->pos = save;
        return false;
    }

    p->pos++;
    inner->loop_hint = xstrdup(hint);
    *out = inner;
    return true;
}

static Node *parse_loop_argument(Parser *p, Node *loop) {
    Node *arg = NULL;
    if (try_parse_loop_wrapped_arg(p, TK_LBRACKET, TK_RBRACKET, "array", &arg) ||
        try_parse_loop_wrapped_arg(p, TK_LBRACE, TK_RBRACE, "map", &arg)) {
        if (loop->loop_hint) die_at(arg->line, arg->col, "loop accepts one collection wrapper");
        loop->loop_hint = xstrdup(arg->loop_hint);
        return arg;
    }
    return parse_expression(p, 0);
}

static Node *parse_loop(Parser *p) {
    Token tok = expect(p, TK_LOOP, "expected loop"); Node *n = node_new(NK_LOOP, tok);
    expect(p, TK_LPAREN, "expected '(' after loop");
    if (cur(p)->kind != TK_RPAREN) {
        for (;;) {
            vec_push(&n->args, parse_loop_argument(p, n));
            if (cur(p)->kind == TK_AS) break;
            if (!match(p, TK_COMMA)) break;
        }

        if (match(p, TK_AS)) {
            Token name = expect(p, TK_NAME, "expected loop context name after as");
            n->name = xstrdup(name.text);
            if (cur(p)->kind == TK_COMMA) die_at(cur(p)->line, cur(p)->col, "loop context name must come after all loop arguments");
        }
    }
    expect(p, TK_RPAREN, "expected ')' after loop arguments"); n->body = parse_block(p); return n;
}

static bool starts_type(Parser *p) {
    TokenKind k = cur(p)->kind;
    return k == TK_INT || k == TK_FLT || k == TK_BOL || k == TK_STR || k == TK_NUL_TYPE || k == TK_ARR || k == TK_MAP || k == TK_REF || k == TK_TSK || (k == TK_NAME && (peek(p, 1)->kind == TK_NAME || (peek(p, 1)->kind == TK_DOT && peek(p, 2)->kind == TK_NAME && peek(p, 3)->kind == TK_NAME)));
}

static int scan_newlines(Parser *p, int pos) {
    while (p->toks[pos].kind == TK_NEWLINE) pos++;
    return pos;
}

static bool scan_type_end(Parser *p, int pos, int *out);

static bool scan_bracketed_type(Parser *p, int pos, int *out) {
    if (p->toks[pos].kind != TK_LBRACKET) return false;
    pos++;
    pos = scan_newlines(p, pos);
    if (!scan_type_end(p, pos, &pos)) return false;
    pos = scan_newlines(p, pos);
    if (p->toks[pos].kind != TK_RBRACKET) return false;
    *out = pos + 1;
    return true;
}

static bool scan_type_end(Parser *p, int pos, int *out) {
    TokenKind k = p->toks[pos].kind;
    if (k == TK_INT || k == TK_FLT || k == TK_BOL || k == TK_STR || k == TK_NUL_TYPE) {
        *out = pos + 1;
        return true;
    }
    if (k == TK_ARR || k == TK_REF || k == TK_TSK) {
        return scan_bracketed_type(p, pos + 1, out);
    }
    if (k == TK_MAP) {
        pos++;
        if (p->toks[pos].kind != TK_LBRACKET) return false;
        pos++;
        pos = scan_newlines(p, pos);
        if (!scan_type_end(p, pos, &pos)) return false;
        pos = scan_newlines(p, pos);
        if (p->toks[pos].kind != TK_COMMA) return false;
        pos++;
        pos = scan_newlines(p, pos);
        if (!scan_type_end(p, pos, &pos)) return false;
        pos = scan_newlines(p, pos);
        if (p->toks[pos].kind != TK_RBRACKET) return false;
        *out = pos + 1;
        return true;
    }
    if (k == TK_FN) {
        pos++;
        if (p->toks[pos].kind != TK_LBRACKET) return false;
        pos++;
        pos = scan_newlines(p, pos);
        if (!scan_type_end(p, pos, &pos)) return false;
        pos = scan_newlines(p, pos);
        if (p->toks[pos].kind != TK_ARROW) return false;
        pos++;
        pos = scan_newlines(p, pos);
        if (!scan_type_end(p, pos, &pos)) return false;
        pos = scan_newlines(p, pos);
        if (p->toks[pos].kind != TK_RBRACKET) return false;
        *out = pos + 1;
        return true;
    }
    if (k == TK_NAME) {
        pos++;
        if (p->toks[pos].kind == TK_DOT && p->toks[pos + 1].kind == TK_NAME) {
            TokenKind after_member = p->toks[pos + 2].kind;
            if (after_member != TK_COMMA && after_member != TK_RBRACE && after_member != TK_AS) pos += 2;
        }
        *out = pos;
        return true;
    }
    return false;
}

static bool starts_destructure(Parser *p) {
    if (cur(p)->kind != TK_LBRACE) return false;
    int pos = scan_newlines(p, p->pos + 1);
    if (p->toks[pos].kind == TK_RBRACE) return false;
    if (!scan_type_end(p, pos, &pos)) return false;
    pos = scan_newlines(p, pos);
    return p->toks[pos].kind == TK_NAME || p->toks[pos].kind == TK_DOT;
}

static ParsedType parse_destructure_type(Parser *p) {
    if (cur(p)->kind == TK_NAME && peek(p, 1)->kind == TK_DOT && peek(p, 2)->kind == TK_NAME) {
        TokenKind after_member = peek(p, 3)->kind;
        if (after_member == TK_COMMA || after_member == TK_RBRACE || after_member == TK_AS) {
            Token tok = *cur(p);
            p->pos++;
            return (ParsedType){normalize_source_type_name(tok.text), tok};
        }
    }
    return parse_type(p);
}

static Node *parse_destructure_statement(Parser *p) {
    Token open = expect(p, TK_LBRACE, "expected '{' to start destructure pattern");
    Node *n = node_new(NK_DESTRUCTURE, open);
    skip_newlines(p);
    int position = 0;

    while (cur(p)->kind != TK_RBRACE) {
        if (cur(p)->kind == TK_EOF) die_at(cur(p)->line, cur(p)->col, "unterminated destructure pattern");

        ParsedType t = parse_destructure_type(p);
        Node *field = node_new(NK_FIELD, t.token);
        field->declared_type = t.type;
        field->int_value = position++;

        if (match(p, TK_DOT)) {
            Token source = expect(p, TK_NAME, "expected source field name after '.' in destructure pattern");
            field->text = xstrdup(source.text);
            if (match(p, TK_AS)) {
                Token alias = expect(p, TK_NAME, "expected local name after 'as' in destructure pattern");
                field->name = xstrdup(alias.text);
            } else {
                field->name = xstrdup(source.text);
            }
        } else {
            Token local = expect(p, TK_NAME, "expected local name in destructure pattern");
            field->name = xstrdup(local.text);
        }

        vec_push(&n->fields, field);
        skip_newlines(p);
        if (!match(p, TK_COMMA)) break;
        skip_newlines(p);
    }

    expect(p, TK_RBRACE, "expected '}' after destructure pattern");
    expect(p, TK_EQUAL, "expected '=' after destructure pattern");
    n->value = parse_expression(p, 0);
    return n;
}

static Node *parse_statement(Parser *p) {
    Token tok = *cur(p);
    if (tok.kind == TK_FN) return parse_fn_decl(p);
    if (starts_destructure(p)) return parse_destructure_statement(p);
    if (starts_type(p)) {
        ParsedType t = parse_type(p); Token name = expect(p, TK_NAME, "expected variable name"); Node *n = node_new(NK_VAR, t.token); n->declared_type = t.type; n->name = xstrdup(name.text); if (match(p, TK_EQUAL)) n->value = parse_expression(p, 0); return n;
    }
    if (tok.kind == TK_RET) { p->pos++; Node *n = node_new(NK_RET, tok); if (cur(p)->kind != TK_NEWLINE && cur(p)->kind != TK_RPAREN && cur(p)->kind != TK_EOF) n->value = parse_expression(p, 0); return n; }
    if (tok.kind == TK_IF) return parse_if(p);
    if (tok.kind == TK_LOOP) return parse_loop(p);
    if (tok.kind == TK_PASS) { p->pos++; return node_new(NK_PASS, tok); }
    if (tok.kind == TK_THROW) { p->pos++; Node *n = node_new(NK_THROW, tok); expect(p, TK_LPAREN, "expected '(' after throw"); n->value = parse_expression(p, 0); expect(p, TK_RPAREN, "expected ')' after throw message"); return n; }
    if (tok.kind == TK_FREE) {
        p->pos++;
        Node *n = node_new(NK_FREE, tok);
        if (match(p, TK_LPAREN)) {
            n->expr = parse_expression(p, 0);
            expect(p, TK_RPAREN, "expected ')' after FREE value");
        } else {
            n->expr = parse_expression(p, 0);
        }
        return n;
    }
    Node *expr = parse_expression(p, 0);
    if (match(p, TK_EQUAL)) { Node *n = node_new(NK_ASSIGN, tok); n->op = xstrdup("="); n->target = expr; n->value = parse_expression(p, 0); return n; }
    if (match(p, TK_PLUSEQ)) { Node *n = node_new(NK_ASSIGN, tok); n->op = xstrdup("+="); n->target = expr; n->value = parse_expression(p, 0); return n; }
    if (match(p, TK_MINUSEQ)) { Node *n = node_new(NK_ASSIGN, tok); n->op = xstrdup("-="); n->target = expr; n->value = parse_expression(p, 0); return n; }
    if (match(p, TK_PLUSPLUS)) { Node *n = node_new(NK_ASSIGN, tok); n->op = xstrdup("++"); n->target = expr; return n; }
    Node *n = node_new(NK_EXPR_STMT, tok); n->expr = expr; return n;
}

static void parse_type_struct_fields(Parser *p, Node *decl, const char *decl_name, TokenKind close_kind, const char *close_message) {
    skip_newlines(p);
    while (cur(p)->kind != close_kind) {
        if (cur(p)->kind == TK_EOF) die_at(cur(p)->line, cur(p)->col, "unterminated TYPE struct");

        if (starts_inline_array_map(p) && peek(p, 4)->kind == TK_NAME && peek(p, 5)->kind == TK_EQUAL) {
            Token arr_tok = expect(p, TK_ARR, "expected ARR");
            expect(p, TK_LBRACKET, "expected '[' after ARR");
            expect(p, TK_MAP, "expected MAP inside ARR[MAP]");
            expect(p, TK_RBRACKET, "expected ']' after ARR[MAP]");
            Token field_name = expect(p, TK_NAME, "expected nested ARR[MAP] field name");
            expect(p, TK_EQUAL, "expected '=' after nested ARR[MAP] field name");
            expect(p, TK_LBRACKET, "expected '[' before nested ARR[MAP] element definition");
            expect(p, TK_LBRACE, "expected '{' for nested ARR[MAP] element definition");

            char *nested_name = strf("%s_%s", decl_name, field_name.text);
            Node *nested = node_new(NK_TYPE_STRUCT, arr_tok);
            nested->name = xstrdup(nested_name);
            parse_type_struct_fields(p, nested, nested_name, TK_RBRACE, "expected '}' after nested ARR[MAP] element definition");
            expect(p, TK_RBRACKET, "expected ']' after nested ARR[MAP] element definition");
            vec_push(&decl->type_decls, nested);

            Node *field = node_new(NK_FIELD, arr_tok);
            field->declared_type = array_type(nested_name);
            field->name = xstrdup(field_name.text);
            field->text = xstrdup(field->declared_type);
            field->value = node_new(NK_ARRAY, arr_tok);
            vec_push(&decl->fields, field);
        } else if (cur(p)->kind == TK_MAP && peek(p, 1)->kind == TK_NAME && peek(p, 2)->kind == TK_EQUAL) {
            Token map_tok = expect(p, TK_MAP, "expected MAP");
            Token field_name = expect(p, TK_NAME, "expected nested MAP field name");
            expect(p, TK_EQUAL, "expected '=' after nested MAP field name");
            if (match(p, TK_LPAREN)) {
                die_at(prev(p)->line, prev(p)->col, "fixed MAP definitions use '{...}', not '(...)'");
            }
            expect(p, TK_LBRACE, "expected '{' for nested MAP definition");

            char *nested_name = strf("%s_%s", decl_name, field_name.text);
            Node *nested = node_new(NK_TYPE_STRUCT, map_tok);
            nested->name = xstrdup(nested_name);
            parse_type_struct_fields(p, nested, nested_name, TK_RBRACE, "expected '}' after nested MAP definition");
            vec_push(&decl->type_decls, nested);

            Node *field = node_new(NK_FIELD, map_tok);
            field->declared_type = xstrdup(nested_name);
            field->name = xstrdup(field_name.text);
            field->text = xstrdup(nested_name);
            field->value = node_new(NK_INIT, map_tok);
            vec_push(&decl->fields, field);
        } else {
            ParsedType field_type = parse_type(p);
            Token field_name = expect(p, TK_NAME, "expected field name");
            Node *field = node_new(NK_FIELD, field_type.token);
            field->declared_type = field_type.type;
            field->name = xstrdup(field_name.text);
            field->text = xstrdup(field_type.type);
            if (match(p, TK_EQUAL)) field->value = parse_expression(p, 0);
            vec_push(&decl->fields, field);
        }

        skip_newlines(p);
        match(p, TK_COMMA);
        skip_newlines(p);
    }
    expect(p, close_kind, close_message);
}

static Node *parse_inline_array_map_alias(Parser *p) {
    Token arr_tok = expect(p, TK_ARR, "expected ARR");
    expect(p, TK_LBRACKET, "expected '[' after ARR");
    expect(p, TK_MAP, "expected MAP inside ARR[MAP]");
    expect(p, TK_RBRACKET, "expected ']' after ARR[MAP]");
    Token name = expect(p, TK_NAME, "expected array name after ARR[MAP]");
    expect(p, TK_EQUAL, "expected '=' after ARR[MAP] name");
    expect(p, TK_LBRACKET, "expected '[' before ARR[MAP] element definition");
    expect(p, TK_LBRACE, "expected '{' for ARR[MAP] element definition");

    char *nested_name = strf("%s_item", name.text);
    Node *nested = node_new(NK_TYPE_STRUCT, arr_tok);
    nested->name = xstrdup(nested_name);
    parse_type_struct_fields(p, nested, nested_name, TK_RBRACE, "expected '}' after ARR[MAP] element definition");
    expect(p, TK_RBRACKET, "expected ']' after ARR[MAP] element definition");

    Node *decl = node_new(NK_TYPE_ALIAS, arr_tok);
    decl->name = xstrdup(name.text);
    decl->declared_type = array_type(nested_name);
    decl->value = node_new(NK_ARRAY, arr_tok);
    vec_push(&decl->type_decls, nested);
    return decl;
}

static Node *parse_type_decl(Parser *p) {
    Token tok = *cur(p);
    if (!match(p, TK_TYPE) && !match(p, TK_MAP)) die_at(tok.line, tok.col, "expected MAP declaration");
    Token name = expect(p, TK_NAME, "expected map/type name after MAP");
    expect(p, TK_EQUAL, "expected '=' in MAP declaration");

    if (match(p, TK_LPAREN)) {
        die_at(prev(p)->line, prev(p)->col, "fixed MAP definitions use '{...}', not '(...)'");
    }

    if (match(p, TK_LBRACE)) {
        TokenKind close_kind = TK_RBRACE;
        const char *close_message = "expected '}' after TYPE struct";
        Node *decl = node_new(NK_TYPE_STRUCT, tok);
        decl->name = xstrdup(name.text);
        parse_type_struct_fields(p, decl, decl->name, close_kind, close_message);
        return decl;
    }

    if (tok.kind == TK_MAP) {
        die_at(cur(p)->line, cur(p)->col, "named dynamic MAP and ARR values use type-first syntax, for example MAP[STR, INT] Scores = {...} or ARR[BOL] Row = []");
    }

    ParsedType alias = parse_type(p);
    Node *decl = node_new(NK_TYPE_ALIAS, tok);
    decl->name = xstrdup(name.text);
    decl->declared_type = alias.type;
    if (cur(p)->kind != TK_NEWLINE && cur(p)->kind != TK_EOF) decl->value = parse_expression(p, 0);
    return decl;
}

static Node *parse_fn_decl(Parser *p) {
    Token ft = expect(p, TK_FN, "expected function declaration");
    Token name = expect(p, TK_NAME, "expected function name after FN");
    Node *fn = node_new(NK_FN, ft);
    fn->name = xstrdup(name.text);

    expect(p, TK_LPAREN, "expected '(' after function name");
    skip_newlines(p);
    if (cur(p)->kind != TK_RPAREN) {
        for (;;) {
            ParsedType t = parse_type(p);
            Token pn = expect(p, TK_NAME, "expected parameter name");
            Node *param = node_new(NK_PARAM, t.token);
            param->declared_type = t.type;
            param->name = xstrdup(pn.text);
            vec_push(&fn->params, param);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
            skip_newlines(p);
        }
    }
    expect(p, TK_RPAREN, "expected ')' after function parameters");
    if (match(p, TK_ARROW)) {
        ParsedType ret = parse_type(p);
        fn->declared_type = ret.type;
    }
    expect(p, TK_EQUAL, "expected '=' before function body");
    fn->body = parse_block(p);
    return fn;
}

static Node *parse_program(TokenVec toks) {
    Parser p = {0};
    p.toks = toks.items;
    Token first = *cur(&p); Node *prog = node_new(NK_PROGRAM, first); skip_newlines(&p);
    while (cur(&p)->kind != TK_EOF) {
        if (starts_inline_array_map(&p) && peek(&p, 4)->kind == TK_NAME && peek(&p, 5)->kind == TK_EQUAL) {
            vec_push(&prog->type_decls, parse_inline_array_map_alias(&p));
            skip_newlines(&p);
            continue;
        }

        if ((cur(&p)->kind == TK_TYPE || cur(&p)->kind == TK_MAP) && peek(&p, 1)->kind == TK_NAME && peek(&p, 2)->kind == TK_EQUAL) {
            vec_push(&prog->type_decls, parse_type_decl(&p));
            skip_newlines(&p);
            continue;
        }

        if (starts_type(&p)) {
            ParsedType t = parse_type(&p);
            Token name = expect(&p, TK_NAME, "expected top-level value name");
            Node *decl = node_new(NK_TYPE_ALIAS, t.token);
            decl->declared_type = t.type;
            decl->name = xstrdup(name.text);
            if (match(&p, TK_EQUAL)) {
                decl->value = parse_expression(&p, 0);
            } else {
                die_at(name.line, name.col, "expected '=' after top-level named value");
            }
            vec_push(&prog->type_decls, decl);
            skip_newlines(&p);
            continue;
        }

        if (cur(&p)->kind == TK_FN) {
            vec_push(&prog->functions, parse_fn_decl(&p));
            skip_newlines(&p);
            continue;
        }

        vec_push(&prog->statements, parse_statement(&p));
        skip_newlines(&p);
    }
    return prog;
}
