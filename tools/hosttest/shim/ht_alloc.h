/**
 * @file ht_alloc.h
 * @brief Counted malloc for host tests that have to see a free+strdup of the same size.
 *
 * Pointer equality cannot: a sequential free then strdup of the same length is handed the
 * same address back, so a test that only compared pointers would stay green on the bug it
 * exists to catch. library.c is compiled with malloc/strdup mapped here; the test file
 * provides the implementations and reads title_allocs.
 */
#ifndef HT_ALLOC_H
#define HT_ALLOC_H

#include <stddef.h>

extern int title_allocs;

void *ht_malloc (size_t n);
void  ht_free (void *p);
char *ht_strdup (const char *s);

#endif
