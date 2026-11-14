#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// A file's range in the mounted RomFS image, valid until the next game reset.
typedef struct { uint64_t offset, size; } RomfsFile;

// Look up a path relative to the RomFS root after the game has mounted the image.
bool romfs_find_file(const char *path, RomfsFile *file);
ssize_t romfs_pread_file(const RomfsFile *file, void *dst, size_t size, uint64_t offset);

// Read one exact-size file from game_root/romfs.bin or romfs.src before mounting.
// path is relative to the image root. Use dest only on success.
bool romfs_read_local_file(const char *game_root, const char *path, uint8_t *dest, size_t expected_size);

#ifdef __cplusplus
}
#endif
