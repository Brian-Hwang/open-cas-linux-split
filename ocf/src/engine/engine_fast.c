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

static int _ocf_read_fast_do(struct ocf_request *req)
 {
    //  uint32_t mod_value = (req->byte_position + req->byte_length) % 10; 
 
    //  / Cache miss handling /
     if (ocf_engine_is_miss(req)) {
         OCF_DEBUG_RQ(req, "Switching to read PT");
         ocf_read_pt_do(req);
         return 0;
     }
 
    // //  / Submit Backend /
    //  if (mod_value < 2) {
    //      OCF_DEBUG_RQ(req, "Submit Backend");
    //      OCF_DEBUG_RQ(req, "Handling request in BACKEND - 20%%");
    //      ocf_read_pt_do(req);
    //      return 0;
    //  }
 
    //  / Get OCF request - increase reference counter /
     ocf_req_get(req);
 
    //  / Handle partition reallocation if needed /
     if (ocf_engine_needs_repart(req)) {
         OCF_DEBUG_RQ(req, "Re-Part");
         ocf_hb_req_prot_lock_wr(req);
         ocf_user_part_move(req);
         ocf_hb_req_prot_unlock_wr(req);
     }
 
    //  / Submit Cache /
     OCF_DEBUG_RQ(req, "Submit Cache");
     OCF_DEBUG_RQ(req, "Handling request in CACHE - 80%%");

     env_atomic_set(&req->req_remaining, ocf_engine_io_count(req));
     ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0, req->byte_length, ocf_engine_io_count(req), _ocf_read_fast_complete);
 
    //  / Update statistics /
     ocf_engine_update_request_stats(req);
     ocf_engine_update_block_stats(req);
 
    //  / Put OCF request - decrease reference counter */
     ocf_req_put(req);
 
     return 0;
 }



// static void _ocf_read_fast_volume_complete(struct ocf_io *io, int error)
// {
// 	struct ocf_request *req = io->priv1;
	
// 	OCF_DEBUG_PARAM(req->cache, "Volume IO complete with status: %d", error);
// 	_ocf_read_fast_complete(req, error);
// 	ocf_io_put(io);
// }

// static uint64_t _ocf_io_bytes_per_line(struct ocf_request *req, uint64_t line_no)
// {
//     struct ocf_cache *cache = req->cache;
//     uint64_t line_size = ocf_line_size(cache);
//     uint64_t bytes = line_size;

//     /* First line might start mid-line */
//     if (line_no == req->core_line_first) {
//         uint64_t seek = req->byte_position % line_size;
//         bytes -= seek;
//     }

//     /* Last line might end mid-line */
//     if (line_no == req->core_line_last) {
//         uint64_t skip = (line_size - ((req->byte_position + req->byte_length) % line_size)) % line_size;
//         bytes -= skip;
//     }

//     return bytes;
// }

// static uint64_t _ocf_calculate_bytes(struct ocf_request *req, uint32_t start_io, uint32_t io_count)
// {
//     uint64_t total_bytes = 0;
//     uint32_t i;

//     for (i = 0; i < io_count; i++) {
//         uint64_t line_no = req->core_line_first + start_io + i;
//         if (line_no > req->core_line_last)
//             break;
//         total_bytes += _ocf_io_bytes_per_line(req, line_no);
//     }

//     return total_bytes;
// }

// static int _ocf_read_fast_do(struct ocf_request *req)
// {
// 	uint32_t cache_ios, volume_ios;
// 	struct ocf_io *volume_io;
// 	uint64_t volume_pos, volume_bytes, cache_bytes;
// 	uint32_t total_ios = ocf_engine_io_count(req);

// 	/* Cache miss handling */
// 	if (ocf_engine_is_miss(req)) {
// 		OCF_DEBUG_RQ(req, "Switching to read PT");
// 		ocf_read_pt_do(req);
// 		return 0;
// 	}

// 	/* Get OCF request - increase reference counter */
// 	ocf_req_get(req);

// 	/* Handle partition reallocation if needed */
// 	if (ocf_engine_needs_repart(req)) {
// 		OCF_DEBUG_RQ(req, "Re-Part");
// 		ocf_hb_req_prot_lock_wr(req);
// 		ocf_user_part_move(req);
// 		ocf_hb_req_prot_unlock_wr(req);
// 	}

// 	/* Split I/O count 80:20 */
// 	if (total_ios == 1) {
// 		cache_ios = 1;
// 		volume_ios = 0;
// 	} else {
// 		cache_ios = (total_ios * 8) / 10;  // 80% to cache
// 		volume_ios = total_ios - cache_ios; // 20% to volume
// 	}

// 	/* Calculate actual bytes for cache and volume portions */
// 	cache_bytes = _ocf_calculate_bytes(req, 0, cache_ios);
// 	volume_bytes = req->byte_length - cache_bytes;
// 	volume_pos = req->byte_position + cache_bytes;

// 	/* Validate split */
// 	if (cache_bytes + volume_bytes != req->byte_length) {
// 		OCF_DEBUG_RQ(req, "Invalid split - Cache: %llu, Volume: %llu, Total: %u",
// 			cache_bytes, volume_bytes, req->byte_length);
// 		req->error = -OCF_ERR_INVAL;
// 		_ocf_read_fast_complete(req, -OCF_ERR_INVAL);
// 		return 0;
// 	}

// 	/* Set remaining request counter */
// 	env_atomic_set(&req->req_remaining, total_ios);

// 	OCF_DEBUG_PARAM(req->cache, "Submit - Cache IOs: %u, Volume IOs: %u, Cache bytes: %llu, Volume bytes: %llu", 
// 		cache_ios, volume_ios, cache_bytes, volume_bytes);

// 	/* Submit cache request (80%) */
// 	if (cache_ios > 0) {
// 		OCF_DEBUG_RQ(req, "Submitting cache read - I/Os: %u, Bytes: %llu", cache_ios, cache_bytes);
// 		ocf_submit_cache_reqs(req->cache, req, OCF_READ, 0, cache_bytes, cache_ios, _ocf_read_fast_complete);
// 	}

// 	/* Submit volume request (20%) */
// 	if (volume_ios > 0) {
// 		OCF_DEBUG_RQ(req, "Submitting backend read - I/Os: %u, Bytes: %llu, Pos: %llu",
// 			volume_ios, volume_bytes, volume_pos);

// 		volume_io = ocf_volume_new_io(&req->core->volume, req->io_queue,
// 			volume_pos, volume_bytes, OCF_READ, 0, req->ioi.io.flags);

// 		if (!volume_io) {
// 			OCF_DEBUG_RQ(req, "Failed to allocate volume I/O");
// 			req->error = -OCF_ERR_NO_MEM;
// 			_ocf_read_fast_complete(req, -OCF_ERR_NO_MEM);
// 			return 0;
// 		}

// 		if (ocf_io_set_data(volume_io, req->data + cache_bytes, 0)) {
// 			OCF_DEBUG_RQ(req, "Failed to set volume I/O data");
// 			ocf_io_put(volume_io);
// 			req->error = -OCF_ERR_NO_MEM;
// 			_ocf_read_fast_complete(req, -OCF_ERR_NO_MEM);
// 			return 0;
// 		}

// 		ocf_io_set_cmpl(volume_io, req, NULL, _ocf_read_fast_volume_complete);
// 		ocf_volume_submit_io(volume_io);
// 	}

// 	/* Update statistics */
// 	ocf_engine_update_request_stats(req);
// 	ocf_engine_update_block_stats(req);

// 	/* Put OCF request - decrease reference counter */
// 	ocf_req_put(req);

// 	return 0;
// }

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
			// OCF_DEBUG_PARAM(req->cache, "Fast path success");

			if (lock != OCF_LOCK_ACQUIRED) {
				/* Lock was not acquired, need to wait for resume */
				OCF_DEBUG_PARAM(req->cache, "NO LOCK");
			} else {
				/* Lock was acquired can perform IO */
				ocf_write_wb_do(req);
			}
		} else {
			// OCF_DEBUG_PARAM(req->cache, "Fast path lock failure");
			req->complete(req, lock);
			ocf_req_put(req);
		}
	} else {
		// OCF_DEBUG_PARAM(req->cache, "Fast path failure");
	}

	/* Put OCF request - decrease reference counter */
	ocf_req_put(req);

	return (mapped && part_has_space) ?  OCF_FAST_PATH_YES : OCF_FAST_PATH_NO;
}
