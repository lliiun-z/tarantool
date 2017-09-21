#ifndef INCLUDES_TARANTOOL_BOX_SEQUENCE_H
#define INCLUDES_TARANTOOL_BOX_SEQUENCE_H
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct iterator;

/** Sequence metadata. */
struct sequence_def {
	/** Sequence id. */
	uint32_t id;
	/** Owner of the sequence. */
	uint32_t uid;
	/**
	 * The value added to the sequence at each step.
	 * If it is positive, the sequence is ascending,
	 * otherwise it is descending.
	 */
	int64_t step;
	/** Min sequence value. */
	int64_t min;
	/** Max sequence value. */
	int64_t max;
	/** Initial sequence value. */
	int64_t start;
	/** Number of values to preallocate. Not implemented yet. */
	int64_t cache;
	/**
	 * If this flag is set, the sequence will wrap
	 * upon reaching min or max value by a descending
	 * or ascending sequence respectively.
	 */
	bool cycle;
	/** Sequence name. */
	char name[0];
};

/** Sequence object. */
struct sequence {
	/** Sequence definition. */
	struct sequence_def *def;
};

static inline size_t
sequence_def_sizeof(uint32_t name_len)
{
	return sizeof(struct sequence_def) + name_len + 1;
}

/** Sequence state. */
struct sequence_data {
	/** Sequence id. */
	uint32_t id;
	/** Sequence value. */
	int64_t value;
};

static inline bool
sequence_data_equal(struct sequence_data data1, struct sequence_data data2)
{
	return data1.id == data2.id;
}

static inline bool
sequence_data_equal_key(struct sequence_data data, uint32_t id)
{
	return data.id == id;
}

#define LIGHT_NAME _sequence
#define LIGHT_DATA_TYPE struct sequence_data
#define LIGHT_KEY_TYPE uint32_t
#define LIGHT_CMP_ARG_TYPE int
#define LIGHT_EQUAL(a, b, c) sequence_data_equal(a, b)
#define LIGHT_EQUAL_KEY(a, b, c) sequence_data_equal_key(a, b)
#include "salad/light.h"

extern struct light_sequence_core sequence_data_index;

#undef LIGHT_NAME
#undef LIGHT_DATA_TYPE
#undef LIGHT_KEY_TYPE
#undef LIGHT_CMP_ARG_TYPE
#undef LIGHT_EQUAL
#undef LIGHT_EQUAL_KEY

/** Init sequence subsystem. */
void
sequence_init(void);

/** Destroy sequence subsystem. */
void
sequence_free(void);

/** Reset a sequence. */
void
sequence_reset(struct sequence *seq);

/**
 * Set a sequence value.
 *
 * Return 0 on success, -1 on memory allocation failure.
 */
int
sequence_set(struct sequence *seq, int64_t value);

/**
 * Update the sequence if the given value is newer than
 * the last generated value.
 *
 * Return 0 on success, -1 on memory allocation failure.
 */
int
sequence_update(struct sequence *seq, int64_t value);

/**
 * Advance a sequence.
 *
 * On success, return 0 and assign the next sequence to
 * @result, otherwise return -1 and set diag.
 *
 * The function may fail for two reasons:
 * - sequence isn't cyclic and has reached its limit
 * - memory allocation failure
 */
int
sequence_next(struct sequence *seq, int64_t *result);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

/**
 * Create an iterator over sequence data.
 *
 * The iterator creates a snapshot of sequence data and walks
 * over it, i.e. updates done after the iterator was open are
 * invisible. Used to make a snapshot of _sequence_data space.
 */
struct snapshot_iterator *
sequence_data_iterator_create(void);

#endif /* INCLUDES_TARANTOOL_BOX_SEQUENCE_H */
