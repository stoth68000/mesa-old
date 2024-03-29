/*
 * Copyright 2013 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Marek Olšák
 */

#include "r600_cs.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include <inttypes.h>
#include <stdio.h>

bool r600_rings_is_buffer_referenced(struct r600_common_context *ctx,
				     struct pb_buffer *buf,
				     enum radeon_bo_usage usage)
{
	if (ctx->ws->cs_is_buffer_referenced(ctx->gfx.cs, buf, usage)) {
		return true;
	}
	if (radeon_emitted(ctx->dma.cs, 0) &&
	    ctx->ws->cs_is_buffer_referenced(ctx->dma.cs, buf, usage)) {
		return true;
	}
	return false;
}

void *r600_buffer_map_sync_with_rings(struct r600_common_context *ctx,
                                      struct r600_resource *resource,
                                      unsigned usage)
{
	enum radeon_bo_usage rusage = RADEON_USAGE_READWRITE;
	bool busy = false;

	if (usage & PIPE_TRANSFER_UNSYNCHRONIZED) {
		return ctx->ws->buffer_map(resource->buf, NULL, usage);
	}

	if (!(usage & PIPE_TRANSFER_WRITE)) {
		/* have to wait for the last write */
		rusage = RADEON_USAGE_WRITE;
	}

	if (radeon_emitted(ctx->gfx.cs, ctx->initial_gfx_cs_size) &&
	    ctx->ws->cs_is_buffer_referenced(ctx->gfx.cs,
					     resource->buf, rusage)) {
		if (usage & PIPE_TRANSFER_DONTBLOCK) {
			ctx->gfx.flush(ctx, RADEON_FLUSH_ASYNC, NULL);
			return NULL;
		} else {
			ctx->gfx.flush(ctx, 0, NULL);
			busy = true;
		}
	}
	if (radeon_emitted(ctx->dma.cs, 0) &&
	    ctx->ws->cs_is_buffer_referenced(ctx->dma.cs,
					     resource->buf, rusage)) {
		if (usage & PIPE_TRANSFER_DONTBLOCK) {
			ctx->dma.flush(ctx, RADEON_FLUSH_ASYNC, NULL);
			return NULL;
		} else {
			ctx->dma.flush(ctx, 0, NULL);
			busy = true;
		}
	}

	if (busy || !ctx->ws->buffer_wait(resource->buf, 0, rusage)) {
		if (usage & PIPE_TRANSFER_DONTBLOCK) {
			return NULL;
		} else {
			/* We will be wait for the GPU. Wait for any offloaded
			 * CS flush to complete to avoid busy-waiting in the winsys. */
			ctx->ws->cs_sync_flush(ctx->gfx.cs);
			if (ctx->dma.cs)
				ctx->ws->cs_sync_flush(ctx->dma.cs);
		}
	}

	/* Setting the CS to NULL will prevent doing checks we have done already. */
	return ctx->ws->buffer_map(resource->buf, NULL, usage);
}

void r600_init_resource_fields(struct r600_common_screen *rscreen,
			       struct r600_resource *res,
			       uint64_t size, unsigned alignment)
{
	struct r600_texture *rtex = (struct r600_texture*)res;

	res->bo_size = size;
	res->bo_alignment = alignment;
	res->flags = 0;

	switch (res->b.b.usage) {
	case PIPE_USAGE_STREAM:
		res->flags = RADEON_FLAG_GTT_WC;
		/* fall through */
	case PIPE_USAGE_STAGING:
		/* Transfers are likely to occur more often with these
		 * resources. */
		res->domains = RADEON_DOMAIN_GTT;
		break;
	case PIPE_USAGE_DYNAMIC:
		/* Older kernels didn't always flush the HDP cache before
		 * CS execution
		 */
		if (rscreen->info.drm_major == 2 &&
		    rscreen->info.drm_minor < 40) {
			res->domains = RADEON_DOMAIN_GTT;
			res->flags |= RADEON_FLAG_GTT_WC;
			break;
		}
		res->flags |= RADEON_FLAG_CPU_ACCESS;
		/* fall through */
	case PIPE_USAGE_DEFAULT:
	case PIPE_USAGE_IMMUTABLE:
	default:
		/* Not listing GTT here improves performance in some
		 * apps. */
		res->domains = RADEON_DOMAIN_VRAM;
		res->flags |= RADEON_FLAG_GTT_WC;
		break;
	}

	if (res->b.b.target == PIPE_BUFFER &&
	    res->b.b.flags & (PIPE_RESOURCE_FLAG_MAP_PERSISTENT |
			      PIPE_RESOURCE_FLAG_MAP_COHERENT)) {
		/* Use GTT for all persistent mappings with older
		 * kernels, because they didn't always flush the HDP
		 * cache before CS execution.
		 *
		 * Write-combined CPU mappings are fine, the kernel
		 * ensures all CPU writes finish before the GPU
		 * executes a command stream.
		 */
		if (rscreen->info.drm_major == 2 &&
		    rscreen->info.drm_minor < 40)
			res->domains = RADEON_DOMAIN_GTT;
		else if (res->domains & RADEON_DOMAIN_VRAM)
			res->flags |= RADEON_FLAG_CPU_ACCESS;
	}

	/* Tiled textures are unmappable. Always put them in VRAM. */
	if (res->b.b.target != PIPE_BUFFER &&
	    rtex->surface.level[0].mode >= RADEON_SURF_MODE_1D) {
		res->domains = RADEON_DOMAIN_VRAM;
		res->flags &= ~RADEON_FLAG_CPU_ACCESS;
		res->flags |= RADEON_FLAG_NO_CPU_ACCESS |
			 RADEON_FLAG_GTT_WC;
	}

	/* If VRAM is just stolen system memory, allow both VRAM and
	 * GTT, whichever has free space. If a buffer is evicted from
	 * VRAM to GTT, it will stay there.
	 */
	if (!rscreen->info.has_dedicated_vram &&
	    res->domains == RADEON_DOMAIN_VRAM)
		res->domains = RADEON_DOMAIN_VRAM_GTT;

	if (rscreen->debug_flags & DBG_NO_WC)
		res->flags &= ~RADEON_FLAG_GTT_WC;

	/* Set expected VRAM and GART usage for the buffer. */
	res->vram_usage = 0;
	res->gart_usage = 0;

	if (res->domains & RADEON_DOMAIN_VRAM)
		res->vram_usage = size;
	else if (res->domains & RADEON_DOMAIN_GTT)
		res->gart_usage = size;
}

bool r600_alloc_resource(struct r600_common_screen *rscreen,
			 struct r600_resource *res)
{
	struct pb_buffer *old_buf, *new_buf;

	/* Allocate a new resource. */
	new_buf = rscreen->ws->buffer_create(rscreen->ws, res->bo_size,
					     res->bo_alignment,
					     res->domains, res->flags);
	if (!new_buf) {
		return false;
	}

	/* Replace the pointer such that if res->buf wasn't NULL, it won't be
	 * NULL. This should prevent crashes with multiple contexts using
	 * the same buffer where one of the contexts invalidates it while
	 * the others are using it. */
	old_buf = res->buf;
	res->buf = new_buf; /* should be atomic */

	if (rscreen->info.has_virtual_memory)
		res->gpu_address = rscreen->ws->buffer_get_virtual_address(res->buf);
	else
		res->gpu_address = 0;

	pb_reference(&old_buf, NULL);

	util_range_set_empty(&res->valid_buffer_range);
	res->TC_L2_dirty = false;

	/* Print debug information. */
	if (rscreen->debug_flags & DBG_VM && res->b.b.target == PIPE_BUFFER) {
		fprintf(stderr, "VM start=0x%"PRIX64"  end=0x%"PRIX64" | Buffer %"PRIu64" bytes\n",
			res->gpu_address, res->gpu_address + res->buf->size,
			res->buf->size);
	}
	return true;
}

static void r600_buffer_destroy(struct pipe_screen *screen,
				struct pipe_resource *buf)
{
	struct r600_resource *rbuffer = r600_resource(buf);

	util_range_destroy(&rbuffer->valid_buffer_range);
	pb_reference(&rbuffer->buf, NULL);
	FREE(rbuffer);
}

static bool
r600_invalidate_buffer(struct r600_common_context *rctx,
		       struct r600_resource *rbuffer)
{
	/* Shared buffers can't be reallocated. */
	if (rbuffer->is_shared)
		return false;

	/* In AMD_pinned_memory, the user pointer association only gets
	 * broken when the buffer is explicitly re-allocated.
	 */
	if (rctx->ws->buffer_is_user_ptr(rbuffer->buf))
		return false;

	/* Check if mapping this buffer would cause waiting for the GPU. */
	if (r600_rings_is_buffer_referenced(rctx, rbuffer->buf, RADEON_USAGE_READWRITE) ||
	    !rctx->ws->buffer_wait(rbuffer->buf, 0, RADEON_USAGE_READWRITE)) {
		rctx->invalidate_buffer(&rctx->b, &rbuffer->b.b);
	} else {
		util_range_set_empty(&rbuffer->valid_buffer_range);
	}

	return true;
}

void r600_invalidate_resource(struct pipe_context *ctx,
			      struct pipe_resource *resource)
{
	struct r600_common_context *rctx = (struct r600_common_context*)ctx;
	struct r600_resource *rbuffer = r600_resource(resource);

	/* We currently only do anyting here for buffers */
	if (resource->target == PIPE_BUFFER)
		(void)r600_invalidate_buffer(rctx, rbuffer);
}

static void *r600_buffer_get_transfer(struct pipe_context *ctx,
				      struct pipe_resource *resource,
                                      unsigned level,
                                      unsigned usage,
                                      const struct pipe_box *box,
				      struct pipe_transfer **ptransfer,
				      void *data, struct r600_resource *staging,
				      unsigned offset)
{
	struct r600_common_context *rctx = (struct r600_common_context*)ctx;
	struct r600_transfer *transfer = slab_alloc_st(&rctx->pool_transfers);

	transfer->transfer.resource = resource;
	transfer->transfer.level = level;
	transfer->transfer.usage = usage;
	transfer->transfer.box = *box;
	transfer->transfer.stride = 0;
	transfer->transfer.layer_stride = 0;
	transfer->offset = offset;
	transfer->staging = staging;
	*ptransfer = &transfer->transfer;
	return data;
}

static bool r600_can_dma_copy_buffer(struct r600_common_context *rctx,
				     unsigned dstx, unsigned srcx, unsigned size)
{
	bool dword_aligned = !(dstx % 4) && !(srcx % 4) && !(size % 4);

	return rctx->screen->has_cp_dma ||
	       (dword_aligned && (rctx->dma.cs ||
				  rctx->screen->has_streamout));

}

static void *r600_buffer_transfer_map(struct pipe_context *ctx,
                                      struct pipe_resource *resource,
                                      unsigned level,
                                      unsigned usage,
                                      const struct pipe_box *box,
                                      struct pipe_transfer **ptransfer)
{
	struct r600_common_context *rctx = (struct r600_common_context*)ctx;
	struct r600_common_screen *rscreen = (struct r600_common_screen*)ctx->screen;
        struct r600_resource *rbuffer = r600_resource(resource);
        uint8_t *data;

	assert(box->x + box->width <= resource->width0);

	/* See if the buffer range being mapped has never been initialized,
	 * in which case it can be mapped unsynchronized. */
	if (!(usage & PIPE_TRANSFER_UNSYNCHRONIZED) &&
	    usage & PIPE_TRANSFER_WRITE &&
	    !rbuffer->is_shared &&
	    !util_ranges_intersect(&rbuffer->valid_buffer_range, box->x, box->x + box->width)) {
		usage |= PIPE_TRANSFER_UNSYNCHRONIZED;
	}

	/* If discarding the entire range, discard the whole resource instead. */
	if (usage & PIPE_TRANSFER_DISCARD_RANGE &&
	    box->x == 0 && box->width == resource->width0) {
		usage |= PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE;
	}

	if (usage & PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE &&
	    !(usage & PIPE_TRANSFER_UNSYNCHRONIZED)) {
		assert(usage & PIPE_TRANSFER_WRITE);

		if (r600_invalidate_buffer(rctx, rbuffer)) {
			/* At this point, the buffer is always idle. */
			usage |= PIPE_TRANSFER_UNSYNCHRONIZED;
		} else {
			/* Fall back to a temporary buffer. */
			usage |= PIPE_TRANSFER_DISCARD_RANGE;
		}
	}

	if ((usage & PIPE_TRANSFER_DISCARD_RANGE) &&
	    !(usage & (PIPE_TRANSFER_UNSYNCHRONIZED |
		       PIPE_TRANSFER_PERSISTENT)) &&
	    !(rscreen->debug_flags & DBG_NO_DISCARD_RANGE) &&
	    r600_can_dma_copy_buffer(rctx, box->x, 0, box->width)) {
		assert(usage & PIPE_TRANSFER_WRITE);

		/* Check if mapping this buffer would cause waiting for the GPU. */
		if (r600_rings_is_buffer_referenced(rctx, rbuffer->buf, RADEON_USAGE_READWRITE) ||
		    !rctx->ws->buffer_wait(rbuffer->buf, 0, RADEON_USAGE_READWRITE)) {
			/* Do a wait-free write-only transfer using a temporary buffer. */
			unsigned offset;
			struct r600_resource *staging = NULL;

			u_upload_alloc(rctx->uploader, 0, box->width + (box->x % R600_MAP_BUFFER_ALIGNMENT),
				       256, &offset, (struct pipe_resource**)&staging, (void**)&data);

			if (staging) {
				data += box->x % R600_MAP_BUFFER_ALIGNMENT;
				return r600_buffer_get_transfer(ctx, resource, level, usage, box,
								ptransfer, data, staging, offset);
			}
		} else {
			/* At this point, the buffer is always idle (we checked it above). */
			usage |= PIPE_TRANSFER_UNSYNCHRONIZED;
		}
	}
	/* Using a staging buffer in GTT for larger reads is much faster. */
	else if ((usage & PIPE_TRANSFER_READ) &&
		 !(usage & (PIPE_TRANSFER_WRITE |
			    PIPE_TRANSFER_PERSISTENT)) &&
		 rbuffer->domains & RADEON_DOMAIN_VRAM &&
		 r600_can_dma_copy_buffer(rctx, 0, box->x, box->width)) {
		struct r600_resource *staging;

		staging = (struct r600_resource*) pipe_buffer_create(
				ctx->screen, 0, PIPE_USAGE_STAGING,
				box->width + (box->x % R600_MAP_BUFFER_ALIGNMENT));
		if (staging) {
			/* Copy the VRAM buffer to the staging buffer. */
			ctx->resource_copy_region(ctx, &staging->b.b, 0,
						  box->x % R600_MAP_BUFFER_ALIGNMENT,
						  0, 0, resource, level, box);

			data = r600_buffer_map_sync_with_rings(rctx, staging, PIPE_TRANSFER_READ);
			if (!data) {
				r600_resource_reference(&staging, NULL);
				return NULL;
			}
			data += box->x % R600_MAP_BUFFER_ALIGNMENT;

			return r600_buffer_get_transfer(ctx, resource, level, usage, box,
							ptransfer, data, staging, 0);
		}
	}

	data = r600_buffer_map_sync_with_rings(rctx, rbuffer, usage);
	if (!data) {
		return NULL;
	}
	data += box->x;

	return r600_buffer_get_transfer(ctx, resource, level, usage, box,
					ptransfer, data, NULL, 0);
}

static void r600_buffer_do_flush_region(struct pipe_context *ctx,
					struct pipe_transfer *transfer,
				        const struct pipe_box *box)
{
	struct r600_transfer *rtransfer = (struct r600_transfer*)transfer;
	struct r600_resource *rbuffer = r600_resource(transfer->resource);

	if (rtransfer->staging) {
		struct pipe_resource *dst, *src;
		unsigned soffset;
		struct pipe_box dma_box;

		dst = transfer->resource;
		src = &rtransfer->staging->b.b;
		soffset = rtransfer->offset + box->x % R600_MAP_BUFFER_ALIGNMENT;

		u_box_1d(soffset, box->width, &dma_box);

		/* Copy the staging buffer into the original one. */
		ctx->resource_copy_region(ctx, dst, 0, box->x, 0, 0, src, 0, &dma_box);
	}

	util_range_add(&rbuffer->valid_buffer_range, box->x,
		       box->x + box->width);
}

static void r600_buffer_flush_region(struct pipe_context *ctx,
				     struct pipe_transfer *transfer,
				     const struct pipe_box *rel_box)
{
	if (transfer->usage & (PIPE_TRANSFER_WRITE |
			       PIPE_TRANSFER_FLUSH_EXPLICIT)) {
		struct pipe_box box;

		u_box_1d(transfer->box.x + rel_box->x, rel_box->width, &box);
		r600_buffer_do_flush_region(ctx, transfer, &box);
	}
}

static void r600_buffer_transfer_unmap(struct pipe_context *ctx,
				       struct pipe_transfer *transfer)
{
	struct r600_common_context *rctx = (struct r600_common_context*)ctx;
	struct r600_transfer *rtransfer = (struct r600_transfer*)transfer;

	if (transfer->usage & PIPE_TRANSFER_WRITE &&
	    !(transfer->usage & PIPE_TRANSFER_FLUSH_EXPLICIT))
		r600_buffer_do_flush_region(ctx, transfer, &transfer->box);

	if (rtransfer->staging)
		r600_resource_reference(&rtransfer->staging, NULL);

	slab_free_st(&rctx->pool_transfers, transfer);
}

void r600_buffer_subdata(struct pipe_context *ctx,
			 struct pipe_resource *buffer,
			 unsigned usage, unsigned offset,
			 unsigned size, const void *data)
{
	struct pipe_transfer *transfer = NULL;
	struct pipe_box box;
	uint8_t *map = NULL;

	u_box_1d(offset, size, &box);
	map = r600_buffer_transfer_map(ctx, buffer, 0,
				       PIPE_TRANSFER_WRITE |
				       PIPE_TRANSFER_DISCARD_RANGE |
				       usage,
				       &box, &transfer);
	if (!map)
		return;

	memcpy(map, data, size);
	r600_buffer_transfer_unmap(ctx, transfer);
}

static const struct u_resource_vtbl r600_buffer_vtbl =
{
	NULL,				/* get_handle */
	r600_buffer_destroy,		/* resource_destroy */
	r600_buffer_transfer_map,	/* transfer_map */
	r600_buffer_flush_region,	/* transfer_flush_region */
	r600_buffer_transfer_unmap,	/* transfer_unmap */
};

static struct r600_resource *
r600_alloc_buffer_struct(struct pipe_screen *screen,
			 const struct pipe_resource *templ)
{
	struct r600_resource *rbuffer;

	rbuffer = MALLOC_STRUCT(r600_resource);

	rbuffer->b.b = *templ;
	pipe_reference_init(&rbuffer->b.b.reference, 1);
	rbuffer->b.b.screen = screen;
	rbuffer->b.vtbl = &r600_buffer_vtbl;
	rbuffer->buf = NULL;
	rbuffer->TC_L2_dirty = false;
	rbuffer->is_shared = false;
	util_range_init(&rbuffer->valid_buffer_range);
	return rbuffer;
}

struct pipe_resource *r600_buffer_create(struct pipe_screen *screen,
					 const struct pipe_resource *templ,
					 unsigned alignment)
{
	struct r600_common_screen *rscreen = (struct r600_common_screen*)screen;
	struct r600_resource *rbuffer = r600_alloc_buffer_struct(screen, templ);

	r600_init_resource_fields(rscreen, rbuffer, templ->width0, alignment);

	if (!r600_alloc_resource(rscreen, rbuffer)) {
		FREE(rbuffer);
		return NULL;
	}
	return &rbuffer->b.b;
}

struct pipe_resource *r600_aligned_buffer_create(struct pipe_screen *screen,
						 unsigned bind,
						 unsigned usage,
						 unsigned size,
						 unsigned alignment)
{
	struct pipe_resource buffer;

	memset(&buffer, 0, sizeof buffer);
	buffer.target = PIPE_BUFFER;
	buffer.format = PIPE_FORMAT_R8_UNORM;
	buffer.bind = bind;
	buffer.usage = usage;
	buffer.flags = 0;
	buffer.width0 = size;
	buffer.height0 = 1;
	buffer.depth0 = 1;
	buffer.array_size = 1;
	return r600_buffer_create(screen, &buffer, alignment);
}

struct pipe_resource *
r600_buffer_from_user_memory(struct pipe_screen *screen,
			     const struct pipe_resource *templ,
			     void *user_memory)
{
	struct r600_common_screen *rscreen = (struct r600_common_screen*)screen;
	struct radeon_winsys *ws = rscreen->ws;
	struct r600_resource *rbuffer = r600_alloc_buffer_struct(screen, templ);

	rbuffer->domains = RADEON_DOMAIN_GTT;
	util_range_add(&rbuffer->valid_buffer_range, 0, templ->width0);

	/* Convert a user pointer to a buffer. */
	rbuffer->buf = ws->buffer_from_ptr(ws, user_memory, templ->width0);
	if (!rbuffer->buf) {
		FREE(rbuffer);
		return NULL;
	}

	if (rscreen->info.has_virtual_memory)
		rbuffer->gpu_address =
			ws->buffer_get_virtual_address(rbuffer->buf);
	else
		rbuffer->gpu_address = 0;

	return &rbuffer->b.b;
}
