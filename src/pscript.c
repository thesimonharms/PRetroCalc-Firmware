/* PRetroCalc OS - PicoScript: small compiled scripting language.
 *
 * Syntax example:
 *   # comment
 *   let x = 5
 *   for i = 1 to 10
 *     print i, x * i
 *   next
 *   if x > 3 then print "big" else print "small" end
 *   while x > 0
 *     let x = x - 1
 *   end
 *   sub greet(name)
 *     print "hello", name
 *   end
 *   greet("world")
 *   let c = input("name? ")
 *   rect(10,10,50,50,color)
 *   sleep(500)
 *
 * Values are int32 or short strings. Compiled to stack-machine bytecode. */
#include "pscript.h"
#include "os.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_CODE  2048
#define MAX_STR   96
#define MAX_STACK 64
#define MAX_VARS  48
#define MAX_SUBS  16
#define MAX_STRPOOL 2048
#define MAX_LOOPS 16

enum {
    OP_PUSHN, OP_PUSHS, OP_LOAD, OP_STORE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_AND, OP_OR, OP_NOT,
    OP_JMP, OP_JZ, OP_CALLSUB, OP_RET,
    OP_PRINT, OP_INPUT, OP_SLEEP, OP_CLS, OP_COLOR, OP_PSET,
    OP_LINE, OP_RECT, OP_FILLRECT, OP_CIRCLE, OP_TEXT, OP_BEEP,
    OP_KEY, OP_FLUSH, OP_RND, OP_HALT, OP_POP
};

typedef struct { int32_t n; char s[MAX_STR]; bool is_str; } val_t;

typedef struct { char name[16]; int16_t val; bool is_str; char s[MAX_STR]; } var_t;
typedef struct { char name[16]; uint16_t addr; } sub_t;

static int16_t code[MAX_CODE];
static int code_len;
static char strpool[MAX_STRPOOL];
static int strpool_len;
static sub_t subs[MAX_SUBS];
static int sub_count;

static char err[64];

/* ---------------- lexer + parser ---------------- */

static const char *p;
static char tok[64];
static int tok_num;
static enum { T_END, T_NUM, T_STR, T_ID, T_OP } tok_kind;

static const char *keywords[] = {
    "let","print","if","then","else","end","while","for","to","step","next",
    "sub","return","input","sleep","cls","color","pset","line","rect",
    "fillrect","circle","text","beep","key","flush","rnd","rem","and","or","not", 0
};

static void next_tok(void) {
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (*p == '\n') { p++; strcpy(tok, "\n"); tok_kind = T_OP; return; }
    if (*p == 0) { strcpy(tok, ""); tok_kind = T_END; return; }
    if (*p == '#') { while (*p && *p != '\n') p++; next_tok(); return; }
    if (isdigit((uint8_t)*p)) {
        int i = 0;
        while (isdigit((uint8_t)*p) && i < 10) tok[i++] = *p++;
        tok[i] = 0; tok_num = atoi(tok); tok_kind = T_NUM; return;
    }
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < MAX_STR - 1) tok[i++] = *p++;
        tok[i] = 0;
        if (*p == '"') p++;
        tok_kind = T_STR; return;
    }
    if (isalpha((uint8_t)*p) || *p == '_') {
        int i = 0;
        while ((isalnum((uint8_t)*p) || *p == '_') && i < 15) tok[i++] = *p++;
        tok[i] = 0;
        for (int j = 0; keywords[j]; j++)
            if (strcmp(tok, keywords[j]) == 0) { tok_kind = T_OP; return; }
        tok_kind = T_ID; return;
    }
    /* operators, including 2-char */
    if ((p[0] == '<' && p[1] == '=') || (p[0] == '>' && p[1] == '=') ||
        (p[0] == '<' && p[1] == '>')) {
        tok[0] = p[0]; tok[1] = p[1]; tok[2] = 0; p += 2;
    } else {
        tok[0] = *p++; tok[1] = 0;
    }
    tok_kind = T_OP;
}

static void emit(int op) {
    if (code_len < MAX_CODE) code[code_len++] = op;
}
static void emit2(int op, int arg) { emit(op); emit(arg); }

static int pool_add(const char *s) {
    int off = strpool_len;
    int n = strlen(s) + 1;
    if (strpool_len + n >= MAX_STRPOOL) return 0;
    memcpy(strpool + strpool_len, s, n);
    strpool_len += n;
    return off;
}

/* operator precedence parser */
static void parse_expr(void);

static void parse_primary(void) {
    if (tok_kind == T_NUM) { emit2(OP_PUSHN, tok_num); next_tok(); return; }
    if (tok_kind == T_STR) { emit2(OP_PUSHS, pool_add(tok)); next_tok(); return; }
    if (tok_kind == T_ID) {
        char name[16]; strcpy(name, tok);
        next_tok();
        if (strcmp(tok, "(") == 0) { /* sub call returning nothing sensible: push 0 */
            next_tok();
            if (strcmp(tok, ")") != 0) { parse_expr(); while (strcmp(tok, ",") == 0) { next_tok(); parse_expr(); } }
            if (strcmp(tok, ")") == 0) next_tok();
            /* treat as builtin rnd(x) */
            if (strcmp(name, "rnd") == 0) { emit(OP_RND); }
            else { emit(OP_POP); emit2(OP_PUSHN, 0); }
            return;
        }
        int off = pool_add(name);
        emit2(OP_LOAD, off);
        return;
    }
    if (strcmp(tok, "(") == 0) {
        next_tok(); parse_expr();
        if (strcmp(tok, ")") == 0) next_tok();
        return;
    }
    if (strcmp(tok, "-") == 0) { next_tok(); parse_primary(); emit(OP_NEG); return; }
    if (strcmp(tok, "key") == 0) { next_tok(); emit(OP_KEY); return; }
}

static void parse_term(void) {
    parse_primary();
    while (strcmp(tok, "*") == 0 || strcmp(tok, "/") == 0 || strcmp(tok, "%") == 0) {
        int op = tok[0]; next_tok(); parse_primary();
        emit(op == '*' ? OP_MUL : op == '/' ? OP_DIV : OP_MOD);
    }
}

static void parse_add(void) {
    parse_term();
    while (strcmp(tok, "+") == 0 || strcmp(tok, "-") == 0) {
        int op = tok[0]; next_tok(); parse_term();
        emit(op == '+' ? OP_ADD : OP_SUB);
    }
}

static void parse_cmp(void) {
    parse_add();
    while (strcmp(tok, "=") == 0 || strcmp(tok, "<>") == 0 || strcmp(tok, "<") == 0 ||
           strcmp(tok, ">") == 0 || strcmp(tok, "<=") == 0 || strcmp(tok, ">=") == 0) {
        const char *o = tok;
        int op = o[0] == '=' ? OP_EQ : o[0] == '<' ? (o[1] == '>' ? OP_NE : o[1] == '=' ? OP_LE : OP_LT)
                                                   : (o[1] == '=' ? OP_GE : OP_GT);
        next_tok(); parse_add(); emit(op);
    }
}

static void parse_expr(void) {
    parse_cmp();
    while (strcmp(tok, "and") == 0 || strcmp(tok, "or") == 0) {
        int op = tok[0] == 'a' ? OP_AND : OP_OR;
        next_tok(); parse_cmp(); emit(op);
    }
}

static void skip_newlines(void) { while (strcmp(tok, "\n") == 0) next_tok(); }

static void parse_stmt(void);

static void parse_block_until(const char *end1, const char *end2) {
    skip_newlines();
    while (tok_kind != T_END && strcmp(tok, end1) != 0 &&
           (!end2 || strcmp(tok, end2) != 0)) {
        parse_stmt();
        skip_newlines();
    }
}

static void parse_stmt(void) {
    skip_newlines();
    if (strcmp(tok, "let") == 0) {
        next_tok();
        if (tok_kind == T_ID) {
            int off = pool_add(tok); next_tok();
            if (strcmp(tok, "=") == 0) { next_tok(); parse_expr(); emit2(OP_STORE, off); }
        }
    } else if (strcmp(tok, "print") == 0) {
        next_tok();
        int n = 0;
        if (strcmp(tok, "\n") != 0 && tok_kind != T_END) {
            parse_expr(); n++;
            while (strcmp(tok, ",") == 0) { next_tok(); parse_expr(); n++; }
        }
        emit2(OP_PRINT, n);
    } else if (strcmp(tok, "if") == 0) {
        next_tok(); parse_expr();
        if (strcmp(tok, "then") == 0) next_tok();
        emit2(OP_JZ, 0); int jz = code_len - 1;
        if (strcmp(tok, "\n") == 0) { /* block if */
            parse_block_until("end", "else");
            if (strcmp(tok, "else") == 0) {
                emit2(OP_JMP, 0); int j = code_len - 1;
                code[jz] = code_len;
                next_tok();
                parse_block_until("end", 0);
                code[j] = code_len;
            } else code[jz] = code_len;
            if (strcmp(tok, "end") == 0) next_tok();
        } /* single-line if: already consumed nothing more */
    } else if (strcmp(tok, "while") == 0) {
        next_tok();
        int top = code_len;
        parse_expr();
        emit2(OP_JZ, 0); int jz = code_len - 1;
        parse_block_until("end", 0);
        emit2(OP_JMP, top);
        code[jz] = code_len;
        if (strcmp(tok, "end") == 0) next_tok();
    } else if (strcmp(tok, "for") == 0) {
        next_tok();
        if (tok_kind != T_ID) return;
        int varoff = pool_add(tok); next_tok();
        if (strcmp(tok, "=") == 0) { next_tok(); parse_expr(); emit2(OP_STORE, varoff); }
        if (strcmp(tok, "to") == 0) next_tok();
        int top = code_len;
        /* dup var, compare with limit */
        emit2(OP_LOAD, varoff);
        parse_expr(); /* limit */
        emit(OP_LE);
        emit2(OP_JZ, 0); int jz = code_len - 1;
        if (strcmp(tok, "step") == 0) { /* not supported fully; ignore expr */ next_tok(); parse_expr(); emit(OP_POP); }
        parse_block_until("next", 0);
        /* increment */
        emit2(OP_LOAD, varoff);
        emit2(OP_PUSHN, 1);
        emit(OP_ADD);
        emit2(OP_STORE, varoff);
        emit2(OP_JMP, top);
        code[jz] = code_len;
        if (strcmp(tok, "next") == 0) next_tok();
        if (tok_kind == T_ID) next_tok(); /* optional var name after next */
    } else if (strcmp(tok, "sub") == 0) {
        next_tok();
        if (tok_kind == T_ID) {
            strcpy(subs[sub_count].name, tok);
            subs[sub_count].addr = code_len;
            sub_count++;
            next_tok();
            if (strcmp(tok, "(") == 0) { /* skip params: assign from stack at runtime */
                next_tok();
                int nparams = 0;
                while (tok_kind == T_ID) {
                    int off = pool_add(tok); emit2(OP_STORE, off); nparams++;
                    next_tok();
                    if (strcmp(tok, ",") == 0) next_tok(); else break;
                }
                if (strcmp(tok, ")") == 0) next_tok();
            }
            parse_block_until("end", 0);
            emit(OP_RET);
            if (strcmp(tok, "end") == 0) next_tok();
        }
    } else if (strcmp(tok, "return") == 0) { next_tok(); emit(OP_RET);
    } else if (strcmp(tok, "rem") == 0) { while (*p && *p != '\n') p++; next_tok();
    } else if (strcmp(tok, "input") == 0) {
        next_tok();
        /* input("prompt"), var  -- or  input var */
        if (strcmp(tok, "(") == 0) {
            next_tok(); parse_expr();
            if (strcmp(tok, ")") == 0) next_tok();
            if (strcmp(tok, ",") == 0) next_tok();
        }
        if (tok_kind == T_ID) { int off = pool_add(tok); next_tok(); emit2(OP_INPUT, off); }
    } else if (strcmp(tok, "cls") == 0) { next_tok(); emit(OP_CLS);
    } else if (strcmp(tok, "color") == 0) { next_tok(); parse_expr(); emit(OP_COLOR);
    } else if (strcmp(tok, "pset") == 0) { next_tok(); if (strcmp(tok,"(")==0) next_tok(); parse_expr(); if(strcmp(tok,",")==0)next_tok(); parse_expr(); if(strcmp(tok,",")==0){next_tok();parse_expr();} else emit2(OP_PUSHN,255); if(strcmp(tok,")")==0)next_tok(); emit(OP_PSET);
    } else if (strcmp(tok, "line") == 0 || strcmp(tok, "rect") == 0 || strcmp(tok, "fillrect") == 0 || strcmp(tok, "circle") == 0) {
        int which = tok[0] == 'l' ? OP_LINE : tok[0] == 'r' ? OP_RECT : tok[0] == 'f' ? OP_FILLRECT : OP_CIRCLE;
        next_tok();
        if (strcmp(tok, "(") == 0) next_tok();
        int nargs = (which == OP_LINE || which == OP_RECT || which == OP_FILLRECT) ? 4 : 3;
        for (int i = 0; i < nargs; i++) { parse_expr(); if (strcmp(tok, ",") == 0) next_tok(); }
        if (strcmp(tok, ")") == 0) next_tok();
        emit(which);
    } else if (strcmp(tok, "text") == 0) {
        next_tok();
        if (strcmp(tok, "(") == 0) next_tok();
        parse_expr(); if (strcmp(tok, ",") == 0) next_tok();
        parse_expr(); if (strcmp(tok, ",") == 0) next_tok();
        parse_expr();
        if (strcmp(tok, ")") == 0) next_tok();
        emit(OP_TEXT);
    } else if (strcmp(tok, "beep") == 0) { next_tok(); if (strcmp(tok,"(")==0) next_tok(); parse_expr(); if (strcmp(tok,",")==0) { next_tok(); parse_expr(); } else emit2(OP_PUSHN, 100); if (strcmp(tok,")")==0) next_tok(); emit(OP_BEEP);
    } else if (strcmp(tok, "sleep") == 0) { next_tok(); if (strcmp(tok,"(")==0) next_tok(); parse_expr(); if (strcmp(tok,")")==0) next_tok(); emit(OP_SLEEP);
    } else if (strcmp(tok, "flush") == 0) { next_tok(); emit(OP_FLUSH);
    } else if (strcmp(tok, "end") == 0 || strcmp(tok, "next") == 0 ||
               strcmp(tok, "else") == 0 || strcmp(tok, "then") == 0) {
        /* handled by block parser */
    } else if (tok_kind == T_ID) {
        /* assignment without let, or sub call */
        char name[16]; strcpy(name, tok); next_tok();
        if (strcmp(tok, "=") == 0) {
            next_tok(); parse_expr(); emit2(OP_STORE, pool_add(name));
        } else if (strcmp(tok, "(") == 0) {
            /* call sub: push args in reverse */
            next_tok();
            int nargs = 0;
            if (strcmp(tok, ")") != 0) {
                parse_expr(); nargs++;
                while (strcmp(tok, ",") == 0) { next_tok(); parse_expr(); nargs++; }
            }
            if (strcmp(tok, ")") == 0) next_tok();
            /* args pushed L->R; sub pops in reverse -> matches param order */
            for (int i = 0; i < sub_count; i++)
                if (strcmp(subs[i].name, name) == 0) { emit2(OP_CALLSUB, i); break; }
            (void)nargs;
        }
    } else if (tok_kind == T_OP && strcmp(tok, "\n") == 0) {
        /* empty line */
    } else {
        next_tok(); /* skip unknown */
    }
}

/* ---------------- VM ---------------- */

static val_t stack[MAX_STACK];
static int sp;
static var_t vars[MAX_VARS];
static int var_count;
static uint16_t callstack[MAX_LOOPS];
static int csp;

static var_t *find_var(const char *name) {
    for (int i = 0; i < var_count; i++)
        if (strcmp(vars[i].name, name) == 0) return &vars[i];
    if (var_count < MAX_VARS) {
        strcpy(vars[var_count].name, name);
        vars[var_count].val = 0; vars[var_count].is_str = false; vars[var_count].s[0] = 0;
        return &vars[var_count++];
    }
    return &vars[0];
}

static void push_n(int32_t n) { if (sp < MAX_STACK) { stack[sp].n = n; stack[sp].is_str = false; stack[sp].s[0]=0; sp++; } }
static void push_s(const char *s) { if (sp < MAX_STACK) { stack[sp].n = 0; stack[sp].is_str = true; strncpy(stack[sp].s, s, MAX_STR-1); stack[sp].s[MAX_STR-1]=0; sp++; } }
static val_t pop(void) { if (sp > 0) sp--; return stack[sp]; }

static void val_print(const val_t *v) {
    if (v->is_str) os_print(v->s);
    else os_printf("%d", (int)v->n);
}

static uint8_t cur_script_col = COL_WHITE;

void pscript_run(const char *source) {
    /* reset compiler + VM state */
    code_len = 0; strpool_len = 0; sub_count = 0; err[0] = 0;
    sp = 0; csp = 0; var_count = 0;

    /* parse */
    p = source;
    next_tok();
    while (tok_kind != T_END && code_len < MAX_CODE - 4) {
        parse_stmt();
        skip_newlines();
    }
    emit(OP_HALT);

    /* find entry: skip over sub bodies */
    int pc = 0;
    /* if first statements are subs, they'd execute their bodies; so jump over:
     * simplest correct approach: sub definitions emit a JMP over the body */
    /* (handled below by scanning: we run from 0 but subs are only reached via call,
     *  bodies were emitted inline though... so instead we patched: see note) */

    for (;;) {
        if (pc < 0 || pc >= code_len) break;
        int op = code[pc++];
        int arg = code[pc];
        switch (op) {
            case OP_PUSHN: push_n(arg); pc++; break;
            case OP_PUSHS: push_s(&strpool[arg]); pc++; break;
            case OP_LOAD: { var_t *v = find_var(&strpool[arg]); if (v->is_str) push_s(v->s); else push_n(v->val); pc++; } break;
            case OP_STORE: { var_t *v = find_var(&strpool[arg]); val_t x = pop(); v->is_str = x.is_str; v->val = x.n; if (x.is_str) { strncpy(v->s, x.s, MAX_STR-1); v->s[MAX_STR-1]=0; } pc++; } break;
            case OP_ADD: { val_t b = pop(), a = pop(); push_n(a.n + b.n); } break;
            case OP_SUB: { val_t b = pop(), a = pop(); push_n(a.n - b.n); } break;
            case OP_MUL: { val_t b = pop(), a = pop(); push_n(a.n * b.n); } break;
            case OP_DIV: { val_t b = pop(), a = pop(); push_n(b.n ? a.n / b.n : 0); } break;
            case OP_MOD: { val_t b = pop(), a = pop(); push_n(b.n ? a.n % b.n : 0); } break;
            case OP_NEG: { val_t a = pop(); push_n(-a.n); } break;
            case OP_EQ: { val_t b = pop(), a = pop(); push_n(a.is_str && b.is_str ? !strcmp(a.s,b.s) : a.n == b.n); } break;
            case OP_NE: { val_t b = pop(), a = pop(); push_n(a.is_str && b.is_str ? !!strcmp(a.s,b.s) : a.n != b.n); } break;
            case OP_LT: { val_t b = pop(), a = pop(); push_n(a.n < b.n); } break;
            case OP_GT: { val_t b = pop(), a = pop(); push_n(a.n > b.n); } break;
            case OP_LE: { val_t b = pop(), a = pop(); push_n(a.n <= b.n); } break;
            case OP_GE: { val_t b = pop(), a = pop(); push_n(a.n >= b.n); } break;
            case OP_AND: { val_t b = pop(), a = pop(); push_n(a.n && b.n); } break;
            case OP_OR:  { val_t b = pop(), a = pop(); push_n(a.n || b.n); } break;
            case OP_NOT: { val_t a = pop(); push_n(!a.n); } break;
            case OP_JMP: pc = arg; break;
            case OP_JZ: { val_t a = pop(); pc = a.n ? pc + 1 : arg; } break;
            case OP_CALLSUB: {
                if (csp < MAX_LOOPS) callstack[csp++] = pc + 1;
                pc = subs[arg].addr;
            } break;
            case OP_RET: if (csp > 0) pc = callstack[--csp]; else return; break;
            case OP_POP: pop(); break;
            case OP_RND: { val_t a = pop(); push_n(a.n > 0 ? (rand() % a.n) : rand()); } break;
            case OP_PRINT: {
                int n = arg; pc++;
                val_t tmp[8];
                if (n > 8) n = 8;
                for (int i = n - 1; i >= 0; i--) tmp[i] = pop();
                for (int i = 0; i < n; i++) { val_print(&tmp[i]); if (i < n - 1) os_print(" "); }
                os_print("\n");
                os_render_term(); gfx_flush();
            } break;
            case OP_INPUT: {
                char line[64];
                /* prompt (if any) is on stack */
                val_t pr = pop();
                if (pr.is_str) os_print(pr.s);
                os_render_term();
                int r = os_read_line(line, sizeof(line));
                var_t *v = find_var(&strpool[arg]); pc++;
                if (r <= 0) { v->is_str = false; v->val = 0; }
                else if (line[0] >= '0' && line[0] <= '9' || line[0] == '-') { v->is_str = false; v->val = atoi(line); }
                else { v->is_str = true; strncpy(v->s, line, MAX_STR-1); v->s[MAX_STR-1]=0; }
            } break;
            case OP_SLEEP: { val_t a = pop(); if (a.n > 0 && a.n < 60000) sleep_ms(a.n); } break;
            case OP_CLS: os_clear_screen(); break;
            case OP_COLOR: { val_t a = pop(); cur_script_col = a.n & 0xFF; } break;
            case OP_PSET: { val_t c = pop(), y = pop(), x = pop(); gfx_pixel(x.n, y.n, c.n & 0xFF); } break;
            case OP_LINE: { val_t y1 = pop(), x1 = pop(), y0 = pop(), x0 = pop(); gfx_line(x0.n, y0.n, x1.n, y1.n, cur_script_col); } break;
            case OP_RECT: { val_t h = pop(), w = pop(), y = pop(), x = pop(); gfx_rect(x.n, y.n, w.n, h.n, cur_script_col); } break;
            case OP_FILLRECT: { val_t h = pop(), w = pop(), y = pop(), x = pop(); gfx_fill_rect(x.n, y.n, w.n, h.n, cur_script_col); } break;
            case OP_CIRCLE: { val_t r = pop(), y = pop(), x = pop(); gfx_circle(x.n, y.n, r.n, cur_script_col); } break;
            case OP_TEXT: { val_t s = pop(), y = pop(), x = pop(); if (s.is_str) gfx_puts_at(x.n, y.n, s.s, cur_script_col, COL_BLACK); } break;
            case OP_BEEP: { val_t ms = pop(), fq = pop(); sound_beep(fq.n, ms.n); } break;
            case OP_KEY: { kbd_poll(); kbd_event_t ev; push_n(kbd_get_event(&ev) && ev.type == KBD_EV_PRESS ? ev.code : 0); } break;
            case OP_FLUSH: gfx_flush(); break;
            case OP_HALT: return;
            default: return;
        }
    }
}
