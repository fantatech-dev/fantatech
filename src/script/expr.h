/*
 * Script variables and expressions.
 *
 * Mirrors mana01's evaluator (kt_expr_fold 0x42c600, kt_expr_operand 0x42c2f0):
 *  - binary operators fold strictly LEFT TO RIGHT with no precedence:
 *    + - * / %  <= >= == != < >  && ||   (x / 0 leaves x unchanged; && || give 0/1)
 *  - operands: ( expression ), name[index], declared variable, integer literal
 *    (starting with a digit or '-'); "strings" evaluate to 0
 *  - names are case-insensitive and must be declared; an undeclared name reads as 0
 *    and counts as an error, except in declare mode (statements like "a1" or "x=1"),
 *    which creates it.
 * Variables also carry a string value (name="..." assignments, select results).
 */
#pragma once

#include "core/ft_common.h"

typedef struct ft_var {
    char *name;
    int32_t value;
    char *str;          /* string value, or NULL */
    int32_t *array;     /* name[n] elements, or NULL */
    uint32_t array_len;
} ft_var;

typedef struct ft_vars {
    ft_var *v;
    uint32_t count, cap;
    uint32_t errors;            /* undeclared reads / bad syntax since last reset */
    char last_error[160];
} ft_vars;

void ft_vars_init(ft_vars *vs);
void ft_vars_free(ft_vars *vs);
/* Replaces `dst` with a deep copy of `src` (not the error state); false on out of memory. */
bool ft_vars_copy(ft_vars *dst, const ft_vars *src);

ft_var *ft_vars_find(ft_vars *vs, const char *name);
ft_var *ft_vars_declare(ft_vars *vs, const char *name);
void ft_vars_set_int(ft_vars *vs, const char *name, int32_t value);
void ft_vars_set_str(ft_vars *vs, const char *name, const char *str);

/* Handles a ".name[n]" / ".name=expr" declaration line (without the dot). */
bool ft_vars_declare_line(ft_vars *vs, const char *decl);

/* Evaluates an expression. `declare` creates unknown names (statement mode). */
int32_t ft_expr_eval(ft_vars *vs, const char *expr, bool declare);

/*
 * Executes a statement line: "lvalue = expression", "lvalue = "string"", or a bare
 * expression (declaring its names). Returns false on a syntax error.
 */
bool ft_expr_exec(ft_vars *vs, const char *stmt);
