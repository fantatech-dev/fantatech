/*
 * Installs the game from the original disc image: finds the InstallShield cabinet
 * (data1.hdr, dataN.cab) inside the ISO and unpacks the game component (volume 2 on: the
 * 17 archive pairs, the opening movie and exec/) into a directory, MD5-checking every file.
 * Volume 1 only holds InstallShield's own support files and is skipped.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Called as bytes are written: the file being unpacked and bytes done / total over all files. */
typedef void (*ft_install_progress_fn)(void *user, const char *file, uint64_t done, uint64_t total);

bool ft_install_from_iso(const char *iso_path, const char *out_dir, ft_install_progress_fn progress, void *user);
