/**
 * @file table_internal.h
 * @brief Internal hash table entry points not exposed in the public API.
 */

#ifndef TTAK_TABLE_INTERNAL_H
#define TTAK_TABLE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ttak/ht/table.h>

/**
 * @brief Insert a key/value pair only if the key is not already present.
 *
 * Performs a single probe: on a key match the table is left untouched and
 * false is returned; on an empty slot the pair is inserted and true is
 * returned. Unlike ttak_table_put(), an existing value is never replaced
 * (and its val_free destructor is never invoked).
 *
 * @param table   Table instance to update.
 * @param key     Key to store.
 * @param key_len Length of the key payload.
 * @param value   Value associated with the key.
 * @param now     Timestamp for safe memory access.
 * @return true if the pair was inserted, false if the key already existed.
 */
bool ttak_table_put_if_absent(ttak_table_t *table, void *key, size_t key_len,
                              void *value, uint64_t now);

#endif // TTAK_TABLE_INTERNAL_H
