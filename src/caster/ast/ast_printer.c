// AST Printer
// -----------
//
// The CLI prints this JSON-like tree after analysis. That means it includes
// checkedType and lowered helper fields such as cExpr for contextual loop names.

// ----------------------------- AST printer -----------------------------

static void indent(int n){ for(int i=0;i<n;i++) putchar(' '); }
static void print_node(Node *n, int in);
static void print_vec(const char *name, PtrVec *v, int in){ indent(in); printf("\"%s\": [\n",name); for(int i=0;i<v->len;i++){ print_node(v->items[i],in+2); if(i+1<v->len) printf(","); printf("\n"); } indent(in); printf("]"); }
static void print_node(Node *n, int in){ indent(in); printf("{\n"); indent(in+2); printf("\"kind\": \"%s\",\n",node_kind_name(n->kind)); indent(in+2); printf("\"loc\": {\"line\": %d, \"col\": %d}",n->line,n->col); if(n->checked_type) printf(",\n"), indent(in+2), printf("\"checkedType\": \"%s\"",n->checked_type); if(n->name) printf(",\n"), indent(in+2), printf("\"name\": \"%s\"",n->name); if(n->alias) printf(",\n"), indent(in+2), printf("\"alias\": \"%s\"",n->alias); if(n->declared_type) printf(",\n"), indent(in+2), printf("\"declaredType\": \"%s\"",n->declared_type); if(n->inferred_decl) printf(",\n"), indent(in+2), printf("\"inferredDecl\": true"); if(n->c_expr) printf(",\n"), indent(in+2), printf("\"cExpr\": \"%s\"",n->c_expr); if(n->loop_kind) printf(",\n"), indent(in+2), printf("\"loopKind\": \"%s\"",n->loop_kind); if(n->kind==NK_INT) printf(",\n"), indent(in+2), printf("\"value\": %lld",(long long)n->int_value); if(n->kind==NK_FLT) printf(",\n"), indent(in+2), printf("\"value\": %.17g",n->float_value); if(n->kind==NK_BOL) printf(",\n"), indent(in+2), printf("\"value\": %s",n->bool_value?"true":"false"); if(n->kind==NK_STR) printf(",\n"), indent(in+2), printf("\"value\": \"%s\"",n->text); if(n->kind==NK_FIELD && n->text) printf(",\n"), indent(in+2), printf("\"key\": \"%s\"",n->text); if(n->op) printf(",\n"), indent(in+2), printf("\"op\": \"%s\"",n->op);
#define CHILD(field,name) if(n->field){ printf(",\n"); indent(in+2); printf("\"%s\": ",name); print_node(n->field,in+2); }
    CHILD(body,"body"); CHILD(value,"value"); CHILD(target,"target"); CHILD(expr,"expr"); CHILD(condition,"condition"); CHILD(then_block,"thenBlock"); CHILD(else_block,"elseBlock"); CHILD(object,"object"); CHILD(index,"index"); CHILD(callee,"callee"); CHILD(left,"left"); CHILD(right,"right");
#undef CHILD
#define VEC(field,name) if(n->field.len){ printf(",\n"); print_vec(name,&n->field,in+2); }
    VEC(type_decls,"typeDecls"); VEC(globals,"globals"); VEC(fields,"fields"); VEC(functions,"functions"); VEC(params,"params"); VEC(statements,"statements"); VEC(args,"args"); VEC(elements,"elements"); VEC(elx_branches,"elxBranches");
#undef VEC
    printf("\n"); indent(in); printf("}"); }
