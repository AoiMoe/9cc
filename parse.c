#include "9cc.h"

// This is a recursive-descendent parser which constructs abstract
// syntax tree from input tokens.
//
// Variable names are resolved at this stage. We create a Var object
// when we see a variable definition and use it when we see a variable
// reference.
//
// Types are added to variables and literals. For other nodes, Sema
// will add type for them.
//
// Semantic checking is omitted from this parser to make the code in
// this file closely resemble the C's BNF. Invalid expressions, such
// as `1+2=3`, are accepted at this stage. Such errors are detected in
// a later pass.

int nlabel = 1;

//
// token utils
//
static Vector *tokens;
static int pos;

static void expect(int ty) {
  Token *t = tokens->data[pos];
  if (t->ty == ty) {
    pos++;
    return;
  }

  if (isprint(ty))
    bad_token(t, format("%c expected", ty));
  assert(ty == TK_WHILE);
  bad_token(t, "'while' expected");
}

static bool consume(int ty) {
  Token *t = tokens->data[pos];
  if (t->ty != ty)
    return false;
  pos++;
  return true;
}

static Token *peek() {
  return tokens->data[pos];
}

static Token *get() {
  return tokens->data[pos++];
}

static void unget(Token *t) {
  pos--;
  assert(tokens->data[pos] == t);
}

//
// env utils
//
typedef struct Env {
  Map *vars;
  Map *typedefs;
  Map *tags;
  struct Env *prev;
} Env;
struct Env *env;

static Env *new_env(Env *prev) {
  Env *env = calloc(1, sizeof(Env));
  env->vars = new_map();
  env->typedefs = new_map();
  env->tags = new_map();
  env->prev = prev;
  return env;
}

static void push_env() {
  env = new_env(env);
}

static void pop_env() {
  assert(env);
  env = env->prev;
}

static Var *new_var(Type *ty, char *name, bool is_local, char *data) {
  Var *var = calloc(1, sizeof(Var));
  var->ty = ty;
  var->is_local = is_local;
  var->name = name;
  var->data = data;
  return var;
}

static void add_var(Var *var) {
  map_put(env->vars, var->name, var);
}

static Var *find_var(char *name) {
  for (Env *e = env; e; e = e->prev) {
    Var *var = map_get(e->vars, name);
    if (var)
      return var;
  }
  return NULL;
}

static Type *find_typedef(char *name) {
  for (Env *e = env; e; e = e->prev) {
    Type *ty = map_get(e->typedefs, name);
    if (ty)
      return ty;
  }
  return NULL;
}

static Type *find_tag(char *name) {
  for (Env *e = env; e; e = e->prev) {
    Type *ty = map_get(e->tags, name);
    if (ty)
      return ty;
  }
  return NULL;
}

static Program *prog;

static Vector *lvars;
static Vector *breaks;
static Vector *continues;
static Vector *switches;

static Node null_stmt = {ND_NULL};

static void alloc_local_storage(Var *var) {
  assert(var->is_local);
  assert(env->prev != NULL);
  vec_push(lvars, var);
}

static void alloc_global_storage(Var *var) {
  assert(!var->is_local);
  vec_push(prog->gvars, var);
}

static Node *assign();
static Node *expr();
static Node *stmt();

static bool is_typename() {
  Token *t = peek();
  if (t->ty == TK_IDENT)
    return find_typedef(t->name);
  return t->ty == TK_INT || t->ty == TK_CHAR || t->ty == TK_VOID ||
         t->ty == TK_STRUCT || t->ty == TK_TYPEOF || t->ty == TK_BOOL;
}

static Node *declaration_type();

static void fix_struct_offsets(Type *ty) {
  Vector *types = ty->members->vals;

  int off = 0;
  for (int i = 0; i < types->len; i++) {
    Type *t2 = types->data[i];
    off = roundup(off, t2->align);
    t2->offset = off;
    off += t2->size;

    if (ty->align < t2->align)
      ty->align = t2->align;
  }
  ty->size = roundup(off, ty->align);
}

static Type *decl_specifiers() {
  Token *t = get();

  if (t->ty == TK_IDENT) {
    Type *ty = find_typedef(t->name);
    if (!ty)
      unget(t);
    return ty;
  }

  if (t->ty == TK_VOID)
    return void_ty();
  if (t->ty == TK_BOOL)
    return bool_ty();
  if (t->ty == TK_CHAR)
    return char_ty();
  if (t->ty == TK_INT)
    return int_ty();

  if (t->ty == TK_TYPEOF) {
    expect('(');
    Node *node = expr();
    expect(')');
    return get_type(node);
  }

  if (t->ty == TK_STRUCT) {
    Token *t = peek();
    Type *ty = NULL;
    char *tag = NULL;

    if (consume(TK_IDENT)) {
      tag = t->name;
      ty = find_tag(tag);
    }

    if (!ty) {
      ty = calloc(1, sizeof(Type));
      ty->ty = STRUCT;
    }

    if (consume('{')) {
      ty->members = new_map();
      while (!consume('}')) {
        Node *node = declaration_type();
        map_put(ty->members, node->name, node->ty);
      }
      fix_struct_offsets(ty);
    }

    if (!tag && !ty->members)
      bad_token(t, "bad struct definition");
    if (tag)
      map_put(env->tags, tag, ty);
    return ty;
  }

  bad_token(t, "typename expected");
}

static Node *new_node(int op, Token *t) {
  Node *node = calloc(1, sizeof(Node));
  node->op = op;
  node->token = t;
  return node;
}

static Node *new_binop(int op, Token *t, Node *lhs, Node *rhs) {
  Node *node = new_node(op, t);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

static Node *new_expr(int op, Token *t, Node *expr) {
  Node *node = new_node(op, t);
  node->expr = expr;
  return node;
}

static Node *new_varref(Token *t, Var *var) {
  Node *node = new_node(ND_VARREF, t);
  node->ty = var->ty;
  node->var = var;
  return node;
}

static Node *new_deref(Token *t, Var *var) {
  return new_expr(ND_DEREF, t, new_varref(t, var));
}

Node *new_int_node(int val, Token *t) {
  Node *node = new_node(ND_NUM, t);
  node->ty = int_ty();
  node->val = val;
  return node;
}

static Node *compound_stmt();

static char *ident() {
  Token *t = get();
  if (t->ty != TK_IDENT)
    bad_token(t, "identifier expected");
  return t->name;
}

static Node *string_literal(Token *t) {
  Type *ty = ary_of(char_ty(), t->len);
  char *name = format(".L.str%d", nlabel++);

  Node *node = new_node(ND_VARREF, t);
  node->ty = ty;
  node->var = new_var(ty, name, false, t->str);
  alloc_global_storage(node->var);
  return node;
}

static Node *local_variable(Token *t) {
  Var *var = find_var(t->name);
  if (!var)
    bad_token(t, "undefined variable");
  Node *node = new_node(ND_VARREF, t);
  node->ty = var->ty;
  node->name = t->name;
  node->var = var;
  return node;
}

static Node *function_call(Token *t) {
  Var *var = find_var(t->name);

  Node *node = new_node(ND_CALL, t);
  node->name = t->name;
  node->args = new_vec();

  if (var && var->ty->ty == FUNC) {
    node->ty = var->ty;
  } else {
    warn_token(t, "undefined function");
    node->ty = func_ty(int_ty());
  }

  while (!consume(')')) {
    if (node->args->len > 0)
      expect(',');
    vec_push(node->args, assign());
  }
  return node;
}

static Node *stmt_expr() {
  Token *t = peek();
  Vector *v = new_vec();

  push_env();
  do {
    vec_push(v, stmt());
  } while (!consume('}'));
  expect(')');
  pop_env();

  Node *last = vec_pop(v);
  if (last->op != ND_EXPR_STMT)
    bad_token(last->token, "statement expression returning void");

  Node *node = new_node(ND_STMT_EXPR, t);
  node->stmts = v;
  node->expr = last->expr;
  return node;
}

static Node *primary() {
  Token *t = get();

  if (t->ty == '(') {
    if (consume('{'))
      return stmt_expr();
    Node *node = expr();
    expect(')');
    return node;
  }

  if (t->ty == TK_NUM)
    return new_int_node(t->val, t);

  if (t->ty == TK_STR)
    return string_literal(t);

  if (t->ty == TK_IDENT) {
    if (consume('('))
      return function_call(t);
    return local_variable(t);
  }

  bad_token(t, "primary expression expected");
}

static Node *mul();

static Node *new_stmt_expr(Token *t, Vector *exprs) {
  Node *last = vec_pop(exprs);

  Vector *v = new_vec();
  for (int i = 0; i < exprs->len; i++)
    vec_push(v, new_expr(ND_EXPR_STMT, t, exprs->data[i]));

  Node *node = new_node(ND_STMT_EXPR, t);
  node->stmts = v;
  node->expr = last;
  return node;
}

// `x++` where x is of type T is compiled as
// `({ T *y = &x; T z = *y; *y = *y + 1; *z; })`.
static Node *new_post_inc(Token *t, Node *e, int imm) {
  Vector *v = new_vec();

  Var *var1 = new_var(ptr_to(e->ty), ".tmp", true, NULL);
  alloc_local_storage(var1);
  Var *var2 = new_var(e->ty, ".tmp", true, NULL);
  alloc_local_storage(var2);

  vec_push(v, new_binop('=', t, new_varref(t, var1), new_expr(ND_ADDR, t, e)));
  vec_push(v, new_binop('=', t, new_varref(t, var2), new_deref(t, var1)));
  vec_push(v, new_binop(
                  '=', t, new_deref(t, var1),
                  new_binop('+', t, new_deref(t, var1), new_int_node(imm, t))));
  vec_push(v, new_varref(t, var2));
  return new_stmt_expr(t, v);
}

static Node *postfix() {
  Node *lhs = primary();

  for (;;) {
    Token *t = peek();

    if (consume(TK_INC)) {
      lhs = new_post_inc(t, lhs, 1);
      continue;
    }

    if (consume(TK_DEC)) {
      lhs = new_post_inc(t, lhs, -1);
      continue;
    }

    if (consume('.')) {
      lhs = new_expr(ND_DOT, t, lhs);
      lhs->name = ident();
      continue;
    }

    if (consume(TK_ARROW)) {
      lhs = new_expr(ND_DOT, t, new_expr(ND_DEREF, t, lhs));
      lhs->name = ident();
      continue;
    }

    if (consume('[')) {
      Node *node = new_binop('+', t, lhs, assign());
      lhs = new_expr(ND_DEREF, t, node);
      expect(']');
      continue;
    }
    return lhs;
  }
}

static Node *new_assign_eq(int op, Node *lhs, Node *rhs);

static Node *unary() {
  Token *t = peek();

  if (consume('-'))
    return new_binop('-', t, new_int_node(0, t), unary());
  if (consume('*'))
    return new_expr(ND_DEREF, t, unary());
  if (consume('&'))
    return new_expr(ND_ADDR, t, unary());
  if (consume('!'))
    return new_expr('!', t, unary());
  if (consume('~'))
    return new_expr('~', t, unary());
  if (consume(TK_SIZEOF))
    return new_int_node(get_type(unary())->size, t);
  if (consume(TK_ALIGNOF))
    return new_int_node(get_type(unary())->align, t);
  if (consume(TK_INC))
    return new_assign_eq('+', unary(), new_int_node(1, t));
  if (consume(TK_DEC))
    return new_assign_eq('-', unary(), new_int_node(1, t));
  return postfix();
}

static Node *mul() {
  Node *lhs = unary();
  for (;;) {
    Token *t = peek();
    if (consume('*'))
      lhs = new_binop('*', t, lhs, unary());
    else if (consume('/'))
      lhs = new_binop('/', t, lhs, unary());
    else if (consume('%'))
      lhs = new_binop('%', t, lhs, unary());
    else
      return lhs;
  }
}

static Node *add() {
  Node *lhs = mul();
  for (;;) {
    Token *t = peek();
    if (consume('+'))
      lhs = new_binop('+', t, lhs, mul());
    else if (consume('-'))
      lhs = new_binop('-', t, lhs, mul());
    else
      return lhs;
  }
}

static Node *shift() {
  Node *lhs = add();
  for (;;) {
    Token *t = peek();
    if (consume(TK_SHL))
      lhs = new_binop(ND_SHL, t, lhs, add());
    else if (consume(TK_SHR))
      lhs = new_binop(ND_SHR, t, lhs, add());
    else
      return lhs;
  }
}

static Node *relational() {
  Node *lhs = shift();
  for (;;) {
    Token *t = peek();
    if (consume('<'))
      lhs = new_binop('<', t, lhs, shift());
    else if (consume('>'))
      lhs = new_binop('<', t, shift(), lhs);
    else if (consume(TK_LE))
      lhs = new_binop(ND_LE, t, lhs, shift());
    else if (consume(TK_GE))
      lhs = new_binop(ND_LE, t, shift(), lhs);
    else
      return lhs;
  }
}

static Node *equality() {
  Node *lhs = relational();
  for (;;) {
    Token *t = peek();
    if (consume(TK_EQ))
      lhs = new_binop(ND_EQ, t, lhs, relational());
    else if (consume(TK_NE))
      lhs = new_binop(ND_NE, t, lhs, relational());
    else
      return lhs;
  }
}

static Node *bit_and() {
  Node *lhs = equality();
  while (consume('&')) {
    Token *t = peek();
    lhs = new_binop('&', t, lhs, equality());
  }
  return lhs;
}

static Node *bit_xor() {
  Node *lhs = bit_and();
  while (consume('^')) {
    Token *t = peek();
    lhs = new_binop('^', t, lhs, bit_and());
  }
  return lhs;
}

static Node *bit_or() {
  Node *lhs = bit_xor();
  while (consume('|')) {
    Token *t = peek();
    lhs = new_binop('|', t, lhs, bit_xor());
  }
  return lhs;
}

static Node *logand() {
  Node *lhs = bit_or();
  while (consume(TK_LOGAND)) {
    Token *t = peek();
    lhs = new_binop(ND_LOGAND, t, lhs, bit_or());
  }
  return lhs;
}

static Node *logor() {
  Node *lhs = logand();
  while (consume(TK_LOGOR)) {
    Token *t = peek();
    lhs = new_binop(ND_LOGOR, t, lhs, logand());
  }
  return lhs;
}

static Node *conditional() {
  Node *cond = logor();
  Token *t = peek();
  if (!consume('?'))
    return cond;

  Node *node = new_node('?', t);
  node->cond = cond;
  node->then = expr();
  expect(':');
  node->els = conditional();
  return node;
}

// `x op= y` where x is of type T is compiled as
// `({ T *z = &x; *z = *z op y; })`.
static Node *new_assign_eq(int op, Node *lhs, Node *rhs) {
  Vector *v = new_vec();
  Token *t = lhs->token;

  // T *z = &x
  Var *var = new_var(ptr_to(lhs->ty), ".tmp", true, NULL);
  alloc_local_storage(var);
  vec_push(v, new_binop('=', t, new_varref(t, var), new_expr(ND_ADDR, t, lhs)));

  // *z = *z op y
  vec_push(v, new_binop('=', t, new_deref(t, var),
                        new_binop(op, t, new_deref(t, var), rhs)));
  return new_stmt_expr(t, v);
}

static Node *assign() {
  Node *lhs = conditional();
  Token *t = peek();

  if (consume('='))
    return new_binop('=', t, lhs, assign());
  if (consume(TK_MUL_EQ))
    return new_assign_eq('*', lhs, assign());
  if (consume(TK_DIV_EQ))
    return new_assign_eq('/', lhs, assign());
  if (consume(TK_MOD_EQ))
    return new_assign_eq('%', lhs, assign());
  if (consume(TK_ADD_EQ))
    return new_assign_eq('+', lhs, assign());
  if (consume(TK_SUB_EQ))
    return new_assign_eq('-', lhs, assign());
  if (consume(TK_SHL_EQ))
    return new_assign_eq(ND_SHL, lhs, assign());
  if (consume(TK_SHR_EQ))
    return new_assign_eq(ND_SHR, lhs, assign());
  if (consume(TK_AND_EQ))
    return new_assign_eq(ND_LOGAND, lhs, assign());
  if (consume(TK_XOR_EQ))
    return new_assign_eq('^', lhs, assign());
  if (consume(TK_OR_EQ))
    return new_assign_eq('|', lhs, assign());
  return lhs;
}

static Node *expr() {
  Node *lhs = assign();
  Token *t = peek();
  if (!consume(','))
    return lhs;
  return new_binop(',', t, lhs, expr());
}

static int const_expr() {
  Token *t = peek();
  Node *node = expr();
  if (node->op != ND_NUM)
    bad_token(t, "constant expression expected");
  return node->val;
}

static Type *read_array(Type *ty) {
  Vector *v = new_vec();

  while (consume('[')) {
    if (consume(']')) {
      vec_pushi(v, -1);
      continue;
    }
    vec_pushi(v, const_expr());
    expect(']');
  }

  for (int i = v->len - 1; i >= 0; i--) {
    int len = (intptr_t)v->data[i];
    ty = ary_of(ty, len);
  }
  return ty;
}

static Node *declarator(Type *ty);

static Node *direct_decl(Type *ty) {
  Token *t = peek();
  Node *node;
  Type *placeholder = calloc(1, sizeof(Type));

  if (t->ty == TK_IDENT) {
    node = new_node(ND_VARDEF, t);
    node->ty = placeholder;
    node->name = ident();
  } else if (consume('(')) {
    node = declarator(placeholder);
    expect(')');
  } else {
    bad_token(t, "bad direct-declarator");
  }

  // Read the second half of type name (e.g. `[3][5]`).
  *placeholder = *read_array(ty);

  // Read an initializer.
  if (consume('='))
    node->init = assign();
  return node;
}

static Node *declarator(Type *ty) {
  while (consume('*'))
    ty = ptr_to(ty);
  return direct_decl(ty);
}

static Node *declaration_type() {
  Type *ty = decl_specifiers();
  Node *node = declarator(ty);
  expect(';');
  return node;
}

static Node *declaration() {
  Type *ty = decl_specifiers();
  Node *node = declarator(ty);
  expect(';');
  Var *var = new_var(node->ty, node->name, true, NULL);
  add_var(var);
  alloc_local_storage(var);

  if (!node->init)
    return &null_stmt;

  // Convert `T var = init` to `T var; var = init`.
  Token *t = node->token;
  Node *lhs = new_varref(t, var);
  Node *rhs = node->init;
  node->init = NULL;

  Node *expr = new_binop('=', t, lhs, rhs);
  return new_expr(ND_EXPR_STMT, t, expr);
}

static Var *param_declaration() {
  Type *ty = decl_specifiers();
  Node *node = declarator(ty);
  ty = node->ty;
  if (ty->ty == ARY)
    ty = ptr_to(ty->ary_of);
  return new_var(ty, node->name, true, NULL);
}

static Node *expr_stmt() {
  Token *t = peek();
  Node *node = new_expr(ND_EXPR_STMT, t, expr());
  expect(';');
  return node;
}

static Node *stmt() {
  Token *t = get();

  switch (t->ty) {
  case TK_TYPEDEF: {
    Node *node = declaration_type();
    assert(node->name);
    map_put(env->typedefs, node->name, node->ty);
    return &null_stmt;
  }
  case TK_IF: {
    Node *node = new_node(ND_IF, t);
    expect('(');
    node->cond = expr();
    expect(')');

    node->then = stmt();

    if (consume(TK_ELSE))
      node->els = stmt();
    return node;
  }
  case TK_FOR: {
    Node *node = new_node(ND_FOR, t);
    expect('(');
    push_env();
    vec_push(breaks, node);
    vec_push(continues, node);

    if (is_typename())
      node->init = declaration();
    else if (!consume(';'))
      node->init = expr_stmt();

    if (!consume(';')) {
      node->cond = expr();
      expect(';');
    }

    if (!consume(')')) {
      node->inc = expr();
      expect(')');
    }

    node->body = stmt();

    vec_pop(breaks);
    vec_pop(continues);
    pop_env();
    return node;
  }
  case TK_WHILE: {
    Node *node = new_node(ND_FOR, t);
    vec_push(breaks, node);
    vec_push(continues, node);

    expect('(');
    node->cond = expr();
    expect(')');
    node->body = stmt();

    vec_pop(breaks);
    vec_pop(continues);
    return node;
  }
  case TK_DO: {
    Node *node = new_node(ND_DO_WHILE, t);
    vec_push(breaks, node);
    vec_push(continues, node);

    node->body = stmt();
    expect(TK_WHILE);
    expect('(');
    node->cond = expr();
    expect(')');
    expect(';');

    vec_pop(breaks);
    vec_pop(continues);
    return node;
  }
  case TK_SWITCH: {
    Node *node = new_node(ND_SWITCH, t);
    node->cases = new_vec();

    expect('(');
    node->cond = expr();
    expect(')');

    vec_push(breaks, node);
    vec_push(switches, node);
    node->body = stmt();
    vec_pop(breaks);
    vec_pop(switches);
    return node;
  }
  case TK_CASE: {
    if (switches->len == 0)
      bad_token(t, "stray case");
    Node *node = new_node(ND_CASE, t);
    node->val = const_expr();
    expect(':');
    node->body = stmt();

    Node *n = vec_last(switches);
    vec_push(n->cases, node);
    return node;
  }
  case TK_BREAK: {
    if (breaks->len == 0)
      bad_token(t, "stray break");
    Node *node = new_node(ND_BREAK, t);
    node->target = vec_last(breaks);
    return node;
  }
  case TK_CONTINUE: {
    if (continues->len == 0)
      bad_token(t, "stray continue");
    Node *node = new_node(ND_CONTINUE, t);
    node->target = vec_last(breaks);
    return node;
  }
  case TK_RETURN: {
    Node *node = new_node(ND_RETURN, t);
    node->expr = expr();
    expect(';');
    return node;
  }
  case '{': {
    push_env();
    Node *node = compound_stmt();
    pop_env();
    return node;
  }
  case ';':
    return &null_stmt;
  default:
    unget(t);
    if (is_typename())
      return declaration();
    return expr_stmt();
  }
}

static Node *compound_stmt() {
  Token *t = peek();
  Node *node = new_node(ND_COMP_STMT, t);
  node->stmts = new_vec();

  while (!consume('}'))
    vec_push(node->stmts, stmt());

  return node;
}

static void toplevel() {
  bool is_typedef = consume(TK_TYPEDEF);
  bool is_extern = consume(TK_EXTERN);

  Type *ty = decl_specifiers();
  while (consume('*'))
    ty = ptr_to(ty);

  char *name = ident();

  // Function prototype or definition
  if (consume('(')) {
    Vector *params = new_vec();
    while (!consume(')')) {
      if (params->len > 0)
        expect(',');
      vec_push(params, param_declaration());
    }

    if (consume(';')) {
      // Function prototype
      // XXX: not implemented
    } else {
      // Function definition
      lvars = new_vec();
      breaks = new_vec();
      continues = new_vec();
      switches = new_vec();

      Type *funty = calloc(1, sizeof(Type));
      funty->ty = FUNC;
      funty->returning = ty;
      add_var(new_var(funty, name, false, NULL));

      Token *t = peek();
      Node *node = new_node(ND_FUNC, t);
      node->name = name;
      node->params = params;
      node->ty = funty;

      expect('{');
      if (is_typedef)
        bad_token(t, "typedef has function definition");

      push_env();
      for (int i=0; i < params->len; i++) {
        add_var(params->data[i]);
        alloc_local_storage(params->data[i]);
      }

      node->body = compound_stmt();
      pop_env();

      Function *fn = calloc(1, sizeof(Function));
      fn->name = name;
      fn->node = node;
      fn->lvars = lvars;
      fn->bbs = new_vec();
      vec_push(prog->funcs, fn);
    }
    return;
  }

  ty = read_array(ty);
  expect(';');

  if (is_typedef) {
    map_put(env->typedefs, name, ty);
    return;
  }

  // Global variable
  Var *var = new_var(ty, name, false, NULL);
  add_var(var);
  if (!is_extern)
    alloc_global_storage(var);
}

static bool is_eof() {
  Token *t = peek();
  return t->ty == TK_EOF;
}

Program *parse(Vector *tokens_) {
  tokens = tokens_;
  pos = 0;
  env = new_env(NULL);

  prog = calloc(1, sizeof(Program));
  prog->gvars = new_vec();
  prog->funcs = new_vec();

  while (!is_eof())
    toplevel();
  return prog;
}
