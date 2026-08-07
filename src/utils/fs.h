#ifndef UTILS_FS_H__
#define UTILS_FS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @def FS_SECTOR_SIZE
 * @brief The size of a file system sector in bytes.
 */
#define FS_SECTOR_SIZE      (512)

/**
 * @file fs.h
 * @brief File system utility functions for file and directory operations.
 * @ingroup utils
 */

/**
 * @brief Strip the file system prefix from a path.
 *
 * Removes the file system prefix (such as ":/") from the provided path string.
 *
 * @param path The path from which to strip the prefix.
 * @return A pointer to the path without the prefix.
 */
char *strip_fs_prefix(char *path);

/**
 * @brief Get the basename of a path.
 *
 * Returns a pointer to the basename (the final component) of the provided path.
 *
 * @param path The path from which to get the basename.
 * @return A pointer to the basename of the path.
 */
char *file_basename(char *path);

/**
 * @brief Check if a file exists at the given path.
 *
 * Checks if a file exists at the specified path.
 *
 * @param path The path to the file.
 * @return true if the file exists, false otherwise.
 */
bool file_exists(char *path);

/**
 * @brief Get the size of a file at the given path.
 *
 * Returns the size of the file at the specified path in bytes.
 *
 * @param path The path to the file.
 * @return The size of the file in bytes, or -1 if the file does not exist or an error occurs.
 */
int64_t file_get_size(char *path);

/**
 * @brief Allocate a file of the specified size at the given path.
 *
 * Creates a file of the specified size at the provided path. The file is filled with zeros.
 *
 * @param path The path to the file.
 * @param size The size of the file to create in bytes.
 * @return true if the file was successfully created, false otherwise.
 */
bool file_allocate(char *path, size_t size);

/**
 * @brief Fill a file with the specified value.
 *
 * Fills the file at the given path with the specified byte value.
 *
 * @param path The path to the file.
 * @param value The value to fill the file with (byte).
 * @return true if the file was successfully filled, false otherwise.
 */
bool file_fill(char *path, uint8_t value);

/**
 * @brief Check if a file has one of the specified extensions.
 *
 * Checks if the file at the given path has one of the specified extensions.
 *
 * @param path The path to the file.
 * @param extensions An array of extensions to check (NULL-terminated).
 * @return true if the file has one of the specified extensions, false otherwise.
 */
bool file_has_extensions(char *path, const char *extensions[]);

/**
 * @brief Check if a directory exists at the given path.
 *
 * Checks if a directory exists at the specified path.
 *
 * @param path The path to the directory.
 * @return true if the directory exists, false otherwise.
 */
bool directory_exists(char *path);

/**
 * @brief Create a directory at the given path.
 *
 * Creates a directory at the specified path, including any necessary parent directories.
 *
 * @param path The path to the directory.
 * @return false if the directory was successfully created, true if there was an error.
 */
bool directory_create(char *path);

/**
 * @brief Delete the regular files directly inside @p path. The directory itself stays, empty.
 *
 * **Deliberately not recursive.** It deletes files it finds one level down and nothing else; a
 * subdirectory is left where it is. That is the safety property, not a limitation. The one caller
 * is profile_erase_saves(), and a profile's save folder is flat -- so recursion would buy nothing
 * and would turn a wrong path into an unbounded delete instead of a bounded one.
 *
 * The empty directory is left behind too, and that is fine: it is what the next occupant of the
 * slot needs, and cart_load.c would recreate it on their first launch regardless.
 *
 * @param path      the directory
 * @param dry_run   count what would go without removing anything
 * @return how many files were removed, or would be
 */
int directory_erase(const char *path, bool dry_run);

#endif // UTILS_FS_H__
