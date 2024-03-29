/*
 * Copyright © 2011 Intel Corporation
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

#include "main/macros.h"
#include "intel_batchbuffer.h"
#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"

/**
 * The following diagram shows how we partition the URB:
 *
 *        16kB or 32kB               Rest of the URB space
 *   __________-__________   _________________-_________________
 *  /                     \ /                                   \
 * +-------------------------------------------------------------+
 * |  VS/HS/DS/GS/FS Push  |           VS/HS/DS/GS URB           |
 * |       Constants       |               Entries               |
 * +-------------------------------------------------------------+
 *
 * Notably, push constants must be stored at the beginning of the URB
 * space, while entries can be stored anywhere.  Ivybridge and Haswell
 * GT1/GT2 have a maximum constant buffer size of 16kB, while Haswell GT3
 * doubles this (32kB).
 *
 * Ivybridge and Haswell GT1/GT2 allow push constants to be located (and
 * sized) in increments of 1kB.  Haswell GT3 requires them to be located and
 * sized in increments of 2kB.
 *
 * Currently we split the constant buffer space evenly among whatever stages
 * are active.  This is probably not ideal, but simple.
 *
 * Ivybridge GT1 and Haswell GT1 have 128kB of URB space.
 * Ivybridge GT2 and Haswell GT2 have 256kB of URB space.
 * Haswell GT3 has 512kB of URB space.
 *
 * See "Volume 2a: 3D Pipeline," section 1.8, "Volume 1b: Configurations",
 * and the documentation for 3DSTATE_PUSH_CONSTANT_ALLOC_xS.
 */
static void
gen7_allocate_push_constants(struct brw_context *brw)
{
   /* BRW_NEW_GEOMETRY_PROGRAM */
   bool gs_present = brw->geometry_program;

   /* BRW_NEW_TESS_PROGRAMS */
   bool tess_present = brw->tess_eval_program;

   unsigned avail_size = 16;
   unsigned multiplier =
      (brw->gen >= 8 || (brw->is_haswell && brw->gt == 3)) ? 2 : 1;

   int stages = 2 + gs_present + 2 * tess_present;

   /* Divide up the available space equally between stages.  Because we
    * round down (using floor division), there may be some left over
    * space.  We allocate that to the pixel shader stage.
    */
   unsigned size_per_stage = avail_size / stages;

   unsigned vs_size = size_per_stage;
   unsigned hs_size = tess_present ? size_per_stage : 0;
   unsigned ds_size = tess_present ? size_per_stage : 0;
   unsigned gs_size = gs_present ? size_per_stage : 0;
   unsigned fs_size = avail_size - size_per_stage * (stages - 1);

   gen7_emit_push_constant_state(brw, multiplier * vs_size,
                                 multiplier * hs_size, multiplier * ds_size,
                                 multiplier * gs_size, multiplier * fs_size);

   /* From p115 of the Ivy Bridge PRM (3.2.1.4 3DSTATE_PUSH_CONSTANT_ALLOC_VS):
    *
    *     Programming Restriction:
    *
    *     The 3DSTATE_CONSTANT_VS must be reprogrammed prior to the next
    *     3DPRIMITIVE command after programming the
    *     3DSTATE_PUSH_CONSTANT_ALLOC_VS.
    *
    * Similar text exists for the other 3DSTATE_PUSH_CONSTANT_ALLOC_*
    * commands.
    */
   brw->ctx.NewDriverState |= BRW_NEW_PUSH_CONSTANT_ALLOCATION;
}

void
gen7_emit_push_constant_state(struct brw_context *brw, unsigned vs_size,
                              unsigned hs_size, unsigned ds_size,
                              unsigned gs_size, unsigned fs_size)
{
   unsigned offset = 0;

   BEGIN_BATCH(10);
   OUT_BATCH(_3DSTATE_PUSH_CONSTANT_ALLOC_VS << 16 | (2 - 2));
   OUT_BATCH(vs_size | offset << GEN7_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT);
   offset += vs_size;

   OUT_BATCH(_3DSTATE_PUSH_CONSTANT_ALLOC_HS << 16 | (2 - 2));
   OUT_BATCH(hs_size | offset << GEN7_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT);
   offset += hs_size;

   OUT_BATCH(_3DSTATE_PUSH_CONSTANT_ALLOC_DS << 16 | (2 - 2));
   OUT_BATCH(ds_size | offset << GEN7_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT);
   offset += ds_size;

   OUT_BATCH(_3DSTATE_PUSH_CONSTANT_ALLOC_GS << 16 | (2 - 2));
   OUT_BATCH(gs_size | offset << GEN7_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT);
   offset += gs_size;

   OUT_BATCH(_3DSTATE_PUSH_CONSTANT_ALLOC_PS << 16 | (2 - 2));
   OUT_BATCH(fs_size | offset << GEN7_PUSH_CONSTANT_BUFFER_OFFSET_SHIFT);
   ADVANCE_BATCH();

   /* From p292 of the Ivy Bridge PRM (11.2.4 3DSTATE_PUSH_CONSTANT_ALLOC_PS):
    *
    *     A PIPE_CONTROL command with the CS Stall bit set must be programmed
    *     in the ring after this instruction.
    *
    * No such restriction exists for Haswell or Baytrail.
    */
   if (brw->gen < 8 && !brw->is_haswell && !brw->is_baytrail)
      gen7_emit_cs_stall_flush(brw);
}

const struct brw_tracked_state gen7_push_constant_space = {
   .dirty = {
      .mesa = 0,
      .brw = BRW_NEW_CONTEXT |
             BRW_NEW_GEOMETRY_PROGRAM |
             BRW_NEW_TESS_PROGRAMS,
   },
   .emit = gen7_allocate_push_constants,
};

static void
gen7_emit_urb_state(struct brw_context *brw,
                    unsigned nr_vs_entries,
                    unsigned vs_size, unsigned vs_start,
                    unsigned nr_hs_entries,
                    unsigned hs_size, unsigned hs_start,
                    unsigned nr_ds_entries,
                    unsigned ds_size, unsigned ds_start,
                    unsigned nr_gs_entries,
                    unsigned gs_size, unsigned gs_start)
{
   BEGIN_BATCH(8);
   OUT_BATCH(_3DSTATE_URB_VS << 16 | (2 - 2));
   OUT_BATCH(nr_vs_entries |
             ((vs_size - 1) << GEN7_URB_ENTRY_SIZE_SHIFT) |
             (vs_start << GEN7_URB_STARTING_ADDRESS_SHIFT));

   OUT_BATCH(_3DSTATE_URB_GS << 16 | (2 - 2));
   OUT_BATCH(nr_gs_entries |
             ((gs_size - 1) << GEN7_URB_ENTRY_SIZE_SHIFT) |
             (gs_start << GEN7_URB_STARTING_ADDRESS_SHIFT));

   OUT_BATCH(_3DSTATE_URB_HS << 16 | (2 - 2));
   OUT_BATCH(nr_hs_entries |
             ((hs_size - 1) << GEN7_URB_ENTRY_SIZE_SHIFT) |
             (hs_start << GEN7_URB_STARTING_ADDRESS_SHIFT));

   OUT_BATCH(_3DSTATE_URB_DS << 16 | (2 - 2));
   OUT_BATCH(nr_ds_entries |
             ((ds_size - 1) << GEN7_URB_ENTRY_SIZE_SHIFT) |
             (ds_start << GEN7_URB_STARTING_ADDRESS_SHIFT));
   ADVANCE_BATCH();
}

static void
upload_urb(struct brw_context *brw)
{
   /* BRW_NEW_VS_PROG_DATA */
   const unsigned vs_size = MAX2(brw->vs.prog_data->base.urb_entry_size, 1);
   /* BRW_NEW_GEOMETRY_PROGRAM, BRW_NEW_GS_PROG_DATA */
   const bool gs_present = brw->geometry_program;
   /* BRW_NEW_TESS_PROGRAMS */
   const bool tess_present = brw->tess_eval_program;

   gen7_upload_urb(brw, vs_size, gs_present, tess_present);
}

void
gen7_upload_urb(struct brw_context *brw, unsigned vs_size,
                bool gs_present, bool tess_present)
{
   const struct gen_device_info *devinfo = brw->intelScreen->devinfo;
   const int push_size_kB =
      (brw->gen >= 8 || (brw->is_haswell && brw->gt == 3)) ? 32 : 16;

   /* BRW_NEW_VS_PROG_DATA */
   unsigned vs_entry_size_bytes = vs_size * 64;
   /* BRW_NEW_GEOMETRY_PROGRAM, BRW_NEW_GS_PROG_DATA */
   unsigned gs_size = gs_present ? brw->gs.prog_data->base.urb_entry_size : 1;
   unsigned gs_entry_size_bytes = gs_size * 64;

   /* BRW_NEW_TCS_PROG_DATA */
   unsigned hs_size = tess_present ? brw->tcs.prog_data->base.urb_entry_size : 1;
   unsigned hs_entry_size_bytes = hs_size * 64;
   /* BRW_NEW_TES_PROG_DATA */
   unsigned ds_size = tess_present ? brw->tes.prog_data->base.urb_entry_size : 1;
   unsigned ds_entry_size_bytes = ds_size * 64;

   /* If we're just switching between programs with the same URB requirements,
    * skip the rest of the logic.
    */
   if (!(brw->ctx.NewDriverState & BRW_NEW_CONTEXT) &&
       !(brw->ctx.NewDriverState & BRW_NEW_URB_SIZE) &&
       brw->urb.vsize == vs_size &&
       brw->urb.gs_present == gs_present &&
       brw->urb.gsize == gs_size &&
       brw->urb.tess_present == tess_present &&
       brw->urb.hsize == hs_size &&
       brw->urb.dsize == ds_size) {
      return;
   }
   brw->urb.vsize = vs_size;
   brw->urb.gs_present = gs_present;
   brw->urb.gsize = gs_size;
   brw->urb.tess_present = tess_present;
   brw->urb.hsize = hs_size;
   brw->urb.dsize = ds_size;

   /* From p35 of the Ivy Bridge PRM (section 1.7.1: 3DSTATE_URB_GS):
    *
    *     VS Number of URB Entries must be divisible by 8 if the VS URB Entry
    *     Allocation Size is less than 9 512-bit URB entries.
    *
    * Similar text exists for HS, DS and GS.
    */
   unsigned vs_granularity = (vs_size < 9) ? 8 : 1;
   unsigned hs_granularity = (hs_size < 9) ? 8 : 1;
   unsigned ds_granularity = (ds_size < 9) ? 8 : 1;
   unsigned gs_granularity = (gs_size < 9) ? 8 : 1;

   /* URB allocations must be done in 8k chunks. */
   unsigned chunk_size_bytes = 8192;

   /* Determine the size of the URB in chunks.
    * BRW_NEW_URB_SIZE
    */
   unsigned urb_chunks = brw->urb.size * 1024 / chunk_size_bytes;

   /* Reserve space for push constants */
   unsigned push_constant_bytes = 1024 * push_size_kB;
   unsigned push_constant_chunks =
      push_constant_bytes / chunk_size_bytes;

   /* Initially, assign each stage the minimum amount of URB space it needs,
    * and make a note of how much additional space it "wants" (the amount of
    * additional space it could actually make use of).
    */

   /* VS has a lower limit on the number of URB entries.
    *
    * From the Broadwell PRM, 3DSTATE_URB_VS instruction:
    * "When tessellation is enabled, the VS Number of URB Entries must be
    *  greater than or equal to 192."
    */
   unsigned vs_min_entries =
      tess_present && brw->gen == 8 ? 192 : brw->urb.min_vs_entries;
   /* Min VS Entries isn't a multiple of 8 on Cherryview/Broxton; round up */
   vs_min_entries = ALIGN(vs_min_entries, vs_granularity);

   unsigned vs_chunks =
      DIV_ROUND_UP(vs_min_entries * vs_entry_size_bytes, chunk_size_bytes);
   unsigned vs_wants =
      DIV_ROUND_UP(brw->urb.max_vs_entries * vs_entry_size_bytes,
                   chunk_size_bytes) - vs_chunks;

   unsigned gs_chunks = 0;
   unsigned gs_wants = 0;
   if (gs_present) {
      /* There are two constraints on the minimum amount of URB space we can
       * allocate:
       *
       * (1) We need room for at least 2 URB entries, since we always operate
       * the GS in DUAL_OBJECT mode.
       *
       * (2) We can't allocate less than nr_gs_entries_granularity.
       */
      gs_chunks = DIV_ROUND_UP(MAX2(gs_granularity, 2) * gs_entry_size_bytes,
                               chunk_size_bytes);
      gs_wants = DIV_ROUND_UP(brw->urb.max_gs_entries * gs_entry_size_bytes,
                              chunk_size_bytes) - gs_chunks;
   }

   unsigned hs_chunks = 0;
   unsigned hs_wants = 0;
   unsigned ds_chunks = 0;
   unsigned ds_wants = 0;

   if (tess_present) {
      hs_chunks =
         DIV_ROUND_UP(hs_granularity * hs_entry_size_bytes,
                      chunk_size_bytes);
      hs_wants =
         DIV_ROUND_UP(devinfo->urb.max_hs_entries * hs_entry_size_bytes,
                      chunk_size_bytes) - hs_chunks;

      ds_chunks =
         DIV_ROUND_UP(devinfo->urb.min_ds_entries * ds_entry_size_bytes,
                      chunk_size_bytes);
      ds_wants =
         DIV_ROUND_UP(brw->urb.max_ds_entries * ds_entry_size_bytes,
                      chunk_size_bytes) - ds_chunks;
   }

   /* There should always be enough URB space to satisfy the minimum
    * requirements of each stage.
    */
   unsigned total_needs = push_constant_chunks +
                          vs_chunks + hs_chunks + ds_chunks + gs_chunks;
   assert(total_needs <= urb_chunks);

   /* Mete out remaining space (if any) in proportion to "wants". */
   unsigned total_wants = vs_wants + hs_wants + ds_wants + gs_wants;
   unsigned remaining_space = urb_chunks - total_needs;
   if (remaining_space > total_wants)
      remaining_space = total_wants;
   if (remaining_space > 0) {
      unsigned vs_additional = (unsigned)
         roundf(vs_wants * (((float) remaining_space) / total_wants));
      vs_chunks += vs_additional;
      remaining_space -= vs_additional;
      total_wants -= vs_wants;

      if (total_wants > 0) {
         unsigned hs_additional = (unsigned)
            round(hs_wants * (((double) remaining_space) / total_wants));
         hs_chunks += hs_additional;
         remaining_space -= hs_additional;
         total_wants -= hs_wants;
      }

      if (total_wants > 0) {
         unsigned ds_additional = (unsigned)
            round(ds_wants * (((double) remaining_space) / total_wants));
         ds_chunks += ds_additional;
         remaining_space -= ds_additional;
         total_wants -= ds_wants;
      }

      gs_chunks += remaining_space;
   }

   /* Sanity check that we haven't over-allocated. */
   assert(push_constant_chunks +
          vs_chunks + hs_chunks + ds_chunks + gs_chunks <= urb_chunks);

   /* Finally, compute the number of entries that can fit in the space
    * allocated to each stage.
    */
   unsigned nr_vs_entries = vs_chunks * chunk_size_bytes / vs_entry_size_bytes;
   unsigned nr_hs_entries = hs_chunks * chunk_size_bytes / hs_entry_size_bytes;
   unsigned nr_ds_entries = ds_chunks * chunk_size_bytes / ds_entry_size_bytes;
   unsigned nr_gs_entries = gs_chunks * chunk_size_bytes / gs_entry_size_bytes;

   /* Since we rounded up when computing *_wants, this may be slightly more
    * than the maximum allowed amount, so correct for that.
    */
   nr_vs_entries = MIN2(nr_vs_entries, brw->urb.max_vs_entries);
   nr_hs_entries = MIN2(nr_hs_entries, brw->urb.max_hs_entries);
   nr_ds_entries = MIN2(nr_ds_entries, brw->urb.max_ds_entries);
   nr_gs_entries = MIN2(nr_gs_entries, brw->urb.max_gs_entries);

   /* Ensure that we program a multiple of the granularity. */
   nr_vs_entries = ROUND_DOWN_TO(nr_vs_entries, vs_granularity);
   nr_hs_entries = ROUND_DOWN_TO(nr_hs_entries, hs_granularity);
   nr_ds_entries = ROUND_DOWN_TO(nr_ds_entries, ds_granularity);
   nr_gs_entries = ROUND_DOWN_TO(nr_gs_entries, gs_granularity);

   /* Finally, sanity check to make sure we have at least the minimum number
    * of entries needed for each stage.
    */
   assert(nr_vs_entries >= vs_min_entries);
   if (gs_present)
      assert(nr_gs_entries >= 2);
   if (tess_present) {
      assert(nr_hs_entries >= 1);
      assert(nr_ds_entries >= devinfo->urb.min_ds_entries);
   }

   /* Gen7 doesn't actually use brw->urb.nr_{vs,gs}_entries, but it seems
    * better to put reasonable data in there rather than leave them
    * uninitialized.
    */
   brw->urb.nr_vs_entries = nr_vs_entries;
   brw->urb.nr_hs_entries = nr_hs_entries;
   brw->urb.nr_ds_entries = nr_ds_entries;
   brw->urb.nr_gs_entries = nr_gs_entries;

   /* Lay out the URB in the following order:
    * - push constants
    * - VS
    * - HS
    * - DS
    * - GS
    */
   brw->urb.vs_start = push_constant_chunks;
   brw->urb.hs_start = push_constant_chunks + vs_chunks;
   brw->urb.ds_start = push_constant_chunks + vs_chunks + hs_chunks;
   brw->urb.gs_start = push_constant_chunks + vs_chunks + hs_chunks +
                       ds_chunks;

   if (brw->gen == 7 && !brw->is_haswell && !brw->is_baytrail)
      gen7_emit_vs_workaround_flush(brw);
   gen7_emit_urb_state(brw,
                       brw->urb.nr_vs_entries, vs_size, brw->urb.vs_start,
                       brw->urb.nr_hs_entries, hs_size, brw->urb.hs_start,
                       brw->urb.nr_ds_entries, ds_size, brw->urb.ds_start,
                       brw->urb.nr_gs_entries, gs_size, brw->urb.gs_start);
}

const struct brw_tracked_state gen7_urb = {
   .dirty = {
      .mesa = 0,
      .brw = BRW_NEW_CONTEXT |
             BRW_NEW_URB_SIZE |
             BRW_NEW_GEOMETRY_PROGRAM |
             BRW_NEW_TESS_PROGRAMS |
             BRW_NEW_GS_PROG_DATA |
             BRW_NEW_TCS_PROG_DATA |
             BRW_NEW_TES_PROG_DATA |
             BRW_NEW_VS_PROG_DATA,
   },
   .emit = upload_urb,
};
