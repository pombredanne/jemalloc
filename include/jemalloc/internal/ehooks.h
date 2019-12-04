#ifndef JEMALLOC_INTERNAL_EHOOKS_H
#define JEMALLOC_INTERNAL_EHOOKS_H

/*
 * This module is the internal interface to the extent hooks (both
 * user-specified and external).  Eventually, this will give us the flexibility
 * to use multiple different versions of user-visible extent-hook APIs under a
 * single user interface.
 */

#include "jemalloc/internal/atomic.h"

extern const extent_hooks_t ehooks_default_extent_hooks;

typedef struct ehooks_s ehooks_t;
struct ehooks_s {
	/* Logically an extent_hooks_t *. */
	atomic_p_t ptr;
};

extern const extent_hooks_t ehooks_default_extent_hooks;

/*
 * These are not really part of the public API.  Each hook has a fast-path for
 * the default-hooks case that can avoid various small inefficiencies:
 *   - Forgetting tsd and then calling tsd_get within the hook.
 *   - Getting more state than necessary out of the extent_t.
 *   - Doing arena_ind -> arena -> arena_ind lookups.
 * By making the calls to these functions visible to the compiler, it can move
 * those extra bits of computation down below the fast-paths where they get ignored.
 */
void *ehooks_default_alloc_impl(tsdn_t *tsdn, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind);
bool ehooks_default_dalloc_impl(void *addr, size_t size);
void ehooks_default_destroy_impl(void *addr, size_t size);
bool ehooks_default_commit_impl(void *addr, size_t offset, size_t length);
bool ehooks_default_decommit_impl(void *addr, size_t offset, size_t length);
#ifdef PAGES_CAN_PURGE_LAZY
bool ehooks_default_purge_lazy_impl(void *addr, size_t offset, size_t length);
#endif
#ifdef PAGES_CAN_PURGE_FORCED
bool ehooks_default_purge_forced_impl(void *addr, size_t offset, size_t length);
#endif
bool ehooks_default_split_impl();
bool ehooks_default_merge_impl(void *addr_a, void *addr_b);

/*
 * We don't officially support reentrancy from wtihin the extent hooks.  But
 * various people who sit within throwing distance of the jemalloc team want
 * that functionality in certain limited cases.  The default reentrancy guards
 * assert that we're not reentrant from a0 (since it's the bootstrap arena,
 * where reentrant allocations would be redirected), which we would incorrectly
 * trigger in cases where a0 has extent hooks (those hooks themselves can't be
 * reentrant, then, but there are reasonable uses for such functionality, like
 * putting internal metadata on hugepages).  Therefore, we use the raw
 * reentrancy guards.
 *
 * Eventually, we need to think more carefully about whether and where we
 * support allocating from within extent hooks (and what that means for things
 * like profiling, stats collection, etc.), and document what the guarantee is.
 */
static inline void
ehooks_pre_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_pre_reentrancy_raw(tsd);
}

static inline void
ehooks_post_reentrancy(tsdn_t *tsdn) {
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	tsd_post_reentrancy_raw(tsd);
}

/* Beginning of the public API. */
void ehooks_init(ehooks_t *ehooks, extent_hooks_t *extent_hooks);

static inline void
ehooks_set_extent_hooks_ptr(ehooks_t *ehooks, extent_hooks_t *extent_hooks) {
	atomic_store_p(&ehooks->ptr, extent_hooks, ATOMIC_RELEASE);
}

static inline extent_hooks_t *
ehooks_get_extent_hooks_ptr(ehooks_t *ehooks) {
	return (extent_hooks_t *)atomic_load_p(&ehooks->ptr, ATOMIC_ACQUIRE);
}

static inline bool
ehooks_are_default(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks) ==
	    &ehooks_default_extent_hooks;
}

/*
 * In some cases, a caller needs to allocate resources before attempting to call
 * a hook.  If that hook is doomed to fail, this is wasteful.  We therefore
 * include some checks for such cases.
 */
static inline bool
ehooks_split_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->split == NULL;
}

static inline bool
ehooks_merge_will_fail(ehooks_t *ehooks) {
	return ehooks_get_extent_hooks_ptr(ehooks)->merge == NULL;
}

static inline void *
ehooks_alloc(tsdn_t *tsdn, ehooks_t *ehooks, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_alloc_impl(tsdn, new_addr, size,
		    alignment, zero, commit, arena_ind);
	}
	ehooks_pre_reentrancy(tsdn);
	void *ret = extent_hooks->alloc(extent_hooks, new_addr, size, alignment,
	    zero, commit, arena_ind);
	ehooks_post_reentrancy(tsdn);
	return ret;
}

static inline bool
ehooks_dalloc(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_dalloc_impl(addr, size);
	} else if (extent_hooks->dalloc == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->dalloc(extent_hooks, addr, size,
		    committed, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline void
ehooks_destroy(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    bool committed, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_destroy_impl(addr, size);
	} else if (extent_hooks->destroy == NULL) {
		return;
	} else {
		ehooks_pre_reentrancy(tsdn);
		extent_hooks->destroy(extent_hooks, addr, size, committed,
		    arena_ind);
		ehooks_post_reentrancy(tsdn);
	}
}

static inline bool
ehooks_commit(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_commit_impl(addr, offset, length);
	} else if (extent_hooks->commit == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->commit(extent_hooks, addr, size,
		    offset, length, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_decommit(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_decommit_impl(addr, offset, length);
	} else if (extent_hooks->decommit == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->decommit(extent_hooks, addr, size,
		    offset, length, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_purge_lazy(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
#ifdef PAGES_CAN_PURGE_LAZY
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_purge_lazy_impl(addr, offset, length);
	}
#endif
	if (extent_hooks->purge_lazy == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->purge_lazy(extent_hooks, addr, size,
		    offset, length, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_purge_forced(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t offset, size_t length, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
#ifdef PAGES_CAN_PURGE_FORCED
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_purge_forced_impl(addr, offset, length);
	}
#endif
	if (extent_hooks->purge_forced == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->purge_forced(extent_hooks, addr, size,
		    offset, length, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_split(tsdn_t *tsdn, ehooks_t *ehooks, void *addr, size_t size,
    size_t size_a, size_t size_b, bool committed, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (ehooks_are_default(ehooks)) {
		return ehooks_default_split_impl();
	} else if (extent_hooks->split == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->split(extent_hooks, addr, size, size_a,
		    size_b, committed, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

static inline bool
ehooks_merge(tsdn_t *tsdn, ehooks_t *ehooks, void *addr_a, size_t size_a,
    void *addr_b, size_t size_b, bool committed, unsigned arena_ind) {
	extent_hooks_t *extent_hooks = ehooks_get_extent_hooks_ptr(ehooks);
	if (extent_hooks == &ehooks_default_extent_hooks) {
		return ehooks_default_merge_impl(addr_a, addr_b);
	} else if (extent_hooks->merge == NULL) {
		return true;
	} else {
		ehooks_pre_reentrancy(tsdn);
		bool err = extent_hooks->merge(extent_hooks, addr_a, size_a,
		    addr_b, size_b, committed, arena_ind);
		ehooks_post_reentrancy(tsdn);
		return err;
	}
}

#endif /* JEMALLOC_INTERNAL_EHOOKS_H */
