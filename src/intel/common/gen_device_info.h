 /*
  * Copyright © 2013 Intel Corporation
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
  *
  */

#pragma once
#include <stdbool.h>

/**
 * Intel hardware information and quirks
 */
struct gen_device_info
{
   int gen; /**< Generation number: 4, 5, 6, 7, ... */
   int gt;

   bool is_g4x;
   bool is_ivybridge;
   bool is_baytrail;
   bool is_haswell;
   bool is_cherryview;
   bool is_broxton;

   bool has_hiz_and_separate_stencil;
   bool must_use_separate_stencil;

   bool has_llc;

   bool has_pln;
   bool has_compr4;
   bool has_surface_tile_offset;
   bool supports_simd16_3src;
   bool has_resource_streamer;

   /**
    * \name Intel hardware quirks
    *  @{
    */
   bool has_negative_rhw_bug;

   /**
    * Some versions of Gen hardware don't do centroid interpolation correctly
    * on unlit pixels, causing incorrect values for derivatives near triangle
    * edges.  Enabling this flag causes the fragment shader to use
    * non-centroid interpolation for unlit pixels, at the expense of two extra
    * fragment shader instructions.
    */
   bool needs_unlit_centroid_workaround;
   /** @} */

   /**
    * \name GPU hardware limits
    *
    * In general, you can find shader thread maximums by looking at the "Maximum
    * Number of Threads" field in the Intel PRM description of the 3DSTATE_VS,
    * 3DSTATE_GS, 3DSTATE_HS, 3DSTATE_DS, and 3DSTATE_PS commands. URB entry
    * limits come from the "Number of URB Entries" field in the
    * 3DSTATE_URB_VS command and friends.
    *
    * These fields are used to calculate the scratch space to allocate.  The
    * amount of scratch space can be larger without being harmful on modern
    * GPUs, however, prior to Haswell, programming the maximum number of threads
    * to greater than the hardware maximum would cause GPU performance to tank.
    *
    *  @{
    */
   /**
    * Total number of slices present on the device whether or not they've been
    * fused off.
    *
    * XXX: CS thread counts are limited by the inability to do cross subslice
    * communication. It is the effectively the number of logical threads which
    * can be executed in a subslice. Fuse configurations may cause this number
    * to change, so we program @max_cs_threads as the lower maximum.
    */
   unsigned num_slices;
   unsigned max_vs_threads;   /**< Maximum Vertex Shader threads */
   unsigned max_hs_threads;   /**< Maximum Hull Shader threads */
   unsigned max_ds_threads;   /**< Maximum Domain Shader threads */
   unsigned max_gs_threads;   /**< Maximum Geometry Shader threads. */
   /**
    * Theoretical maximum number of Pixel Shader threads.
    *
    * PSD means Pixel Shader Dispatcher. On modern Intel GPUs, hardware will
    * automatically scale pixel shader thread count, based on a single value
    * programmed into 3DSTATE_PS.
    *
    * To calculate the maximum number of threads for Gen8 beyond (which have
    * multiple Pixel Shader Dispatchers):
    *
    * - Look up 3DSTATE_PS and find "Maximum Number of Threads Per PSD"
    * - Usually there's only one PSD per subslice, so use the number of
    *   subslices for number of PSDs.
    * - For max_wm_threads, the total should be PSD threads * #PSDs.
    */
   unsigned max_wm_threads;

   /**
    * Maximum Compute Shader threads.
    *
    * Thread count * number of EUs per subslice
    */
   unsigned max_cs_threads;

   struct {
      /**
       * Hardware default URB size.
       *
       * The units this is expressed in are somewhat inconsistent: 512b units
       * on Gen4-5, KB on Gen6-7, and KB times the slice count on Gen8+.
       *
       * Look up "URB Size" in the "Device Attributes" page, and take the
       * maximum.  Look up the slice count for each GT SKU on the same page.
       * urb.size = URB Size (kbytes) / slice count
       */
      unsigned size;
      unsigned min_vs_entries;
      unsigned max_vs_entries;
      unsigned max_hs_entries;
      unsigned min_ds_entries;
      unsigned max_ds_entries;
      unsigned max_gs_entries;
   } urb;
   /** @} */
};

const struct gen_device_info *gen_get_device_info(int devid);
const char *gen_get_device_name(int devid);
