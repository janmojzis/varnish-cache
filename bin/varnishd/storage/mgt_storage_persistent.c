/*-
 * Copyright (c) 2008-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Persistent storage method
 *
 * XXX: Before we start the client or maybe after it stops, we should give the
 * XXX: stevedores a chance to examine their storage for consistency.
 *
 * XXX: Do we ever free the LRU-lists ?
 */

#include "config.h"

#include "cache/cache_varnishd.h"
#include "common/heritage.h"

#include <sys/mman.h>

#include <stdio.h>
#include <stdlib.h>

#include "storage/storage.h"
#include "storage/storage_simple.h"

#include "vsha256.h"

#include "storage/storage_persistent.h"

#ifdef HAVE_SYS_PERSONALITY_H
#include <sys/personality.h>
#endif

#ifndef MAP_NOCORE
#ifdef MAP_CONCEAL
#define MAP_NOCORE MAP_CONCEAL /* XXX OpenBSD */
#else
#define MAP_NOCORE 0 /* XXX Linux */
#endif
#endif

#ifndef MAP_NOSYNC
#define MAP_NOSYNC 0 /* XXX Linux */
#endif

/*--------------------------------------------------------------------
 * Calculate cleaner metrics from silo dimensions
 */

static void
smp_metrics(struct smp_sc *sc)
{

	/*
	 * We do not want to loose too big chunks of the silos
	 * content when we are forced to clean a segment.
	 *
	 * For now insist that a segment covers no more than 1% of the silo.
	 *
	 * XXX: This should possibly depend on the size of the silo so
	 * XXX: trivially small silos do not run into trouble along
	 * XXX: the lines of "one object per segment".
	 */

	sc->min_nseg = 10;
	sc->max_segl = smp_stuff_len(sc, SMP_SPC_STUFF) / sc->min_nseg;

	fprintf(stderr, "min_nseg = %u, max_segl = %ju\n",
	    sc->min_nseg, (uintmax_t)sc->max_segl);

	/*
	 * The number of segments are limited by the size of the segment
	 * table(s) and from that follows the minimum size of a segmement.
	 */

	sc->max_nseg = smp_stuff_len(sc, SMP_SEG1_STUFF) / sc->min_nseg;
	sc->min_segl = smp_stuff_len(sc, SMP_SPC_STUFF) / sc->max_nseg;

	while (sc->min_segl < sizeof(struct object)) {
		sc->max_nseg /= 2;
		sc->min_segl = smp_stuff_len(sc, SMP_SPC_STUFF) / sc->max_nseg;
	}

	fprintf(stderr, "max_nseg = %u, min_segl = %ju\n",
	    sc->max_nseg, (uintmax_t)sc->min_segl);

	/*
	 * Set our initial aim point at the exponential average of the
	 * two extremes.
	 *
	 * XXX: This is a pretty arbitrary choice, but having no idea
	 * XXX: object count, size distribution or ttl pattern at this
	 * XXX: point, we have to do something.
	 */

	sc->aim_nseg =
	   (unsigned) exp((log(sc->min_nseg) + log(sc->max_nseg))*.5);
	sc->aim_segl = smp_stuff_len(sc, SMP_SPC_STUFF) / sc->aim_nseg;

	fprintf(stderr, "aim_nseg = %u, aim_segl = %ju\n",
	    sc->aim_nseg, (uintmax_t)sc->aim_segl);

	/*
	 * How much space in the free reserve pool ?
	 */
	sc->free_reserve = sc->aim_segl * 10;

	fprintf(stderr, "free_reserve = %ju\n", (uintmax_t)sc->free_reserve);
}

/*--------------------------------------------------------------------
 * Set up persistent storage silo in the master process.
 */

void v_matchproto_(storage_init_f)
smp_mgt_init(struct stevedore *parent, int ac, char * const *av)
{
	struct smp_sc		*sc;
	struct smp_sign		sgn;
	void *target;
	int i, mmap_flags;

	ASSERT_MGT();

	AZ(av[ac]);


#ifdef HAVE_SYS_PERSONALITY_H
	i = personality(0xffffffff); /* Fetch old personality. */
	if (!(i & ADDR_NO_RANDOMIZE)) {
		i = personality(i | ADDR_NO_RANDOMIZE);
		if (i < 0)
			fprintf(stderr, "WARNING: Could not disable ASLR\n");
		else
			fprintf(stderr, "NB: Disabled ASLR for Persistent\n");
	}
#endif

	/* Necessary alignment. See also smp_object::__filler__ */
	assert(sizeof(struct smp_object) % 8 == 0);

#define SIZOF(foo)       fprintf(stderr, \
    "sizeof(%s) = %zu = 0x%zx\n", #foo, sizeof(foo), sizeof(foo));
	SIZOF(struct smp_ident);
	SIZOF(struct smp_sign);
	SIZOF(struct smp_segptr);
	SIZOF(struct smp_object);
#undef SIZOF

	/* See comments in storage_persistent.h */
	assert(sizeof(struct smp_ident) == SMP_IDENT_SIZE);

	/* Allocate softc */
	ALLOC_OBJ(sc, SMP_SC_MAGIC);
	XXXAN(sc);
	sc->parent = parent;
	sc->fd = -1;
	VTAILQ_INIT(&sc->segments);

	/* Argument processing */
	if (ac != 2)
		ARGV_ERR("(-spersistent) wrong number of arguments\n");

	i = STV_GetFile(av[0], &sc->fd, &sc->filename, "-spersistent");
	if (i == 2)
		ARGV_ERR("(-spersistent) need filename (not directory)\n");

	sc->align = sizeof(void*) * 2;
	sc->granularity = getpagesize();
	sc->mediasize = STV_FileSize(sc->fd, av[1], &sc->granularity,
	    "-spersistent");

	AZ(ftruncate(sc->fd, sc->mediasize));

	/* Try to determine correct mmap address */
	i = read(sc->fd, &sgn, sizeof sgn);
	assert(i == sizeof sgn);
	if (!memcmp(sgn.ident, "SILO", 5))
		target = (void*)(uintptr_t)sgn.mapped;
	else
		target = NULL;

	mmap_flags = MAP_NOCORE | MAP_NOSYNC | MAP_SHARED;
	if (target) {
		mmap_flags |= MAP_FIXED;
#ifdef MAP_EXCL
		mmap_flags |= MAP_EXCL;
#endif
	} else {
#ifdef __FreeBSD__
		/*
		 * I guess the people who came up with ASLR never learned
		 * that virtual memory can have benficial uses, because they
		 * added no facility for realiably and portably allocing
		 * stable address-space.
		 * This stevedore is only for testing these days, so we
		 * can get away with just hacking something up: 16M below
		 * the break seems to work on FreeBSD.
		 */
		uintptr_t up;
		up = (uintptr_t)sbrk(0);
		up -= 1ULL<<24;
		up -= sc->mediasize;
		up &= ~(getpagesize() - 1ULL);
		target = (void *)up;
#endif

#ifdef MAP_ALIGNED_SUPER
		mmap_flags |= MAP_ALIGNED_SUPER;
#endif
	}
	sc->base = (void*)mmap(target, sc->mediasize, PROT_READ|PROT_WRITE,
	    mmap_flags, sc->fd, 0);

	if (sc->base == MAP_FAILED)
		ARGV_ERR("(-spersistent) failed to mmap (%s) @%p\n",
		    VAS_errtxt(errno), target);
	if (target != NULL && sc->base != target)
		fprintf(stderr, "WARNING: Persistent silo lost to ASLR %s\n",
		    sc->filename);

	smp_def_sign(sc, &sc->idn, 0, "SILO");
	sc->ident = SIGN_DATA(&sc->idn);

	i = smp_valid_silo(sc);
	if (i) {
		printf("Warning SILO (%s) not reloaded (reason=%d)\n",
		    sc->filename, i);
		smp_newsilo(sc);
	}
	AZ(smp_valid_silo(sc));

	smp_metrics(sc);

	parent->priv = sc;

	/* XXX: only for sendfile I guess... */
	MCH_Fd_Inherit(sc->fd, "storage_persistent");
}
