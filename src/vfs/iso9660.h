/*
 * Minimal ISO 9660 lookup: finds a file in a disc image by path, using the primary volume
 * descriptor's directory tree (names compared case-insensitively, ";1" and trailing dots
 * ignored). Enough for the game disc, whose installer files all have 8.3 names.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* `path` uses '/' between components. On success, the file's byte offset and size in the image. */
bool ft_iso_find(FILE *iso, const char *path, uint64_t *offset, uint64_t *size);
