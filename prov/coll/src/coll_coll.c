/*
 * Copyright (c) 2019-2022 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "coll.h"
#include "ofi_coll.h"

static uint64_t coll_form_tag(uint32_t coll_id, uint32_t rank)
{
	uint64_t tag;
	uint64_t src_rank = rank;

	tag = coll_id;
	tag |= (src_rank << 32);

	return tag;
}

static uint32_t coll_get_next_id(struct util_coll_mc *coll_mc)
{
	uint32_t cid = coll_mc->group_id;
	return cid << 16 | coll_mc->seq++;
}

static struct util_coll_operation *
coll_create_op(struct fid_ep *ep, struct util_coll_mc *coll_mc,
	       enum util_coll_op_type type, uint64_t flags,
	       void *context, util_coll_comp_fn_t comp_fn)
{
	struct util_coll_operation *coll_op;

	coll_op = calloc(1, sizeof(*coll_op));
	if (!coll_op)
		return NULL;

	coll_op->ep = ep;
	coll_op->cid = coll_get_next_id(coll_mc);
	coll_op->mc = coll_mc;
	coll_op->type = type;
	coll_op->flags = flags;
	coll_op->context = context;
	coll_op->comp_fn = comp_fn;
	dlist_init(&coll_op->work_queue);

	return coll_op;
}

static void coll_log_work(struct util_coll_operation *coll_op)
{
#if ENABLE_DEBUG
	struct util_coll_work_item *cur_item = NULL;
	struct util_coll_xfer_item *xfer_item;
	struct dlist_entry *tmp = NULL;
	size_t count = 0;

	FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
	       "Remaining Work for %s:\n",
	       log_util_coll_op_type[coll_op->type]);
	dlist_foreach_container_safe(&coll_op->work_queue,
				     struct util_coll_work_item,
				     cur_item, waiting_entry, tmp)
	{
		switch (cur_item->type) {
		case UTIL_COLL_SEND:
			xfer_item = container_of(cur_item,
						 struct util_coll_xfer_item,
						 hdr);
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "\t%ld: { %p [%s] SEND TO: 0x%02x FROM: 0x%02lx "
			       "cnt: %d typesize: %ld tag: 0x%02lx }\n",
			       count, cur_item,
			       log_util_coll_state[cur_item->state],
			       xfer_item->remote_rank, coll_op->mc->local_rank,
			       xfer_item->count,
			       ofi_datatype_size(xfer_item->datatype),
			       xfer_item->tag);
			break;

		case UTIL_COLL_RECV:
			xfer_item = container_of(cur_item,
						 struct util_coll_xfer_item,
						 hdr);
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "\t%ld: { %p [%s] RECV FROM: 0x%02x TO: 0x%02lx "
			       "cnt: %d typesize: %ld tag: 0x%02lx }\n",
			       count, cur_item,
			       log_util_coll_state[cur_item->state],
			       xfer_item->remote_rank, coll_op->mc->local_rank,
			       xfer_item->count,
			       ofi_datatype_size(xfer_item->datatype),
			       xfer_item->tag);
			break;

		case UTIL_COLL_REDUCE:
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "\t%ld: { %p [%s] REDUCTION }\n",
			       count, cur_item,
			       log_util_coll_state[cur_item->state]);
			break;

		case UTIL_COLL_COPY:
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "\t%ld: { %p [%s] COPY }\n", count, cur_item,
			       log_util_coll_state[cur_item->state]);
			break;

		case UTIL_COLL_COMP:
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "\t%ld: { %p [%s] COMPLETION }\n", count, cur_item,
			       log_util_coll_state[cur_item->state]);
			break;

		default:
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "\t%ld: { %p [%s] UNKNOWN }\n", count, cur_item,
			       log_util_coll_state[cur_item->state]);
			break;
		}
		count++;
	}
#endif
}

static void coll_progress_work(struct util_ep *util_ep,
		   	       struct util_coll_operation *coll_op)
{
	struct util_coll_work_item *next_ready = NULL;
	struct util_coll_work_item *cur_item = NULL;
	struct util_coll_work_item *prev_item = NULL;
	struct dlist_entry *tmp = NULL;
	int previous_is_head;

	/* clean up any completed items while searching for the next ready */
	dlist_foreach_container_safe(&coll_op->work_queue,
				     struct util_coll_work_item,
				     cur_item, waiting_entry, tmp) {

		previous_is_head = (cur_item->waiting_entry.prev ==
				    &cur_item->coll_op->work_queue);
		if (!previous_is_head) {
			prev_item = container_of(cur_item->waiting_entry.prev,
						 struct util_coll_work_item,
						 waiting_entry);
		}

		if (cur_item->state == UTIL_COLL_COMPLETE) {
			/*
			 * If there is work before cur and cur is fencing,
			 * we can't complete.
			 */
			if (cur_item->fence && !previous_is_head)
				continue;

			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "Removing Completed Work item: %p \n", cur_item);
			dlist_remove(&cur_item->waiting_entry);
			free(cur_item);

			/* if the work queue is empty, we're done */
			if (dlist_empty(&coll_op->work_queue)) {
				free(coll_op);
				return;
			}
			continue;
		}

		/* we can't progress if prior work is fencing */
		if (!previous_is_head && prev_item && prev_item->fence) {
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "%p fenced by: %p \n", cur_item, prev_item);
			return;
		}

		/*
		 * If the current item isn't waiting, it's not the next
		 * ready item.
		 */
		if (cur_item->state != UTIL_COLL_WAITING) {
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "Work item not waiting: %p [%s]\n", cur_item,
			       log_util_coll_state[cur_item->state]);
			continue;
		}

		FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
		       "Ready item: %p \n", cur_item);
		next_ready = cur_item;
		break;
	}

	if (!next_ready)
		return;

	coll_log_work(coll_op);

	next_ready->state = UTIL_COLL_PROCESSING;
	slist_insert_tail(&next_ready->ready_entry, &util_ep->coll_ready_queue);
}

static void coll_bind_work(struct util_coll_operation *coll_op,
			   struct util_coll_work_item *item)
{
	item->coll_op = coll_op;
	dlist_insert_tail(&item->waiting_entry, &coll_op->work_queue);
}

static int coll_sched_send(struct util_coll_operation *coll_op,
			   uint64_t dest, void *buf, size_t count,
			   enum fi_datatype datatype, int fence)
{
	struct util_coll_xfer_item *xfer_item;

	xfer_item = calloc(1, sizeof(*xfer_item));
	if (!xfer_item)
		return -FI_ENOMEM;

	xfer_item->hdr.type = UTIL_COLL_SEND;
	xfer_item->hdr.state = UTIL_COLL_WAITING;
	xfer_item->hdr.fence = fence;
	xfer_item->tag = coll_form_tag(coll_op->cid,
				       (uint32_t) coll_op->mc->local_rank);
	xfer_item->buf = buf;
	xfer_item->count = (int) count;
	xfer_item->datatype = datatype;
	xfer_item->remote_rank = (int) dest;

	coll_bind_work(coll_op, &xfer_item->hdr);
	return FI_SUCCESS;
}

static int coll_sched_recv(struct util_coll_operation *coll_op,
			   uint64_t src, void *buf, size_t count,
			   enum fi_datatype datatype, int fence)
{
	struct util_coll_xfer_item *xfer_item;

	xfer_item = calloc(1, sizeof(*xfer_item));
	if (!xfer_item)
		return -FI_ENOMEM;

	xfer_item->hdr.type = UTIL_COLL_RECV;
	xfer_item->hdr.state = UTIL_COLL_WAITING;
	xfer_item->hdr.fence = fence;
	xfer_item->tag = coll_form_tag(coll_op->cid, (uint32_t) src);
	xfer_item->buf = buf;
	xfer_item->count = (int) count;
	xfer_item->datatype = datatype;
	xfer_item->remote_rank = (int) src;

	coll_bind_work(coll_op, &xfer_item->hdr);
	return FI_SUCCESS;
}

static int coll_sched_reduce(struct util_coll_operation *coll_op,
			     void *in_buf, void *inout_buf,
			     size_t count, enum fi_datatype datatype,
			     enum fi_op op, int fence)
{
	struct util_coll_reduce_item *reduce_item;

	reduce_item = calloc(1, sizeof(*reduce_item));
	if (!reduce_item)
		return -FI_ENOMEM;

	reduce_item->hdr.type = UTIL_COLL_REDUCE;
	reduce_item->hdr.state = UTIL_COLL_WAITING;
	reduce_item->hdr.fence = fence;
	reduce_item->in_buf = in_buf;
	reduce_item->inout_buf = inout_buf;
	reduce_item->count = (int) count;
	reduce_item->datatype = datatype;
	reduce_item->op = op;

	coll_bind_work(coll_op, &reduce_item->hdr);
	return FI_SUCCESS;
}

static int coll_sched_copy(struct util_coll_operation *coll_op,
			   void *in_buf, void *out_buf, size_t count,
			   enum fi_datatype datatype, int fence)
{
	struct util_coll_copy_item *copy_item;

	copy_item = calloc(1, sizeof(*copy_item));
	if (!copy_item)
		return -FI_ENOMEM;

	copy_item->hdr.type = UTIL_COLL_COPY;
	copy_item->hdr.state = UTIL_COLL_WAITING;
	copy_item->hdr.fence = fence;
	copy_item->in_buf = in_buf;
	copy_item->out_buf = out_buf;
	copy_item->count = (int) count;
	copy_item->datatype = datatype;

	coll_bind_work(coll_op, &copy_item->hdr);
	return FI_SUCCESS;
}

static int coll_sched_comp(struct util_coll_operation *coll_op)
{
	struct util_coll_work_item *comp_item;

	comp_item = calloc(1, sizeof(*comp_item));
	if (!comp_item)
		return -FI_ENOMEM;

	comp_item->type = UTIL_COLL_COMP;
	comp_item->state = UTIL_COLL_WAITING;
	comp_item->fence = 1;

	coll_bind_work(coll_op, comp_item);
	return FI_SUCCESS;
}

/*
 * TODO:
 * when this fails, clean up the already scheduled work in this function
 */
static int coll_do_allreduce(struct util_coll_operation *coll_op,
			     const void *send_buf, void *result,
			     void* tmp_buf, uint64_t count,
			     enum fi_datatype datatype, enum fi_op op)
{
	uint64_t rem, pof2, my_new_id;
	uint64_t local, remote, next_remote;
	int ret;
	uint64_t mask = 1;

	pof2 = rounddown_power_of_two(coll_op->mc->av_set->fi_addr_count);
	rem = coll_op->mc->av_set->fi_addr_count - pof2;
	local = coll_op->mc->local_rank;

	/* copy initial send data to result */
	memcpy(result, send_buf, count * ofi_datatype_size(datatype));

	if (local < 2 * rem) {
		if (local % 2 == 0) {
			ret = coll_sched_send(coll_op, local + 1, result,
					      count, datatype, 1);
			if (ret)
				return ret;

			my_new_id = (uint64_t)-1;
		} else {
			ret = coll_sched_recv(coll_op, local - 1,
					      tmp_buf, count, datatype, 1);
			if (ret)
				return ret;

			my_new_id = local / 2;

			ret = coll_sched_reduce(coll_op, tmp_buf, result,
						count, datatype, op, 1);
			if (ret)
				return ret;
		}
	} else {
		my_new_id = local - rem;
	}

	if (my_new_id != -1) {
		while (mask < pof2) {
			next_remote = my_new_id ^ mask;
			remote = (next_remote < rem) ? next_remote * 2 + 1 :
				next_remote + rem;

			/* receive remote data into tmp buf */
			ret = coll_sched_recv(coll_op, remote, tmp_buf,
					      count, datatype, 0);
			if (ret)
				return ret;

			/* send result buf, which has the current total */
			ret = coll_sched_send(coll_op, remote, result,
					      count, datatype, 1);
			if (ret)
				return ret;

			if (remote < local) {
				/* reduce received remote into result buf */
				ret = coll_sched_reduce(coll_op, tmp_buf,
						        result, count,
						        datatype, op, 1);
				if (ret)
					return ret;
			} else {
				/* reduce local result into received data */
				ret = coll_sched_reduce(coll_op, result,
							tmp_buf, count,
							datatype, op, 1);
				if (ret)
					return ret;

				/* copy total into result */
				ret = coll_sched_copy(coll_op, tmp_buf,
						      result, count,
						      datatype, 1);
				if (ret)
					return ret;
			}
			mask <<= 1;
		}
	}

	if (local < 2 * rem) {
		if (local % 2) {
			ret = coll_sched_send(coll_op, local - 1, result,
					      count, datatype, 1);
			if (ret)
				return ret;
		} else {
			ret = coll_sched_recv(coll_op, local + 1, result,
					      count, datatype, 1);
			if (ret)
				return ret;
		}
	}
	return FI_SUCCESS;
}

/* allgather implemented using ring algorithm */
static int coll_do_allgather(struct util_coll_operation *coll_op,
			     const void *send_buf, void *result, size_t count,
			     enum fi_datatype datatype)
{
	uint64_t i, cur_offset, next_offset;
	int ret;
	size_t nbytes, numranks;
	uint64_t local_rank, left_rank, right_rank;

	local_rank = coll_op->mc->local_rank;
	nbytes = ofi_datatype_size(datatype) * count;
	numranks = coll_op->mc->av_set->fi_addr_count;

	/* copy the local value to the appropriate place in result buffer */
	ret = coll_sched_copy(coll_op, (void *) send_buf,
			      (char *) result + (local_rank * nbytes),
			      count, datatype, 1);
	if (ret)
		return ret;

	/* send to right, recv from left */
	left_rank = (numranks + local_rank - 1) % numranks;
	right_rank = (local_rank + 1) % numranks;

	cur_offset = local_rank;
	next_offset = left_rank;

	/* fill in result with data going right to left */
	for (i = 1; i < numranks; i++) {
		ret = coll_sched_send(coll_op, right_rank,
				      (char *) result + (cur_offset * nbytes),
				      count, datatype, 0);
		if (ret)
			return ret;

		ret = coll_sched_recv(coll_op, left_rank,
				      (char *) result + (next_offset * nbytes),
				      count, datatype, 1);
		if (ret)
			return ret;

		cur_offset = next_offset;
		next_offset = (numranks + next_offset - 1) % numranks;
	}

	return FI_SUCCESS;
}

static size_t util_binomial_tree_values_to_recv(uint64_t rank, size_t numranks)
{
	size_t nvalues = 0x1ULL << (ofi_lsb(rank) - 1);
	if (numranks < rank + nvalues)
		nvalues = numranks - rank;

	return nvalues;
}

/* Scatter implemented with binomial tree algorithm */
static int coll_do_scatter(struct util_coll_operation *coll_op,
			   const void *data, void *result, void **temp,
			   size_t count, uint64_t root,
			   enum fi_datatype datatype)
{
	int64_t remote_rank;
	uint64_t local_rank, relative_rank, mask;
	size_t nbytes, numranks, send_cnt, cur_cnt = 0;
	int ret;
	void *send_data;

	local_rank = coll_op->mc->local_rank;
	numranks = coll_op->mc->av_set->fi_addr_count;
	relative_rank = (local_rank >= root) ?
			local_rank - root : local_rank - root + numranks;
	nbytes = count * ofi_datatype_size(datatype);

	/* check if we need to participate */
	if (count == 0)
		return FI_SUCCESS;

	/*
	 * Non-root even nodes get a temp buffer for receiving data.
	 * These nodes may need to send part of what they receive.
	 */
	if (relative_rank && !(relative_rank % 2)) {
		cur_cnt = count *
			  util_binomial_tree_values_to_recv(relative_rank,
							    numranks);
		*temp = malloc(cur_cnt * ofi_datatype_size(datatype));
		if (!*temp)
			return -FI_ENOMEM;
	}

	if (local_rank == root) {
		cur_cnt = count * numranks;
		if (root != 0) {
			/*
			 * If we're root but not rank 0, we need to reorder the
			 * send buffer according to destination rank.
			 * E.g. if we're rank 3, data intended for ranks 0-2
			 * will be moved to the end
			 */
			*temp = malloc(cur_cnt * ofi_datatype_size(datatype));
			if (!*temp)
				return -FI_ENOMEM;

			ret = coll_sched_copy(coll_op,
					      (char *) data + nbytes * local_rank,
					      *temp,
					      (numranks - local_rank) * count,
					      datatype, 1);
			if (ret)
				return ret;

			ret = coll_sched_copy(coll_op, (char *) data,
					      (char *) *temp + (numranks - local_rank) * nbytes,
					      local_rank * count, datatype, 1);
			if (ret)
				return ret;
		}
	}

	/* set up all receives */
	mask = 0x1;
	while (mask < numranks) {
		if (relative_rank & mask) {
			remote_rank = local_rank - mask;
			if (remote_rank < 0)
				remote_rank += numranks;

			if (relative_rank % 2) {
				/* leaf node, receive our data */
				ret = coll_sched_recv(coll_op, remote_rank,
						      result, count, datatype,
						      1);
				if (ret)
					return ret;
			} else {
				/* branch node, receive data to forward */
				ret = coll_sched_recv(coll_op, remote_rank,
						      *temp, cur_cnt, datatype,
						      1);
				if (ret)
					return ret;
			}
			break;
		}
		mask <<= 1;
	}

	/* set up all sends */
	send_data = root == local_rank && root == 0 ? (void *) data : *temp;
	mask >>= 1;
	while (mask > 0) {
		if (relative_rank + mask < numranks) {
			/*
			 * To this point, cur_cnt has represented the number of
			 * values to expect to store in our data buf.  From here
			 * on, cur_cnt is the number of values we have left to
			 * forward from the data buf.
			 */
			send_cnt = cur_cnt - count * mask;

			remote_rank = local_rank + mask;
			if ((uint64_t) remote_rank >= numranks)
				remote_rank -= numranks;

			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "MASK: 0x%0lx CUR_CNT: %ld SENDING: %ld TO: %ld\n",
			       mask, cur_cnt, send_cnt, remote_rank);

			assert(send_cnt > 0);

			ret = coll_sched_send(coll_op, remote_rank,
					      (char *) send_data + nbytes * mask,
					      send_cnt, datatype, 1);
			if (ret)
				return ret;

			cur_cnt -= send_cnt;
		}
		mask >>= 1;
	}

	if (!(relative_rank % 2)) {
		/*
		 * for the root and all even nodes, we've got to copy
		 * our local data to the result buffer
		 */
		ret = coll_sched_copy(coll_op, send_data, result,
				      count, datatype, 1);
		if (ret)
			return ret;
	}

	return FI_SUCCESS;
}

static int coll_close(struct fid *fid)
{
	struct util_coll_mc *coll_mc;

	coll_mc = container_of(fid, struct util_coll_mc, mc_fid.fid);

	ofi_atomic_dec32(&coll_mc->av_set->ref);
	free(coll_mc);

	return FI_SUCCESS;
}

static struct fi_ops util_coll_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = coll_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static int coll_find_local_rank(struct fid_ep *ep,
				struct util_coll_mc *coll_mc)
{
	struct coll_av *av = container_of(coll_mc->av_set->av, struct coll_av,
					  util_av.av_fid);
	fi_addr_t my_addr;
	int i;

	my_addr = av->peer_av->owner_ops->ep_addr(av->peer_av, ep);

	coll_mc->local_rank = FI_ADDR_NOTAVAIL;
	if (my_addr != FI_ADDR_NOTAVAIL) {
		for (i = 0; i < coll_mc->av_set->fi_addr_count; i++)
			if (coll_mc->av_set->fi_addr_array[i] == my_addr) {
				coll_mc->local_rank = i;
				break;
			}
	}

	return FI_SUCCESS;
}

void coll_join_comp(struct util_coll_operation *coll_op)
{
	struct fi_eq_entry entry;
	struct coll_ep *ep;
	struct ofi_coll_eq *eq;

	ep = container_of(coll_op->ep, struct coll_ep, util_ep.ep_fid);
	eq = container_of(ep->util_ep.eq, struct ofi_coll_eq, util_eq.eq_fid);

	coll_op->data.join.new_mc->seq = 0;
	coll_op->data.join.new_mc->group_id =
		(uint16_t) ofi_bitmask_get_lsbset(coll_op->data.join.data);

	/* mark the local mask bit */
	ofi_bitmask_unset(ep->util_ep.coll_cid_mask,
			  coll_op->data.join.new_mc->group_id);

	/* write to the eq */
	memset(&entry, 0, sizeof(entry));
	entry.fid = &coll_op->mc->mc_fid.fid;
	entry.context = coll_op->context;

	if (fi_eq_write(eq->peer_eq, FI_JOIN_COMPLETE, &entry,
			sizeof(struct fi_eq_entry), FI_COLLECTIVE) < 0)
		FI_WARN(ep->util_ep.domain->fabric->prov, FI_LOG_DOMAIN,
			"join collective - eq write failed\n");

	ofi_bitmask_free(&coll_op->data.join.data);
	ofi_bitmask_free(&coll_op->data.join.tmp);
}

void coll_collective_comp(struct util_coll_operation *coll_op)
{
	struct coll_ep *ep;
	struct ofi_coll_cq *cq;

	ep = container_of(coll_op->ep, struct coll_ep, util_ep.ep_fid);
	cq = container_of(ep->util_ep.tx_cq, struct ofi_coll_cq, util_cq);

	if (cq->peer_cq->owner_ops->write(cq->peer_cq, coll_op->context,
					  FI_COLLECTIVE, 0, 0, 0, 0, 0))
		FI_WARN(ep->util_ep.domain->fabric->prov, FI_LOG_DOMAIN,
			"collective - cq write failed\n");

	switch (coll_op->type) {
	case UTIL_COLL_ALLREDUCE_OP:
		free(coll_op->data.allreduce.data);
		break;

	case UTIL_COLL_SCATTER_OP:
		free(coll_op->data.scatter);
		break;

	case UTIL_COLL_BROADCAST_OP:
		free(coll_op->data.broadcast.chunk);
		free(coll_op->data.broadcast.scatter);
		break;

	case UTIL_COLL_JOIN_OP:
	case UTIL_COLL_BARRIER_OP:
	case UTIL_COLL_ALLGATHER_OP:
	default:
		/* nothing to clean up */
		break;
	}
}

static ssize_t coll_process_reduce_item(struct util_coll_reduce_item *reduce_item)
{
	if (reduce_item->op < FI_MIN || reduce_item->op > FI_BXOR)
		return -FI_ENOSYS;

	ofi_atomic_write_handler(reduce_item->op, reduce_item->datatype,
				 reduce_item->inout_buf,
				 reduce_item->in_buf,
				 reduce_item->count);
	return FI_SUCCESS;
}

static ssize_t coll_process_xfer_item(struct util_coll_xfer_item *item)
{
	struct util_coll_operation *coll_op;
	struct coll_ep *ep;
	struct fi_msg_tagged msg;
	struct iovec iov;
	ssize_t ret;

	coll_op = item->hdr.coll_op;
	ep = container_of(coll_op->ep, struct coll_ep, util_ep.ep_fid);

	msg.msg_iov = &iov;
	msg.desc = NULL;
	msg.iov_count = 1;
	msg.ignore = 0;
	msg.context = item;
	msg.data = 0;
	msg.tag = item->tag;
	msg.addr = coll_op->mc->av_set->fi_addr_array[item->remote_rank];

	iov.iov_base = item->buf;
	iov.iov_len = (item->count * ofi_datatype_size(item->datatype));

	if (item->hdr.type == UTIL_COLL_SEND) {
		ret = fi_tsendmsg(ep->peer_ep, &msg, FI_PEER_TRANSFER);
		if (!ret)
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "%p SEND [0x%02lx] -> [0x%02x] cnt: %d sz: %ld\n",
			       item, coll_op->mc->local_rank, item->remote_rank,
			       item->count,
			       item->count * ofi_datatype_size(item->datatype));
		return ret;
	} else if (item->hdr.type == UTIL_COLL_RECV) {
		ret = fi_trecvmsg(ep->peer_ep, &msg, FI_PEER_TRANSFER);
		if (!ret)
			FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
			       "%p RECV [0x%02lx] <- [0x%02x] cnt: %d sz: %ld\n",
			       item, coll_op->mc->local_rank, item->remote_rank,
			       item->count,
			       item->count * ofi_datatype_size(item->datatype));
		return ret;
	}

	return -FI_ENOSYS;
}

void coll_ep_progress(struct util_ep *util_ep)
{
	struct util_coll_work_item *work_item;
	struct util_coll_reduce_item *reduce_item;
	struct util_coll_copy_item *copy_item;
	struct util_coll_xfer_item *xfer_item;
	struct util_coll_operation *coll_op;
	ssize_t ret;

	while (!slist_empty(&util_ep->coll_ready_queue)) {
		slist_remove_head_container(&util_ep->coll_ready_queue,
					    struct util_coll_work_item,
					    work_item, ready_entry);
		coll_op = work_item->coll_op;
		switch (work_item->type) {
		case UTIL_COLL_SEND:
			xfer_item = container_of(work_item,
						 struct util_coll_xfer_item,
						 hdr);
			ret = coll_process_xfer_item(xfer_item);
			if (ret && ret == -FI_EAGAIN) {
				slist_insert_tail(&work_item->ready_entry,
						  &util_ep->coll_ready_queue);
				goto out;
			}
			break;

		case UTIL_COLL_RECV:
			xfer_item = container_of(work_item,
						 struct util_coll_xfer_item,
						 hdr);
			ret = coll_process_xfer_item(xfer_item);
			if (ret)
				goto out;
			break;

		case UTIL_COLL_REDUCE:
			reduce_item = container_of(work_item,
						   struct util_coll_reduce_item,
						   hdr);
			ret = coll_process_reduce_item(reduce_item);
			if (ret)
				goto out;

			reduce_item->hdr.state = UTIL_COLL_COMPLETE;
			break;

		case UTIL_COLL_COPY:
			copy_item = container_of(work_item,
						 struct util_coll_copy_item,
						 hdr);
			memcpy(copy_item->out_buf, copy_item->in_buf,
			       copy_item->count *
				       ofi_datatype_size(copy_item->datatype));

			copy_item->hdr.state = UTIL_COLL_COMPLETE;
			break;

		case UTIL_COLL_COMP:
			if (work_item->coll_op->comp_fn)
				work_item->coll_op->comp_fn(work_item->coll_op);

			work_item->state = UTIL_COLL_COMPLETE;
			break;

		default:
			goto out;
		}

		coll_progress_work(util_ep, coll_op);
	}

out:
	return;
}

static struct util_coll_mc *coll_create_mc(struct util_av_set *av_set,
					   void *context)
{
	struct util_coll_mc *coll_mc;

	coll_mc = calloc(1, sizeof(*coll_mc));
	if (!coll_mc)
		return NULL;

	coll_mc->mc_fid.fid.fclass = FI_CLASS_MC;
	coll_mc->mc_fid.fid.context = context;
	coll_mc->mc_fid.fid.ops = &util_coll_fi_ops;
	coll_mc->mc_fid.fi_addr = (uintptr_t) coll_mc;

	ofi_atomic_inc32(&av_set->ref);
	coll_mc->av_set = av_set;

	return coll_mc;
}

int coll_join_collective(struct fid_ep *ep, const void *addr,
		         uint64_t flags, struct fid_mc **mc, void *context)
{
	struct util_coll_mc *new_coll_mc;
	struct util_av_set *av_set;
	struct util_coll_mc *coll_mc;
	struct util_coll_operation *join_op;
	struct util_ep *util_ep;
	struct fi_collective_addr *c_addr;
	fi_addr_t coll_addr;
	const struct fid_av_set *set;
	int ret;

	if (!(flags & FI_COLLECTIVE))
		return -FI_ENOSYS;

	c_addr = (struct fi_collective_addr *)addr;
	coll_addr = c_addr->coll_addr;
	set = c_addr->set;

	av_set = container_of(set, struct util_av_set, av_set_fid);

	if (coll_addr == FI_ADDR_NOTAVAIL) {
		ofi_genlock_lock(&av_set->av->lock);
		assert(av_set->av->av_set);
		coll_mc = &av_set->av->av_set->coll_mc;
		ofi_genlock_unlock(&av_set->av->lock);
	} else {
		coll_mc = (struct util_coll_mc*) ((uintptr_t) coll_addr);
	}

	new_coll_mc = coll_create_mc(av_set, context);
	if (!new_coll_mc)
		return -FI_ENOMEM;

	/* get the rank */
	coll_find_local_rank(ep, new_coll_mc);
	coll_find_local_rank(ep, coll_mc);

	join_op = coll_create_op(ep, coll_mc, UTIL_COLL_JOIN_OP, flags,
				 context, coll_join_comp);
	if (!join_op) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	join_op->data.join.new_mc = new_coll_mc;

	ret = ofi_bitmask_create(&join_op->data.join.data, OFI_MAX_GROUP_ID);
	if (ret)
		goto err2;

	ret = ofi_bitmask_create(&join_op->data.join.tmp, OFI_MAX_GROUP_ID);
	if (ret)
		goto err3;

	util_ep = container_of(ep, struct util_ep, ep_fid);
	ret = coll_do_allreduce(join_op, util_ep->coll_cid_mask->bytes,
				join_op->data.join.data.bytes,
				join_op->data.join.tmp.bytes,
				(int) ofi_bitmask_bytesize(util_ep->coll_cid_mask),
				FI_UINT8, FI_BAND);
	if (ret)
		goto err4;

	ret = coll_sched_comp(join_op);
	if (ret)
		goto err4;

	coll_progress_work(util_ep, join_op);

	*mc = &new_coll_mc->mc_fid;
	return FI_SUCCESS;

err4:
	ofi_bitmask_free(&join_op->data.join.tmp);
err3:
	ofi_bitmask_free(&join_op->data.join.data);
err2:
	free(join_op);
err1:
	fi_close(&new_coll_mc->mc_fid.fid);
	return ret;
}

ssize_t coll_ep_barrier2(struct fid_ep *ep, fi_addr_t coll_addr, uint64_t flags,
			 void *context)
{
	struct util_coll_mc *coll_mc;
	struct util_coll_operation *barrier_op;
	struct util_ep *util_ep;
	uint64_t send;
	int ret;

	coll_mc = (struct util_coll_mc*) ((uintptr_t) coll_addr);

	barrier_op = coll_create_op(ep, coll_mc, UTIL_COLL_BARRIER_OP,
				    flags, context,
				    coll_collective_comp);
	if (!barrier_op)
		return -FI_ENOMEM;

	send = ~barrier_op->mc->local_rank;
	ret = coll_do_allreduce(barrier_op, &send,
				&barrier_op->data.barrier.data,
				&barrier_op->data.barrier.tmp, 1, FI_UINT64,
				FI_BAND);
	if (ret)
		goto err1;

	ret = coll_sched_comp(barrier_op);
	if (ret)
		goto err1;

	util_ep = container_of(ep, struct util_ep, ep_fid);
	coll_progress_work(util_ep, barrier_op);

	return FI_SUCCESS;
err1:
	free(barrier_op);
	return ret;
}

ssize_t coll_ep_barrier(struct fid_ep *ep, fi_addr_t coll_addr, void *context)
{
	return coll_ep_barrier2(ep, coll_addr, 0, context);
}

ssize_t coll_ep_allreduce(struct fid_ep *ep, const void *buf, size_t count,
			  void *desc, void *result, void *result_desc,
			  fi_addr_t coll_addr, enum fi_datatype datatype,
			  enum fi_op op, uint64_t flags, void *context)
{
	struct util_coll_mc *coll_mc;
	struct util_coll_operation *allreduce_op;
	struct util_ep *util_ep;
	int ret;

	coll_mc = (struct util_coll_mc *) ((uintptr_t) coll_addr);
	allreduce_op = coll_create_op(ep, coll_mc, UTIL_COLL_ALLREDUCE_OP,
				      flags, context,
				      coll_collective_comp);
	if (!allreduce_op)
		return -FI_ENOMEM;

	allreduce_op->data.allreduce.size = count * ofi_datatype_size(datatype);
	allreduce_op->data.allreduce.data = calloc(count,
						   ofi_datatype_size(datatype));
	if (!allreduce_op->data.allreduce.data) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	ret = coll_do_allreduce(allreduce_op, buf, result,
				allreduce_op->data.allreduce.data, count,
				datatype, op);
	if (ret)
		goto err2;

	ret = coll_sched_comp(allreduce_op);
	if (ret)
		goto err2;

	util_ep = container_of(ep, struct util_ep, ep_fid);
	coll_progress_work(util_ep, allreduce_op);

	return FI_SUCCESS;

err2:
	free(allreduce_op->data.allreduce.data);
err1:
	free(allreduce_op);
	return ret;
}

ssize_t coll_ep_allgather(struct fid_ep *ep, const void *buf, size_t count,
			  void *desc, void *result, void *result_desc,
			  fi_addr_t coll_addr, enum fi_datatype datatype,
			  uint64_t flags, void *context)
{
	struct util_coll_mc *coll_mc;
	struct util_coll_operation *allgather_op;
	struct util_ep *util_ep;
	int ret;

	coll_mc = (struct util_coll_mc *) ((uintptr_t) coll_addr);
	allgather_op = coll_create_op(ep, coll_mc, UTIL_COLL_ALLGATHER_OP,
				      flags, context,
				      coll_collective_comp);
	if (!allgather_op)
		return -FI_ENOMEM;

	ret = coll_do_allgather(allgather_op, buf, result, count, datatype);
	if (ret)
		goto err;

	ret = coll_sched_comp(allgather_op);
	if (ret)
		goto err;

	util_ep = container_of(ep, struct util_ep, ep_fid);
	coll_progress_work(util_ep, allgather_op);

	return FI_SUCCESS;
err:
	free(allgather_op);
	return ret;
}

ssize_t coll_ep_scatter(struct fid_ep *ep, const void *buf, size_t count,
			void *desc, void *result, void *result_desc,
			fi_addr_t coll_addr, fi_addr_t root_addr,
			enum fi_datatype datatype, uint64_t flags,
		        void *context)
{
	struct util_coll_mc *coll_mc;
	struct util_coll_operation *scatter_op;
	struct util_ep *util_ep;
	int ret;

	coll_mc = (struct util_coll_mc *) ((uintptr_t) coll_addr);
	scatter_op = coll_create_op(ep, coll_mc, UTIL_COLL_SCATTER_OP,
				    flags, context,
				    coll_collective_comp);
	if (!scatter_op)
		return -FI_ENOMEM;

	ret = coll_do_scatter(scatter_op, buf, result,
			      &scatter_op->data.scatter, count,
			      root_addr, datatype);
	if (ret)
		goto err;

	ret = coll_sched_comp(scatter_op);
	if (ret)
		goto err;

	util_ep = container_of(ep, struct util_ep, ep_fid);
	coll_progress_work(util_ep, scatter_op);

	return FI_SUCCESS;
err:
	free(scatter_op);
	return ret;
}

ssize_t coll_ep_broadcast(struct fid_ep *ep, void *buf, size_t count,
			  void *desc, fi_addr_t coll_addr, fi_addr_t root_addr,
			  enum fi_datatype datatype, uint64_t flags,
			  void *context)
{
	struct util_coll_mc *coll_mc;
	struct util_coll_operation *broadcast_op;
	struct util_ep *util_ep;
	uint64_t chunk_cnt, numranks, local;
	int ret;

	coll_mc = (struct util_coll_mc *) ((uintptr_t) coll_addr);
	broadcast_op = coll_create_op(ep, coll_mc, UTIL_COLL_BROADCAST_OP,
				      flags, context,
				      coll_collective_comp);
	if (!broadcast_op)
		return -FI_ENOMEM;

	local = broadcast_op->mc->local_rank;
	numranks = broadcast_op->mc->av_set->fi_addr_count;
	chunk_cnt = (count + numranks - 1) / numranks;
	if (chunk_cnt * local > count &&
	    chunk_cnt * local - (int) count > chunk_cnt)
		chunk_cnt = 0;

	broadcast_op->data.broadcast.chunk =
		malloc(chunk_cnt * ofi_datatype_size(datatype));
	if (!broadcast_op->data.broadcast.chunk) {
		ret = -FI_ENOMEM;
		goto err1;
	}

	ret = coll_do_scatter(broadcast_op, buf,
			      broadcast_op->data.broadcast.chunk,
			      &broadcast_op->data.broadcast.scatter,
			      chunk_cnt, root_addr, datatype);
	if (ret)
		goto err2;

	ret = coll_do_allgather(broadcast_op,
				broadcast_op->data.broadcast.chunk, buf,
				chunk_cnt, datatype);
	if (ret)
		goto err2;

	ret = coll_sched_comp(broadcast_op);
	if (ret)
		goto err2;

	util_ep = container_of(ep, struct util_ep, ep_fid);
	coll_progress_work(util_ep, broadcast_op);

	return FI_SUCCESS;
err2:
	free(broadcast_op->data.broadcast.chunk);
err1:
	free(broadcast_op);
	return ret;
}

ssize_t coll_peer_xfer_complete(struct fid_ep *ep,
				struct fi_cq_tagged_entry *cqe,
				fi_addr_t src_addr)
{
	struct util_coll_operation *coll_op;
	struct util_ep *util_ep;
	struct util_coll_xfer_item *xfer_item;

	xfer_item = cqe->op_context;
	xfer_item->hdr.state = UTIL_COLL_COMPLETE;

	coll_op = xfer_item->hdr.coll_op;
	FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
	       "\tXfer complete: { %p %s Remote: 0x%02x Local: "
	       "0x%02lx cnt: %d typesize: %ld }\n", xfer_item,
	       xfer_item->hdr.type == UTIL_COLL_SEND ? "SEND" : "RECV",
	       xfer_item->remote_rank, coll_op->mc->local_rank,
	       xfer_item->count, ofi_datatype_size(xfer_item->datatype));

	util_ep = container_of(coll_op->ep, struct util_ep, ep_fid);
	coll_progress_work(util_ep, coll_op);

	return 0;
}

ssize_t coll_peer_xfer_error(struct fid_ep *ep, struct fi_cq_err_entry *cqerr)
{
	struct util_coll_operation *coll_op;
	struct util_coll_xfer_item *xfer_item;

	xfer_item = cqerr->op_context;
	xfer_item->hdr.state = UTIL_COLL_COMPLETE;

	coll_op = xfer_item->hdr.coll_op;
	/* Eliminate non-debug build warning */
	(void) coll_op;

	FI_DBG(coll_op->mc->av_set->av->prov, FI_LOG_CQ,
	       "\tXfer error: { %p %s Remote: 0x%02x Local: "
	       "0x%02lx cnt: %d typesize: %ld }\n", xfer_item,
	       xfer_item->hdr.type == UTIL_COLL_SEND ? "SEND" : "RECV",
	       xfer_item->remote_rank, coll_op->mc->local_rank,
	       xfer_item->count, ofi_datatype_size(xfer_item->datatype));

	/* TODO: finish the work with error */

	return 0;
}

int coll_query_collective(struct fid_domain *dom_fid,
			  enum fi_collective_op coll,
			  struct fi_collective_attr *attr, uint64_t flags)
{
	int ret;
	struct coll_domain *domain = container_of(dom_fid, struct coll_domain,
						  util_domain.domain_fid);
	struct fid_domain *peer_domain = domain->peer_domain;

	if (!attr || attr->mode != 0)
		return -FI_EINVAL;

	switch (coll) {
	case FI_BARRIER:
	case FI_ALLGATHER:
	case FI_SCATTER:
	case FI_BROADCAST:
		ret = FI_SUCCESS;
		break;
	case FI_ALLREDUCE:
		if (FI_MIN <= attr->op && FI_BXOR >= attr->op)
			ret = fi_query_atomic(peer_domain, attr->datatype,
					      attr->op, &attr->datatype_attr,
					      flags);
		else
			return -FI_ENOSYS;
		break;
	case FI_ALLTOALL:
	case FI_REDUCE_SCATTER:
	case FI_REDUCE:
	case FI_GATHER:
	default:
		return -FI_ENOSYS;
	}

	if (ret)
		return ret;

	/*
	 * with the currently implemented software based collective operations
	 * the only restriction is the number of ranks we can address, as
	 * limited by the size of the rank portion of the collective tag, which
	 * is 31 bits.  Future collectives may impose further restrictions which
	 * will need to update the calculation.  For example, operations which
	 * require dedicated space in the recieve buffer for each rank would
	 * limit the number of members by buffer size and value type
	 * (8kB buffer / 64B value = 128 member max).
	 */
	attr->max_members = ~(0x80000000);

	return FI_SUCCESS;
}
