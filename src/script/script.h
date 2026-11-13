/*
 * Scenario script parser.
 *
 * A script is a decoded .txt entry (see codec/text.h), one statement per line. Structure
 * traced from mana01's line dispatcher kt_script_exec_line (0x41a770) and its line-table
 * helpers (0x431000-0x432800, 0x42c000-0x42e000):
 *
 *  - Lines starting with ＊ ○ # . ; ； are never executed. ＊/○/# name jump targets,
 *    "." lines are declarations handled at load time, ; and ； start comments.
 *    ☆ lines are save points (and also jump targets).
 *  - Blocks are indentation-based and resolved at load: "if cond" skips the indented lines
 *    below it when false (0x42d000); "loop [cond]" jumps back to the start of the indented
 *    run above it while cond holds, or forever without one (0x42d1c0).
 *  - Label lookup is case-insensitive (kt_name_compare 0x455080).
 *
 * The parser only structures the text; evaluation and command semantics live elsewhere.
 */
#pragma once

#include "core/ft_common.h"

typedef enum ft_line_kind {
    FT_LINE_BLANK,      /* empty or comment-only */
    FT_LINE_LABEL,      /* ○ ＊ # ☆ */
    FT_LINE_DECL,       /* .name[...] declaration */
    FT_LINE_KEYWORD,    /* goto call return if loop load end on.goto on.call loadjump loadMerge sleep dump */
    FT_LINE_COMMAND,    /* %legacy.command or object.command / bare word with arguments */
    FT_LINE_ASSIGN,     /* lvalue = expression */
    FT_LINE_DIALOGUE,   /* first token contains 「 or 、: speaker「$id text」n, or narration */
    FT_LINE_TEXT,       /* anything else: implicit call if the head is a label, otherwise an
                           expression statement (bare names declare variables, e.g. Ai10.txt) */
} ft_line_kind;

typedef enum ft_label_kind {
    FT_LABEL_NONE,
    FT_LABEL_LOCATION,  /* ○ */
    FT_LABEL_CHOICE,    /* ＊ */
    FT_LABEL_HASH,      /* # */
    FT_LABEL_SAVE,      /* ☆ (save point) */
} ft_label_kind;

typedef enum ft_keyword {
    FT_KW_NONE, FT_KW_GOTO, FT_KW_CALL, FT_KW_RETURN, FT_KW_IF, FT_KW_LOOP, FT_KW_LOAD, FT_KW_END,
    FT_KW_ON_GOTO, FT_KW_ON_CALL, FT_KW_LOADJUMP, FT_KW_LOADMERGE, FT_KW_SLEEP, FT_KW_DUMP,
} ft_keyword;

typedef struct ft_sline {
    uint32_t line_no;       /* 1-based source line */
    uint16_t indent;        /* leading tabs (a space counts as one too) */
    uint8_t kind;           /* ft_line_kind */
    uint8_t label_kind;     /* ft_label_kind, for FT_LINE_LABEL */
    uint8_t keyword;        /* ft_keyword, for FT_LINE_KEYWORD */
    char *text;             /* trimmed statement, inline comment removed */
    char *head;             /* first token: keyword / command / label name / speaker */
    char *rest;             /* text after head, trimmed (may be "") */
    int argc;
    char **argv;            /* `rest` split on top-level commas, trimmed */
    int32_t block_end;      /* if: index of the first line after its indented block (-1 if none) */
    int32_t loop_start;     /* loop: index of the first line of the indented run above (-1 if none) */
} ft_sline;

typedef struct ft_label {
    const char *name;       /* points into a line's head */
    uint32_t line;          /* index into lines */
    uint8_t kind;           /* ft_label_kind */
} ft_label;

typedef struct ft_script {
    char name[64];
    ft_sline *lines;
    uint32_t count;
    ft_label *labels;
    uint32_t label_count;
} ft_script;

/* Parses UTF-8 script text. */
bool ft_script_parse(ft_script *s, const char *name, const char *utf8, size_t len);
void ft_script_free(ft_script *s);

/* Case-insensitive label lookup; returns a line index or -1. kind FT_LABEL_NONE matches any kind. */
int32_t ft_script_find_label(const ft_script *s, const char *name, ft_label_kind kind);

const char *ft_line_kind_name(ft_line_kind k);
