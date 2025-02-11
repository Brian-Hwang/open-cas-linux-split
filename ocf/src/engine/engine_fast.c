/*
 * Copyright(c) 2012-2021 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "engine_fast.h"
#include "engine_common.h"
#include "engine_pt.h"
#include "engine_wb.h"
#include "../ocf_request.h"
#include "../utils/utils_user_part.h"
#include "../utils/utils_io.h"
#include "../concurrency/ocf_concurrency.h"
#include "../metadata/metadata.h"

#define OCF_ENGINE_DEBUG 1

#define OCF_ENGINE_DEBUG_IO_NAME "fast"
#include "engine_debug.h"

/*    _____                _   ______        _     _____      _   _
 *   |  __ \              | | |  ____|      | |   |  __ \    | | | |
 *   | |__) |___  __ _  __| | | |__ __ _ ___| |_  | |__) |_ _| |_| |__
 *   |  _  // _ \/ _` |/ _` | |  __/ _` / __| __| |  ___/ _` | __| '_ \
 *   | | \ \  __/ (_| | (_| | | | | (_| \__ \ |_  | |  | (_| | |_| | | |
 *   |_|  \_\___|\__,_|\__,_| |_|  \__,_|___/\__| |_|   \__,_|\__|_| |_|
 */

static void _ocf_read_fast_complete(struct ocf_request *req, int error)
{
	int remaining;

	if (error) {
		OCF_DEBUG_PARAM(req->cache, "IO completion error: %d", error);
		req->error |= error;
	}

	remaining = env_atomic_dec_return(&req->req_remaining);
	OCF_DEBUG_PARAM(req->cache, "IO completion - remaining IOs: %d", remaining);

	if (remaining) {
		/* Not all requests finished */
		return;
	}

	OCF_DEBUG_PARAM(req->cache, "HIT completion with final error status: %d", req->error);
	OCF_DEBUG_PARAM(req->cache, "FAST PATH COMPLETE");

	if (req->error) {
		OCF_DEBUG_PARAM(req->cache, "ERROR occurred, pushing to PT. Error code: %d", req->error);

		ocf_core_stats_cache_error_update(req->core, OCF_READ);
		ocf_engine_push_req_front_pt(req);
	} else {
		OCF_DEBUG_PARAM(req->cache, "Success - unlocking and completing request");
		ocf_req_unlock(ocf_cache_line_concurrency(req->cache), req);

		/* Complete request */
		req->complete(req, req->error);

		/* Free the request at the last point of the completion path */
		ocf_req_put(req);
	}
}

static void _ocf_read_fast_volume_complete(struct ocf_io *io, int error)
{
	struct ocf_request *req = io->priv1;
	
	OCF_DEBUG_PARAM(req->cache, "Volume IO complete with status: %d", error);
	_ocf_read_fast_complete(req, error);
	ocf_io_put(io);
}

static int _ocf_read_fast_do(struct ocf_request *req)
{
	uint32_t cache_ios, volume_ios;
	uint64_t cache_bytes, volume_bytes;
	struct ocf_io *volume_io;

	OCF_DEBUG_PARAM(req->cache, "FAST PATH DO - Request length: %u, IO count: %u",
		req->byte_length, ocf_engine_io_count(req));

	if (ocf_engine_is_miss(req)) {
		/* It seams that after resume, now request is MISS, do PT */
		OCF_DEBUG_PARAM(req->cache, "Switching to read PT - Request is MISS");
		ocf_read_pt_do(req);
		return 0;
	}

	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	if (ocf_engine_needs_repart(req)) {
		OCF_DEBUG_PARAM(req->cache, "Re-Part");

		ocf_hb_req_prot_lock_wr(req);

		/* Probably some cache lines are assigned into wrong
		 * partition. Need to move it to new one
		 */
		ocf_user_part_move(req);

		ocf_hb_req_prot_unlock_wr(req);
	}

	/* Calculate cache vs volume split */
	if (ocf_engine_io_count(req) == 1) {
		/* For single IO, ensure cache gets it */
		cache_ios = 1;
		volume_ios = 0;
		cache_bytes = req->byte_length;
		volume_bytes = 0;
		OCF_DEBUG_PARAM(req->cache, "Single IO - Assigning to cache only");
	} else {
		cache_ios = (ocf_engine_io_count(req) * 8 + 9) / 10;  // Round up for cache
		volume_ios = ocf_engine_io_count(req) - cache_ios;
		
		/* Ensure cache_bytes doesn't exceed request length */
		cache_bytes = OCF_MIN((req->byte_length * 8 + 9) / 10, req->byte_length);
		volume_bytes = req->byte_length - cache_bytes;
		
		OCF_DEBUG_PARAM(req->cache, "Multiple IOs - Split calculated. Request length: %u",
			req->byte_length);
	}

	/* Validate the calculations */
	if (cache_bytes + volume_bytes != req->byte_length) {
		OCF_DEBUG_PARAM(req->cache, "Invalid byte split: cache=%llu volume=%llu total=%u",
			cache_bytes, volume_bytes, req->byte_length);
		req->error = -OCF_ERR_INVAL;
		_ocf_read_fast_complete(req, -OCF_ERR_INVAL);
		return 0;
	}

	/* Set total remaining IO count */
	env_atomic_set(&req->req_remaining, ocf_engine_io_count(req));

	OCF_DEBUG_PARAM(req->cache, "Submit - Cache IOs: %u, Volume IOs: %u, Cache bytes: %llu, Volume bytes: %llu", 
		cache_ios, volume_ios, cache_bytes, volume_bytes);

	/* Submit cache requests (80%) */
	if (cache_ios > 0) {
		OCF_DEBUG_PARAM(req->cache, "Submitting cache request - bytes: %llu, ios: %u",
			cache_bytes, cache_ios);
		ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0,
			cache_bytes, cache_ios, _ocf_read_fast_complete);
	}

	/* Submit volume requests (20%) */
	if (volume_ios > 0) {
		uint64_t volume_pos = req->byte_position + cache_bytes;
		
		OCF_DEBUG_PARAM(req->cache, "Preparing volume request - bytes: %llu, position: %llu, original pos: %llu",
			volume_bytes, volume_pos, req->byte_position);

		/* Validate position and length */
		if (volume_pos + volume_bytes > req->byte_position + req->byte_length) {
			OCF_DEBUG_PARAM(req->cache, "Invalid volume position/length");
			req->error = -OCF_ERR_INVAL;
			_ocf_read_fast_complete(req, -OCF_ERR_INVAL);
			return 0;
		}

		/* Create new IO for volume portion */
		volume_io = ocf_volume_new_io(&req->core->volume, req->io_queue,
			volume_pos, volume_bytes, OCF_READ, 0, req->ioi.io.flags);

		if (!volume_io) {
			OCF_DEBUG_PARAM(req->cache, "Failed to allocate volume IO");
			req->error = -OCF_ERR_NO_MEM;
			_ocf_read_fast_complete(req, -OCF_ERR_NO_MEM);
			return 0;
		}

		/* Set data buffer offset for volume portion */
		if (ocf_io_set_data(volume_io, req->data, cache_bytes)) {
			OCF_DEBUG_PARAM(req->cache, "Failed to set volume IO data");
			ocf_io_put(volume_io);
			req->error = -OCF_ERR_NO_MEM;
			_ocf_read_fast_complete(req, -OCF_ERR_NO_MEM);
			return 0;
		}

		OCF_DEBUG_PARAM(req->cache, "Submitting volume request");
		ocf_io_set_cmpl(volume_io, req, NULL, _ocf_read_fast_volume_complete);
		ocf_volume_submit_io(volume_io);
	}

	/* Update statistics */
	ocf_engine_update_request_stats(req);
	ocf_engine_update_block_stats(req);

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return 0;
}

static const struct ocf_io_if _io_if_read_fast_resume = {
	.read = _ocf_read_fast_do,
	.write = _ocf_read_fast_do,
};

int ocf_read_fast(struct ocf_request *req)
{
	bool hit;
	int lock = OCF_LOCK_NOT_ACQUIRED;
	bool part_has_space;


	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	OCF_DEBUG_PARAM(req->cache, "Read fast");

	/* Set resume io_if */
	req->io_if = &_io_if_read_fast_resume;

	/*- Metadata RD access -----------------------------------------------*/

	ocf_req_hash(req);
	ocf_hb_req_prot_lock_rd(req);

	/* Traverse request to cache if there is hit */
	ocf_engine_traverse(req);

	hit = ocf_engine_is_hit(req);

	part_has_space = ocf_user_part_has_space(req);

	OCF_DEBUG_PARAM(req->cache, "Hit: %d, Part has space: %d", hit, part_has_space);
	if (hit && part_has_space) {
		ocf_io_start(&req->ioi.io);
		lock = ocf_req_async_lock_rd(
				ocf_cache_line_concurrency(req->cache),
				req, ocf_engine_on_resume);
	}

	ocf_hb_req_prot_unlock_rd(req);

	if (hit && part_has_space) {
		OCF_DEBUG_PARAM(req->cache, "Fast path success");
		OCF_DEBUG_PARAM(req->cache, "Fast path success");

		if (lock >= 0) {
			if (lock != OCF_LOCK_ACQUIRED) {
				/* Lock was not acquired, need to wait for resume */
				OCF_DEBUG_PARAM(req->cache, "NO LOCK");
			} else {
				/* Lock was acquired can perform IO */
				_ocf_read_fast_do(req);
			}
		} else {
			OCF_DEBUG_PARAM(req->cache, "LOCK ERROR");
			req->complete(req, lock);
			ocf_req_put(req);
		}
	} else {
		OCF_DEBUG_PARAM(req->cache, "Fast path failure");
	}

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return (hit && part_has_space) ? OCF_FAST_PATH_YES : OCF_FAST_PATH_NO;
}

/*  __          __   _ _         ______        _     _____      _   _
 *  \ \        / /  (_) |       |  ____|      | |   |  __ \    | | | |
 *   \ \  /\  / / __ _| |_ ___  | |__ __ _ ___| |_  | |__) |_ _| |_| |__
 *    \ \/  \/ / '__| | __/ _ \ |  __/ _` / __| __| |  ___/ _` | __| '_ \
 *     \  /\  /| |  | | ||  __/ | | | (_| \__ \ |_  | |  | (_| | |_| | | |
 *      \/  \/ |_|  |_|\__\___| |_|  \__,_|___/\__| |_|   \__,_|\__|_| |_|
 */

static const struct ocf_io_if _io_if_write_fast_resume = {
	.read = ocf_write_wb_do,
	.write = ocf_write_wb_do,
};

int ocf_write_fast(struct ocf_request *req)
{
	bool mapped;
	int lock = OCF_LOCK_NOT_ACQUIRED;
	int part_has_space = false;

	/* Get OCF request - increase reference counter */
	ocf_req_get(req);

	/* Set resume io_if */
	req->io_if = &_io_if_write_fast_resume;

	/*- Metadata RD access -----------------------------------------------*/

	ocf_req_hash(req);
	ocf_hb_req_prot_lock_rd(req);

	/* Traverse request to cache if there is hit */
	ocf_engine_traverse(req);

	mapped = ocf_engine_is_mapped(req);

	part_has_space = ocf_user_part_has_space(req);

	if (mapped && part_has_space) {
		ocf_io_start(&req->ioi.io);
		lock = ocf_req_async_lock_wr(
				ocf_cache_line_concurrency(req->cache),
				req, ocf_engine_on_resume);
	}

	ocf_hb_req_prot_unlock_rd(req);

	if (mapped && part_has_space) {
		if (lock >= 0) {
			OCF_DEBUG_PARAM(req->cache, "Fast path success");

			if (lock != OCF_LOCK_ACQUIRED) {
				/* Lock was not acquired, need to wait for resume */
				OCF_DEBUG_PARAM(req->cache, "NO LOCK");
			} else {
				/* Lock was acquired can perform IO */
				ocf_write_wb_do(req);
			}
		} else {
			OCF_DEBUG_PARAM(req->cache, "Fast path lock failure");
			req->complete(req, lock);
			ocf_req_put(req);
		}
	} else {
		OCF_DEBUG_PARAM(req->cache, "Fast path failure");
	}

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return (mapped && part_has_space) ?  OCF_FAST_PATH_YES : OCF_FAST_PATH_NO;
}
