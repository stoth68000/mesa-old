/*
 * Copyright 2010 Red Hat Inc.
 *           2010 Jerome Glisse
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
 * Authors: Dave Airlie <airlied@redhat.com>
 *          Jerome Glisse <jglisse@redhat.com>
 */
#include "r600_formats.h"
#include "r600_shader.h"
#include "r600d.h"

#include "util/u_format_s3tc.h"
#include "util/u_index_modify.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "util/u_math.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_scan.h"
#include "tgsi/tgsi_ureg.h"

void r600_init_command_buffer(struct r600_command_buffer *cb, unsigned num_dw)
{
	assert(!cb->buf);
	cb->buf = CALLOC(1, 4 * num_dw);
	cb->max_num_dw = num_dw;
}

void r600_release_command_buffer(struct r600_command_buffer *cb)
{
	FREE(cb->buf);
}

void r600_add_atom(struct r600_context *rctx,
		   struct r600_atom *atom,
		   unsigned id)
{
	assert(id < R600_NUM_ATOMS);
	assert(rctx->atoms[id] == NULL);
	rctx->atoms[id] = atom;
	atom->id = id;
}

void r600_init_atom(struct r600_context *rctx,
		    struct r600_atom *atom,
		    unsigned id,
		    void (*emit)(struct r600_context *ctx, struct r600_atom *state),
		    unsigned num_dw)
{
	atom->emit = (void*)emit;
	atom->num_dw = num_dw;
	r600_add_atom(rctx, atom, id);
}

void r600_emit_cso_state(struct r600_context *rctx, struct r600_atom *atom)
{
	r600_emit_command_buffer(rctx->b.gfx.cs, ((struct r600_cso_state*)atom)->cb);
}

void r600_emit_alphatest_state(struct r600_context *rctx, struct r600_atom *atom)
{
	struct radeon_winsys_cs *cs = rctx->b.gfx.cs;
	struct r600_alphatest_state *a = (struct r600_alphatest_state*)atom;
	unsigned alpha_ref = a->sx_alpha_ref;

	if (rctx->b.chip_class >= EVERGREEN && a->cb0_export_16bpc) {
		alpha_ref &= ~0x1FFF;
	}

	radeon_set_context_reg(cs, R_028410_SX_ALPHA_TEST_CONTROL,
			       a->sx_alpha_test_control |
			       S_028410_ALPHA_TEST_BYPASS(a->bypass));
	radeon_set_context_reg(cs, R_028438_SX_ALPHA_REF, alpha_ref);
}

static void r600_texture_barrier(struct pipe_context *ctx)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	rctx->b.flags |= R600_CONTEXT_INV_TEX_CACHE |
		       R600_CONTEXT_FLUSH_AND_INV_CB |
		       R600_CONTEXT_FLUSH_AND_INV |
		       R600_CONTEXT_WAIT_3D_IDLE;
}

static unsigned r600_conv_pipe_prim(unsigned prim)
{
	static const unsigned prim_conv[] = {
		[PIPE_PRIM_POINTS]			= V_008958_DI_PT_POINTLIST,
		[PIPE_PRIM_LINES]			= V_008958_DI_PT_LINELIST,
		[PIPE_PRIM_LINE_LOOP]			= V_008958_DI_PT_LINELOOP,
		[PIPE_PRIM_LINE_STRIP]			= V_008958_DI_PT_LINESTRIP,
		[PIPE_PRIM_TRIANGLES]			= V_008958_DI_PT_TRILIST,
		[PIPE_PRIM_TRIANGLE_STRIP]		= V_008958_DI_PT_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_FAN]		= V_008958_DI_PT_TRIFAN,
		[PIPE_PRIM_QUADS]			= V_008958_DI_PT_QUADLIST,
		[PIPE_PRIM_QUAD_STRIP]			= V_008958_DI_PT_QUADSTRIP,
		[PIPE_PRIM_POLYGON]			= V_008958_DI_PT_POLYGON,
		[PIPE_PRIM_LINES_ADJACENCY]		= V_008958_DI_PT_LINELIST_ADJ,
		[PIPE_PRIM_LINE_STRIP_ADJACENCY]	= V_008958_DI_PT_LINESTRIP_ADJ,
		[PIPE_PRIM_TRIANGLES_ADJACENCY]		= V_008958_DI_PT_TRILIST_ADJ,
		[PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY]	= V_008958_DI_PT_TRISTRIP_ADJ,
		[PIPE_PRIM_PATCHES]                     = V_008958_DI_PT_PATCH,
		[R600_PRIM_RECTANGLE_LIST]		= V_008958_DI_PT_RECTLIST
	};
	assert(prim < ARRAY_SIZE(prim_conv));
	return prim_conv[prim];
}

unsigned r600_conv_prim_to_gs_out(unsigned mode)
{
	static const int prim_conv[] = {
		[PIPE_PRIM_POINTS]			= V_028A6C_OUTPRIM_TYPE_POINTLIST,
		[PIPE_PRIM_LINES]			= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_LINE_LOOP]			= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_LINE_STRIP]			= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_TRIANGLES]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_STRIP]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_FAN]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_QUADS]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_QUAD_STRIP]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_POLYGON]			= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_LINES_ADJACENCY]		= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_LINE_STRIP_ADJACENCY]	= V_028A6C_OUTPRIM_TYPE_LINESTRIP,
		[PIPE_PRIM_TRIANGLES_ADJACENCY]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY]	= V_028A6C_OUTPRIM_TYPE_TRISTRIP,
		[PIPE_PRIM_PATCHES]			= V_028A6C_OUTPRIM_TYPE_POINTLIST,
		[R600_PRIM_RECTANGLE_LIST]		= V_028A6C_OUTPRIM_TYPE_TRISTRIP
	};
	assert(mode < ARRAY_SIZE(prim_conv));

	return prim_conv[mode];
}

/* common state between evergreen and r600 */

static void r600_bind_blend_state_internal(struct r600_context *rctx,
		struct r600_blend_state *blend, bool blend_disable)
{
	unsigned color_control;
	bool update_cb = false;

	rctx->alpha_to_one = blend->alpha_to_one;
	rctx->dual_src_blend = blend->dual_src_blend;

	if (!blend_disable) {
		r600_set_cso_state_with_cb(rctx, &rctx->blend_state, blend, &blend->buffer);
		color_control = blend->cb_color_control;
	} else {
		/* Blending is disabled. */
		r600_set_cso_state_with_cb(rctx, &rctx->blend_state, blend, &blend->buffer_no_blend);
		color_control = blend->cb_color_control_no_blend;
	}

	/* Update derived states. */
	if (rctx->cb_misc_state.blend_colormask != blend->cb_target_mask) {
		rctx->cb_misc_state.blend_colormask = blend->cb_target_mask;
		update_cb = true;
	}
	if (rctx->b.chip_class <= R700 &&
	    rctx->cb_misc_state.cb_color_control != color_control) {
		rctx->cb_misc_state.cb_color_control = color_control;
		update_cb = true;
	}
	if (rctx->cb_misc_state.dual_src_blend != blend->dual_src_blend) {
		rctx->cb_misc_state.dual_src_blend = blend->dual_src_blend;
		update_cb = true;
	}
	if (update_cb) {
		r600_mark_atom_dirty(rctx, &rctx->cb_misc_state.atom);
	}
}

static void r600_bind_blend_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_blend_state *blend = (struct r600_blend_state *)state;

	if (!blend) {
		r600_set_cso_state_with_cb(rctx, &rctx->blend_state, NULL, NULL);
		return;
	}

	r600_bind_blend_state_internal(rctx, blend, rctx->force_blend_disable);
}

static void r600_set_blend_color(struct pipe_context *ctx,
				 const struct pipe_blend_color *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	rctx->blend_color.state = *state;
	r600_mark_atom_dirty(rctx, &rctx->blend_color.atom);
}

void r600_emit_blend_color(struct r600_context *rctx, struct r600_atom *atom)
{
	struct radeon_winsys_cs *cs = rctx->b.gfx.cs;
	struct pipe_blend_color *state = &rctx->blend_color.state;

	radeon_set_context_reg_seq(cs, R_028414_CB_BLEND_RED, 4);
	radeon_emit(cs, fui(state->color[0])); /* R_028414_CB_BLEND_RED */
	radeon_emit(cs, fui(state->color[1])); /* R_028418_CB_BLEND_GREEN */
	radeon_emit(cs, fui(state->color[2])); /* R_02841C_CB_BLEND_BLUE */
	radeon_emit(cs, fui(state->color[3])); /* R_028420_CB_BLEND_ALPHA */
}

void r600_emit_vgt_state(struct r600_context *rctx, struct r600_atom *atom)
{
	struct radeon_winsys_cs *cs = rctx->b.gfx.cs;
	struct r600_vgt_state *a = (struct r600_vgt_state *)atom;

	radeon_set_context_reg(cs, R_028A94_VGT_MULTI_PRIM_IB_RESET_EN, a->vgt_multi_prim_ib_reset_en);
	radeon_set_context_reg_seq(cs, R_028408_VGT_INDX_OFFSET, 2);
	radeon_emit(cs, a->vgt_indx_offset); /* R_028408_VGT_INDX_OFFSET */
	radeon_emit(cs, a->vgt_multi_prim_ib_reset_indx); /* R_02840C_VGT_MULTI_PRIM_IB_RESET_INDX */
	if (a->last_draw_was_indirect) {
		a->last_draw_was_indirect = false;
		radeon_set_ctl_const(cs, R_03CFF0_SQ_VTX_BASE_VTX_LOC, 0);
	}
}

static void r600_set_clip_state(struct pipe_context *ctx,
				const struct pipe_clip_state *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	rctx->clip_state.state = *state;
	r600_mark_atom_dirty(rctx, &rctx->clip_state.atom);
	rctx->driver_consts[PIPE_SHADER_VERTEX].vs_ucp_dirty = true;
}

static void r600_set_stencil_ref(struct pipe_context *ctx,
				 const struct r600_stencil_ref *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	rctx->stencil_ref.state = *state;
	r600_mark_atom_dirty(rctx, &rctx->stencil_ref.atom);
}

void r600_emit_stencil_ref(struct r600_context *rctx, struct r600_atom *atom)
{
	struct radeon_winsys_cs *cs = rctx->b.gfx.cs;
	struct r600_stencil_ref_state *a = (struct r600_stencil_ref_state*)atom;

	radeon_set_context_reg_seq(cs, R_028430_DB_STENCILREFMASK, 2);
	radeon_emit(cs, /* R_028430_DB_STENCILREFMASK */
			 S_028430_STENCILREF(a->state.ref_value[0]) |
			 S_028430_STENCILMASK(a->state.valuemask[0]) |
			 S_028430_STENCILWRITEMASK(a->state.writemask[0]));
	radeon_emit(cs, /* R_028434_DB_STENCILREFMASK_BF */
			 S_028434_STENCILREF_BF(a->state.ref_value[1]) |
			 S_028434_STENCILMASK_BF(a->state.valuemask[1]) |
			 S_028434_STENCILWRITEMASK_BF(a->state.writemask[1]));
}

static void r600_set_pipe_stencil_ref(struct pipe_context *ctx,
				      const struct pipe_stencil_ref *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_dsa_state *dsa = (struct r600_dsa_state*)rctx->dsa_state.cso;
	struct r600_stencil_ref ref;

	rctx->stencil_ref.pipe_state = *state;

	if (!dsa)
		return;

	ref.ref_value[0] = state->ref_value[0];
	ref.ref_value[1] = state->ref_value[1];
	ref.valuemask[0] = dsa->valuemask[0];
	ref.valuemask[1] = dsa->valuemask[1];
	ref.writemask[0] = dsa->writemask[0];
	ref.writemask[1] = dsa->writemask[1];

	r600_set_stencil_ref(ctx, &ref);
}

static void r600_bind_dsa_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_dsa_state *dsa = state;
	struct r600_stencil_ref ref;

	if (!state) {
		r600_set_cso_state_with_cb(rctx, &rctx->dsa_state, NULL, NULL);
		return;
	}

	r600_set_cso_state_with_cb(rctx, &rctx->dsa_state, dsa, &dsa->buffer);

	ref.ref_value[0] = rctx->stencil_ref.pipe_state.ref_value[0];
	ref.ref_value[1] = rctx->stencil_ref.pipe_state.ref_value[1];
	ref.valuemask[0] = dsa->valuemask[0];
	ref.valuemask[1] = dsa->valuemask[1];
	ref.writemask[0] = dsa->writemask[0];
	ref.writemask[1] = dsa->writemask[1];
	if (rctx->zwritemask != dsa->zwritemask) {
		rctx->zwritemask = dsa->zwritemask;
		if (rctx->b.chip_class >= EVERGREEN) {
			/* work around some issue when not writing to zbuffer
			 * we are having lockup on evergreen so do not enable
			 * hyperz when not writing zbuffer
			 */
			r600_mark_atom_dirty(rctx, &rctx->db_misc_state.atom);
		}
	}

	r600_set_stencil_ref(ctx, &ref);

	/* Update alphatest state. */
	if (rctx->alphatest_state.sx_alpha_test_control != dsa->sx_alpha_test_control ||
	    rctx->alphatest_state.sx_alpha_ref != dsa->alpha_ref) {
		rctx->alphatest_state.sx_alpha_test_control = dsa->sx_alpha_test_control;
		rctx->alphatest_state.sx_alpha_ref = dsa->alpha_ref;
		r600_mark_atom_dirty(rctx, &rctx->alphatest_state.atom);
	}
}

static void r600_bind_rs_state(struct pipe_context *ctx, void *state)
{
	struct r600_rasterizer_state *rs = (struct r600_rasterizer_state *)state;
	struct r600_context *rctx = (struct r600_context *)ctx;

	if (!state)
		return;

	rctx->rasterizer = rs;

	r600_set_cso_state_with_cb(rctx, &rctx->rasterizer_state, rs, &rs->buffer);

	if (rs->offset_enable &&
	    (rs->offset_units != rctx->poly_offset_state.offset_units ||
	     rs->offset_scale != rctx->poly_offset_state.offset_scale ||
	     rs->offset_units_unscaled != rctx->poly_offset_state.offset_units_unscaled)) {
		rctx->poly_offset_state.offset_units = rs->offset_units;
		rctx->poly_offset_state.offset_scale = rs->offset_scale;
		rctx->poly_offset_state.offset_units_unscaled = rs->offset_units_unscaled;
		r600_mark_atom_dirty(rctx, &rctx->poly_offset_state.atom);
	}

	/* Update clip_misc_state. */
	if (rctx->clip_misc_state.pa_cl_clip_cntl != rs->pa_cl_clip_cntl ||
	    rctx->clip_misc_state.clip_plane_enable != rs->clip_plane_enable) {
		rctx->clip_misc_state.pa_cl_clip_cntl = rs->pa_cl_clip_cntl;
		rctx->clip_misc_state.clip_plane_enable = rs->clip_plane_enable;
		r600_mark_atom_dirty(rctx, &rctx->clip_misc_state.atom);
	}

	r600_viewport_set_rast_deps(&rctx->b, rs->scissor_enable, rs->clip_halfz);

	/* Re-emit PA_SC_LINE_STIPPLE. */
	rctx->last_primitive_type = -1;
}

static void r600_delete_rs_state(struct pipe_context *ctx, void *state)
{
	struct r600_rasterizer_state *rs = (struct r600_rasterizer_state *)state;

	r600_release_command_buffer(&rs->buffer);
	FREE(rs);
}

static void r600_sampler_view_destroy(struct pipe_context *ctx,
				      struct pipe_sampler_view *state)
{
	struct r600_pipe_sampler_view *view = (struct r600_pipe_sampler_view *)state;

	if (view->tex_resource->gpu_address &&
	    view->tex_resource->b.b.target == PIPE_BUFFER)
		LIST_DELINIT(&view->list);

	pipe_resource_reference(&state->texture, NULL);
	FREE(view);
}

void r600_sampler_states_dirty(struct r600_context *rctx,
			       struct r600_sampler_states *state)
{
	if (state->dirty_mask) {
		if (state->dirty_mask & state->has_bordercolor_mask) {
			rctx->b.flags |= R600_CONTEXT_WAIT_3D_IDLE;
		}
		state->atom.num_dw =
			util_bitcount(state->dirty_mask & state->has_bordercolor_mask) * 11 +
			util_bitcount(state->dirty_mask & ~state->has_bordercolor_mask) * 5;
		r600_mark_atom_dirty(rctx, &state->atom);
	}
}

static void r600_bind_sampler_states(struct pipe_context *pipe,
			       enum pipe_shader_type shader,
			       unsigned start,
			       unsigned count, void **states)
{
	struct r600_context *rctx = (struct r600_context *)pipe;
	struct r600_textures_info *dst = &rctx->samplers[shader];
	struct r600_pipe_sampler_state **rstates = (struct r600_pipe_sampler_state**)states;
	int seamless_cube_map = -1;
	unsigned i;
	/* This sets 1-bit for states with index >= count. */
	uint32_t disable_mask = ~((1ull << count) - 1);
	/* These are the new states set by this function. */
	uint32_t new_mask = 0;

	assert(start == 0); /* XXX fix below */

	if (!states) {
		disable_mask = ~0u;
		count = 0;
	}

	for (i = 0; i < count; i++) {
		struct r600_pipe_sampler_state *rstate = rstates[i];

		if (rstate == dst->states.states[i]) {
			continue;
		}

		if (rstate) {
			if (rstate->border_color_use) {
				dst->states.has_bordercolor_mask |= 1 << i;
			} else {
				dst->states.has_bordercolor_mask &= ~(1 << i);
			}
			seamless_cube_map = rstate->seamless_cube_map;

			new_mask |= 1 << i;
		} else {
			disable_mask |= 1 << i;
		}
	}

	memcpy(dst->states.states, rstates, sizeof(void*) * count);
	memset(dst->states.states + count, 0, sizeof(void*) * (NUM_TEX_UNITS - count));

	dst->states.enabled_mask &= ~disable_mask;
	dst->states.dirty_mask &= dst->states.enabled_mask;
	dst->states.enabled_mask |= new_mask;
	dst->states.dirty_mask |= new_mask;
	dst->states.has_bordercolor_mask &= dst->states.enabled_mask;

	r600_sampler_states_dirty(rctx, &dst->states);

	/* Seamless cubemap state. */
	if (rctx->b.chip_class <= R700 &&
	    seamless_cube_map != -1 &&
	    seamless_cube_map != rctx->seamless_cube_map.enabled) {
		/* change in TA_CNTL_AUX need a pipeline flush */
		rctx->b.flags |= R600_CONTEXT_WAIT_3D_IDLE;
		rctx->seamless_cube_map.enabled = seamless_cube_map;
		r600_mark_atom_dirty(rctx, &rctx->seamless_cube_map.atom);
	}
}

static void r600_delete_sampler_state(struct pipe_context *ctx, void *state)
{
	free(state);
}

static void r600_delete_blend_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_blend_state *blend = (struct r600_blend_state*)state;

	if (rctx->blend_state.cso == state) {
		ctx->bind_blend_state(ctx, NULL);
	}

	r600_release_command_buffer(&blend->buffer);
	r600_release_command_buffer(&blend->buffer_no_blend);
	FREE(blend);
}

static void r600_delete_dsa_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_dsa_state *dsa = (struct r600_dsa_state *)state;

	if (rctx->dsa_state.cso == state) {
		ctx->bind_depth_stencil_alpha_state(ctx, NULL);
	}

	r600_release_command_buffer(&dsa->buffer);
	free(dsa);
}

static void r600_bind_vertex_elements(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	r600_set_cso_state(rctx, &rctx->vertex_fetch_shader, state);
}

static void r600_delete_vertex_elements(struct pipe_context *ctx, void *state)
{
	struct r600_fetch_shader *shader = (struct r600_fetch_shader*)state;
	r600_resource_reference(&shader->buffer, NULL);
	FREE(shader);
}

static void r600_set_index_buffer(struct pipe_context *ctx,
			   const struct pipe_index_buffer *ib)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	if (ib) {
		pipe_resource_reference(&rctx->index_buffer.buffer, ib->buffer);
		memcpy(&rctx->index_buffer, ib, sizeof(*ib));
		r600_context_add_resource_size(ctx, ib->buffer);
	} else {
		pipe_resource_reference(&rctx->index_buffer.buffer, NULL);
	}
}

void r600_vertex_buffers_dirty(struct r600_context *rctx)
{
	if (rctx->vertex_buffer_state.dirty_mask) {
		rctx->vertex_buffer_state.atom.num_dw = (rctx->b.chip_class >= EVERGREEN ? 12 : 11) *
					       util_bitcount(rctx->vertex_buffer_state.dirty_mask);
		r600_mark_atom_dirty(rctx, &rctx->vertex_buffer_state.atom);
	}
}

static void r600_set_vertex_buffers(struct pipe_context *ctx,
				    unsigned start_slot, unsigned count,
				    const struct pipe_vertex_buffer *input)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_vertexbuf_state *state = &rctx->vertex_buffer_state;
	struct pipe_vertex_buffer *vb = state->vb + start_slot;
	unsigned i;
	uint32_t disable_mask = 0;
	/* These are the new buffers set by this function. */
	uint32_t new_buffer_mask = 0;

	/* Set vertex buffers. */
	if (input) {
		for (i = 0; i < count; i++) {
			if (memcmp(&input[i], &vb[i], sizeof(struct pipe_vertex_buffer))) {
				if (input[i].buffer) {
					vb[i].stride = input[i].stride;
					vb[i].buffer_offset = input[i].buffer_offset;
					pipe_resource_reference(&vb[i].buffer, input[i].buffer);
					new_buffer_mask |= 1 << i;
					r600_context_add_resource_size(ctx, input[i].buffer);
				} else {
					pipe_resource_reference(&vb[i].buffer, NULL);
					disable_mask |= 1 << i;
				}
			}
		}
	} else {
		for (i = 0; i < count; i++) {
			pipe_resource_reference(&vb[i].buffer, NULL);
		}
		disable_mask = ((1ull << count) - 1);
	}

	disable_mask <<= start_slot;
	new_buffer_mask <<= start_slot;

	rctx->vertex_buffer_state.enabled_mask &= ~disable_mask;
	rctx->vertex_buffer_state.dirty_mask &= rctx->vertex_buffer_state.enabled_mask;
	rctx->vertex_buffer_state.enabled_mask |= new_buffer_mask;
	rctx->vertex_buffer_state.dirty_mask |= new_buffer_mask;

	r600_vertex_buffers_dirty(rctx);
}

void r600_sampler_views_dirty(struct r600_context *rctx,
			      struct r600_samplerview_state *state)
{
	if (state->dirty_mask) {
		state->atom.num_dw = (rctx->b.chip_class >= EVERGREEN ? 14 : 13) *
				     util_bitcount(state->dirty_mask);
		r600_mark_atom_dirty(rctx, &state->atom);
	}
}

static void r600_set_sampler_views(struct pipe_context *pipe,
				   enum pipe_shader_type shader,
				   unsigned start, unsigned count,
				   struct pipe_sampler_view **views)
{
	struct r600_context *rctx = (struct r600_context *) pipe;
	struct r600_textures_info *dst = &rctx->samplers[shader];
	struct r600_pipe_sampler_view **rviews = (struct r600_pipe_sampler_view **)views;
	uint32_t dirty_sampler_states_mask = 0;
	unsigned i;
	/* This sets 1-bit for textures with index >= count. */
	uint32_t disable_mask = ~((1ull << count) - 1);
	/* These are the new textures set by this function. */
	uint32_t new_mask = 0;

	/* Set textures with index >= count to NULL. */
	uint32_t remaining_mask;

	assert(start == 0); /* XXX fix below */

	if (!views) {
		disable_mask = ~0u;
		count = 0;
	}

	remaining_mask = dst->views.enabled_mask & disable_mask;

	while (remaining_mask) {
		i = u_bit_scan(&remaining_mask);
		assert(dst->views.views[i]);

		pipe_sampler_view_reference((struct pipe_sampler_view **)&dst->views.views[i], NULL);
	}

	for (i = 0; i < count; i++) {
		if (rviews[i] == dst->views.views[i]) {
			continue;
		}

		if (rviews[i]) {
			struct r600_texture *rtex =
				(struct r600_texture*)rviews[i]->base.texture;
			bool is_buffer = rviews[i]->base.texture->target == PIPE_BUFFER;

			if (!is_buffer && rtex->db_compatible) {
				dst->views.compressed_depthtex_mask |= 1 << i;
			} else {
				dst->views.compressed_depthtex_mask &= ~(1 << i);
			}

			/* Track compressed colorbuffers. */
			if (!is_buffer && rtex->cmask.size) {
				dst->views.compressed_colortex_mask |= 1 << i;
			} else {
				dst->views.compressed_colortex_mask &= ~(1 << i);
			}

			/* Changing from array to non-arrays textures and vice versa requires
			 * updating TEX_ARRAY_OVERRIDE in sampler states on R6xx-R7xx. */
			if (rctx->b.chip_class <= R700 &&
			    (dst->states.enabled_mask & (1 << i)) &&
			    (rviews[i]->base.texture->target == PIPE_TEXTURE_1D_ARRAY ||
			     rviews[i]->base.texture->target == PIPE_TEXTURE_2D_ARRAY) != dst->is_array_sampler[i]) {
				dirty_sampler_states_mask |= 1 << i;
			}

			pipe_sampler_view_reference((struct pipe_sampler_view **)&dst->views.views[i], views[i]);
			new_mask |= 1 << i;
			r600_context_add_resource_size(pipe, views[i]->texture);
		} else {
			pipe_sampler_view_reference((struct pipe_sampler_view **)&dst->views.views[i], NULL);
			disable_mask |= 1 << i;
		}
	}

	dst->views.enabled_mask &= ~disable_mask;
	dst->views.dirty_mask &= dst->views.enabled_mask;
	dst->views.enabled_mask |= new_mask;
	dst->views.dirty_mask |= new_mask;
	dst->views.compressed_depthtex_mask &= dst->views.enabled_mask;
	dst->views.compressed_colortex_mask &= dst->views.enabled_mask;
	dst->views.dirty_buffer_constants = TRUE;
	r600_sampler_views_dirty(rctx, &dst->views);

	if (dirty_sampler_states_mask) {
		dst->states.dirty_mask |= dirty_sampler_states_mask;
		r600_sampler_states_dirty(rctx, &dst->states);
	}
}

static void r600_update_compressed_colortex_mask(struct r600_samplerview_state *views)
{
	uint32_t mask = views->enabled_mask;

	while (mask) {
		unsigned i = u_bit_scan(&mask);
		struct pipe_resource *res = views->views[i]->base.texture;

		if (res && res->target != PIPE_BUFFER) {
			struct r600_texture *rtex = (struct r600_texture *)res;

			if (rtex->cmask.size) {
				views->compressed_colortex_mask |= 1 << i;
			} else {
				views->compressed_colortex_mask &= ~(1 << i);
			}
		}
	}
}

/* Compute the key for the hw shader variant */
static inline union r600_shader_key r600_shader_selector_key(struct pipe_context * ctx,
		struct r600_pipe_shader_selector * sel)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	union r600_shader_key key;
	memset(&key, 0, sizeof(key));

	switch (sel->type) {
	case PIPE_SHADER_VERTEX: {
		key.vs.as_ls = (rctx->tes_shader != NULL);
		if (!key.vs.as_ls)
			key.vs.as_es = (rctx->gs_shader != NULL);

		if (rctx->ps_shader->current->shader.gs_prim_id_input && !rctx->gs_shader) {
			key.vs.as_gs_a = true;
			key.vs.prim_id_out = rctx->ps_shader->current->shader.input[rctx->ps_shader->current->shader.ps_prim_id_input].spi_sid;
		}
		break;
	}
	case PIPE_SHADER_GEOMETRY:
		break;
	case PIPE_SHADER_FRAGMENT: {
		key.ps.color_two_side = rctx->rasterizer && rctx->rasterizer->two_side;
		key.ps.alpha_to_one = rctx->alpha_to_one &&
				      rctx->rasterizer && rctx->rasterizer->multisample_enable &&
				      !rctx->framebuffer.cb0_is_integer;
		key.ps.nr_cbufs = rctx->framebuffer.state.nr_cbufs;
		/* Dual-source blending only makes sense with nr_cbufs == 1. */
		if (key.ps.nr_cbufs == 1 && rctx->dual_src_blend)
			key.ps.nr_cbufs = 2;
		break;
	}
	case PIPE_SHADER_TESS_EVAL:
		key.tes.as_es = (rctx->gs_shader != NULL);
		break;
	case PIPE_SHADER_TESS_CTRL:
		key.tcs.prim_mode = rctx->tes_shader->info.properties[TGSI_PROPERTY_TES_PRIM_MODE];
		break;
	default:
		assert(0);
	}

	return key;
}

/* Select the hw shader variant depending on the current state.
 * (*dirty) is set to 1 if current variant was changed */
static int r600_shader_select(struct pipe_context *ctx,
        struct r600_pipe_shader_selector* sel,
        bool *dirty)
{
	union r600_shader_key key;
	struct r600_pipe_shader * shader = NULL;
	int r;

	memset(&key, 0, sizeof(key));
	key = r600_shader_selector_key(ctx, sel);

	/* Check if we don't need to change anything.
	 * This path is also used for most shaders that don't need multiple
	 * variants, it will cost just a computation of the key and this
	 * test. */
	if (likely(sel->current && memcmp(&sel->current->key, &key, sizeof(key)) == 0)) {
		return 0;
	}

	/* lookup if we have other variants in the list */
	if (sel->num_shaders > 1) {
		struct r600_pipe_shader *p = sel->current, *c = p->next_variant;

		while (c && memcmp(&c->key, &key, sizeof(key)) != 0) {
			p = c;
			c = c->next_variant;
		}

		if (c) {
			p->next_variant = c->next_variant;
			shader = c;
		}
	}

	if (unlikely(!shader)) {
		shader = CALLOC(1, sizeof(struct r600_pipe_shader));
		shader->selector = sel;

		r = r600_pipe_shader_create(ctx, shader, key);
		if (unlikely(r)) {
			R600_ERR("Failed to build shader variant (type=%u) %d\n",
				 sel->type, r);
			sel->current = NULL;
			FREE(shader);
			return r;
		}

		/* We don't know the value of nr_ps_max_color_exports until we built
		 * at least one variant, so we may need to recompute the key after
		 * building first variant. */
		if (sel->type == PIPE_SHADER_FRAGMENT &&
				sel->num_shaders == 0) {
			sel->nr_ps_max_color_exports = shader->shader.nr_ps_max_color_exports;
			key = r600_shader_selector_key(ctx, sel);
		}

		memcpy(&shader->key, &key, sizeof(key));
		sel->num_shaders++;
	}

	if (dirty)
		*dirty = true;

	shader->next_variant = sel->current;
	sel->current = shader;

	return 0;
}

static void *r600_create_shader_state(struct pipe_context *ctx,
			       const struct pipe_shader_state *state,
			       unsigned pipe_shader_type)
{
	struct r600_pipe_shader_selector *sel = CALLOC_STRUCT(r600_pipe_shader_selector);
	int i;

	sel->type = pipe_shader_type;
	sel->tokens = tgsi_dup_tokens(state->tokens);
	sel->so = state->stream_output;
	tgsi_scan_shader(state->tokens, &sel->info);

	switch (pipe_shader_type) {
	case PIPE_SHADER_GEOMETRY:
		sel->gs_output_prim =
			sel->info.properties[TGSI_PROPERTY_GS_OUTPUT_PRIM];
		sel->gs_max_out_vertices =
			sel->info.properties[TGSI_PROPERTY_GS_MAX_OUTPUT_VERTICES];
		sel->gs_num_invocations =
			sel->info.properties[TGSI_PROPERTY_GS_INVOCATIONS];
		break;
	case PIPE_SHADER_VERTEX:
	case PIPE_SHADER_TESS_CTRL:
		sel->lds_patch_outputs_written_mask = 0;
		sel->lds_outputs_written_mask = 0;

		for (i = 0; i < sel->info.num_outputs; i++) {
			unsigned name = sel->info.output_semantic_name[i];
			unsigned index = sel->info.output_semantic_index[i];

			switch (name) {
			case TGSI_SEMANTIC_TESSINNER:
			case TGSI_SEMANTIC_TESSOUTER:
			case TGSI_SEMANTIC_PATCH:
				sel->lds_patch_outputs_written_mask |=
					1llu << r600_get_lds_unique_index(name, index);
				break;
			default:
				sel->lds_outputs_written_mask |=
					1llu << r600_get_lds_unique_index(name, index);
			}
		}
		break;
	default:
		break;
	}

	return sel;
}

static void *r600_create_ps_state(struct pipe_context *ctx,
					 const struct pipe_shader_state *state)
{
	return r600_create_shader_state(ctx, state, PIPE_SHADER_FRAGMENT);
}

static void *r600_create_vs_state(struct pipe_context *ctx,
					 const struct pipe_shader_state *state)
{
	return r600_create_shader_state(ctx, state, PIPE_SHADER_VERTEX);
}

static void *r600_create_gs_state(struct pipe_context *ctx,
					 const struct pipe_shader_state *state)
{
	return r600_create_shader_state(ctx, state, PIPE_SHADER_GEOMETRY);
}

static void *r600_create_tcs_state(struct pipe_context *ctx,
					 const struct pipe_shader_state *state)
{
	return r600_create_shader_state(ctx, state, PIPE_SHADER_TESS_CTRL);
}

static void *r600_create_tes_state(struct pipe_context *ctx,
					 const struct pipe_shader_state *state)
{
	return r600_create_shader_state(ctx, state, PIPE_SHADER_TESS_EVAL);
}

static void r600_bind_ps_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	if (!state)
		state = rctx->dummy_pixel_shader;

	rctx->ps_shader = (struct r600_pipe_shader_selector *)state;
}

static struct tgsi_shader_info *r600_get_vs_info(struct r600_context *rctx)
{
	if (rctx->gs_shader)
		return &rctx->gs_shader->info;
	else if (rctx->tes_shader)
		return &rctx->tes_shader->info;
	else if (rctx->vs_shader)
		return &rctx->vs_shader->info;
	else
		return NULL;
}

static void r600_bind_vs_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	if (!state)
		return;

	rctx->vs_shader = (struct r600_pipe_shader_selector *)state;
	r600_update_vs_writes_viewport_index(&rctx->b, r600_get_vs_info(rctx));
	rctx->b.streamout.stride_in_dw = rctx->vs_shader->so.stride;
}

static void r600_bind_gs_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	rctx->gs_shader = (struct r600_pipe_shader_selector *)state;
	r600_update_vs_writes_viewport_index(&rctx->b, r600_get_vs_info(rctx));

	if (!state)
		return;
	rctx->b.streamout.stride_in_dw = rctx->gs_shader->so.stride;
}

static void r600_bind_tcs_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	rctx->tcs_shader = (struct r600_pipe_shader_selector *)state;
}

static void r600_bind_tes_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	rctx->tes_shader = (struct r600_pipe_shader_selector *)state;
	r600_update_vs_writes_viewport_index(&rctx->b, r600_get_vs_info(rctx));

	if (!state)
		return;
	rctx->b.streamout.stride_in_dw = rctx->tes_shader->so.stride;
}

static void r600_delete_shader_selector(struct pipe_context *ctx,
		struct r600_pipe_shader_selector *sel)
{
	struct r600_pipe_shader *p = sel->current, *c;
	while (p) {
		c = p->next_variant;
		r600_pipe_shader_destroy(ctx, p);
		free(p);
		p = c;
	}

	free(sel->tokens);
	free(sel);
}


static void r600_delete_ps_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_pipe_shader_selector *sel = (struct r600_pipe_shader_selector *)state;

	if (rctx->ps_shader == sel) {
		rctx->ps_shader = NULL;
	}

	r600_delete_shader_selector(ctx, sel);
}

static void r600_delete_vs_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_pipe_shader_selector *sel = (struct r600_pipe_shader_selector *)state;

	if (rctx->vs_shader == sel) {
		rctx->vs_shader = NULL;
	}

	r600_delete_shader_selector(ctx, sel);
}


static void r600_delete_gs_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_pipe_shader_selector *sel = (struct r600_pipe_shader_selector *)state;

	if (rctx->gs_shader == sel) {
		rctx->gs_shader = NULL;
	}

	r600_delete_shader_selector(ctx, sel);
}

static void r600_delete_tcs_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_pipe_shader_selector *sel = (struct r600_pipe_shader_selector *)state;

	if (rctx->tcs_shader == sel) {
		rctx->tcs_shader = NULL;
	}

	r600_delete_shader_selector(ctx, sel);
}

static void r600_delete_tes_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_pipe_shader_selector *sel = (struct r600_pipe_shader_selector *)state;

	if (rctx->tes_shader == sel) {
		rctx->tes_shader = NULL;
	}

	r600_delete_shader_selector(ctx, sel);
}

void r600_constant_buffers_dirty(struct r600_context *rctx, struct r600_constbuf_state *state)
{
	if (state->dirty_mask) {
		state->atom.num_dw = rctx->b.chip_class >= EVERGREEN ? util_bitcount(state->dirty_mask)*20
								   : util_bitcount(state->dirty_mask)*19;
		r600_mark_atom_dirty(rctx, &state->atom);
	}
}

static void r600_set_constant_buffer(struct pipe_context *ctx, uint shader, uint index,
				     const struct pipe_constant_buffer *input)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct r600_constbuf_state *state = &rctx->constbuf_state[shader];
	struct pipe_constant_buffer *cb;
	const uint8_t *ptr;

	/* Note that the state tracker can unbind constant buffers by
	 * passing NULL here.
	 */
	if (unlikely(!input || (!input->buffer && !input->user_buffer))) {
		state->enabled_mask &= ~(1 << index);
		state->dirty_mask &= ~(1 << index);
		pipe_resource_reference(&state->cb[index].buffer, NULL);
		return;
	}

	cb = &state->cb[index];
	cb->buffer_size = input->buffer_size;

	ptr = input->user_buffer;

	if (ptr) {
		/* Upload the user buffer. */
		if (R600_BIG_ENDIAN) {
			uint32_t *tmpPtr;
			unsigned i, size = input->buffer_size;

			if (!(tmpPtr = malloc(size))) {
				R600_ERR("Failed to allocate BE swap buffer.\n");
				return;
			}

			for (i = 0; i < size / 4; ++i) {
				tmpPtr[i] = util_cpu_to_le32(((uint32_t *)ptr)[i]);
			}

			u_upload_data(rctx->b.uploader, 0, size, 256, tmpPtr, &cb->buffer_offset, &cb->buffer);
			free(tmpPtr);
		} else {
			u_upload_data(rctx->b.uploader, 0, input->buffer_size, 256, ptr, &cb->buffer_offset, &cb->buffer);
		}
		/* account it in gtt */
		rctx->b.gtt += input->buffer_size;
	} else {
		/* Setup the hw buffer. */
		cb->buffer_offset = input->buffer_offset;
		pipe_resource_reference(&cb->buffer, input->buffer);
		r600_context_add_resource_size(ctx, input->buffer);
	}

	state->enabled_mask |= 1 << index;
	state->dirty_mask |= 1 << index;
	r600_constant_buffers_dirty(rctx, state);
}

static void r600_set_sample_mask(struct pipe_context *pipe, unsigned sample_mask)
{
	struct r600_context *rctx = (struct r600_context*)pipe;

	if (rctx->sample_mask.sample_mask == (uint16_t)sample_mask)
		return;

	rctx->sample_mask.sample_mask = sample_mask;
	r600_mark_atom_dirty(rctx, &rctx->sample_mask.atom);
}

static void r600_update_driver_const_buffers(struct r600_context *rctx)
{
	int sh, size;
	void *ptr;
	struct pipe_constant_buffer cb;
	for (sh = 0; sh < PIPE_SHADER_TYPES; sh++) {
		struct r600_shader_driver_constants_info *info = &rctx->driver_consts[sh];
		if (!info->vs_ucp_dirty &&
		    !info->texture_const_dirty &&
		    !info->ps_sample_pos_dirty)
			continue;

		ptr = info->constants;
		size = info->alloc_size;
		if (info->vs_ucp_dirty) {
			assert(sh == PIPE_SHADER_VERTEX);
			if (!size) {
				ptr = rctx->clip_state.state.ucp;
				size = R600_UCP_SIZE;
			} else {
				memcpy(ptr, rctx->clip_state.state.ucp, R600_UCP_SIZE);
			}
			info->vs_ucp_dirty = false;
		}

		if (info->ps_sample_pos_dirty) {
			assert(sh == PIPE_SHADER_FRAGMENT);
			if (!size) {
				ptr = rctx->sample_positions;
				size = R600_UCP_SIZE;
			} else {
				memcpy(ptr, rctx->sample_positions, R600_UCP_SIZE);
			}
			info->ps_sample_pos_dirty = false;
		}

		if (info->texture_const_dirty) {
			assert (ptr);
			assert (size);
			if (sh == PIPE_SHADER_VERTEX)
				memcpy(ptr, rctx->clip_state.state.ucp, R600_UCP_SIZE);
			if (sh == PIPE_SHADER_FRAGMENT)
				memcpy(ptr, rctx->sample_positions, R600_UCP_SIZE);
		}
		info->texture_const_dirty = false;

		cb.buffer = NULL;
		cb.user_buffer = ptr;
		cb.buffer_offset = 0;
		cb.buffer_size = size;
		rctx->b.b.set_constant_buffer(&rctx->b.b, sh, R600_BUFFER_INFO_CONST_BUFFER, &cb);
		pipe_resource_reference(&cb.buffer, NULL);
	}
}

static void *r600_alloc_buf_consts(struct r600_context *rctx, int shader_type,
				   int array_size, uint32_t *base_offset)
{
	struct r600_shader_driver_constants_info *info = &rctx->driver_consts[shader_type];
	if (array_size + R600_UCP_SIZE > info->alloc_size) {
		info->constants = realloc(info->constants, array_size + R600_UCP_SIZE);
		info->alloc_size = array_size + R600_UCP_SIZE;
	}
	memset(info->constants + (R600_UCP_SIZE / 4), 0, array_size);
	info->texture_const_dirty = true;
	*base_offset = R600_UCP_SIZE;
	return info->constants;
}
/*
 * On r600/700 hw we don't have vertex fetch swizzle, though TBO
 * doesn't require full swizzles it does need masking and setting alpha
 * to one, so we setup a set of 5 constants with the masks + alpha value
 * then in the shader, we AND the 4 components with 0xffffffff or 0,
 * then OR the alpha with the value given here.
 * We use a 6th constant to store the txq buffer size in
 * we use 7th slot for number of cube layers in a cube map array.
 */
static void r600_setup_buffer_constants(struct r600_context *rctx, int shader_type)
{
	struct r600_textures_info *samplers = &rctx->samplers[shader_type];
	int bits;
	uint32_t array_size;
	int i, j;
	uint32_t *constants;
	uint32_t base_offset;
	if (!samplers->views.dirty_buffer_constants)
		return;

	samplers->views.dirty_buffer_constants = FALSE;

	bits = util_last_bit(samplers->views.enabled_mask);
	array_size = bits * 8 * sizeof(uint32_t) * 4;

	constants = r600_alloc_buf_consts(rctx, shader_type, array_size, &base_offset);

	for (i = 0; i < bits; i++) {
		if (samplers->views.enabled_mask & (1 << i)) {
			int offset = (base_offset / 4) + i * 8;
			const struct util_format_description *desc;
			desc = util_format_description(samplers->views.views[i]->base.format);

			for (j = 0; j < 4; j++)
				if (j < desc->nr_channels)
					constants[offset+j] = 0xffffffff;
				else
					constants[offset+j] = 0x0;
			if (desc->nr_channels < 4) {
				if (desc->channel[0].pure_integer)
					constants[offset+4] = 1;
				else
					constants[offset+4] = fui(1.0);
			} else
				constants[offset + 4] = 0;

			constants[offset + 5] = samplers->views.views[i]->base.texture->width0 / util_format_get_blocksize(samplers->views.views[i]->base.format);
			constants[offset + 6] = samplers->views.views[i]->base.texture->array_size / 6;
		}
	}

}

/* On evergreen we store two values
 * 1. buffer size for TXQ
 * 2. number of cube layers in a cube map array.
 */
static void eg_setup_buffer_constants(struct r600_context *rctx, int shader_type)
{
	struct r600_textures_info *samplers = &rctx->samplers[shader_type];
	int bits;
	uint32_t array_size;
	int i;
	uint32_t *constants;
	uint32_t base_offset;
	if (!samplers->views.dirty_buffer_constants)
		return;

	samplers->views.dirty_buffer_constants = FALSE;

	bits = util_last_bit(samplers->views.enabled_mask);
	array_size = bits * 2 * sizeof(uint32_t) * 4;

	constants = r600_alloc_buf_consts(rctx, shader_type, array_size,
					  &base_offset);

	for (i = 0; i < bits; i++) {
		if (samplers->views.enabled_mask & (1 << i)) {
			uint32_t offset = (base_offset / 4) + i * 2;
			constants[offset] = samplers->views.views[i]->base.texture->width0 / util_format_get_blocksize(samplers->views.views[i]->base.format);
			constants[offset + 1] = samplers->views.views[i]->base.texture->array_size / 6;
		}
	}
}

/* set sample xy locations as array of fragment shader constants */
void r600_set_sample_locations_constant_buffer(struct r600_context *rctx)
{
	int i;
	struct pipe_context *ctx = &rctx->b.b;

	assert(rctx->framebuffer.nr_samples < R600_UCP_SIZE);
	assert(rctx->framebuffer.nr_samples <= ARRAY_SIZE(rctx->sample_positions)/4);

	memset(rctx->sample_positions, 0, 4 * 4 * 16);
	for (i = 0; i < rctx->framebuffer.nr_samples; i++) {
		ctx->get_sample_position(ctx, rctx->framebuffer.nr_samples, i, &rctx->sample_positions[4*i]);
		/* Also fill in center-zeroed positions used for interpolateAtSample */
		rctx->sample_positions[4*i + 2] = rctx->sample_positions[4*i + 0] - 0.5f;
		rctx->sample_positions[4*i + 3] = rctx->sample_positions[4*i + 1] - 0.5f;
	}

	rctx->driver_consts[PIPE_SHADER_FRAGMENT].ps_sample_pos_dirty = true;
}

static void update_shader_atom(struct pipe_context *ctx,
			       struct r600_shader_state *state,
			       struct r600_pipe_shader *shader)
{
	struct r600_context *rctx = (struct r600_context *)ctx;

	state->shader = shader;
	if (shader) {
		state->atom.num_dw = shader->command_buffer.num_dw;
		r600_context_add_resource_size(ctx, (struct pipe_resource *)shader->bo);
	} else {
		state->atom.num_dw = 0;
	}
	r600_mark_atom_dirty(rctx, &state->atom);
}

static void update_gs_block_state(struct r600_context *rctx, unsigned enable)
{
	if (rctx->shader_stages.geom_enable != enable) {
		rctx->shader_stages.geom_enable = enable;
		r600_mark_atom_dirty(rctx, &rctx->shader_stages.atom);
	}

	if (rctx->gs_rings.enable != enable) {
		rctx->gs_rings.enable = enable;
		r600_mark_atom_dirty(rctx, &rctx->gs_rings.atom);

		if (enable && !rctx->gs_rings.esgs_ring.buffer) {
			unsigned size = 0x1C000;
			rctx->gs_rings.esgs_ring.buffer =
					pipe_buffer_create(rctx->b.b.screen, PIPE_BIND_CUSTOM,
							PIPE_USAGE_DEFAULT, size);
			rctx->gs_rings.esgs_ring.buffer_size = size;

			size = 0x4000000;

			rctx->gs_rings.gsvs_ring.buffer =
					pipe_buffer_create(rctx->b.b.screen, PIPE_BIND_CUSTOM,
							PIPE_USAGE_DEFAULT, size);
			rctx->gs_rings.gsvs_ring.buffer_size = size;
		}

		if (enable) {
			r600_set_constant_buffer(&rctx->b.b, PIPE_SHADER_GEOMETRY,
					R600_GS_RING_CONST_BUFFER, &rctx->gs_rings.esgs_ring);
			if (rctx->tes_shader) {
				r600_set_constant_buffer(&rctx->b.b, PIPE_SHADER_TESS_EVAL,
							 R600_GS_RING_CONST_BUFFER, &rctx->gs_rings.gsvs_ring);
			} else {
				r600_set_constant_buffer(&rctx->b.b, PIPE_SHADER_VERTEX,
							 R600_GS_RING_CONST_BUFFER, &rctx->gs_rings.gsvs_ring);
			}
		} else {
			r600_set_constant_buffer(&rctx->b.b, PIPE_SHADER_GEOMETRY,
					R600_GS_RING_CONST_BUFFER, NULL);
			r600_set_constant_buffer(&rctx->b.b, PIPE_SHADER_VERTEX,
					R600_GS_RING_CONST_BUFFER, NULL);
			r600_set_constant_buffer(&rctx->b.b, PIPE_SHADER_TESS_EVAL,
					R600_GS_RING_CONST_BUFFER, NULL);
		}
	}
}

static void r600_update_clip_state(struct r600_context *rctx,
				   struct r600_pipe_shader *current)
{
	if (current->pa_cl_vs_out_cntl != rctx->clip_misc_state.pa_cl_vs_out_cntl ||
	    current->shader.clip_dist_write != rctx->clip_misc_state.clip_dist_write ||
	    current->shader.vs_position_window_space != rctx->clip_misc_state.clip_disable ||
	    current->shader.vs_out_viewport != rctx->clip_misc_state.vs_out_viewport) {
		rctx->clip_misc_state.pa_cl_vs_out_cntl = current->pa_cl_vs_out_cntl;
		rctx->clip_misc_state.clip_dist_write = current->shader.clip_dist_write;
		rctx->clip_misc_state.clip_disable = current->shader.vs_position_window_space;
		rctx->clip_misc_state.vs_out_viewport = current->shader.vs_out_viewport;
		r600_mark_atom_dirty(rctx, &rctx->clip_misc_state.atom);
	}
}

static void r600_generate_fixed_func_tcs(struct r600_context *rctx)
{
	struct ureg_src const0, const1;
	struct ureg_dst tessouter, tessinner;
	struct ureg_program *ureg = ureg_create(PIPE_SHADER_TESS_CTRL);

	if (!ureg)
		return; /* if we get here, we're screwed */

	assert(!rctx->fixed_func_tcs_shader);

	ureg_DECL_constant2D(ureg, 0, 3, R600_LDS_INFO_CONST_BUFFER);
	const0 = ureg_src_dimension(ureg_src_register(TGSI_FILE_CONSTANT, 2),
				    R600_LDS_INFO_CONST_BUFFER);
	const1 = ureg_src_dimension(ureg_src_register(TGSI_FILE_CONSTANT, 3),
				    R600_LDS_INFO_CONST_BUFFER);

	tessouter = ureg_DECL_output(ureg, TGSI_SEMANTIC_TESSOUTER, 0);
	tessinner = ureg_DECL_output(ureg, TGSI_SEMANTIC_TESSINNER, 0);

	ureg_MOV(ureg, tessouter, const0);
	ureg_MOV(ureg, tessinner, const1);
	ureg_END(ureg);

	rctx->fixed_func_tcs_shader =
		ureg_create_shader_and_destroy(ureg, &rctx->b.b);
}

#define SELECT_SHADER_OR_FAIL(x) do {					\
		r600_shader_select(ctx, rctx->x##_shader, &x##_dirty);	\
		if (unlikely(!rctx->x##_shader->current))		\
			return false;					\
	} while(0)

#define UPDATE_SHADER(hw, sw) do {					\
		if (sw##_dirty || (rctx->hw_shader_stages[(hw)].shader != rctx->sw##_shader->current)) \
			update_shader_atom(ctx, &rctx->hw_shader_stages[(hw)], rctx->sw##_shader->current); \
	} while(0)

#define UPDATE_SHADER_CLIP(hw, sw) do {					\
		if (sw##_dirty || (rctx->hw_shader_stages[(hw)].shader != rctx->sw##_shader->current)) { \
			update_shader_atom(ctx, &rctx->hw_shader_stages[(hw)], rctx->sw##_shader->current); \
			clip_so_current = rctx->sw##_shader->current;   \
		}                                                       \
	} while(0)

#define UPDATE_SHADER_GS(hw, hw2, sw) do {				\
		if (sw##_dirty || (rctx->hw_shader_stages[(hw)].shader != rctx->sw##_shader->current)) { \
			update_shader_atom(ctx, &rctx->hw_shader_stages[(hw)], rctx->sw##_shader->current); \
			update_shader_atom(ctx, &rctx->hw_shader_stages[(hw2)], rctx->sw##_shader->current->gs_copy_shader); \
			clip_so_current = rctx->sw##_shader->current->gs_copy_shader; \
		}                                                       \
	} while(0)

#define SET_NULL_SHADER(hw) do {						\
		if (rctx->hw_shader_stages[(hw)].shader)	\
			update_shader_atom(ctx, &rctx->hw_shader_stages[(hw)], NULL); \
	} while (0)

static bool r600_update_derived_state(struct r600_context *rctx)
{
	struct pipe_context * ctx = (struct pipe_context*)rctx;
	bool ps_dirty = false, vs_dirty = false, gs_dirty = false;
	bool tcs_dirty = false, tes_dirty = false, fixed_func_tcs_dirty = false;
	bool blend_disable;
	bool need_buf_const;
	struct r600_pipe_shader *clip_so_current = NULL;

	if (!rctx->blitter->running) {
		unsigned i;
		unsigned counter;

		counter = p_atomic_read(&rctx->screen->b.compressed_colortex_counter);
		if (counter != rctx->b.last_compressed_colortex_counter) {
			rctx->b.last_compressed_colortex_counter = counter;

			for (i = 0; i < PIPE_SHADER_TYPES; ++i) {
				r600_update_compressed_colortex_mask(&rctx->samplers[i].views);
			}
		}

		/* Decompress textures if needed. */
		for (i = 0; i < PIPE_SHADER_TYPES; i++) {
			struct r600_samplerview_state *views = &rctx->samplers[i].views;
			if (views->compressed_depthtex_mask) {
				r600_decompress_depth_textures(rctx, views);
			}
			if (views->compressed_colortex_mask) {
				r600_decompress_color_textures(rctx, views);
			}
		}
	}

	SELECT_SHADER_OR_FAIL(ps);

	r600_mark_atom_dirty(rctx, &rctx->shader_stages.atom);

	update_gs_block_state(rctx, rctx->gs_shader != NULL);

	if (rctx->gs_shader)
		SELECT_SHADER_OR_FAIL(gs);

	/* Hull Shader */
	if (rctx->tcs_shader) {
		SELECT_SHADER_OR_FAIL(tcs);

		UPDATE_SHADER(EG_HW_STAGE_HS, tcs);
	} else if (rctx->tes_shader) {
		if (!rctx->fixed_func_tcs_shader) {
			r600_generate_fixed_func_tcs(rctx);
			if (!rctx->fixed_func_tcs_shader)
				return false;

		}
		SELECT_SHADER_OR_FAIL(fixed_func_tcs);

		UPDATE_SHADER(EG_HW_STAGE_HS, fixed_func_tcs);
	} else
		SET_NULL_SHADER(EG_HW_STAGE_HS);

	if (rctx->tes_shader) {
		SELECT_SHADER_OR_FAIL(tes);
	}

	SELECT_SHADER_OR_FAIL(vs);

	if (rctx->gs_shader) {
		if (!rctx->shader_stages.geom_enable) {
			rctx->shader_stages.geom_enable = true;
			r600_mark_atom_dirty(rctx, &rctx->shader_stages.atom);
		}

		/* gs_shader provides GS and VS (copy shader) */
		UPDATE_SHADER_GS(R600_HW_STAGE_GS, R600_HW_STAGE_VS, gs);

		/* vs_shader is used as ES */

		if (rctx->tes_shader) {
			/* VS goes to LS, TES goes to ES */
			UPDATE_SHADER(R600_HW_STAGE_ES, tes);
			UPDATE_SHADER(EG_HW_STAGE_LS, vs);
               } else {
			/* vs_shader is used as ES */
			UPDATE_SHADER(R600_HW_STAGE_ES, vs);
			SET_NULL_SHADER(EG_HW_STAGE_LS);
		}
	} else {
		if (unlikely(rctx->hw_shader_stages[R600_HW_STAGE_GS].shader)) {
			SET_NULL_SHADER(R600_HW_STAGE_GS);
			SET_NULL_SHADER(R600_HW_STAGE_ES);
			rctx->shader_stages.geom_enable = false;
			r600_mark_atom_dirty(rctx, &rctx->shader_stages.atom);
		}

		if (rctx->tes_shader) {
			/* if TES is loaded and no geometry, TES runs on hw VS, VS runs on hw LS */
			UPDATE_SHADER_CLIP(R600_HW_STAGE_VS, tes);
			UPDATE_SHADER(EG_HW_STAGE_LS, vs);
		} else {
			SET_NULL_SHADER(EG_HW_STAGE_LS);
			UPDATE_SHADER_CLIP(R600_HW_STAGE_VS, vs);
		}
	}

	/* Update clip misc state. */
	if (clip_so_current) {
		r600_update_clip_state(rctx, clip_so_current);
		rctx->b.streamout.enabled_stream_buffers_mask = clip_so_current->enabled_stream_buffers_mask;
	}

	if (unlikely(ps_dirty || rctx->hw_shader_stages[R600_HW_STAGE_PS].shader != rctx->ps_shader->current ||
		rctx->rasterizer->sprite_coord_enable != rctx->ps_shader->current->sprite_coord_enable ||
		rctx->rasterizer->flatshade != rctx->ps_shader->current->flatshade)) {

		if (rctx->cb_misc_state.nr_ps_color_outputs != rctx->ps_shader->current->nr_ps_color_outputs) {
			rctx->cb_misc_state.nr_ps_color_outputs = rctx->ps_shader->current->nr_ps_color_outputs;
			r600_mark_atom_dirty(rctx, &rctx->cb_misc_state.atom);
		}

		if (rctx->b.chip_class <= R700) {
			bool multiwrite = rctx->ps_shader->current->shader.fs_write_all;

			if (rctx->cb_misc_state.multiwrite != multiwrite) {
				rctx->cb_misc_state.multiwrite = multiwrite;
				r600_mark_atom_dirty(rctx, &rctx->cb_misc_state.atom);
			}
		}

		if (unlikely(!ps_dirty && rctx->ps_shader && rctx->rasterizer &&
				((rctx->rasterizer->sprite_coord_enable != rctx->ps_shader->current->sprite_coord_enable) ||
						(rctx->rasterizer->flatshade != rctx->ps_shader->current->flatshade)))) {

			if (rctx->b.chip_class >= EVERGREEN)
				evergreen_update_ps_state(ctx, rctx->ps_shader->current);
			else
				r600_update_ps_state(ctx, rctx->ps_shader->current);
		}

		r600_mark_atom_dirty(rctx, &rctx->shader_stages.atom);
	}
	UPDATE_SHADER(R600_HW_STAGE_PS, ps);

	if (rctx->b.chip_class >= EVERGREEN) {
		evergreen_update_db_shader_control(rctx);
	} else {
		r600_update_db_shader_control(rctx);
	}

	/* on R600 we stuff masks + txq info into one constant buffer */
	/* on evergreen we only need a txq info one */
	if (rctx->ps_shader) {
		need_buf_const = rctx->ps_shader->current->shader.uses_tex_buffers || rctx->ps_shader->current->shader.has_txq_cube_array_z_comp;
		if (need_buf_const) {
			if (rctx->b.chip_class < EVERGREEN)
				r600_setup_buffer_constants(rctx, PIPE_SHADER_FRAGMENT);
			else
				eg_setup_buffer_constants(rctx, PIPE_SHADER_FRAGMENT);
		}
	}

	if (rctx->vs_shader) {
		need_buf_const = rctx->vs_shader->current->shader.uses_tex_buffers || rctx->vs_shader->current->shader.has_txq_cube_array_z_comp;
		if (need_buf_const) {
			if (rctx->b.chip_class < EVERGREEN)
				r600_setup_buffer_constants(rctx, PIPE_SHADER_VERTEX);
			else
				eg_setup_buffer_constants(rctx, PIPE_SHADER_VERTEX);
		}
	}

	if (rctx->gs_shader) {
		need_buf_const = rctx->gs_shader->current->shader.uses_tex_buffers || rctx->gs_shader->current->shader.has_txq_cube_array_z_comp;
		if (need_buf_const) {
			if (rctx->b.chip_class < EVERGREEN)
				r600_setup_buffer_constants(rctx, PIPE_SHADER_GEOMETRY);
			else
				eg_setup_buffer_constants(rctx, PIPE_SHADER_GEOMETRY);
		}
	}

	r600_update_driver_const_buffers(rctx);

	if (rctx->b.chip_class < EVERGREEN && rctx->ps_shader && rctx->vs_shader) {
		if (!r600_adjust_gprs(rctx)) {
			/* discard rendering */
			return false;
		}
	}

	if (rctx->b.chip_class == EVERGREEN) {
		if (!evergreen_adjust_gprs(rctx)) {
			/* discard rendering */
			return false;
		}
	}

	blend_disable = (rctx->dual_src_blend &&
			rctx->ps_shader->current->nr_ps_color_outputs < 2);

	if (blend_disable != rctx->force_blend_disable) {
		rctx->force_blend_disable = blend_disable;
		r600_bind_blend_state_internal(rctx,
					       rctx->blend_state.cso,
					       blend_disable);
	}

	return true;
}

void r600_emit_clip_misc_state(struct r600_context *rctx, struct r600_atom *atom)
{
	struct radeon_winsys_cs *cs = rctx->b.gfx.cs;
	struct r600_clip_misc_state *state = &rctx->clip_misc_state;

	radeon_set_context_reg(cs, R_028810_PA_CL_CLIP_CNTL,
			       state->pa_cl_clip_cntl |
			       (state->clip_dist_write ? 0 : state->clip_plane_enable & 0x3F) |
                               S_028810_CLIP_DISABLE(state->clip_disable));
	radeon_set_context_reg(cs, R_02881C_PA_CL_VS_OUT_CNTL,
			       state->pa_cl_vs_out_cntl |
			       (state->clip_plane_enable & state->clip_dist_write));
	/* reuse needs to be set off if we write oViewport */
	if (rctx->b.chip_class >= EVERGREEN)
		radeon_set_context_reg(cs, R_028AB4_VGT_REUSE_OFF,
				       S_028AB4_REUSE_OFF(state->vs_out_viewport));
}

static void r600_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *dinfo)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct pipe_draw_info info = *dinfo;
	struct pipe_index_buffer ib = {};
	struct radeon_winsys_cs *cs = rctx->b.gfx.cs;
	bool render_cond_bit = rctx->b.render_cond && !rctx->b.render_cond_force_off;
	uint64_t mask;
	unsigned num_patches, dirty_fb_counter;

	if (!info.indirect && !info.count && (info.indexed || !info.count_from_stream_output)) {
		return;
	}

	if (!rctx->vs_shader || !rctx->ps_shader) {
		assert(0);
		return;
	}

	/* make sure that the gfx ring is only one active */
	if (radeon_emitted(rctx->b.dma.cs, 0)) {
		rctx->b.dma.flush(rctx, RADEON_FLUSH_ASYNC, NULL);
	}

	/* Re-emit the framebuffer state if needed. */
	dirty_fb_counter = p_atomic_read(&rctx->b.screen->dirty_fb_counter);
	if (dirty_fb_counter != rctx->b.last_dirty_fb_counter) {
		rctx->b.last_dirty_fb_counter = dirty_fb_counter;
		r600_mark_atom_dirty(rctx, &rctx->framebuffer.atom);
	}

	if (!r600_update_derived_state(rctx)) {
		/* useless to render because current rendering command
		 * can't be achieved
		 */
		return;
	}

	if (info.indexed) {
		/* Initialize the index buffer struct. */
		pipe_resource_reference(&ib.buffer, rctx->index_buffer.buffer);
		ib.user_buffer = rctx->index_buffer.user_buffer;
		ib.index_size = rctx->index_buffer.index_size;
		ib.offset = rctx->index_buffer.offset;
		if (!info.indirect) {
			ib.offset += info.start * ib.index_size;
		}

		/* Translate 8-bit indices to 16-bit. */
		if (unlikely(ib.index_size == 1)) {
			struct pipe_resource *out_buffer = NULL;
			unsigned out_offset;
			void *ptr;
			unsigned start, count;

			if (likely(!info.indirect)) {
				start = 0;
				count = info.count;
			}
			else {
				/* Have to get start/count from indirect buffer, slow path ahead... */
				struct r600_resource *indirect_resource = (struct r600_resource *)info.indirect;
				unsigned *data = r600_buffer_map_sync_with_rings(&rctx->b, indirect_resource,
					PIPE_TRANSFER_READ);
				if (data) {
					data += info.indirect_offset / sizeof(unsigned);
					start = data[2] * ib.index_size;
					count = data[0];
				}
				else {
					start = 0;
					count = 0;
				}
			}

			u_upload_alloc(rctx->b.uploader, start, count * 2, 256,
				       &out_offset, &out_buffer, &ptr);

			util_shorten_ubyte_elts_to_userptr(
						&rctx->b.b, &ib, 0, ib.offset + start, count, ptr);

			pipe_resource_reference(&ib.buffer, NULL);
			ib.user_buffer = NULL;
			ib.buffer = out_buffer;
			ib.offset = out_offset;
			ib.index_size = 2;
		}

		/* Upload the index buffer.
		 * The upload is skipped for small index counts on little-endian machines
		 * and the indices are emitted via PKT3_DRAW_INDEX_IMMD.
		 * Indirect draws never use immediate indices.
		 * Note: Instanced rendering in combination with immediate indices hangs. */
		if (ib.user_buffer && (R600_BIG_ENDIAN || info.indirect ||
						 info.instance_count > 1 ||
						 info.count*ib.index_size > 20)) {
			u_upload_data(rctx->b.uploader, 0, info.count * ib.index_size, 256,
				      ib.user_buffer, &ib.offset, &ib.buffer);
			ib.user_buffer = NULL;
		}
	} else {
		info.index_bias = info.start;
	}

	/* Set the index offset and primitive restart. */
	if (rctx->vgt_state.vgt_multi_prim_ib_reset_en != info.primitive_restart ||
	    rctx->vgt_state.vgt_multi_prim_ib_reset_indx != info.restart_index ||
	    rctx->vgt_state.vgt_indx_offset != info.index_bias ||
	    (rctx->vgt_state.last_draw_was_indirect && !info.indirect)) {
		rctx->vgt_state.vgt_multi_prim_ib_reset_en = info.primitive_restart;
		rctx->vgt_state.vgt_multi_prim_ib_reset_indx = info.restart_index;
		rctx->vgt_state.vgt_indx_offset = info.index_bias;
		r600_mark_atom_dirty(rctx, &rctx->vgt_state.atom);
	}

	/* Workaround for hardware deadlock on certain R600 ASICs: write into a CB register. */
	if (rctx->b.chip_class == R600) {
		rctx->b.flags |= R600_CONTEXT_PS_PARTIAL_FLUSH;
		r600_mark_atom_dirty(rctx, &rctx->cb_misc_state.atom);
	}

	if (rctx->b.chip_class >= EVERGREEN)
		evergreen_setup_tess_constants(rctx, &info, &num_patches);

	/* Emit states. */
	r600_need_cs_space(rctx, ib.user_buffer ? 5 : 0, TRUE);
	r600_flush_emit(rctx);

	mask = rctx->dirty_atoms;
	while (mask != 0) {
		r600_emit_atom(rctx, rctx->atoms[u_bit_scan64(&mask)]);
	}

	if (rctx->b.chip_class == CAYMAN) {
		/* Copied from radeonsi. */
		unsigned primgroup_size = 128; /* recommended without a GS */
		bool ia_switch_on_eop = false;
		bool partial_vs_wave = false;

		if (rctx->gs_shader)
			primgroup_size = 64; /* recommended with a GS */

		if ((rctx->rasterizer && rctx->rasterizer->pa_sc_line_stipple) ||
		    (rctx->b.screen->debug_flags & DBG_SWITCH_ON_EOP)) {
			ia_switch_on_eop = true;
		}

		if (r600_get_strmout_en(&rctx->b))
			partial_vs_wave = true;

		radeon_set_context_reg(cs, CM_R_028AA8_IA_MULTI_VGT_PARAM,
				       S_028AA8_SWITCH_ON_EOP(ia_switch_on_eop) |
				       S_028AA8_PARTIAL_VS_WAVE_ON(partial_vs_wave) |
				       S_028AA8_PRIMGROUP_SIZE(primgroup_size - 1));
	}

	if (rctx->b.chip_class >= EVERGREEN) {
		uint32_t ls_hs_config = evergreen_get_ls_hs_config(rctx, &info,
								   num_patches);

		evergreen_set_ls_hs_config(rctx, cs, ls_hs_config);
		evergreen_set_lds_alloc(rctx, cs, rctx->lds_alloc);
	}

	/* On R6xx, CULL_FRONT=1 culls all points, lines, and rectangles,
	 * even though it should have no effect on those. */
	if (rctx->b.chip_class == R600 && rctx->rasterizer) {
		unsigned su_sc_mode_cntl = rctx->rasterizer->pa_su_sc_mode_cntl;
		unsigned prim = info.mode;

		if (rctx->gs_shader) {
			prim = rctx->gs_shader->gs_output_prim;
		}
		prim = r600_conv_prim_to_gs_out(prim); /* decrease the number of types to 3 */

		if (prim == V_028A6C_OUTPRIM_TYPE_POINTLIST ||
		    prim == V_028A6C_OUTPRIM_TYPE_LINESTRIP ||
		    info.mode == R600_PRIM_RECTANGLE_LIST) {
			su_sc_mode_cntl &= C_028814_CULL_FRONT;
		}
		radeon_set_context_reg(cs, R_028814_PA_SU_SC_MODE_CNTL, su_sc_mode_cntl);
	}

	/* Update start instance. */
	if (!info.indirect && rctx->last_start_instance != info.start_instance) {
		radeon_set_ctl_const(cs, R_03CFF4_SQ_VTX_START_INST_LOC, info.start_instance);
		rctx->last_start_instance = info.start_instance;
	}

	/* Update the primitive type. */
	if (rctx->last_primitive_type != info.mode) {
		unsigned ls_mask = 0;

		if (info.mode == PIPE_PRIM_LINES)
			ls_mask = 1;
		else if (info.mode == PIPE_PRIM_LINE_STRIP ||
			 info.mode == PIPE_PRIM_LINE_LOOP)
			ls_mask = 2;

		radeon_set_context_reg(cs, R_028A0C_PA_SC_LINE_STIPPLE,
				       S_028A0C_AUTO_RESET_CNTL(ls_mask) |
				       (rctx->rasterizer ? rctx->rasterizer->pa_sc_line_stipple : 0));
		radeon_set_config_reg(cs, R_008958_VGT_PRIMITIVE_TYPE,
				      r600_conv_pipe_prim(info.mode));

		rctx->last_primitive_type = info.mode;
	}

	/* Draw packets. */
	if (!info.indirect) {
		radeon_emit(cs, PKT3(PKT3_NUM_INSTANCES, 0, 0));
		radeon_emit(cs, info.instance_count);
	}

	if (unlikely(info.indirect)) {
		uint64_t va = r600_resource(info.indirect)->gpu_address;
		assert(rctx->b.chip_class >= EVERGREEN);

		// Invalidate so non-indirect draw calls reset this state
		rctx->vgt_state.last_draw_was_indirect = true;
		rctx->last_start_instance = -1;

		radeon_emit(cs, PKT3(EG_PKT3_SET_BASE, 2, 0));
		radeon_emit(cs, EG_DRAW_INDEX_INDIRECT_PATCH_TABLE_BASE);
		radeon_emit(cs, va);
		radeon_emit(cs, (va >> 32UL) & 0xFF);

		radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
		radeon_emit(cs, radeon_add_to_buffer_list(&rctx->b, &rctx->b.gfx,
							  (struct r600_resource*)info.indirect,
							  RADEON_USAGE_READ,
                                                          RADEON_PRIO_DRAW_INDIRECT));
	}

	if (info.indexed) {
		radeon_emit(cs, PKT3(PKT3_INDEX_TYPE, 0, 0));
		radeon_emit(cs, ib.index_size == 4 ?
				(VGT_INDEX_32 | (R600_BIG_ENDIAN ? VGT_DMA_SWAP_32_BIT : 0)) :
				(VGT_INDEX_16 | (R600_BIG_ENDIAN ? VGT_DMA_SWAP_16_BIT : 0)));

		if (ib.user_buffer) {
			unsigned size_bytes = info.count*ib.index_size;
			unsigned size_dw = align(size_bytes, 4) / 4;
			radeon_emit(cs, PKT3(PKT3_DRAW_INDEX_IMMD, 1 + size_dw, render_cond_bit));
			radeon_emit(cs, info.count);
			radeon_emit(cs, V_0287F0_DI_SRC_SEL_IMMEDIATE);
			radeon_emit_array(cs, ib.user_buffer, size_dw);
		} else {
			uint64_t va = r600_resource(ib.buffer)->gpu_address + ib.offset;

			if (likely(!info.indirect)) {
				radeon_emit(cs, PKT3(PKT3_DRAW_INDEX, 3, render_cond_bit));
				radeon_emit(cs, va);
				radeon_emit(cs, (va >> 32UL) & 0xFF);
				radeon_emit(cs, info.count);
				radeon_emit(cs, V_0287F0_DI_SRC_SEL_DMA);
				radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
				radeon_emit(cs, radeon_add_to_buffer_list(&rctx->b, &rctx->b.gfx,
									  (struct r600_resource*)ib.buffer,
									  RADEON_USAGE_READ,
                                                                          RADEON_PRIO_INDEX_BUFFER));
			}
			else {
				uint32_t max_size = (ib.buffer->width0 - ib.offset) / ib.index_size;

				radeon_emit(cs, PKT3(EG_PKT3_INDEX_BASE, 1, 0));
				radeon_emit(cs, va);
				radeon_emit(cs, (va >> 32UL) & 0xFF);

				radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
				radeon_emit(cs, radeon_add_to_buffer_list(&rctx->b, &rctx->b.gfx,
									  (struct r600_resource*)ib.buffer,
									  RADEON_USAGE_READ,
                                                                          RADEON_PRIO_INDEX_BUFFER));

				radeon_emit(cs, PKT3(EG_PKT3_INDEX_BUFFER_SIZE, 0, 0));
				radeon_emit(cs, max_size);

				radeon_emit(cs, PKT3(EG_PKT3_DRAW_INDEX_INDIRECT, 1, render_cond_bit));
				radeon_emit(cs, info.indirect_offset);
				radeon_emit(cs, V_0287F0_DI_SRC_SEL_DMA);
			}
		}
	} else {
		if (unlikely(info.count_from_stream_output)) {
			struct r600_so_target *t = (struct r600_so_target*)info.count_from_stream_output;
			uint64_t va = t->buf_filled_size->gpu_address + t->buf_filled_size_offset;

			radeon_set_context_reg(cs, R_028B30_VGT_STRMOUT_DRAW_OPAQUE_VERTEX_STRIDE, t->stride_in_dw);

			radeon_emit(cs, PKT3(PKT3_COPY_DW, 4, 0));
			radeon_emit(cs, COPY_DW_SRC_IS_MEM | COPY_DW_DST_IS_REG);
			radeon_emit(cs, va & 0xFFFFFFFFUL);     /* src address lo */
			radeon_emit(cs, (va >> 32UL) & 0xFFUL); /* src address hi */
			radeon_emit(cs, R_028B2C_VGT_STRMOUT_DRAW_OPAQUE_BUFFER_FILLED_SIZE >> 2); /* dst register */
			radeon_emit(cs, 0); /* unused */

			radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
			radeon_emit(cs, radeon_add_to_buffer_list(&rctx->b, &rctx->b.gfx,
								  t->buf_filled_size, RADEON_USAGE_READ,
								  RADEON_PRIO_SO_FILLED_SIZE));
		}

		if (likely(!info.indirect)) {
			radeon_emit(cs, PKT3(PKT3_DRAW_INDEX_AUTO, 1, render_cond_bit));
			radeon_emit(cs, info.count);
		}
		else {
			radeon_emit(cs, PKT3(EG_PKT3_DRAW_INDIRECT, 1, render_cond_bit));
			radeon_emit(cs, info.indirect_offset);
		}
		radeon_emit(cs, V_0287F0_DI_SRC_SEL_AUTO_INDEX |
				(info.count_from_stream_output ? S_0287F0_USE_OPAQUE(1) : 0));
	}

	/* SMX returns CONTEXT_DONE too early workaround */
	if (rctx->b.family == CHIP_R600 ||
	    rctx->b.family == CHIP_RV610 ||
	    rctx->b.family == CHIP_RV630 ||
	    rctx->b.family == CHIP_RV635) {
		/* if we have gs shader or streamout
		   we need to do a wait idle after every draw */
		if (rctx->gs_shader || r600_get_strmout_en(&rctx->b)) {
			radeon_set_config_reg(cs, R_008040_WAIT_UNTIL, S_008040_WAIT_3D_IDLE(1));
		}
	}

	/* ES ring rolling over at EOP - workaround */
	if (rctx->b.chip_class == R600) {
		radeon_emit(cs, PKT3(PKT3_EVENT_WRITE, 0, 0));
		radeon_emit(cs, EVENT_TYPE(EVENT_TYPE_SQ_NON_EVENT));
	}

	/* Set the depth buffer as dirty. */
	if (rctx->framebuffer.state.zsbuf) {
		struct pipe_surface *surf = rctx->framebuffer.state.zsbuf;
		struct r600_texture *rtex = (struct r600_texture *)surf->texture;

		rtex->dirty_level_mask |= 1 << surf->u.tex.level;

		if (rtex->surface.flags & RADEON_SURF_SBUFFER)
			rtex->stencil_dirty_level_mask |= 1 << surf->u.tex.level;
	}
	if (rctx->framebuffer.compressed_cb_mask) {
		struct pipe_surface *surf;
		struct r600_texture *rtex;
		unsigned mask = rctx->framebuffer.compressed_cb_mask;

		do {
			unsigned i = u_bit_scan(&mask);
			surf = rctx->framebuffer.state.cbufs[i];
			rtex = (struct r600_texture*)surf->texture;

			rtex->dirty_level_mask |= 1 << surf->u.tex.level;

		} while (mask);
	}

	pipe_resource_reference(&ib.buffer, NULL);
	rctx->b.num_draw_calls++;
}

uint32_t r600_translate_stencil_op(int s_op)
{
	switch (s_op) {
	case PIPE_STENCIL_OP_KEEP:
		return V_028800_STENCIL_KEEP;
	case PIPE_STENCIL_OP_ZERO:
		return V_028800_STENCIL_ZERO;
	case PIPE_STENCIL_OP_REPLACE:
		return V_028800_STENCIL_REPLACE;
	case PIPE_STENCIL_OP_INCR:
		return V_028800_STENCIL_INCR;
	case PIPE_STENCIL_OP_DECR:
		return V_028800_STENCIL_DECR;
	case PIPE_STENCIL_OP_INCR_WRAP:
		return V_028800_STENCIL_INCR_WRAP;
	case PIPE_STENCIL_OP_DECR_WRAP:
		return V_028800_STENCIL_DECR_WRAP;
	case PIPE_STENCIL_OP_INVERT:
		return V_028800_STENCIL_INVERT;
	default:
		R600_ERR("Unknown stencil op %d", s_op);
		assert(0);
		break;
	}
	return 0;
}

uint32_t r600_translate_fill(uint32_t func)
{
	switch(func) {
	case PIPE_POLYGON_MODE_FILL:
		return 2;
	case PIPE_POLYGON_MODE_LINE:
		return 1;
	case PIPE_POLYGON_MODE_POINT:
		return 0;
	default:
		assert(0);
		return 0;
	}
}

unsigned r600_tex_wrap(unsigned wrap)
{
	switch (wrap) {
	default:
	case PIPE_TEX_WRAP_REPEAT:
		return V_03C000_SQ_TEX_WRAP;
	case PIPE_TEX_WRAP_CLAMP:
		return V_03C000_SQ_TEX_CLAMP_HALF_BORDER;
	case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
		return V_03C000_SQ_TEX_CLAMP_LAST_TEXEL;
	case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
		return V_03C000_SQ_TEX_CLAMP_BORDER;
	case PIPE_TEX_WRAP_MIRROR_REPEAT:
		return V_03C000_SQ_TEX_MIRROR;
	case PIPE_TEX_WRAP_MIRROR_CLAMP:
		return V_03C000_SQ_TEX_MIRROR_ONCE_HALF_BORDER;
	case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
		return V_03C000_SQ_TEX_MIRROR_ONCE_LAST_TEXEL;
	case PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
		return V_03C000_SQ_TEX_MIRROR_ONCE_BORDER;
	}
}

unsigned r600_tex_mipfilter(unsigned filter)
{
	switch (filter) {
	case PIPE_TEX_MIPFILTER_NEAREST:
		return V_03C000_SQ_TEX_Z_FILTER_POINT;
	case PIPE_TEX_MIPFILTER_LINEAR:
		return V_03C000_SQ_TEX_Z_FILTER_LINEAR;
	default:
	case PIPE_TEX_MIPFILTER_NONE:
		return V_03C000_SQ_TEX_Z_FILTER_NONE;
	}
}

unsigned r600_tex_compare(unsigned compare)
{
	switch (compare) {
	default:
	case PIPE_FUNC_NEVER:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_NEVER;
	case PIPE_FUNC_LESS:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_LESS;
	case PIPE_FUNC_EQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_EQUAL;
	case PIPE_FUNC_LEQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_LESSEQUAL;
	case PIPE_FUNC_GREATER:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_GREATER;
	case PIPE_FUNC_NOTEQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_NOTEQUAL;
	case PIPE_FUNC_GEQUAL:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_GREATEREQUAL;
	case PIPE_FUNC_ALWAYS:
		return V_03C000_SQ_TEX_DEPTH_COMPARE_ALWAYS;
	}
}

static bool wrap_mode_uses_border_color(unsigned wrap, bool linear_filter)
{
	return wrap == PIPE_TEX_WRAP_CLAMP_TO_BORDER ||
	       wrap == PIPE_TEX_WRAP_MIRROR_CLAMP_TO_BORDER ||
	       (linear_filter &&
	        (wrap == PIPE_TEX_WRAP_CLAMP ||
		 wrap == PIPE_TEX_WRAP_MIRROR_CLAMP));
}

bool sampler_state_needs_border_color(const struct pipe_sampler_state *state)
{
	bool linear_filter = state->min_img_filter != PIPE_TEX_FILTER_NEAREST ||
			     state->mag_img_filter != PIPE_TEX_FILTER_NEAREST;

	return (state->border_color.ui[0] || state->border_color.ui[1] ||
		state->border_color.ui[2] || state->border_color.ui[3]) &&
	       (wrap_mode_uses_border_color(state->wrap_s, linear_filter) ||
		wrap_mode_uses_border_color(state->wrap_t, linear_filter) ||
		wrap_mode_uses_border_color(state->wrap_r, linear_filter));
}

void r600_emit_shader(struct r600_context *rctx, struct r600_atom *a)
{

	struct radeon_winsys_cs *cs = rctx->b.gfx.cs;
	struct r600_pipe_shader *shader = ((struct r600_shader_state*)a)->shader;

	if (!shader)
		return;

	r600_emit_command_buffer(cs, &shader->command_buffer);
	radeon_emit(cs, PKT3(PKT3_NOP, 0, 0));
	radeon_emit(cs, radeon_add_to_buffer_list(&rctx->b, &rctx->b.gfx, shader->bo,
					      RADEON_USAGE_READ, RADEON_PRIO_SHADER_BINARY));
}

unsigned r600_get_swizzle_combined(const unsigned char *swizzle_format,
				   const unsigned char *swizzle_view,
				   boolean vtx)
{
	unsigned i;
	unsigned char swizzle[4];
	unsigned result = 0;
	const uint32_t tex_swizzle_shift[4] = {
		16, 19, 22, 25,
	};
	const uint32_t vtx_swizzle_shift[4] = {
		3, 6, 9, 12,
	};
	const uint32_t swizzle_bit[4] = {
		0, 1, 2, 3,
	};
	const uint32_t *swizzle_shift = tex_swizzle_shift;

	if (vtx)
		swizzle_shift = vtx_swizzle_shift;

	if (swizzle_view) {
		util_format_compose_swizzles(swizzle_format, swizzle_view, swizzle);
	} else {
		memcpy(swizzle, swizzle_format, 4);
	}

	/* Get swizzle. */
	for (i = 0; i < 4; i++) {
		switch (swizzle[i]) {
		case PIPE_SWIZZLE_Y:
			result |= swizzle_bit[1] << swizzle_shift[i];
			break;
		case PIPE_SWIZZLE_Z:
			result |= swizzle_bit[2] << swizzle_shift[i];
			break;
		case PIPE_SWIZZLE_W:
			result |= swizzle_bit[3] << swizzle_shift[i];
			break;
		case PIPE_SWIZZLE_0:
			result |= V_038010_SQ_SEL_0 << swizzle_shift[i];
			break;
		case PIPE_SWIZZLE_1:
			result |= V_038010_SQ_SEL_1 << swizzle_shift[i];
			break;
		default: /* PIPE_SWIZZLE_X */
			result |= swizzle_bit[0] << swizzle_shift[i];
		}
	}
	return result;
}

/* texture format translate */
uint32_t r600_translate_texformat(struct pipe_screen *screen,
				  enum pipe_format format,
				  const unsigned char *swizzle_view,
				  uint32_t *word4_p, uint32_t *yuv_format_p,
				  bool do_endian_swap)
{
	struct r600_screen *rscreen = (struct r600_screen *)screen;
	uint32_t result = 0, word4 = 0, yuv_format = 0;
	const struct util_format_description *desc;
	boolean uniform = TRUE;
	bool is_srgb_valid = FALSE;
	const unsigned char swizzle_xxxx[4] = {0, 0, 0, 0};
	const unsigned char swizzle_yyyy[4] = {1, 1, 1, 1};
	const unsigned char swizzle_xxxy[4] = {0, 0, 0, 1};
	const unsigned char swizzle_zyx1[4] = {2, 1, 0, 5};
	const unsigned char swizzle_zyxw[4] = {2, 1, 0, 3};

	int i;
	const uint32_t sign_bit[4] = {
		S_038010_FORMAT_COMP_X(V_038010_SQ_FORMAT_COMP_SIGNED),
		S_038010_FORMAT_COMP_Y(V_038010_SQ_FORMAT_COMP_SIGNED),
		S_038010_FORMAT_COMP_Z(V_038010_SQ_FORMAT_COMP_SIGNED),
		S_038010_FORMAT_COMP_W(V_038010_SQ_FORMAT_COMP_SIGNED)
	};

	/* Need to replace the specified texture formats in case of big-endian.
	 * These formats are formats that have channels with number of bits
	 * not divisible by 8.
	 * Mesa conversion functions don't swap bits for those formats, and because
	 * we transmit this over a serial bus to the GPU (PCIe), the
	 * bit-endianess is important!!!
	 * In case we have an "opposite" format, just use that for the swizzling
	 * information. If we don't have such an "opposite" format, we need
	 * to use a fixed swizzle info instead (see below)
	 */
	if (format == PIPE_FORMAT_R4A4_UNORM && do_endian_swap)
		format = PIPE_FORMAT_A4R4_UNORM;

	desc = util_format_description(format);

	/* Depth and stencil swizzling is handled separately. */
	if (desc->colorspace != UTIL_FORMAT_COLORSPACE_ZS) {
		/* Need to check for specific texture formats that don't have
		 * an "opposite" format we can use. For those formats, we directly
		 * specify the swizzling, which is the LE swizzling as defined in
		 * u_format.csv
		 */
		if (do_endian_swap) {
			if (format == PIPE_FORMAT_L4A4_UNORM)
				word4 |= r600_get_swizzle_combined(swizzle_xxxy, swizzle_view, FALSE);
			else if (format == PIPE_FORMAT_B4G4R4A4_UNORM)
				word4 |= r600_get_swizzle_combined(swizzle_zyxw, swizzle_view, FALSE);
			else if (format == PIPE_FORMAT_B4G4R4X4_UNORM || format == PIPE_FORMAT_B5G6R5_UNORM)
				word4 |= r600_get_swizzle_combined(swizzle_zyx1, swizzle_view, FALSE);
			else
				word4 |= r600_get_swizzle_combined(desc->swizzle, swizzle_view, FALSE);
		} else {
			word4 |= r600_get_swizzle_combined(desc->swizzle, swizzle_view, FALSE);
		}
	}

	/* Colorspace (return non-RGB formats directly). */
	switch (desc->colorspace) {
	/* Depth stencil formats */
	case UTIL_FORMAT_COLORSPACE_ZS:
		switch (format) {
		/* Depth sampler formats. */
		case PIPE_FORMAT_Z16_UNORM:
			word4 |= r600_get_swizzle_combined(swizzle_xxxx, swizzle_view, FALSE);
			result = FMT_16;
			goto out_word4;
		case PIPE_FORMAT_Z24X8_UNORM:
		case PIPE_FORMAT_Z24_UNORM_S8_UINT:
			word4 |= r600_get_swizzle_combined(swizzle_xxxx, swizzle_view, FALSE);
			result = FMT_8_24;
			goto out_word4;
		case PIPE_FORMAT_X8Z24_UNORM:
		case PIPE_FORMAT_S8_UINT_Z24_UNORM:
			if (rscreen->b.chip_class < EVERGREEN)
				goto out_unknown;
			word4 |= r600_get_swizzle_combined(swizzle_yyyy, swizzle_view, FALSE);
			result = FMT_24_8;
			goto out_word4;
		case PIPE_FORMAT_Z32_FLOAT:
			word4 |= r600_get_swizzle_combined(swizzle_xxxx, swizzle_view, FALSE);
			result = FMT_32_FLOAT;
			goto out_word4;
		case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
			word4 |= r600_get_swizzle_combined(swizzle_xxxx, swizzle_view, FALSE);
			result = FMT_X24_8_32_FLOAT;
			goto out_word4;
		/* Stencil sampler formats. */
		case PIPE_FORMAT_S8_UINT:
			word4 |= S_038010_NUM_FORMAT_ALL(V_038010_SQ_NUM_FORMAT_INT);
			word4 |= r600_get_swizzle_combined(swizzle_xxxx, swizzle_view, FALSE);
			result = FMT_8;
			goto out_word4;
		case PIPE_FORMAT_X24S8_UINT:
			word4 |= S_038010_NUM_FORMAT_ALL(V_038010_SQ_NUM_FORMAT_INT);
			word4 |= r600_get_swizzle_combined(swizzle_yyyy, swizzle_view, FALSE);
			result = FMT_8_24;
			goto out_word4;
		case PIPE_FORMAT_S8X24_UINT:
			if (rscreen->b.chip_class < EVERGREEN)
				goto out_unknown;
			word4 |= S_038010_NUM_FORMAT_ALL(V_038010_SQ_NUM_FORMAT_INT);
			word4 |= r600_get_swizzle_combined(swizzle_xxxx, swizzle_view, FALSE);
			result = FMT_24_8;
			goto out_word4;
		case PIPE_FORMAT_X32_S8X24_UINT:
			word4 |= S_038010_NUM_FORMAT_ALL(V_038010_SQ_NUM_FORMAT_INT);
			word4 |= r600_get_swizzle_combined(swizzle_yyyy, swizzle_view, FALSE);
			result = FMT_X24_8_32_FLOAT;
			goto out_word4;
		default:
			goto out_unknown;
		}

	case UTIL_FORMAT_COLORSPACE_YUV:
		yuv_format |= (1 << 30);
		switch (format) {
		case PIPE_FORMAT_UYVY:
		case PIPE_FORMAT_YUYV:
		default:
			break;
		}
		goto out_unknown; /* XXX */

	case UTIL_FORMAT_COLORSPACE_SRGB:
		word4 |= S_038010_FORCE_DEGAMMA(1);
		break;

	default:
		break;
	}

	if (desc->layout == UTIL_FORMAT_LAYOUT_RGTC) {
		switch (format) {
		case PIPE_FORMAT_RGTC1_SNORM:
		case PIPE_FORMAT_LATC1_SNORM:
			word4 |= sign_bit[0];
		case PIPE_FORMAT_RGTC1_UNORM:
		case PIPE_FORMAT_LATC1_UNORM:
			result = FMT_BC4;
			goto out_word4;
		case PIPE_FORMAT_RGTC2_SNORM:
		case PIPE_FORMAT_LATC2_SNORM:
			word4 |= sign_bit[0] | sign_bit[1];
		case PIPE_FORMAT_RGTC2_UNORM:
		case PIPE_FORMAT_LATC2_UNORM:
			result = FMT_BC5;
			goto out_word4;
		default:
			goto out_unknown;
		}
	}

	if (desc->layout == UTIL_FORMAT_LAYOUT_S3TC) {
		if (!util_format_s3tc_enabled) {
			goto out_unknown;
		}

		switch (format) {
		case PIPE_FORMAT_DXT1_RGB:
		case PIPE_FORMAT_DXT1_RGBA:
		case PIPE_FORMAT_DXT1_SRGB:
		case PIPE_FORMAT_DXT1_SRGBA:
			result = FMT_BC1;
			is_srgb_valid = TRUE;
			goto out_word4;
		case PIPE_FORMAT_DXT3_RGBA:
		case PIPE_FORMAT_DXT3_SRGBA:
			result = FMT_BC2;
			is_srgb_valid = TRUE;
			goto out_word4;
		case PIPE_FORMAT_DXT5_RGBA:
		case PIPE_FORMAT_DXT5_SRGBA:
			result = FMT_BC3;
			is_srgb_valid = TRUE;
			goto out_word4;
		default:
			goto out_unknown;
		}
	}

	if (desc->layout == UTIL_FORMAT_LAYOUT_BPTC) {
		if (rscreen->b.chip_class < EVERGREEN)
			goto out_unknown;

		switch (format) {
			case PIPE_FORMAT_BPTC_RGBA_UNORM:
			case PIPE_FORMAT_BPTC_SRGBA:
				result = FMT_BC7;
				is_srgb_valid = TRUE;
				goto out_word4;
			case PIPE_FORMAT_BPTC_RGB_FLOAT:
				word4 |= sign_bit[0] | sign_bit[1] | sign_bit[2];
				/* fall through */
			case PIPE_FORMAT_BPTC_RGB_UFLOAT:
				result = FMT_BC6;
				goto out_word4;
			default:
				goto out_unknown;
		}
	}

	if (desc->layout == UTIL_FORMAT_LAYOUT_SUBSAMPLED) {
		switch (format) {
		case PIPE_FORMAT_R8G8_B8G8_UNORM:
		case PIPE_FORMAT_G8R8_B8R8_UNORM:
			result = FMT_GB_GR;
			goto out_word4;
		case PIPE_FORMAT_G8R8_G8B8_UNORM:
		case PIPE_FORMAT_R8G8_R8B8_UNORM:
			result = FMT_BG_RG;
			goto out_word4;
		default:
			goto out_unknown;
		}
	}

	if (format == PIPE_FORMAT_R9G9B9E5_FLOAT) {
		result = FMT_5_9_9_9_SHAREDEXP;
		goto out_word4;
	} else if (format == PIPE_FORMAT_R11G11B10_FLOAT) {
		result = FMT_10_11_11_FLOAT;
		goto out_word4;
	}


	for (i = 0; i < desc->nr_channels; i++) {
		if (desc->channel[i].type == UTIL_FORMAT_TYPE_SIGNED) {
			word4 |= sign_bit[i];
		}
	}

	/* R8G8Bx_SNORM - XXX CxV8U8 */

	/* See whether the components are of the same size. */
	for (i = 1; i < desc->nr_channels; i++) {
		uniform = uniform && desc->channel[0].size == desc->channel[i].size;
	}

	/* Non-uniform formats. */
	if (!uniform) {
		if (desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB &&
		    desc->channel[0].pure_integer)
			word4 |= S_038010_NUM_FORMAT_ALL(V_038010_SQ_NUM_FORMAT_INT);
		switch(desc->nr_channels) {
		case 3:
			if (desc->channel[0].size == 5 &&
			    desc->channel[1].size == 6 &&
			    desc->channel[2].size == 5) {
				result = FMT_5_6_5;
				goto out_word4;
			}
			goto out_unknown;
		case 4:
			if (desc->channel[0].size == 5 &&
			    desc->channel[1].size == 5 &&
			    desc->channel[2].size == 5 &&
			    desc->channel[3].size == 1) {
				result = FMT_1_5_5_5;
				goto out_word4;
			}
			if (desc->channel[0].size == 10 &&
			    desc->channel[1].size == 10 &&
			    desc->channel[2].size == 10 &&
			    desc->channel[3].size == 2) {
				result = FMT_2_10_10_10;
				goto out_word4;
			}
			goto out_unknown;
		}
		goto out_unknown;
	}

	/* Find the first non-VOID channel. */
	for (i = 0; i < 4; i++) {
		if (desc->channel[i].type != UTIL_FORMAT_TYPE_VOID) {
			break;
		}
	}

	if (i == 4)
		goto out_unknown;

	/* uniform formats */
	switch (desc->channel[i].type) {
	case UTIL_FORMAT_TYPE_UNSIGNED:
	case UTIL_FORMAT_TYPE_SIGNED:
#if 0
		if (!desc->channel[i].normalized &&
		    desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB) {
			goto out_unknown;
		}
#endif
		if (desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB &&
		    desc->channel[i].pure_integer)
			word4 |= S_038010_NUM_FORMAT_ALL(V_038010_SQ_NUM_FORMAT_INT);

		switch (desc->channel[i].size) {
		case 4:
			switch (desc->nr_channels) {
			case 2:
				result = FMT_4_4;
				goto out_word4;
			case 4:
				result = FMT_4_4_4_4;
				goto out_word4;
			}
			goto out_unknown;
		case 8:
			switch (desc->nr_channels) {
			case 1:
				result = FMT_8;
				goto out_word4;
			case 2:
				result = FMT_8_8;
				goto out_word4;
			case 4:
				result = FMT_8_8_8_8;
				is_srgb_valid = TRUE;
				goto out_word4;
			}
			goto out_unknown;
		case 16:
			switch (desc->nr_channels) {
			case 1:
				result = FMT_16;
				goto out_word4;
			case 2:
				result = FMT_16_16;
				goto out_word4;
			case 4:
				result = FMT_16_16_16_16;
				goto out_word4;
			}
			goto out_unknown;
		case 32:
			switch (desc->nr_channels) {
			case 1:
				result = FMT_32;
				goto out_word4;
			case 2:
				result = FMT_32_32;
				goto out_word4;
			case 4:
				result = FMT_32_32_32_32;
				goto out_word4;
			}
		}
		goto out_unknown;

	case UTIL_FORMAT_TYPE_FLOAT:
		switch (desc->channel[i].size) {
		case 16:
			switch (desc->nr_channels) {
			case 1:
				result = FMT_16_FLOAT;
				goto out_word4;
			case 2:
				result = FMT_16_16_FLOAT;
				goto out_word4;
			case 4:
				result = FMT_16_16_16_16_FLOAT;
				goto out_word4;
			}
			goto out_unknown;
		case 32:
			switch (desc->nr_channels) {
			case 1:
				result = FMT_32_FLOAT;
				goto out_word4;
			case 2:
				result = FMT_32_32_FLOAT;
				goto out_word4;
			case 4:
				result = FMT_32_32_32_32_FLOAT;
				goto out_word4;
			}
		}
		goto out_unknown;
	}

out_word4:

	if (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB && !is_srgb_valid)
		return ~0;
	if (word4_p)
		*word4_p = word4;
	if (yuv_format_p)
		*yuv_format_p = yuv_format;
	return result;
out_unknown:
	/* R600_ERR("Unable to handle texformat %d %s\n", format, util_format_name(format)); */
	return ~0;
}

uint32_t r600_translate_colorformat(enum chip_class chip, enum pipe_format format,
						bool do_endian_swap)
{
	const struct util_format_description *desc = util_format_description(format);
	int channel = util_format_get_first_non_void_channel(format);
	bool is_float;

#define HAS_SIZE(x,y,z,w) \
	(desc->channel[0].size == (x) && desc->channel[1].size == (y) && \
         desc->channel[2].size == (z) && desc->channel[3].size == (w))

	if (format == PIPE_FORMAT_R11G11B10_FLOAT) /* isn't plain */
		return V_0280A0_COLOR_10_11_11_FLOAT;

	if (desc->layout != UTIL_FORMAT_LAYOUT_PLAIN ||
	    channel == -1)
		return ~0U;

	is_float = desc->channel[channel].type == UTIL_FORMAT_TYPE_FLOAT;

	switch (desc->nr_channels) {
	case 1:
		switch (desc->channel[0].size) {
		case 8:
			return V_0280A0_COLOR_8;
		case 16:
			if (is_float)
				return V_0280A0_COLOR_16_FLOAT;
			else
				return V_0280A0_COLOR_16;
		case 32:
			if (is_float)
				return V_0280A0_COLOR_32_FLOAT;
			else
				return V_0280A0_COLOR_32;
		}
		break;
	case 2:
		if (desc->channel[0].size == desc->channel[1].size) {
			switch (desc->channel[0].size) {
			case 4:
				if (chip <= R700)
					return V_0280A0_COLOR_4_4;
				else
					return ~0U; /* removed on Evergreen */
			case 8:
				return V_0280A0_COLOR_8_8;
			case 16:
				if (is_float)
					return V_0280A0_COLOR_16_16_FLOAT;
				else
					return V_0280A0_COLOR_16_16;
			case 32:
				if (is_float)
					return V_0280A0_COLOR_32_32_FLOAT;
				else
					return V_0280A0_COLOR_32_32;
			}
		} else if (HAS_SIZE(8,24,0,0)) {
			return (do_endian_swap ? V_0280A0_COLOR_8_24 : V_0280A0_COLOR_24_8);
		} else if (HAS_SIZE(24,8,0,0)) {
			return V_0280A0_COLOR_8_24;
		}
		break;
	case 3:
		if (HAS_SIZE(5,6,5,0)) {
			return V_0280A0_COLOR_5_6_5;
		} else if (HAS_SIZE(32,8,24,0)) {
			return V_0280A0_COLOR_X24_8_32_FLOAT;
		}
		break;
	case 4:
		if (desc->channel[0].size == desc->channel[1].size &&
		    desc->channel[0].size == desc->channel[2].size &&
		    desc->channel[0].size == desc->channel[3].size) {
			switch (desc->channel[0].size) {
			case 4:
				return V_0280A0_COLOR_4_4_4_4;
			case 8:
				return V_0280A0_COLOR_8_8_8_8;
			case 16:
				if (is_float)
					return V_0280A0_COLOR_16_16_16_16_FLOAT;
				else
					return V_0280A0_COLOR_16_16_16_16;
			case 32:
				if (is_float)
					return V_0280A0_COLOR_32_32_32_32_FLOAT;
				else
					return V_0280A0_COLOR_32_32_32_32;
			}
		} else if (HAS_SIZE(5,5,5,1)) {
			return V_0280A0_COLOR_1_5_5_5;
		} else if (HAS_SIZE(10,10,10,2)) {
			return V_0280A0_COLOR_2_10_10_10;
		}
		break;
	}
	return ~0U;
}

uint32_t r600_colorformat_endian_swap(uint32_t colorformat, bool do_endian_swap)
{
	if (R600_BIG_ENDIAN) {
		switch(colorformat) {
		/* 8-bit buffers. */
		case V_0280A0_COLOR_4_4:
		case V_0280A0_COLOR_8:
			return ENDIAN_NONE;

		/* 16-bit buffers. */
		case V_0280A0_COLOR_8_8:
			/*
			 * No need to do endian swaps on array formats,
			 * as mesa<-->pipe formats conversion take into account
			 * the endianess
			 */
			return ENDIAN_NONE;

		case V_0280A0_COLOR_5_6_5:
		case V_0280A0_COLOR_1_5_5_5:
		case V_0280A0_COLOR_4_4_4_4:
		case V_0280A0_COLOR_16:
			return (do_endian_swap ? ENDIAN_8IN16 : ENDIAN_NONE);

		/* 32-bit buffers. */
		case V_0280A0_COLOR_8_8_8_8:
			/*
			 * No need to do endian swaps on array formats,
			 * as mesa<-->pipe formats conversion take into account
			 * the endianess
			 */
			return ENDIAN_NONE;

		case V_0280A0_COLOR_2_10_10_10:
		case V_0280A0_COLOR_8_24:
		case V_0280A0_COLOR_24_8:
		case V_0280A0_COLOR_32_FLOAT:
			return (do_endian_swap ? ENDIAN_8IN32 : ENDIAN_NONE);

		case V_0280A0_COLOR_16_16_FLOAT:
		case V_0280A0_COLOR_16_16:
			return ENDIAN_8IN16;

		/* 64-bit buffers. */
		case V_0280A0_COLOR_16_16_16_16:
		case V_0280A0_COLOR_16_16_16_16_FLOAT:
			return ENDIAN_8IN16;

		case V_0280A0_COLOR_32_32_FLOAT:
		case V_0280A0_COLOR_32_32:
		case V_0280A0_COLOR_X24_8_32_FLOAT:
			return ENDIAN_8IN32;

		/* 128-bit buffers. */
		case V_0280A0_COLOR_32_32_32_32_FLOAT:
		case V_0280A0_COLOR_32_32_32_32:
			return ENDIAN_8IN32;
		default:
			return ENDIAN_NONE; /* Unsupported. */
		}
	} else {
		return ENDIAN_NONE;
	}
}

static void r600_invalidate_buffer(struct pipe_context *ctx, struct pipe_resource *buf)
{
	struct r600_context *rctx = (struct r600_context*)ctx;
	struct r600_resource *rbuffer = r600_resource(buf);
	unsigned i, shader, mask;
	struct r600_pipe_sampler_view *view;

	/* Reallocate the buffer in the same pipe_resource. */
	r600_alloc_resource(&rctx->screen->b, rbuffer);

	/* We changed the buffer, now we need to bind it where the old one was bound. */
	/* Vertex buffers. */
	mask = rctx->vertex_buffer_state.enabled_mask;
	while (mask) {
		i = u_bit_scan(&mask);
		if (rctx->vertex_buffer_state.vb[i].buffer == &rbuffer->b.b) {
			rctx->vertex_buffer_state.dirty_mask |= 1 << i;
			r600_vertex_buffers_dirty(rctx);
		}
	}
	/* Streamout buffers. */
	for (i = 0; i < rctx->b.streamout.num_targets; i++) {
		if (rctx->b.streamout.targets[i] &&
		    rctx->b.streamout.targets[i]->b.buffer == &rbuffer->b.b) {
			if (rctx->b.streamout.begin_emitted) {
				r600_emit_streamout_end(&rctx->b);
			}
			rctx->b.streamout.append_bitmask = rctx->b.streamout.enabled_mask;
			r600_streamout_buffers_dirty(&rctx->b);
		}
	}

	/* Constant buffers. */
	for (shader = 0; shader < PIPE_SHADER_TYPES; shader++) {
		struct r600_constbuf_state *state = &rctx->constbuf_state[shader];
		bool found = false;
		uint32_t mask = state->enabled_mask;

		while (mask) {
			unsigned i = u_bit_scan(&mask);
			if (state->cb[i].buffer == &rbuffer->b.b) {
				found = true;
				state->dirty_mask |= 1 << i;
			}
		}
		if (found) {
			r600_constant_buffers_dirty(rctx, state);
		}
	}

	/* Texture buffer objects - update the virtual addresses in descriptors. */
	LIST_FOR_EACH_ENTRY(view, &rctx->b.texture_buffers, list) {
		if (view->base.texture == &rbuffer->b.b) {
			uint64_t offset = view->base.u.buf.offset;
			uint64_t va = rbuffer->gpu_address + offset;

			view->tex_resource_words[0] = va;
			view->tex_resource_words[2] &= C_038008_BASE_ADDRESS_HI;
			view->tex_resource_words[2] |= S_038008_BASE_ADDRESS_HI(va >> 32);
		}
	}
	/* Texture buffer objects - make bindings dirty if needed. */
	for (shader = 0; shader < PIPE_SHADER_TYPES; shader++) {
		struct r600_samplerview_state *state = &rctx->samplers[shader].views;
		bool found = false;
		uint32_t mask = state->enabled_mask;

		while (mask) {
			unsigned i = u_bit_scan(&mask);
			if (state->views[i]->base.texture == &rbuffer->b.b) {
				found = true;
				state->dirty_mask |= 1 << i;
			}
		}
		if (found) {
			r600_sampler_views_dirty(rctx, state);
		}
	}
}

static void r600_set_active_query_state(struct pipe_context *ctx, boolean enable)
{
	struct r600_context *rctx = (struct r600_context*)ctx;

	/* Pipeline stat & streamout queries. */
	if (enable) {
		rctx->b.flags &= ~R600_CONTEXT_STOP_PIPELINE_STATS;
		rctx->b.flags |= R600_CONTEXT_START_PIPELINE_STATS;
	} else {
		rctx->b.flags &= ~R600_CONTEXT_START_PIPELINE_STATS;
		rctx->b.flags |= R600_CONTEXT_STOP_PIPELINE_STATS;
	}

	/* Occlusion queries. */
	if (rctx->db_misc_state.occlusion_queries_disabled != !enable) {
		rctx->db_misc_state.occlusion_queries_disabled = !enable;
		r600_mark_atom_dirty(rctx, &rctx->db_misc_state.atom);
	}
}

static void r600_set_occlusion_query_state(struct pipe_context *ctx, bool enable)
{
	struct r600_context *rctx = (struct r600_context*)ctx;

	r600_mark_atom_dirty(rctx, &rctx->db_misc_state.atom);
}

static void r600_need_gfx_cs_space(struct pipe_context *ctx, unsigned num_dw,
                                   bool include_draw_vbo)
{
	r600_need_cs_space((struct r600_context*)ctx, num_dw, include_draw_vbo);
}

/* keep this at the end of this file, please */
void r600_init_common_state_functions(struct r600_context *rctx)
{
	rctx->b.b.create_fs_state = r600_create_ps_state;
	rctx->b.b.create_vs_state = r600_create_vs_state;
	rctx->b.b.create_gs_state = r600_create_gs_state;
	rctx->b.b.create_tcs_state = r600_create_tcs_state;
	rctx->b.b.create_tes_state = r600_create_tes_state;
	rctx->b.b.create_vertex_elements_state = r600_create_vertex_fetch_shader;
	rctx->b.b.bind_blend_state = r600_bind_blend_state;
	rctx->b.b.bind_depth_stencil_alpha_state = r600_bind_dsa_state;
	rctx->b.b.bind_sampler_states = r600_bind_sampler_states;
	rctx->b.b.bind_fs_state = r600_bind_ps_state;
	rctx->b.b.bind_rasterizer_state = r600_bind_rs_state;
	rctx->b.b.bind_vertex_elements_state = r600_bind_vertex_elements;
	rctx->b.b.bind_vs_state = r600_bind_vs_state;
	rctx->b.b.bind_gs_state = r600_bind_gs_state;
	rctx->b.b.bind_tcs_state = r600_bind_tcs_state;
	rctx->b.b.bind_tes_state = r600_bind_tes_state;
	rctx->b.b.delete_blend_state = r600_delete_blend_state;
	rctx->b.b.delete_depth_stencil_alpha_state = r600_delete_dsa_state;
	rctx->b.b.delete_fs_state = r600_delete_ps_state;
	rctx->b.b.delete_rasterizer_state = r600_delete_rs_state;
	rctx->b.b.delete_sampler_state = r600_delete_sampler_state;
	rctx->b.b.delete_vertex_elements_state = r600_delete_vertex_elements;
	rctx->b.b.delete_vs_state = r600_delete_vs_state;
	rctx->b.b.delete_gs_state = r600_delete_gs_state;
	rctx->b.b.delete_tcs_state = r600_delete_tcs_state;
	rctx->b.b.delete_tes_state = r600_delete_tes_state;
	rctx->b.b.set_blend_color = r600_set_blend_color;
	rctx->b.b.set_clip_state = r600_set_clip_state;
	rctx->b.b.set_constant_buffer = r600_set_constant_buffer;
	rctx->b.b.set_sample_mask = r600_set_sample_mask;
	rctx->b.b.set_stencil_ref = r600_set_pipe_stencil_ref;
	rctx->b.b.set_vertex_buffers = r600_set_vertex_buffers;
	rctx->b.b.set_index_buffer = r600_set_index_buffer;
	rctx->b.b.set_sampler_views = r600_set_sampler_views;
	rctx->b.b.sampler_view_destroy = r600_sampler_view_destroy;
	rctx->b.b.texture_barrier = r600_texture_barrier;
	rctx->b.b.set_stream_output_targets = r600_set_streamout_targets;
	rctx->b.b.set_active_query_state = r600_set_active_query_state;
	rctx->b.b.draw_vbo = r600_draw_vbo;
	rctx->b.invalidate_buffer = r600_invalidate_buffer;
	rctx->b.set_occlusion_query_state = r600_set_occlusion_query_state;
	rctx->b.need_gfx_cs_space = r600_need_gfx_cs_space;
}
