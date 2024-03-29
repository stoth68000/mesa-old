/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#pragma once

#include <stdint.h>
#include "brw_reg.h"
#include "brw_defines.h"
#include "brw_context.h"

#ifdef __cplusplus
#include "brw_ir_allocator.h"
#endif

#define MAX_SAMPLER_MESSAGE_SIZE 11
#define MAX_VGRF_SIZE 16

#ifdef __cplusplus
struct backend_reg : private brw_reg
{
   backend_reg() {}
   backend_reg(const struct brw_reg &reg) : brw_reg(reg) {}

   const brw_reg &as_brw_reg() const
   {
      assert(file == ARF || file == FIXED_GRF || file == MRF || file == IMM);
      assert(reg_offset == 0);
      return static_cast<const brw_reg &>(*this);
   }

   brw_reg &as_brw_reg()
   {
      assert(file == ARF || file == FIXED_GRF || file == MRF || file == IMM);
      assert(reg_offset == 0);
      return static_cast<brw_reg &>(*this);
   }

   bool equals(const backend_reg &r) const;

   bool is_zero() const;
   bool is_one() const;
   bool is_negative_one() const;
   bool is_null() const;
   bool is_accumulator() const;
   bool in_range(const backend_reg &r, unsigned n) const;

   /**
    * Offset within the virtual register.
    *
    * In the scalar backend, this is in units of a float per pixel for pre-
    * register allocation registers (i.e., one register in SIMD8 mode and two
    * registers in SIMD16 mode).
    *
    * For uniforms, this is in units of 1 float.
    */
   uint16_t reg_offset;

   using brw_reg::type;
   using brw_reg::file;
   using brw_reg::negate;
   using brw_reg::abs;
   using brw_reg::address_mode;
   using brw_reg::subnr;
   using brw_reg::nr;

   using brw_reg::swizzle;
   using brw_reg::writemask;
   using brw_reg::indirect_offset;
   using brw_reg::vstride;
   using brw_reg::width;
   using brw_reg::hstride;

   using brw_reg::df;
   using brw_reg::f;
   using brw_reg::d;
   using brw_reg::ud;
};
#endif

struct cfg_t;
struct bblock_t;

#ifdef __cplusplus
struct backend_instruction : public exec_node {
   bool is_3src(const struct gen_device_info *devinfo) const;
   bool is_tex() const;
   bool is_math() const;
   bool is_control_flow() const;
   bool is_commutative() const;
   bool can_do_source_mods() const;
   bool can_do_saturate() const;
   bool can_do_cmod() const;
   bool reads_accumulator_implicitly() const;
   bool writes_accumulator_implicitly(const struct gen_device_info *devinfo) const;

   void remove(bblock_t *block);
   void insert_after(bblock_t *block, backend_instruction *inst);
   void insert_before(bblock_t *block, backend_instruction *inst);
   void insert_before(bblock_t *block, exec_list *list);

   /**
    * True if the instruction has side effects other than writing to
    * its destination registers.  You are expected not to reorder or
    * optimize these out unless you know what you are doing.
    */
   bool has_side_effects() const;

   /**
    * True if the instruction might be affected by side effects of other
    * instructions.
    */
   bool is_volatile() const;
#else
struct backend_instruction {
   struct exec_node link;
#endif
   /** @{
    * Annotation for the generated IR.  One of the two can be set.
    */
   const void *ir;
   const char *annotation;
   /** @} */

   uint32_t offset; /**< spill/unspill offset or texture offset bitfield */
   uint8_t mlen; /**< SEND message length */
   int8_t base_mrf; /**< First MRF in the SEND message, if mlen is nonzero. */
   uint8_t target; /**< MRT target. */
   uint8_t regs_written; /**< Number of registers written by the instruction. */

   enum opcode opcode; /* BRW_OPCODE_* or FS_OPCODE_* */
   enum brw_conditional_mod conditional_mod; /**< BRW_CONDITIONAL_* */
   enum brw_predicate predicate;
   bool predicate_inverse:1;
   bool writes_accumulator:1; /**< instruction implicitly writes accumulator */
   bool force_writemask_all:1;
   bool no_dd_clear:1;
   bool no_dd_check:1;
   bool saturate:1;
   bool shadow_compare:1;

   /* Chooses which flag subregister (f0.0 or f0.1) is used for conditional
    * mod and predication.
    */
   unsigned flag_subreg:1;

   /** The number of hardware registers used for a message header. */
   uint8_t header_size;
};

#ifdef __cplusplus

enum instruction_scheduler_mode {
   SCHEDULE_PRE,
   SCHEDULE_PRE_NON_LIFO,
   SCHEDULE_PRE_LIFO,
   SCHEDULE_POST,
};

struct backend_shader {
protected:

   backend_shader(const struct brw_compiler *compiler,
                  void *log_data,
                  void *mem_ctx,
                  const nir_shader *shader,
                  struct brw_stage_prog_data *stage_prog_data);

public:

   const struct brw_compiler *compiler;
   void *log_data; /* Passed to compiler->*_log functions */

   const struct gen_device_info * const devinfo;
   const nir_shader *nir;
   struct brw_stage_prog_data * const stage_prog_data;

   /** ralloc context for temporary data used during compile */
   void *mem_ctx;

   /**
    * List of either fs_inst or vec4_instruction (inheriting from
    * backend_instruction)
    */
   exec_list instructions;

   cfg_t *cfg;

   gl_shader_stage stage;
   bool debug_enabled;
   const char *stage_name;
   const char *stage_abbrev;
   bool is_passthrough_shader;

   brw::simple_allocator alloc;

   virtual void dump_instruction(backend_instruction *inst) = 0;
   virtual void dump_instruction(backend_instruction *inst, FILE *file) = 0;
   virtual void dump_instructions();
   virtual void dump_instructions(const char *name);

   void calculate_cfg();

   virtual void invalidate_live_intervals() = 0;
};

uint32_t brw_texture_offset(int *offsets, unsigned num_components);

void brw_setup_image_uniform_values(gl_shader_stage stage,
                                    struct brw_stage_prog_data *stage_prog_data,
                                    unsigned param_start_index,
                                    const gl_uniform_storage *storage);

#else
struct backend_shader;
#endif /* __cplusplus */

enum brw_reg_type brw_type_for_base_type(const struct glsl_type *type);
enum brw_conditional_mod brw_conditional_for_comparison(unsigned int op);
uint32_t brw_math_function(enum opcode op);
const char *brw_instruction_name(const struct gen_device_info *devinfo,
                                 enum opcode op);
bool brw_saturate_immediate(enum brw_reg_type type, struct brw_reg *reg);
bool brw_negate_immediate(enum brw_reg_type type, struct brw_reg *reg);
bool brw_abs_immediate(enum brw_reg_type type, struct brw_reg *reg);

bool opt_predicated_break(struct backend_shader *s);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scratch data used when compiling a GLSL geometry shader.
 */
struct brw_gs_compile
{
   struct brw_gs_prog_key key;
   struct brw_vue_map input_vue_map;

   unsigned control_data_bits_per_vertex;
   unsigned control_data_header_size_bits;
};

uint32_t
brw_assign_common_binding_table_offsets(gl_shader_stage stage,
                                        const struct gen_device_info *devinfo,
                                        const struct gl_shader_program *shader_prog,
                                        const struct gl_program *prog,
                                        struct brw_stage_prog_data *stage_prog_data,
                                        uint32_t next_binding_table_offset);

bool brw_vs_precompile(struct gl_context *ctx,
                       struct gl_shader_program *shader_prog,
                       struct gl_program *prog);
bool brw_tcs_precompile(struct gl_context *ctx,
                        struct gl_shader_program *shader_prog,
                        struct gl_program *prog);
bool brw_tes_precompile(struct gl_context *ctx,
                        struct gl_shader_program *shader_prog,
                        struct gl_program *prog);
bool brw_gs_precompile(struct gl_context *ctx,
                       struct gl_shader_program *shader_prog,
                       struct gl_program *prog);
bool brw_fs_precompile(struct gl_context *ctx,
                       struct gl_shader_program *shader_prog,
                       struct gl_program *prog);
bool brw_cs_precompile(struct gl_context *ctx,
                       struct gl_shader_program *shader_prog,
                       struct gl_program *prog);

GLboolean brw_link_shader(struct gl_context *ctx, struct gl_shader_program *prog);
struct gl_linked_shader *brw_new_shader(gl_shader_stage stage);

unsigned tesslevel_outer_components(GLenum tes_primitive_mode);
unsigned tesslevel_inner_components(GLenum tes_primitive_mode);
unsigned writemask_for_backwards_vector(unsigned mask);

#ifdef __cplusplus
}
#endif
