/*
 * Shared helpers for ftcore: little-endian reads, error reporting, file I/O.
 * No SDL here — ftcore is used headless by ftconv.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static inline uint16_t ft_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t ft_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Last error message set by a failing ft_* call (not thread-safe). */
const char *ft_get_error(void);
void ft_set_error(const char *fmt, ...);

/* Whole-file read into a malloc'd buffer. Returns NULL and sets the error on failure. */
uint8_t *ft_read_file(const char *path, size_t *len_out);
bool ft_write_file(const char *path, const void *data, size_t len);

/* fopen / remove / rename for UTF-8 paths (on Windows the CRT's narrow calls use the ANSI code page). */
FILE *ft_fopen(const char *path, const char *mode);
int ft_remove(const char *path);
int ft_rename(const char *from, const char *to);

/* Creates a directory and its parents ('/' or '\\' separated); existing ones are fine. */
bool ft_mkdirs(const char *path);

/* 64-bit seek/size; voice.bin is ~2 GB. */
bool ft_fseek64(FILE *f, uint64_t offset);
bool ft_fsize64(FILE *f, uint64_t *size_out);

/* ASCII-only case-insensitive compare, like the original's __stricmp under the C locale. */
int ft_stricmp(const char *a, const char *b);
