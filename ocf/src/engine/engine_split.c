#include "ocf/ocf.h"
#include "../ocf_cache_priv.h"
#include "engine_split.h"
#include "engine_common.h"
#include "engine_pt.h"
#include "../utils/utils_io.h"
#include "../metadata/metadata.h"

#define OCF_ENGINE_DEBUG_IO_NAME "split"
#include "engine_debug.h"

struct split_req_context {
    struct ocf_request *orig_req;
    env_atomic req_remaining;
};

static void _ocf_read_split_complete(struct ocf_request *req, int error)
{
    struct split_req_context *ctx = req->priv;
    struct ocf_request *orig_req = ctx->orig_req;

    if (error)
        orig_req->error |= error;

    if (env_atomic_dec_return(&ctx->req_remaining))
        return;

    OCF_DEBUG_PARAM(orig_req->cache, "All split requests completed");
    orig_req->complete(orig_req, orig_req->error);
    env_vfree(ctx);
    ocf_req_put(orig_req);
}

static struct ocf_request *_ocf_request_clone(struct ocf_request *orig,
        uint64_t addr, uint32_t bytes)
{
    struct ocf_request *req;
    int i;
    // Use the same queue as the original request
    req = ocf_req_new(orig->io_queue, orig->core,
            addr, bytes, OCF_READ);
    if (!req)
        return NULL;

    /* Copy essential fields */
    req->d2c = orig->d2c;
    req->io_queue = orig->io_queue;
    req->data = orig->data;
    req->complete = _ocf_read_split_complete;

    /* Copy mapping info for overlapping cache lines */
    for (i = 0; i < req->core_line_count; i++) {
        uint64_t core_line = req->core_line_first + i;
        if (core_line >= orig->core_line_first && 
            core_line <= orig->core_line_last) {
            int orig_idx = core_line - orig->core_line_first;
            req->map[i] = orig->map[orig_idx];
        }
    }

    return req;
}

static uint64_t _ocf_io_bytes_per_line(struct ocf_request *req, uint64_t line_no)
{
    struct ocf_cache *cache = req->cache;
    uint64_t line_size = ocf_line_size(cache);
    uint64_t bytes = line_size;

    /* First line might start mid-line */
    if (line_no == req->core_line_first) {
        uint64_t seek = req->byte_position % line_size;
        bytes -= seek;
    }

    /* Last line might end mid-line */
    if (line_no == req->core_line_last) {
        uint64_t skip = (line_size - ((req->byte_position + req->byte_length) % line_size)) % line_size;
        bytes -= skip;
    }

    return bytes;
}

static uint64_t _ocf_calculate_bytes(struct ocf_request *req, uint32_t start_io, uint32_t io_count)
{
    uint64_t total_bytes = 0;
    uint32_t i;

    for (i = 0; i < io_count; i++) {
        uint64_t line_no = req->core_line_first + start_io + i;
        if (line_no > req->core_line_last)
            break;
        total_bytes += _ocf_io_bytes_per_line(req, line_no);
    }

    return total_bytes;
}

int ocf_read_fast_split_do(struct ocf_request *req)
{
    struct split_req_context *ctx;
    struct ocf_request *cache_req = NULL, *volume_req = NULL;
    uint32_t cache_ios, volume_ios;
    uint64_t cache_bytes;

    if (ocf_engine_is_miss(req)) {
        ocf_read_pt_do(req);
        return 0;
    }

    ocf_req_get(req);

    ctx = env_vmalloc(sizeof(*ctx));
    if (!ctx) {
        req->complete(req, -OCF_ERR_NO_MEM);
        ocf_req_put(req);
        return 0;
    }

    ctx->orig_req = req;

    /* Calculate split */
    if (ocf_engine_io_count(req) == 1) {
        cache_ios = 1;
        volume_ios = 0;
    } else {
        cache_ios = (ocf_engine_io_count(req) * 8) / 10;
        volume_ios = ocf_engine_io_count(req) - cache_ios;
    }

    /* Calculate actual bytes for cache portion */
    cache_bytes = _ocf_calculate_bytes(req, 0, cache_ios);
    env_atomic_set(&ctx->req_remaining, 0);

    /* Create and submit cache request */
    if (cache_ios > 0) {
        cache_req = _ocf_request_clone(req, 0, cache_bytes);
        if (!cache_req) {
            req->complete(req, -OCF_ERR_NO_MEM);
            goto err;
        }
        cache_req->priv = ctx;
        env_atomic_inc(&ctx->req_remaining);
        
        OCF_DEBUG_PARAM(req->cache, "Submitting cache request - ios: %u", cache_ios);
        ocf_engine_push_req_front(cache_req, true);
    }

    /* Create and submit volume request */
    if (volume_ios > 0) {
        uint64_t volume_bytes = req->byte_length - cache_bytes;
        volume_req = _ocf_request_clone(req, cache_bytes, volume_bytes);
        if (!volume_req) {
            req->complete(req, -OCF_ERR_NO_MEM);
            goto err;
        }
        volume_req->priv = ctx;
        env_atomic_inc(&ctx->req_remaining);

        OCF_DEBUG_PARAM(req->cache, "Submitting volume request - ios: %u", volume_ios);
        ocf_submit_volume_req(&req->core->volume, volume_req, 
                _ocf_read_split_complete);
    }

    return 0;

err:
    if (cache_req)
        ocf_req_put(cache_req);
    if (volume_req)
        ocf_req_put(volume_req);
    env_vfree(ctx);
    ocf_req_put(req);
    return 0;
} 