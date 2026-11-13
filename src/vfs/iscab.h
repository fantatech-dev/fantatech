/*
 * InstallShield 6+ cabinet reader (data1.hdr + dataN.cab), enough for the game disc's
 * InstallShield 11 installer. The layout follows the unshield project's notes:
 *   common header (20 B): 'ISc(' signature, version, volume info, cab descriptor offset/size
 *   cab descriptor:       file table offset, directory count, file count, file table offset 2
 *   file descriptor:      0x57 bytes each (version >= 6)
 *   volume header:        0x40 bytes after the common header in each dataN.cab
 *   compressed payload:   chunks of { u16 len; raw deflate[len] }, each inflated on its own
 * Every file carries the MD5 of its expanded bytes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { FT_ISCAB_SPLIT = 1, FT_ISCAB_OBFUSCATED = 2, FT_ISCAB_COMPRESSED = 4, FT_ISCAB_INVALID = 8 };

typedef struct ft_iscab {
    uint8_t *hdr;          /* data1.hdr, owned */
    size_t hdr_len;
    uint32_t version;
    uint32_t dir_count, file_count;
    size_t table;          /* offset of the file table in hdr */
    size_t descs;          /* offset of the file descriptors */
} ft_iscab;

typedef struct ft_iscab_file {
    uint16_t flags;
    uint64_t expanded, compressed, data_offset;
    uint8_t md5[16];
    const char *name;      /* inside hdr (CP932; ASCII on the game disc) */
    const char *dir;
    uint8_t link_flags;    /* 1 = links to a previous file: skip */
    uint16_t volume;
} ft_iscab_file;

/* Opens dataN.cab: an open stream and where the volume lies in it. */
typedef bool (*ft_iscab_open_fn)(void *user, unsigned volume, FILE **f, uint64_t *base, uint64_t *size);
/* Receives expanded bytes in order; return false to abort. */
typedef bool (*ft_iscab_sink_fn)(void *user, const uint8_t *data, size_t len);

/* Takes ownership of `hdr` (malloc'd) even on failure. */
bool ft_iscab_open(ft_iscab *c, uint8_t *hdr, size_t len);
void ft_iscab_close(ft_iscab *c);
bool ft_iscab_file_at(const ft_iscab *c, uint32_t index, ft_iscab_file *out);

/* Streams file `index` to `sink`, following volume splits, and checks its MD5. */
bool ft_iscab_extract(const ft_iscab *c, uint32_t index, ft_iscab_open_fn open, void *open_user, ft_iscab_sink_fn sink,
                      void *sink_user);
