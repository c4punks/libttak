/**
 * @file bigscript.c
 * @brief BigScript interpreter — lexer, parser, and evaluator.
 *
 * BigScript is a minimal scripting language for LibTTAK that operates on
 * arbitrary-precision numeric types (ttak_bigint_t, ttak_bigreal_t,
 * ttak_bigcomplex_t).  Scripts are compiled to an AST and interpreted
 * in a single pass.
 */

#include <ttak/script/bigscript.h>
#include <ttak/mem/mem.h>
#include <ttak/math/bigint.h>
#include <ttak/math/sum_divisors.h>
#include <ttak/security/sha256.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define MAX_CONSTANTS 1024
#define MAX_CODE 16384
#define MAX_LOCALS 256
#define MAX_FUNCTIONS 64

typedef enum {
    OP_HALT = 0, OP_PUSH_CONST, OP_LOAD_LOCAL, OP_STORE_LOCAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_EQ, OP_NEQ, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_AND, OP_OR, OP_NOT,
    OP_JMP, OP_JMP_IF_FALSE, OP_RETURN,
    OP_CALL, OP_CALL_BUILTIN, OP_POP
} opcode_t;

typedef struct { ttak_bigscript_variant_t value; } constant_t;
typedef struct { uint32_t start_ip; uint32_t arity; uint32_t locals_count; char name[64]; } function_t;

struct ttak_bigscript_program_t {
    uint32_t code[MAX_CODE]; uint32_t code_len;
    constant_t constants[MAX_CONSTANTS]; uint32_t constants_len;
    function_t functions[MAX_FUNCTIONS]; uint32_t functions_len;
    ttak_bigscript_limits_t limits;
    char source_sha256[65];
};

struct ttak_bigscript_vm_t {
    ttak_bigscript_limits_t limits;
    ttak_bigscript_variant_t *stack; uint32_t stack_cap; uint32_t stack_top;
    ttak_bigscript_variant_t *locals; uint32_t locals_cap; uint32_t locals_top;
    uint32_t call_depth; uint32_t steps; uint64_t now;
};

static void variant_init(ttak_bigscript_variant_t *v, uint64_t now) {
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->type = TTAK_BIGSCRIPT_VAL_INT;
    ttak_bigint_init(&v->v.i, now);
}

static void variant_free(ttak_bigscript_variant_t *v, uint64_t now) {
    if (!v) return;
    if (v->type == TTAK_BIGSCRIPT_VAL_INT) ttak_bigint_free(&v->v.i, now);
    else if (v->type == TTAK_BIGSCRIPT_VAL_REAL) ttak_bigreal_free(&v->v.r, now);
    else if (v->type == TTAK_BIGSCRIPT_VAL_COMPLEX) ttak_bigcomplex_free(&v->v.c, now);
    v->type = TTAK_BIGSCRIPT_VAL_INT;
}

static bool variant_copy(ttak_bigscript_variant_t *dst, const ttak_bigscript_variant_t *src, uint64_t now) {
    if (!dst || !src) return false;
    if (dst == src) return true;
    variant_free(dst, now);
    dst->type = src->type;
    if (src->type == TTAK_BIGSCRIPT_VAL_INT) {
        ttak_bigint_init(&dst->v.i, now);
        return ttak_bigint_copy(&dst->v.i, &src->v.i, now);
    } else if (src->type == TTAK_BIGSCRIPT_VAL_REAL) {
        ttak_bigreal_init(&dst->v.r, now);
        return ttak_bigreal_copy(&dst->v.r, &src->v.r, now);
    } else if (src->type == TTAK_BIGSCRIPT_VAL_COMPLEX) {
        ttak_bigcomplex_init(&dst->v.c, now);
        return ttak_bigreal_copy(&dst->v.c.real, &src->v.c.real, now) && ttak_bigreal_copy(&dst->v.c.imag, &src->v.c.imag, now);
    }
    return false;
}

static void variant_set_int_u64(ttak_bigscript_variant_t *v, uint64_t val, uint64_t now) {
    if (!v) return;
    variant_free(v, now);
    v->type = TTAK_BIGSCRIPT_VAL_INT;
    ttak_bigint_init_u64(&v->v.i, val, now);
}

typedef enum { BLT_S = 0, BLT_IS_ZERO, BLT_REAL, BLT_COMPLEX } builtin_idx_t;
static const char *BUILTIN_NAMES[] = { "s", "is_zero", "real", "complex" };

typedef enum {
    TOK_EOF = 0, TOK_IDENT, TOK_INT, TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_EQ_EQ, TOK_BANG_EQ, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_AMP_AMP, TOK_PIPE_PIPE, TOK_BANG, TOK_EQ, TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_COMMA, TOK_SEMICOLON,
    TOK_LET, TOK_IF, TOK_ELSE, TOK_WHILE, TOK_RETURN, TOK_FN
} token_type_t;

typedef struct { token_type_t type; const char *start; size_t length; uint32_t line; } token_t;
typedef struct { const char *cur; uint32_t line; uint32_t tokens_emitted; ttak_bigscript_limits_t limits; ttak_bigscript_error_t *err; } lexer_t;

static void set_err(ttak_bigscript_error_t *err, ttak_bigscript_error_code_t code, const char *msg) {
    if (err && err->code == TTAK_BIGSCRIPT_ERR_NONE) { err->code = code; err->message = msg; }
}

static token_t next_token(lexer_t *l) {
    token_t tok = { TOK_EOF, l->cur, 0, l->line };
    if (l->err->code != TTAK_BIGSCRIPT_ERR_NONE) return tok;
skip:
    while (*l->cur == ' ' || *l->cur == '\t' || *l->cur == '\r' || *l->cur == '\n') { if (*l->cur == '\n') { l->line++; } l->cur++; }
    if (l->cur[0] == '/' && l->cur[1] == '/') { while (*l->cur && *l->cur != '\n') { l->cur++; } goto skip; }
    tok.start = l->cur; tok.line = l->line;
    if (*l->cur == '\0') { return tok; }
    char c = *l->cur++;
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)*l->cur) || *l->cur == '_') { l->cur++; }
        tok.length = l->cur - tok.start;
        #define KW(s, t) if (tok.length == strlen(s) && strncmp(tok.start, s, tok.length) == 0) { tok.type = t; return tok; }
        KW("let", TOK_LET); KW("if", TOK_IF); KW("else", TOK_ELSE); KW("while", TOK_WHILE); KW("return", TOK_RETURN); KW("fn", TOK_FN);
        #undef KW
        tok.type = TOK_IDENT; return tok;
    }
    if (isdigit((unsigned char)c)) {
        while (isdigit((unsigned char)*l->cur) || *l->cur == '_') { l->cur++; }
        tok.length = l->cur - tok.start; tok.type = TOK_INT; return tok;
    }
    switch (c) {
        case '+': tok.type = TOK_PLUS; break; case '-': tok.type = TOK_MINUS; break;
        case '*': tok.type = TOK_STAR; break; case '/': tok.type = TOK_SLASH; break;
        case '%': tok.type = TOK_PERCENT; break;
        case '=': if (*l->cur == '=') { l->cur++; tok.type = TOK_EQ_EQ; } else { tok.type = TOK_EQ; } break;
        case '!': if (*l->cur == '=') { l->cur++; tok.type = TOK_BANG_EQ; } else { tok.type = TOK_BANG; } break;
        case '<': if (*l->cur == '=') { l->cur++; tok.type = TOK_LE; } else { tok.type = TOK_LT; } break;
        case '>': if (*l->cur == '=') { l->cur++; tok.type = TOK_GE; } else { tok.type = TOK_GT; } break;
        case '&': if (*l->cur == '&') { l->cur++; tok.type = TOK_AMP_AMP; } break;
        case '|': if (*l->cur == '|') { l->cur++; tok.type = TOK_PIPE_PIPE; } break;
        case '(': tok.type = TOK_LPAREN; break; case ')': tok.type = TOK_RPAREN; break;
        case '{': tok.type = TOK_LBRACE; break; case '}': tok.type = TOK_RBRACE; break;
        case ',': tok.type = TOK_COMMA; break; case ';': tok.type = TOK_SEMICOLON; break;
    }
    tok.length = l->cur - tok.start; return tok;
}

typedef struct { char name[64]; int depth; int local_idx; } local_var_t;
typedef struct { lexer_t *lex; token_t curr; token_t peek; ttak_bigscript_program_t *prog; ttak_bigscript_error_t *err; local_var_t locals[MAX_LOCALS]; int local_count; int scope_depth; uint64_t now; } parser_t;

static void advance(parser_t *p) { p->curr = p->peek; p->peek = next_token(p->lex); }
static bool match(parser_t *p, token_type_t t) { if (p->curr.type == t) { advance(p); return true; } return false; }
static void consume(parser_t *p, token_type_t t, const char *msg) { if (p->curr.type == t) { advance(p); } else { set_err(p->err, TTAK_BIGSCRIPT_ERR_SYNTAX, msg); } }
static void emit(parser_t *p, uint32_t op) { if (p->prog->code_len < MAX_CODE) { p->prog->code[p->prog->code_len++] = op; } }

static void parse_expression(parser_t *p);
static void parse_statement(parser_t *p);
static void parse_block(parser_t *p);

static int resolve_local(parser_t *p, token_t *name) {
    for (int i = p->local_count - 1; i >= 0; i--) { if (strlen(p->locals[i].name) == name->length && strncmp(p->locals[i].name, name->start, name->length) == 0) { return p->locals[i].local_idx; } }
    return -1;
}

static void parse_primary(parser_t *p) {
    if (p->curr.type == TOK_INT) {
        token_t tok = p->curr; advance(p);
        ttak_bigint_t val; ttak_bigint_init_u64(&val, 0, p->now);
        for (size_t i = 0; i < tok.length; i++) {
            if (isdigit((unsigned char)tok.start[i])) {
                ttak_bigint_t t1, t2; ttak_bigint_init_copy(&t1, &val, p->now); ttak_bigint_init_u64(&t2, 0, p->now);
                ttak_bigint_mul_u64(&t2, &t1, 10, p->now);
                ttak_bigint_add_u64(&val, &t2, (uint64_t)(tok.start[i] - '0'), p->now);
                ttak_bigint_free(&t1, p->now); ttak_bigint_free(&t2, p->now);
            }
        }
        variant_init(&p->prog->constants[p->prog->constants_len].value, p->now);
        ttak_bigint_copy(&p->prog->constants[p->prog->constants_len].value.v.i, &val, p->now);
        ttak_bigint_free(&val, p->now);
        emit(p, OP_PUSH_CONST); emit(p, p->prog->constants_len++);
    } else if (p->curr.type == TOK_IDENT) {
        token_t name = p->curr; advance(p);
        if (match(p, TOK_LPAREN)) {
            int argc = 0; if (p->curr.type != TOK_RPAREN) { do { parse_expression(p); argc++; } while (match(p, TOK_COMMA)); }
            consume(p, TOK_RPAREN, "Expected ')'");
            char nbuf[64] = {0}; size_t nl = name.length < 63 ? name.length : 63; memcpy(nbuf, name.start, nl); nbuf[nl] = '\0';
            int bi = -1; for (int i = 0; i < 4; i++) { if (strcmp(nbuf, BUILTIN_NAMES[i]) == 0) { bi = i; break; } }
            if (bi >= 0) { emit(p, OP_CALL_BUILTIN); emit(p, (uint32_t)bi); emit(p, (uint32_t)argc); }
            else {
                int fi = -1; for (uint32_t i = 0; i < p->prog->functions_len; i++) { if (strcmp(p->prog->functions[i].name, nbuf) == 0) { fi = (int)i; break; } }
                if (fi >= 0) { emit(p, OP_CALL); emit(p, (uint32_t)fi); emit(p, (uint32_t)argc); }
                else { set_err(p->err, TTAK_BIGSCRIPT_ERR_SYNTAX, "Undefined fn"); }
            }
        } else { int l = resolve_local(p, &name); if (l >= 0) { emit(p, OP_LOAD_LOCAL); emit(p, (uint32_t)l); } else { set_err(p->err, TTAK_BIGSCRIPT_ERR_SYNTAX, "Undefined var"); } }
    } else if (match(p, TOK_LPAREN)) { parse_expression(p); consume(p, TOK_RPAREN, "Expected ')'"); }
}

static void parse_unary(parser_t *p) { parse_primary(p); }
static void parse_term(parser_t *p) { 
    parse_unary(p); 
    while (p->curr.type == TOK_STAR || p->curr.type == TOK_SLASH) { 
        token_type_t op = p->curr.type; advance(p); parse_unary(p); 
        emit(p, (op == TOK_STAR) ? OP_MUL : OP_DIV); 
    } 
}
static void parse_arith(parser_t *p) { 
    parse_term(p); 
    while (p->curr.type == TOK_PLUS || p->curr.type == TOK_MINUS) { 
        token_type_t op = p->curr.type; advance(p); parse_term(p); 
        emit(p, (op == TOK_PLUS) ? OP_ADD : OP_SUB); 
    } 
}
static void parse_comparison(parser_t *p) { 
    parse_arith(p); 
    while (p->curr.type == TOK_LT || p->curr.type == TOK_GT || p->curr.type == TOK_LE || p->curr.type == TOK_GE) { 
        token_type_t op = p->curr.type; advance(p); parse_arith(p); 
        if (op == TOK_LT) { emit(p, OP_LT); } else if (op == TOK_GT) { emit(p, OP_GT); } else if (op == TOK_LE) { emit(p, OP_LE); } else { emit(p, OP_GE); }
    } 
}
static void parse_equality(parser_t *p) { 
    parse_comparison(p); 
    while (p->curr.type == TOK_EQ_EQ || p->curr.type == TOK_BANG_EQ) { 
        token_type_t op = p->curr.type; advance(p); parse_comparison(p); 
        emit(p, (op == TOK_EQ_EQ) ? OP_EQ : OP_NEQ); 
    } 
}
static void parse_expression(parser_t *p) { parse_equality(p); }

static void parse_statement(parser_t *p) {
    if (match(p, TOK_LET)) {
        token_t name = p->curr; consume(p, TOK_IDENT, "Expected name"); consume(p, TOK_EQ, "Expected '='"); parse_expression(p); consume(p, TOK_SEMICOLON, "Expected ';'");
        local_var_t *l = &p->locals[p->local_count++]; size_t nl = name.length < 63 ? name.length : 63; memcpy(l->name, name.start, nl); l->name[nl] = '\0';
        l->depth = p->scope_depth; l->local_idx = p->local_count - 1; emit(p, OP_STORE_LOCAL); emit(p, (uint32_t)l->local_idx);
    } else if (match(p, TOK_IF)) {
        consume(p, TOK_LPAREN, "Expected '('"); parse_expression(p); consume(p, TOK_RPAREN, "Expected ')'");
        emit(p, OP_JMP_IF_FALSE); uint32_t jf = p->prog->code_len; emit(p, 0); parse_block(p); p->prog->code[jf] = p->prog->code_len;
    } else if (match(p, TOK_RETURN)) { parse_expression(p); consume(p, TOK_SEMICOLON, "Expected ';'"); emit(p, OP_RETURN); }
    else { parse_expression(p); consume(p, TOK_SEMICOLON, "Expected ';'"); emit(p, OP_POP); }
}

static void parse_block(parser_t *p) {
    consume(p, TOK_LBRACE, "Expected '{'"); p->scope_depth++;
    while (p->curr.type != TOK_RBRACE && p->curr.type != TOK_EOF) { parse_statement(p); }
    consume(p, TOK_RBRACE, "Expected '}'"); p->scope_depth--;
    while (p->local_count > 0 && p->locals[p->local_count - 1].depth > p->scope_depth) { p->local_count--; }
}

static void parse_function(parser_t *p) {
    consume(p, TOK_FN, "Expected 'fn'"); token_t name = p->curr; consume(p, TOK_IDENT, "Expected name"); 
    function_t *f = &p->prog->functions[p->prog->functions_len++]; size_t nl = name.length < 63 ? name.length : 63; memcpy(f->name, name.start, nl); f->name[nl] = '\0';
    f->start_ip = p->prog->code_len; f->arity = 0; p->local_count = 0; p->scope_depth = 1;
    consume(p, TOK_LPAREN, "Expected '('");
    if (p->curr.type != TOK_RPAREN) { do { token_t arg = p->curr; consume(p, TOK_IDENT, "Expected arg"); local_var_t *l = &p->locals[p->local_count++]; size_t al = arg.length < 63 ? arg.length : 63; memcpy(l->name, arg.start, al); l->name[al] = '\0'; l->depth = 1; l->local_idx = p->local_count - 1; f->arity++; } while (match(p, TOK_COMMA)); }
    consume(p, TOK_RPAREN, "Expected ')'"); parse_block(p); f->locals_count = (uint32_t)p->local_count;
    variant_init(&p->prog->constants[p->prog->constants_len].value, p->now); emit(p, OP_PUSH_CONST); emit(p, p->prog->constants_len++); emit(p, OP_RETURN);
}

static void parse_program(parser_t *p) { while (p->curr.type != TOK_EOF) { if (p->curr.type == TOK_FN) { parse_function(p); } else { set_err(p->err, TTAK_BIGSCRIPT_ERR_SYNTAX, "Only fns allowed at top level"); return; } if (p->err->code != TTAK_BIGSCRIPT_ERR_NONE) { return; } } }

ttak_bigscript_program_t *ttak_bigscript_compile(const char *source, ttak_bigscript_loader_t *loader, const ttak_bigscript_limits_t *limits, ttak_bigscript_error_t *err, uint64_t now) {
    (void)loader; ttak_bigscript_program_t *prog = (ttak_bigscript_program_t*)ttak_mem_alloc_raw(sizeof(ttak_bigscript_program_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!prog) { return NULL; }
    memset(prog, 0, sizeof(ttak_bigscript_program_t)); if (limits) { prog->limits = *limits; }
    lexer_t lex = { source, 1, 0, prog->limits, err }; parser_t p = { &lex, {TOK_EOF,NULL,0,0}, {TOK_EOF,NULL,0,0}, prog, err, {{{0},0,0}}, 0, 0, now };
    p.peek = next_token(&lex); advance(&p); while (p.curr.type != TOK_EOF) { parse_program(&p); }
    SHA256_CTX ctx; sha256_init(&ctx); sha256_update(&ctx, (const uint8_t*)source, strlen(source)); uint8_t d[32]; sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) { snprintf(&prog->source_sha256[i*2], 3, "%02x", d[i]); }
    return prog;
}

void ttak_bigscript_program_free(ttak_bigscript_program_t *prog, uint64_t now) { if (prog) { for (uint32_t i = 0; i < prog->constants_len; i++) { variant_free(&prog->constants[i].value, now); } ttak_mem_free(prog); } }

ttak_bigscript_vm_t *ttak_bigscript_vm_create(const ttak_bigscript_limits_t *limits, uint64_t now) {
    ttak_bigscript_vm_t *vm = (ttak_bigscript_vm_t*)ttak_mem_alloc_raw(sizeof(ttak_bigscript_vm_t), __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!vm) { return NULL; }
    memset(vm, 0, sizeof(ttak_bigscript_vm_t)); if (limits) { vm->limits = *limits; }
    vm->stack = (ttak_bigscript_variant_t*)ttak_mem_alloc_raw(sizeof(ttak_bigscript_variant_t) * 1024, __TTAK_UNSAFE_MEM_FOREVER__, now);
    vm->locals = (ttak_bigscript_variant_t*)ttak_mem_alloc_raw(sizeof(ttak_bigscript_variant_t) * 1024, __TTAK_UNSAFE_MEM_FOREVER__, now);
    if (!vm->stack || !vm->locals) { ttak_mem_free(vm->stack); ttak_mem_free(vm->locals); ttak_mem_free(vm); return NULL; }
    vm->stack_cap = 1024; for (uint32_t i = 0; i < 1024; i++) { variant_init(&vm->stack[i], now); }
    vm->locals_cap = 1024; for (uint32_t i = 0; i < 1024; i++) { variant_init(&vm->locals[i], now); }
    return vm;
}

void ttak_bigscript_vm_free(ttak_bigscript_vm_t *vm, uint64_t now) { if (vm) { for (uint32_t i = 0; i < vm->stack_cap; i++) { variant_free(&vm->stack[i], now); } for (uint32_t i = 0; i < vm->locals_cap; i++) { variant_free(&vm->locals[i], now); } ttak_mem_free(vm->stack); ttak_mem_free(vm->locals); ttak_mem_free(vm); } }

void ttak_bigscript_hash_program(ttak_bigscript_program_t *prog, char out_hex[65]) { if (prog) { strncpy(out_hex, prog->source_sha256, 64); out_hex[64] = '\0'; } else { memset(out_hex, 0, 65); } }

void ttak_bigscript_value_free(ttak_bigscript_value_t *val, uint64_t now) {
    if (val) { variant_free(&val->value, now); }
}

bool ttak_bigscript_eval_seed(ttak_bigscript_program_t *prog, ttak_bigscript_vm_t *vm, const ttak_bigint_t *seed, const ttak_bigint_t *sn, ttak_bigscript_value_t *out, ttak_bigscript_error_t *err, uint64_t now) {
    (void)err; int mi = -1; for (uint32_t i = 0; i < prog->functions_len; i++) { if (strcmp(prog->functions[i].name, "main") == 0) { mi = (int)i; break; } }
    if (mi < 0) { return false; }
    function_t *f = &prog->functions[mi];
    vm->stack_top = 0; vm->locals_top = 0; vm->call_depth = 0; vm->steps = 0; vm->now = now;
    variant_set_int_u64(&vm->locals[0], 0, now); ttak_bigint_copy(&vm->locals[0].v.i, seed, now);
    variant_set_int_u64(&vm->locals[1], 0, now); ttak_bigint_copy(&vm->locals[1].v.i, sn, now);
    uint32_t ip = f->start_ip;
    while (ip < prog->code_len) {
        uint32_t op = prog->code[ip++];
        #define POP1(a) a = &vm->stack[--vm->stack_top]
        #define POP2(a, b) b = &vm->stack[--vm->stack_top]; a = &vm->stack[--vm->stack_top]
        #define PUSH(v) variant_copy(&vm->stack[vm->stack_top++], v, now)
        switch(op) {
            case OP_PUSH_CONST: { uint32_t i = prog->code[ip++]; PUSH(&prog->constants[i].value); break; }
            case OP_LOAD_LOCAL: { uint32_t i = prog->code[ip++]; PUSH(&vm->locals[i]); break; }
            case OP_STORE_LOCAL: { uint32_t i = prog->code[ip++]; ttak_bigscript_variant_t *v; POP1(v); variant_copy(&vm->locals[i], v, now); break; }
            case OP_ADD: {
                ttak_bigscript_variant_t *a, *b; POP2(a, b);
                ttak_bigscript_variant_t r; variant_init(&r, now);
                if (a->type == TTAK_BIGSCRIPT_VAL_COMPLEX || b->type == TTAK_BIGSCRIPT_VAL_COMPLEX) {
                    ttak_bigscript_variant_t aa, bb; variant_init(&aa, now); variant_init(&bb, now);
                    variant_copy(&aa, a, now); variant_copy(&bb, b, now);
                    r.type = TTAK_BIGSCRIPT_VAL_COMPLEX; ttak_bigcomplex_init(&r.v.c, now);
                    if (aa.type == TTAK_BIGSCRIPT_VAL_COMPLEX && bb.type == TTAK_BIGSCRIPT_VAL_COMPLEX)
                        ttak_bigcomplex_add(&r.v.c, &aa.v.c, &bb.v.c, now);
                    variant_free(&aa, now); variant_free(&bb, now);
                } else if (a->type == TTAK_BIGSCRIPT_VAL_REAL || b->type == TTAK_BIGSCRIPT_VAL_REAL) {
                    r.type = TTAK_BIGSCRIPT_VAL_REAL; ttak_bigreal_init(&r.v.r, now);
                } else {
                    ttak_bigint_add(&r.v.i, &a->v.i, &b->v.i, now);
                }
                PUSH(&r); variant_free(&r, now); break;
            }
            case OP_SUB: {
                ttak_bigscript_variant_t *a, *b; POP2(a, b);
                ttak_bigscript_variant_t r; variant_init(&r, now);
                if (a->type == TTAK_BIGSCRIPT_VAL_INT && b->type == TTAK_BIGSCRIPT_VAL_INT) {
                    ttak_bigint_sub(&r.v.i, &a->v.i, &b->v.i, now);
                }
                PUSH(&r); variant_free(&r, now); break;
            }
            case OP_MUL: {
                ttak_bigscript_variant_t *a, *b; POP2(a, b);
                ttak_bigscript_variant_t r; variant_init(&r, now);
                if (a->type == TTAK_BIGSCRIPT_VAL_INT && b->type == TTAK_BIGSCRIPT_VAL_INT) {
                    ttak_bigint_mul(&r.v.i, &a->v.i, &b->v.i, now);
                }
                PUSH(&r); variant_free(&r, now); break;
            }
            case OP_DIV: {
                ttak_bigscript_variant_t *a, *b; POP2(a, b);
                ttak_bigscript_variant_t q, rr; variant_init(&q, now); variant_init(&rr, now);
                if (a->type == TTAK_BIGSCRIPT_VAL_INT && b->type == TTAK_BIGSCRIPT_VAL_INT) {
                    ttak_bigint_div(&q.v.i, &rr.v.i, &a->v.i, &b->v.i, now);
                }
                PUSH(&q); variant_free(&q, now); variant_free(&rr, now); break;
            }
            case OP_EQ: {
                ttak_bigscript_variant_t *a, *b; POP2(a, b);
                ttak_bigscript_variant_t r; variant_init(&r, now);
                if (a->type == TTAK_BIGSCRIPT_VAL_INT && b->type == TTAK_BIGSCRIPT_VAL_INT) {
                    ttak_bigint_set_u64(&r.v.i, ttak_bigint_cmp(&a->v.i, &b->v.i) == 0 ? 1 : 0, now);
                }
                PUSH(&r); variant_free(&r, now); break;
            }
            case OP_JMP_IF_FALSE: {
                uint32_t d = prog->code[ip++]; ttak_bigscript_variant_t *c; POP1(c);
                if (c->type == TTAK_BIGSCRIPT_VAL_INT && ttak_bigint_cmp_u64(&c->v.i, 0) == 0) { ip = d; }
                break;
            }
            case OP_RETURN: { 
                ttak_bigscript_variant_t *rv; POP1(rv); 
                if (out) { 
                    variant_init(&out->value, now);
                    if (variant_copy(&out->value, rv, now)) {
                        if (rv->type == TTAK_BIGSCRIPT_VAL_INT) out->is_found = (ttak_bigint_cmp_u64(&rv->v.i, 0) != 0);
                        else out->is_found = true;
                    } else {
                        out->is_found = false;
                    }
                } 
                return true; 
            }
            case OP_CALL_BUILTIN: {
                uint32_t bi = prog->code[ip++]; uint32_t ac = prog->code[ip++];
                ttak_bigscript_variant_t res; variant_init(&res, now);
                if (bi == BLT_S) {
                    if (vm->stack[vm->stack_top-1].type == TTAK_BIGSCRIPT_VAL_INT)
                        ttak_sum_proper_divisors_big(&vm->stack[vm->stack_top-1].v.i, &res.v.i, now);
                } else if (bi == BLT_REAL) {
                    if (vm->stack[vm->stack_top-1].type == TTAK_BIGSCRIPT_VAL_INT) {
                        res.type = TTAK_BIGSCRIPT_VAL_REAL; ttak_bigreal_init(&res.v.r, now);
                        ttak_bigint_copy(&res.v.r.mantissa, &vm->stack[vm->stack_top-1].v.i, now);
                        res.v.r.exponent = 0;
                    } else if (vm->stack[vm->stack_top-1].type == TTAK_BIGSCRIPT_VAL_REAL) {
                        variant_copy(&res, &vm->stack[vm->stack_top-1], now);
                    }
                } else if (bi == BLT_COMPLEX) {
                    res.type = TTAK_BIGSCRIPT_VAL_COMPLEX; ttak_bigcomplex_init(&res.v.c, now);
                    if (ac >= 1) {
                        ttak_bigscript_variant_t *re = &vm->stack[vm->stack_top - ac];
                        if (re->type == TTAK_BIGSCRIPT_VAL_INT) ttak_bigint_copy(&res.v.c.real.mantissa, &re->v.i, now);
                        else if (re->type == TTAK_BIGSCRIPT_VAL_REAL) ttak_bigreal_copy(&res.v.c.real, &re->v.r, now);
                    }
                    if (ac >= 2) {
                        ttak_bigscript_variant_t *im = &vm->stack[vm->stack_top - ac + 1];
                        if (im->type == TTAK_BIGSCRIPT_VAL_INT) ttak_bigint_copy(&res.v.c.imag.mantissa, &im->v.i, now);
                        else if (im->type == TTAK_BIGSCRIPT_VAL_REAL) ttak_bigreal_copy(&res.v.c.imag, &im->v.r, now);
                    }
                }
                for(uint32_t i=0; i<ac; i++) { vm->stack_top--; }
                PUSH(&res); variant_free(&res, now); break;
            }
            case OP_POP: { vm->stack_top--; break; }
            default: break;
        }
    }
    return false;
}
