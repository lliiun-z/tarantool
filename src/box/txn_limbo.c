/*
 * Copyright 2010-2020, Tarantool AUTHORS, please see AUTHORS file.
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
#include "txn.h"
#include "txn_limbo.h"
#include "replication.h"

struct txn_limbo txn_limbo;

static inline void
txn_limbo_create(struct txn_limbo *limbo)
{
	rlist_create(&limbo->queue);
	limbo->instance_id = REPLICA_ID_NIL;
	vclock_create(&limbo->vclock);
}

struct txn_limbo_entry *
txn_limbo_append(struct txn_limbo *limbo, uint32_t id, struct txn *txn)
{
	assert(txn_has_flag(txn, TXN_WAIT_SYNC));
	if (id == 0)
		id = instance_id;
	if (limbo->instance_id != id) {
		if (limbo->instance_id == REPLICA_ID_NIL ||
		    rlist_empty(&limbo->queue)) {
			limbo->instance_id = id;
		} else {
			diag_set(ClientError, ER_UNCOMMITTED_FOREIGN_SYNC_TXNS,
				 limbo->instance_id);
			return NULL;
		}
	}
	size_t size;
	struct txn_limbo_entry *e = region_alloc_object(&txn->region,
							typeof(*e), &size);
	if (e == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_object", "e");
		return NULL;
	}
	e->txn = txn;
	e->lsn = -1;
	e->ack_count = 0;
	e->is_commit = false;
	e->is_rollback = false;
	rlist_add_tail_entry(&limbo->queue, e, in_queue);
	return e;
}

static inline void
txn_limbo_remove(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	assert(!rlist_empty(&entry->in_queue));
	assert(rlist_first_entry(&limbo->queue, struct txn_limbo_entry,
				 in_queue) == entry);
	(void) limbo;
	rlist_del_entry(entry, in_queue);
}

void
txn_limbo_abort(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	entry->is_rollback = true;
	txn_limbo_remove(limbo, entry);
}

void
txn_limbo_assign_lsn(struct txn_limbo *limbo, struct txn_limbo_entry *entry,
		     int64_t lsn)
{
	assert(limbo->instance_id != REPLICA_ID_NIL);
	assert(entry->lsn == -1);
	assert(lsn > 0);
	(void) limbo;
	entry->lsn = lsn;
}

static bool
txn_limbo_check_complete(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	if (txn_limbo_entry_is_complete(entry))
		return true;
	struct vclock_iterator iter;
	vclock_iterator_init(&iter, &limbo->vclock);
	int ack_count = 0;
	int64_t lsn = entry->lsn;
	vclock_foreach(&iter, vc)
		ack_count += vc.lsn >= lsn;
	assert(ack_count >= entry->ack_count);
	entry->ack_count = ack_count;
	entry->is_commit = ack_count >= replication_synchro_quorum;
	return entry->is_commit;
}

void
txn_limbo_wait_complete(struct txn_limbo *limbo, struct txn_limbo_entry *entry)
{
	struct txn *txn = entry->txn;
	assert(entry->lsn > 0);
	if (txn_limbo_check_complete(limbo, entry))
		goto complete;

	assert(!txn_has_flag(txn, TXN_IS_DONE));
	assert(txn_has_flag(txn, TXN_WAIT_SYNC));
	bool cancellable = fiber_set_cancellable(false);
	while (!txn_limbo_entry_is_complete(entry))
		fiber_yield();
	fiber_set_cancellable(cancellable);
complete:
	// TODO: implement rollback.
	// TODO: implement confirm.
	assert(!entry->is_rollback);
	txn_limbo_remove(limbo, entry);
	txn_clear_flag(txn, TXN_WAIT_SYNC);
	txn_clear_flag(txn, TXN_WAIT_ACK);
}

void
txn_limbo_ack(struct txn_limbo *limbo, uint32_t replica_id, int64_t lsn)
{
	if (rlist_empty(&limbo->queue))
		return;
	assert(limbo->instance_id != REPLICA_ID_NIL);
	int64_t prev_lsn = vclock_get(&limbo->vclock, replica_id);
	vclock_follow(&limbo->vclock, replica_id, lsn);
	struct txn_limbo_entry *e, *last_quorum = NULL;
	rlist_foreach_entry(e, &limbo->queue, in_queue) {
		if (e->lsn > lsn)
			break;
		if (e->lsn <= prev_lsn)
			continue;
		assert(e->ack_count <= VCLOCK_MAX);
		/*
		 * Sync transactions need to collect acks. Async
		 * transactions are automatically committed right
		 * after all the previous sync transactions are.
		 */
		if (txn_has_flag(e->txn, TXN_WAIT_ACK)) {
			if (++e->ack_count < replication_synchro_quorum)
				continue;
		} else {
			assert(txn_has_flag(e->txn, TXN_WAIT_SYNC));
			if (last_quorum == NULL)
				continue;
		}
		e->is_commit = true;
		last_quorum = e;
	}
	if (last_quorum == NULL)
		return;
	// TODO: implement confirmation message.
	rlist_foreach_entry(e, &limbo->queue, in_queue) {
		if (e->txn->fiber != fiber())
			fiber_wakeup(e->txn->fiber);
		if (e == last_quorum)
			break;
	}
}

void
txn_limbo_init(void)
{
	txn_limbo_create(&txn_limbo);
}
