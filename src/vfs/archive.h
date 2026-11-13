/*
 * .idx + .lb5 / .bin resource archives.
 *
 * .idx = u32 count, then `count` records in one of two layouts:
 *   fixed (paired with .lb5): u32 offset, u32 size, u8 zero, char name[15]   (24 bytes)
 *   variable (paired with .bin: music, se, voice): u32 len, char name[len], u32 offset, u32 size
 * Entries are plain slices of the payload file.
 *
 * The fixed layout matches mana01's kt_archive_find_by_name (0x4534b0): records at a
 * 24-byte stride, name at +9, looked up with __stricmp. The variable layout's loader is
 * SafeDisc-protected in the dump (0x452b70), so it is checked against the data instead:
 * the records must consume the .idx exactly.
 */
#pragma once

#include "core/ft_common.h"

#define FT_ENTRY_NAME_MAX 64

typedef struct ft_entry {
    char name[FT_ENTRY_NAME_MAX];
    uint32_t offset;
    uint32_t size;
} ft_entry;

typedef struct ft_archive {
    char name[32];          /* e.g. "face1024" */
    bool variable_layout;   /* true for the .bin (audio) archives */
    uint32_t count;
    ft_entry *entries;
    FILE *payload;
    uint64_t payload_size;
    uint64_t max_end;       /* max(offset + size); equals payload_size on every shipped archive */
    uint32_t nonzero_flags; /* fixed-layout records whose byte 8 isn't 0 */
} ft_archive;

/* The 17 archive pairs shipped with the game. */
extern const char *const ft_archive_names[];
extern const size_t ft_archive_name_count;

/* Opens <dir>/<name>.idx and its .lb5 or .bin payload. */
bool ft_archive_open(ft_archive *a, const char *dir, const char *name);
void ft_archive_close(ft_archive *a);

/* Case-insensitive lookup, like the original. NULL if absent. */
const ft_entry *ft_archive_find(const ft_archive *a, const char *name);

/* Reads an entry into a malloc'd buffer (with one extra NUL byte past the end). */
uint8_t *ft_archive_read(const ft_archive *a, const ft_entry *e);
