/*
 * Copyright (C) 2012 Red Hat. All rights reserved.
 *
 * This file is released under the GPL.
 */

#ifndef DM_CACHE_POLICY_INTERNAL_H
#define DM_CACHE_POLICY_INTERNAL_H

#include <linux/vmalloc.h>
#include "dm-cache-policy.h"

/*----------------------------------------------------------------*/

/*
 * Little inline functions that simplify calling the policy methods.
 */
static inline int policy_map(struct dm_cache_policy *p, dm_oblock_t oblock,
			     bool can_block, bool can_migrate, bool discarded_oblock,
			     struct bio *bio, struct policy_locker *locker,
			     struct policy_result *result, struct app_group_t *app_group)
{
	return p->map(p, oblock, can_block, can_migrate, discarded_oblock, bio, locker, result, app_group);
}

static inline int policy_lookup(struct dm_cache_policy *p, dm_oblock_t oblock, dm_cblock_t *cblock)
{
	BUG_ON(!p->lookup);
	return p->lookup(p, oblock, cblock);
}

static inline void policy_set_dirty(struct dm_cache_policy *p, dm_oblock_t oblock)
{
	if (p->set_dirty)
		p->set_dirty(p, oblock);
}

static inline void policy_clear_dirty(struct dm_cache_policy *p, dm_oblock_t oblock)
{
	if (p->clear_dirty)
		p->clear_dirty(p, oblock);
}

static inline int policy_load_mapping(struct dm_cache_policy *p,
				      dm_oblock_t oblock, dm_cblock_t cblock,
				      uint32_t hint, bool hint_valid)
{
	return p->load_mapping(p, oblock, cblock, hint, hint_valid);
}

static inline uint32_t policy_get_hint(struct dm_cache_policy *p,
				       dm_cblock_t cblock)
{
	return p->get_hint ? p->get_hint(p, cblock) : 0;
}

static inline int policy_writeback_work(struct dm_cache_policy *p,
					dm_oblock_t *oblock,
					dm_cblock_t *cblock,
					bool critical_only)
{
	return p->writeback_work ? p->writeback_work(p, oblock, cblock, critical_only) : -ENOENT;
}

static inline void policy_remove_mapping(struct dm_cache_policy *p, dm_oblock_t oblock)
{
	p->remove_mapping(p, oblock);
}

static inline int policy_remove_cblock(struct dm_cache_policy *p, dm_cblock_t cblock)
{
	return p->remove_cblock(p, cblock);
}

static inline void policy_force_mapping(struct dm_cache_policy *p,
					dm_oblock_t current_oblock, dm_oblock_t new_oblock)
{
	return p->force_mapping(p, current_oblock, new_oblock);
}

static inline dm_cblock_t policy_residency(struct dm_cache_policy *p)
{
	return p->residency(p);
}

static inline dm_cblock_t policy_invalid_blocks(struct dm_cache_policy *p)
{
	return p->invalid_blocks(p);
}

static inline void policy_tick(struct dm_cache_policy *p, bool can_block)
{
	if (p->tick)
		return p->tick(p, can_block);
}

static inline int policy_emit_config_values(struct dm_cache_policy *p, char *result,
					    unsigned maxlen, ssize_t *sz_ptr)
{
	ssize_t sz = *sz_ptr;
	if (p->emit_config_values)
		return p->emit_config_values(p, result, maxlen, sz_ptr);

	DMEMIT("0 ");
	*sz_ptr = sz;
	return 0;
}

static inline int policy_set_config_value(struct dm_cache_policy *p,
					  const char *key, const char *value)
{
	return p->set_config_value ? p->set_config_value(p, key, value) : -EINVAL;
}

/*----------------------------------------------------------------*/

/*
 * Some utility functions commonly used by policies and the core target.
 */
static inline size_t bitset_size_in_bytes(unsigned nr_entries)
{
	return sizeof(unsigned long) * dm_div_up(nr_entries, BITS_PER_LONG);
}

static inline unsigned long *alloc_bitset(unsigned nr_entries)
{
	size_t s = bitset_size_in_bytes(nr_entries);
	return vzalloc(s);
}

static inline void clear_bitset(void *bitset, unsigned nr_entries)
{
	size_t s = bitset_size_in_bytes(nr_entries);
	memset(bitset, 0, s);
}

static inline void free_bitset(unsigned long *bits)
{
	vfree(bits);
}

/*----------------------------------------------------------------*/

/*
 * Creates a new cache policy given a policy name, a cache size, an origin size and the block size.
 */
struct dm_cache_policy *dm_cache_policy_create(const char *name, dm_cblock_t cache_size,
					       sector_t origin_size, sector_t block_size);

/*
 * Destroys the policy.  This drops references to the policy module as well
 * as calling it's destroy method.  So always use this rather than calling
 * the policy->destroy method directly.
 */
void dm_cache_policy_destroy(struct dm_cache_policy *p);

/*
 * In case we've forgotten.
 */
const char *dm_cache_policy_get_name(struct dm_cache_policy *p);

const unsigned *dm_cache_policy_get_version(struct dm_cache_policy *p);

size_t dm_cache_policy_get_hint_size(struct dm_cache_policy *p);

/*----------------------------------------------------------------*/

#endif /* DM_CACHE_POLICY_INTERNAL_H */
