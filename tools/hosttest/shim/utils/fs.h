/* Host-test shim. */
#ifndef HOSTTEST_FS_H
#define HOSTTEST_FS_H
#include <stdbool.h>
bool directory_create (char *path);
/* paths.c takes these as function pointers for its three-place probe. Declared here so it links;
 * the tests that use paths.c only ever exercise menu_path(), which probes nothing. */
bool file_exists (char *path);
bool directory_exists (char *path);
#endif
