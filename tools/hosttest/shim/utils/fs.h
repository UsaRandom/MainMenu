/* Host-test shim. */
#ifndef HOSTTEST_FS_H
#define HOSTTEST_FS_H
#include <stdbool.h>
bool directory_create (char *path);
/* paths.c takes these as function pointers for its three-place probe. Declared here so it links;
 * the tests that use paths.c only ever exercise menu_path(), which probes nothing. */
bool file_exists (char *path);
bool directory_exists (char *path);

/* The console build walks a directory with libdragon's dir_findfirst, which does not exist here,
 * so this one is POSIX. What the host test therefore exercises is profile_erase_saves() -- which
 * directories it visits, which slot suffix it builds, and that it refuses profile 1 -- and not
 * the walk itself. That split is deliberate and is the risky half: the walk is six lines, and the
 * path construction is what decides whose saves get deleted. */
int directory_erase (const char *path, bool dry_run);
#endif
