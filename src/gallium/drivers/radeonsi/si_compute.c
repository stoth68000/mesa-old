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
 */

#include "tgsi/tgsi_parse.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include "radeon/r600_pipe_common.h"
#include "radeon/radeon_elf_util.h"

#include "radeon/r600_cs.h"
#include "si_pipe.h"
#include "si_shader.h"
#include "sid.h"

#define MAX_GLOBAL_BUFFERS 20

struct si_compute {
	unsigned ir_type;
	unsigned local_size;
	unsigned private_size;
	unsigned input_size;
	struct si_shader shader;

	struct pipe_resource *global_buffers[MAX_GLOBAL_BUFFERS];
};

static void *si_create_compute_state(
	struct pipe_context *ctx,
	const struct pipe_compute_state *cso)
{
	struct si_context *sctx = (struct si_context *)ctx;
	struct si_screen *sscreen = (struct si_screen *)ctx->screen;
	struct si_compute *program = CALLOC_STRUCT(si_compute);
	struct si_shader *shader = &program->shader;


	program->ir_type = cso->ir_type;
	program->local_size = cso->req_local_mem;
	program->private_size = cso->req_private_mem;
	program->input_size = cso->req_input_mem;


	if (cso->ir_type == PIPE_SHADER_IR_TGSI) {
		struct si_shader_selector sel;
		bool scratch_enabled;

		memset(&sel, 0, sizeof(sel));

		sel.tokens = tgsi_dup_tokens(cso->prog);
		if (!sel.tokens) {
			FREE(program);
			return NULL;
		}

		tgsi_scan_shader(cso->prog, &sel.info);
		sel.type = PIPE_SHADER_COMPUTE;
		sel.local_size = cso->req_local_mem;

		p_atomic_inc(&sscreen->b.num_shaders_created);

		program->shader.selector = &sel;

		if (si_shader_create(sscreen, sctx->tm, &program->shader,
		                     &sctx->b.debug)) {
			FREE(sel.tokens);
			FREE(program);
			return NULL;
		}

		scratch_enabled = shader->config.scratch_bytes_per_wave > 0;

		shader->config.rsrc1 =
			   S_00B848_VGPRS((shader->config.num_vgprs - 1) / 4) |
			   S_00B848_SGPRS((shader->config.num_sgprs - 1) / 8) |
			   S_00B848_DX10_CLAMP(1) |
			   S_00B848_FLOAT_MODE(shader->config.float_mode);

		shader->config.rsrc2 = S_00B84C_USER_SGPR(SI_CS_NUM_USER_SGPR) |
			   S_00B84C_SCRATCH_EN(scratch_enabled) |
			   S_00B84C_TGID_X_EN(1) | S_00B84C_TGID_Y_EN(1) |
			   S_00B84C_TGID_Z_EN(1) | S_00B84C_TIDIG_COMP_CNT(2) |
			   S_00B84C_LDS_SIZE(shader->config.lds_size);

		FREE(sel.tokens);
	} else {
		const struct pipe_llvm_program_header *header;
		const char *code;
		header = cso->prog;
		code = cso->prog + sizeof(struct pipe_llvm_program_header);

		radeon_elf_read(code, header->num_bytes, &program->shader.binary);
		si_shader_binary_read_config(&program->shader.binary,
			     &program->shader.config, 0);
		si_shader_dump(sctx->screen, &program->shader, &sctx->b.debug,
			       PIPE_SHADER_COMPUTE, stderr);
		si_shader_binary_upload(sctx->screen, &program->shader);
	}

	return program;
}

static void si_bind_compute_state(struct pipe_context *ctx, void *state)
{
	struct si_context *sctx = (struct si_context*)ctx;
	sctx->cs_shader_state.program = (struct si_compute*)state;
}

static void si_set_global_binding(
	struct pipe_context *ctx, unsigned first, unsigned n,
	struct pipe_resource **resources,
	uint32_t **handles)
{
	unsigned i;
	struct si_context *sctx = (struct si_context*)ctx;
	struct si_compute *program = sctx->cs_shader_state.program;

	if (!resources) {
		for (i = first; i < first + n; i++) {
			pipe_resource_reference(&program->global_buffers[i], NULL);
		}
		return;
	}

	for (i = first; i < first + n; i++) {
		uint64_t va;
		uint32_t offset;
		pipe_resource_reference(&program->global_buffers[i], resources[i]);
		va = r600_resource(resources[i])->gpu_address;
		offset = util_le32_to_cpu(*handles[i]);
		va += offset;
		va = util_cpu_to_le64(va);
		memcpy(handles[i], &va, sizeof(va));
	}
}

static void si_initialize_compute(struct si_context *sctx)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;

	radeon_set_sh_reg_seq(cs, R_00B810_COMPUTE_START_X, 3);
	radeon_emit(cs, 0);
	radeon_emit(cs, 0);
	radeon_emit(cs, 0);

	radeon_set_sh_reg_seq(cs, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0, 2);
	/* R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0 / SE1 */
	radeon_emit(cs, S_00B858_SH0_CU_EN(0xffff) | S_00B858_SH1_CU_EN(0xffff));
	radeon_emit(cs, S_00B85C_SH0_CU_EN(0xffff) | S_00B85C_SH1_CU_EN(0xffff));

	if (sctx->b.chip_class >= CIK) {
		/* Also set R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE2 / SE3 */
		radeon_set_sh_reg_seq(cs,
		                     R_00B864_COMPUTE_STATIC_THREAD_MGMT_SE2, 2);
		radeon_emit(cs, S_00B864_SH0_CU_EN(0xffff) |
		                S_00B864_SH1_CU_EN(0xffff));
		radeon_emit(cs, S_00B868_SH0_CU_EN(0xffff) |
		                S_00B868_SH1_CU_EN(0xffff));
	}

	/* This register has been moved to R_00CD20_COMPUTE_MAX_WAVE_ID
	 * and is now per pipe, so it should be handled in the
	 * kernel if we want to use something other than the default value,
	 * which is now 0x22f.
	 */
	if (sctx->b.chip_class <= SI) {
		/* XXX: This should be:
		 * (number of compute units) * 4 * (waves per simd) - 1 */

		radeon_set_sh_reg(cs, R_00B82C_COMPUTE_MAX_WAVE_ID,
		                  0x190 /* Default value */);
	}

	sctx->cs_shader_state.emitted_program = NULL;
	sctx->cs_shader_state.initialized = true;
}

static bool si_setup_compute_scratch_buffer(struct si_context *sctx,
                                            struct si_shader *shader,
                                            struct si_shader_config *config)
{
	uint64_t scratch_bo_size, scratch_needed;
	scratch_bo_size = 0;
	scratch_needed = config->scratch_bytes_per_wave * sctx->scratch_waves;
	if (sctx->compute_scratch_buffer)
		scratch_bo_size = sctx->compute_scratch_buffer->b.b.width0;

	if (scratch_bo_size < scratch_needed) {
		r600_resource_reference(&sctx->compute_scratch_buffer, NULL);

		sctx->compute_scratch_buffer =
				si_resource_create_custom(&sctx->screen->b.b,
                                PIPE_USAGE_DEFAULT, scratch_needed);

		if (!sctx->compute_scratch_buffer)
			return false;
	}

	if (sctx->compute_scratch_buffer != shader->scratch_bo && scratch_needed) {
		uint64_t scratch_va = sctx->compute_scratch_buffer->gpu_address;

		si_shader_apply_scratch_relocs(sctx, shader, config, scratch_va);

		if (si_shader_binary_upload(sctx->screen, shader))
			return false;

		r600_resource_reference(&shader->scratch_bo,
		                        sctx->compute_scratch_buffer);
	}

	return true;
}

static bool si_switch_compute_shader(struct si_context *sctx,
                                     struct si_compute *program,
                                     struct si_shader *shader, unsigned offset)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	struct si_shader_config inline_config = {0};
	struct si_shader_config *config;
	uint64_t shader_va;

	if (sctx->cs_shader_state.emitted_program == program &&
	    sctx->cs_shader_state.offset == offset)
		return true;

	if (program->ir_type == PIPE_SHADER_IR_TGSI) {
		config = &shader->config;
	} else {
		unsigned lds_blocks;

		config = &inline_config;
		si_shader_binary_read_config(&shader->binary, config, offset);

		lds_blocks = config->lds_size;
		/* XXX: We are over allocating LDS.  For SI, the shader reports
		* LDS in blocks of 256 bytes, so if there are 4 bytes lds
		* allocated in the shader and 4 bytes allocated by the state
		* tracker, then we will set LDS_SIZE to 512 bytes rather than 256.
		*/
		if (sctx->b.chip_class <= SI) {
			lds_blocks += align(program->local_size, 256) >> 8;
		} else {
			lds_blocks += align(program->local_size, 512) >> 9;
		}

		assert(lds_blocks <= 0xFF);

		config->rsrc2 &= C_00B84C_LDS_SIZE;
		config->rsrc2 |=  S_00B84C_LDS_SIZE(lds_blocks);
	}

	if (!si_setup_compute_scratch_buffer(sctx, shader, config))
		return false;

	if (shader->scratch_bo) {
		COMPUTE_DBG(sctx->screen, "Waves: %u; Scratch per wave: %u bytes; "
		            "Total Scratch: %u bytes\n", sctx->scratch_waves,
			    config->scratch_bytes_per_wave,
			    config->scratch_bytes_per_wave *
			    sctx->scratch_waves);

		radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx,
			      shader->scratch_bo, RADEON_USAGE_READWRITE,
			      RADEON_PRIO_SCRATCH_BUFFER);
	}

	shader_va = shader->bo->gpu_address + offset;

	radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx, shader->bo,
	                          RADEON_USAGE_READ, RADEON_PRIO_SHADER_BINARY);

	radeon_set_sh_reg_seq(cs, R_00B830_COMPUTE_PGM_LO, 2);
	radeon_emit(cs, shader_va >> 8);
	radeon_emit(cs, shader_va >> 40);

	radeon_set_sh_reg_seq(cs, R_00B848_COMPUTE_PGM_RSRC1, 2);
	radeon_emit(cs, config->rsrc1);
	radeon_emit(cs, config->rsrc2);

	radeon_set_sh_reg(cs, R_00B860_COMPUTE_TMPRING_SIZE,
	          S_00B860_WAVES(sctx->scratch_waves)
	             | S_00B860_WAVESIZE(config->scratch_bytes_per_wave >> 10));

	sctx->cs_shader_state.emitted_program = program;
	sctx->cs_shader_state.offset = offset;
	sctx->cs_shader_state.uses_scratch =
		config->scratch_bytes_per_wave != 0;

	return true;
}

static void si_upload_compute_input(struct si_context *sctx,
                                  const struct pipe_grid_info *info)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	struct si_compute *program = sctx->cs_shader_state.program;
	struct r600_resource *input_buffer = NULL;
	unsigned kernel_args_size;
	unsigned num_work_size_bytes = 36;
	uint32_t kernel_args_offset = 0;
	uint32_t *kernel_args;
	void *kernel_args_ptr;
	uint64_t kernel_args_va;
	unsigned i;

	/* The extra num_work_size_bytes are for work group / work item size information */
	kernel_args_size = program->input_size + num_work_size_bytes;

	u_upload_alloc(sctx->b.uploader, 0, kernel_args_size, 256,
		       &kernel_args_offset,
		       (struct pipe_resource**)&input_buffer, &kernel_args_ptr);

	kernel_args = (uint32_t*)kernel_args_ptr;
	for (i = 0; i < 3; i++) {
		kernel_args[i] = info->grid[i];
		kernel_args[i + 3] = info->grid[i] * info->block[i];
		kernel_args[i + 6] = info->block[i];
	}

	memcpy(kernel_args + (num_work_size_bytes / 4), info->input,
	       program->input_size);


	for (i = 0; i < (kernel_args_size / 4); i++) {
		COMPUTE_DBG(sctx->screen, "input %u : %u\n", i,
			kernel_args[i]);
	}

	kernel_args_va = input_buffer->gpu_address + kernel_args_offset;

	radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx, input_buffer,
				  RADEON_USAGE_READ, RADEON_PRIO_CONST_BUFFER);

	radeon_set_sh_reg_seq(cs, R_00B900_COMPUTE_USER_DATA_0, 2);
	radeon_emit(cs, kernel_args_va);
	radeon_emit(cs, S_008F04_BASE_ADDRESS_HI (kernel_args_va >> 32) |
	                S_008F04_STRIDE(0));

	r600_resource_reference(&input_buffer, NULL);
}

static void si_setup_tgsi_grid(struct si_context *sctx,
                                const struct pipe_grid_info *info)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	unsigned grid_size_reg = R_00B900_COMPUTE_USER_DATA_0 +
	                          4 * SI_SGPR_GRID_SIZE;

	if (info->indirect) {
		uint64_t base_va = r600_resource(info->indirect)->gpu_address;
		uint64_t va = base_va + info->indirect_offset;
		int i;

		radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx,
		                 (struct r600_resource *)info->indirect,
		                 RADEON_USAGE_READ, RADEON_PRIO_DRAW_INDIRECT);

		for (i = 0; i < 3; ++i) {
			radeon_emit(cs, PKT3(PKT3_COPY_DATA, 4, 0));
			radeon_emit(cs, COPY_DATA_SRC_SEL(COPY_DATA_MEM) |
					COPY_DATA_DST_SEL(COPY_DATA_REG));
			radeon_emit(cs, (va +  4 * i));
			radeon_emit(cs, (va + 4 * i) >> 32);
			radeon_emit(cs, (grid_size_reg >> 2) + i);
			radeon_emit(cs, 0);
		}
	} else {

		radeon_set_sh_reg_seq(cs, grid_size_reg, 3);
		radeon_emit(cs, info->grid[0]);
		radeon_emit(cs, info->grid[1]);
		radeon_emit(cs, info->grid[2]);
	}
}

static void si_emit_dispatch_packets(struct si_context *sctx,
                                     const struct pipe_grid_info *info)
{
	struct radeon_winsys_cs *cs = sctx->b.gfx.cs;
	bool render_cond_bit = sctx->b.render_cond && !sctx->b.render_cond_force_off;
	unsigned waves_per_threadgroup =
		DIV_ROUND_UP(info->block[0] * info->block[1] * info->block[2], 64);

	radeon_set_sh_reg(cs, R_00B854_COMPUTE_RESOURCE_LIMITS,
			  S_00B854_SIMD_DEST_CNTL(waves_per_threadgroup % 4 == 0));

	radeon_set_sh_reg_seq(cs, R_00B81C_COMPUTE_NUM_THREAD_X, 3);
	radeon_emit(cs, S_00B81C_NUM_THREAD_FULL(info->block[0]));
	radeon_emit(cs, S_00B820_NUM_THREAD_FULL(info->block[1]));
	radeon_emit(cs, S_00B824_NUM_THREAD_FULL(info->block[2]));

	if (info->indirect) {
		uint64_t base_va = r600_resource(info->indirect)->gpu_address;

		radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx,
		                 (struct r600_resource *)info->indirect,
		                 RADEON_USAGE_READ, RADEON_PRIO_DRAW_INDIRECT);

		radeon_emit(cs, PKT3(PKT3_SET_BASE, 2, 0) |
		                PKT3_SHADER_TYPE_S(1));
		radeon_emit(cs, 1);
		radeon_emit(cs, base_va);
		radeon_emit(cs, base_va >> 32);

		radeon_emit(cs, PKT3(PKT3_DISPATCH_INDIRECT, 1, render_cond_bit) |
		                PKT3_SHADER_TYPE_S(1));
		radeon_emit(cs, info->indirect_offset);
		radeon_emit(cs, 1);
	} else {
		radeon_emit(cs, PKT3(PKT3_DISPATCH_DIRECT, 3, render_cond_bit) |
		                PKT3_SHADER_TYPE_S(1));
		radeon_emit(cs, info->grid[0]);
		radeon_emit(cs, info->grid[1]);
		radeon_emit(cs, info->grid[2]);
		radeon_emit(cs, 1);
	}
}


static void si_launch_grid(
		struct pipe_context *ctx, const struct pipe_grid_info *info)
{
	struct si_context *sctx = (struct si_context*)ctx;
	struct si_compute *program = sctx->cs_shader_state.program;
	int i;
	/* HW bug workaround when CS threadgroups > 256 threads and async
	 * compute isn't used, i.e. only one compute job can run at a time.
	 * If async compute is possible, the threadgroup size must be limited
	 * to 256 threads on all queues to avoid the bug.
	 * Only SI and certain CIK chips are affected.
	 */
	bool cs_regalloc_hang =
		(sctx->b.chip_class == SI ||
		 sctx->b.family == CHIP_BONAIRE ||
		 sctx->b.family == CHIP_KABINI) &&
		info->block[0] * info->block[1] * info->block[2] > 256;

	if (cs_regalloc_hang)
		sctx->b.flags |= SI_CONTEXT_PS_PARTIAL_FLUSH |
				 SI_CONTEXT_CS_PARTIAL_FLUSH;

	si_decompress_compute_textures(sctx);

	/* Add buffer sizes for memory checking in need_cs_space. */
	r600_context_add_resource_size(ctx, &program->shader.bo->b.b);
	/* TODO: add the scratch buffer */

	if (info->indirect) {
		r600_context_add_resource_size(ctx, info->indirect);

		/* The hw doesn't read the indirect buffer via TC L2. */
		if (r600_resource(info->indirect)->TC_L2_dirty) {
			sctx->b.flags |= SI_CONTEXT_INV_GLOBAL_L2;
			r600_resource(info->indirect)->TC_L2_dirty = false;
		}
	}

	si_need_cs_space(sctx);

	if (!sctx->cs_shader_state.initialized)
		si_initialize_compute(sctx);

	if (sctx->b.flags)
		si_emit_cache_flush(sctx);

	if (!si_switch_compute_shader(sctx, program, &program->shader, info->pc))
		return;

	si_upload_compute_shader_descriptors(sctx);
	si_emit_compute_shader_userdata(sctx);

	if (si_is_atom_dirty(sctx, sctx->atoms.s.render_cond)) {
		sctx->atoms.s.render_cond->emit(&sctx->b,
		                                sctx->atoms.s.render_cond);
		si_set_atom_dirty(sctx, sctx->atoms.s.render_cond, false);
	}

	if (program->input_size || program->ir_type == PIPE_SHADER_IR_NATIVE)
		si_upload_compute_input(sctx, info);

	/* Global buffers */
	for (i = 0; i < MAX_GLOBAL_BUFFERS; i++) {
		struct r600_resource *buffer =
				(struct r600_resource*)program->global_buffers[i];
		if (!buffer) {
			continue;
		}
		radeon_add_to_buffer_list(&sctx->b, &sctx->b.gfx, buffer,
					  RADEON_USAGE_READWRITE,
					  RADEON_PRIO_COMPUTE_GLOBAL);
	}

	if (program->ir_type == PIPE_SHADER_IR_TGSI)
		si_setup_tgsi_grid(sctx, info);

	si_ce_pre_draw_synchronization(sctx);

	si_emit_dispatch_packets(sctx, info);

	si_ce_post_draw_synchronization(sctx);

	sctx->compute_is_busy = true;
	sctx->b.num_compute_calls++;
	if (sctx->cs_shader_state.uses_scratch)
		sctx->b.num_spill_compute_calls++;

	if (cs_regalloc_hang)
		sctx->b.flags |= SI_CONTEXT_CS_PARTIAL_FLUSH;
}


static void si_delete_compute_state(struct pipe_context *ctx, void* state){
	struct si_compute *program = (struct si_compute *)state;
	struct si_context *sctx = (struct si_context*)ctx;

	if (!state) {
		return;
	}

	if (program == sctx->cs_shader_state.program)
		sctx->cs_shader_state.program = NULL;

	if (program == sctx->cs_shader_state.emitted_program)
		sctx->cs_shader_state.emitted_program = NULL;

	si_shader_destroy(&program->shader);
	FREE(program);
}

static void si_set_compute_resources(struct pipe_context * ctx_,
		unsigned start, unsigned count,
		struct pipe_surface ** surfaces) { }

void si_init_compute_functions(struct si_context *sctx)
{
	sctx->b.b.create_compute_state = si_create_compute_state;
	sctx->b.b.delete_compute_state = si_delete_compute_state;
	sctx->b.b.bind_compute_state = si_bind_compute_state;
/*	 ctx->context.create_sampler_view = evergreen_compute_create_sampler_view; */
	sctx->b.b.set_compute_resources = si_set_compute_resources;
	sctx->b.b.set_global_binding = si_set_global_binding;
	sctx->b.b.launch_grid = si_launch_grid;
}
