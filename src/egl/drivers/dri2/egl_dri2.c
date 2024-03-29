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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 */

#define WL_HIDE_DEPRECATED

#include <stdbool.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <c11/threads.h>
#include <time.h>
#ifdef HAVE_LIBDRM
#include <xf86drm.h>
#include <drm_fourcc.h>
#endif
#include <GL/gl.h>
#include <GL/internal/dri_interface.h>
#include "GL/mesa_glinterop.h"
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_WAYLAND_PLATFORM
#include "wayland-drm.h"
#include "wayland-drm-client-protocol.h"
#endif

#include "egl_dri2.h"
#include "util/u_atomic.h"

/* The kernel header drm_fourcc.h defines the DRM formats below.  We duplicate
 * some of the definitions here so that building Mesa won't bleeding-edge
 * kernel headers.
 */
#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8            fourcc_code('R', '8', ' ', ' ') /* [7:0] R */
#endif

#ifndef DRM_FORMAT_RG88
#define DRM_FORMAT_RG88          fourcc_code('R', 'G', '8', '8') /* [15:0] R:G 8:8 little endian */
#endif

#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88          fourcc_code('G', 'R', '8', '8') /* [15:0] G:R 8:8 little endian */
#endif

const __DRIuseInvalidateExtension use_invalidate = {
   .base = { __DRI_USE_INVALIDATE, 1 }
};

EGLint dri2_to_egl_attribute_map[] = {
   0,
   EGL_BUFFER_SIZE,		/* __DRI_ATTRIB_BUFFER_SIZE */
   EGL_LEVEL,			/* __DRI_ATTRIB_LEVEL */
   EGL_RED_SIZE,		/* __DRI_ATTRIB_RED_SIZE */
   EGL_GREEN_SIZE,		/* __DRI_ATTRIB_GREEN_SIZE */
   EGL_BLUE_SIZE,		/* __DRI_ATTRIB_BLUE_SIZE */
   EGL_LUMINANCE_SIZE,		/* __DRI_ATTRIB_LUMINANCE_SIZE */
   EGL_ALPHA_SIZE,		/* __DRI_ATTRIB_ALPHA_SIZE */
   0,				/* __DRI_ATTRIB_ALPHA_MASK_SIZE */
   EGL_DEPTH_SIZE,		/* __DRI_ATTRIB_DEPTH_SIZE */
   EGL_STENCIL_SIZE,		/* __DRI_ATTRIB_STENCIL_SIZE */
   0,				/* __DRI_ATTRIB_ACCUM_RED_SIZE */
   0,				/* __DRI_ATTRIB_ACCUM_GREEN_SIZE */
   0,				/* __DRI_ATTRIB_ACCUM_BLUE_SIZE */
   0,				/* __DRI_ATTRIB_ACCUM_ALPHA_SIZE */
   EGL_SAMPLE_BUFFERS,		/* __DRI_ATTRIB_SAMPLE_BUFFERS */
   EGL_SAMPLES,			/* __DRI_ATTRIB_SAMPLES */
   0,				/* __DRI_ATTRIB_RENDER_TYPE, */
   0,				/* __DRI_ATTRIB_CONFIG_CAVEAT */
   0,				/* __DRI_ATTRIB_CONFORMANT */
   0,				/* __DRI_ATTRIB_DOUBLE_BUFFER */
   0,				/* __DRI_ATTRIB_STEREO */
   0,				/* __DRI_ATTRIB_AUX_BUFFERS */
   0,				/* __DRI_ATTRIB_TRANSPARENT_TYPE */
   0,				/* __DRI_ATTRIB_TRANSPARENT_INDEX_VALUE */
   0,				/* __DRI_ATTRIB_TRANSPARENT_RED_VALUE */
   0,				/* __DRI_ATTRIB_TRANSPARENT_GREEN_VALUE */
   0,				/* __DRI_ATTRIB_TRANSPARENT_BLUE_VALUE */
   0,				/* __DRI_ATTRIB_TRANSPARENT_ALPHA_VALUE */
   0,				/* __DRI_ATTRIB_FLOAT_MODE (deprecated) */
   0,				/* __DRI_ATTRIB_RED_MASK */
   0,				/* __DRI_ATTRIB_GREEN_MASK */
   0,				/* __DRI_ATTRIB_BLUE_MASK */
   0,				/* __DRI_ATTRIB_ALPHA_MASK */
   EGL_MAX_PBUFFER_WIDTH,	/* __DRI_ATTRIB_MAX_PBUFFER_WIDTH */
   EGL_MAX_PBUFFER_HEIGHT,	/* __DRI_ATTRIB_MAX_PBUFFER_HEIGHT */
   EGL_MAX_PBUFFER_PIXELS,	/* __DRI_ATTRIB_MAX_PBUFFER_PIXELS */
   0,				/* __DRI_ATTRIB_OPTIMAL_PBUFFER_WIDTH */
   0,				/* __DRI_ATTRIB_OPTIMAL_PBUFFER_HEIGHT */
   0,				/* __DRI_ATTRIB_VISUAL_SELECT_GROUP */
   0,				/* __DRI_ATTRIB_SWAP_METHOD */
   EGL_MAX_SWAP_INTERVAL,	/* __DRI_ATTRIB_MAX_SWAP_INTERVAL */
   EGL_MIN_SWAP_INTERVAL,	/* __DRI_ATTRIB_MIN_SWAP_INTERVAL */
   0,				/* __DRI_ATTRIB_BIND_TO_TEXTURE_RGB */
   0,				/* __DRI_ATTRIB_BIND_TO_TEXTURE_RGBA */
   0,				/* __DRI_ATTRIB_BIND_TO_MIPMAP_TEXTURE */
   0,				/* __DRI_ATTRIB_BIND_TO_TEXTURE_TARGETS */
   EGL_Y_INVERTED_NOK,		/* __DRI_ATTRIB_YINVERTED */
   0,				/* __DRI_ATTRIB_FRAMEBUFFER_SRGB_CAPABLE */
};

const __DRIconfig *
dri2_get_dri_config(struct dri2_egl_config *conf, EGLint surface_type,
                    EGLenum colorspace)
{
   const bool srgb = colorspace == EGL_GL_COLORSPACE_SRGB_KHR;

   return surface_type == EGL_WINDOW_BIT ? conf->dri_double_config[srgb] :
                                           conf->dri_single_config[srgb];
}

static EGLBoolean
dri2_match_config(const _EGLConfig *conf, const _EGLConfig *criteria)
{
   if (_eglCompareConfigs(conf, criteria, NULL, EGL_FALSE) != 0)
      return EGL_FALSE;

   if (!_eglMatchConfig(conf, criteria))
      return EGL_FALSE;

   return EGL_TRUE;
}

struct dri2_egl_config *
dri2_add_config(_EGLDisplay *disp, const __DRIconfig *dri_config, int id,
		EGLint surface_type, const EGLint *attr_list,
		const unsigned int *rgba_masks)
{
   struct dri2_egl_config *conf;
   struct dri2_egl_display *dri2_dpy;
   _EGLConfig base;
   unsigned int attrib, value, double_buffer;
   bool srgb = false;
   EGLint key, bind_to_texture_rgb, bind_to_texture_rgba;
   unsigned int dri_masks[4] = { 0, 0, 0, 0 };
   _EGLConfig *matching_config;
   EGLint num_configs = 0;
   EGLint config_id;
   int i;

   dri2_dpy = disp->DriverData;
   _eglInitConfig(&base, disp, id);

   i = 0;
   double_buffer = 0;
   bind_to_texture_rgb = 0;
   bind_to_texture_rgba = 0;

   while (dri2_dpy->core->indexConfigAttrib(dri_config, i++, &attrib, &value)) {
      switch (attrib) {
      case __DRI_ATTRIB_RENDER_TYPE:
	 if (value & __DRI_ATTRIB_RGBA_BIT)
	    value = EGL_RGB_BUFFER;
	 else if (value & __DRI_ATTRIB_LUMINANCE_BIT)
	    value = EGL_LUMINANCE_BUFFER;
	 else
	    return NULL;
	 _eglSetConfigKey(&base, EGL_COLOR_BUFFER_TYPE, value);
	 break;

      case __DRI_ATTRIB_CONFIG_CAVEAT:
         if (value & __DRI_ATTRIB_NON_CONFORMANT_CONFIG)
            value = EGL_NON_CONFORMANT_CONFIG;
         else if (value & __DRI_ATTRIB_SLOW_BIT)
            value = EGL_SLOW_CONFIG;
	 else
	    value = EGL_NONE;
	 _eglSetConfigKey(&base, EGL_CONFIG_CAVEAT, value);
         break;

      case __DRI_ATTRIB_BIND_TO_TEXTURE_RGB:
	 bind_to_texture_rgb = value;
	 break;

      case __DRI_ATTRIB_BIND_TO_TEXTURE_RGBA:
	 bind_to_texture_rgba = value;
	 break;

      case __DRI_ATTRIB_DOUBLE_BUFFER:
	 double_buffer = value;
	 break;

      case __DRI_ATTRIB_RED_MASK:
         dri_masks[0] = value;
         break;

      case __DRI_ATTRIB_GREEN_MASK:
         dri_masks[1] = value;
         break;

      case __DRI_ATTRIB_BLUE_MASK:
         dri_masks[2] = value;
         break;

      case __DRI_ATTRIB_ALPHA_MASK:
         dri_masks[3] = value;
         break;

      case __DRI_ATTRIB_ACCUM_RED_SIZE:
      case __DRI_ATTRIB_ACCUM_GREEN_SIZE:
      case __DRI_ATTRIB_ACCUM_BLUE_SIZE:
      case __DRI_ATTRIB_ACCUM_ALPHA_SIZE:
         /* Don't expose visuals with the accumulation buffer. */
         if (value > 0)
            return NULL;
         break;

      case __DRI_ATTRIB_FRAMEBUFFER_SRGB_CAPABLE:
         srgb = value != 0;
         if (!disp->Extensions.KHR_gl_colorspace && srgb)
            return NULL;
         break;

      default:
	 key = dri2_to_egl_attribute_map[attrib];
	 if (key != 0)
	    _eglSetConfigKey(&base, key, value);
	 break;
      }
   }

   if (attr_list)
      for (i = 0; attr_list[i] != EGL_NONE; i += 2)
         _eglSetConfigKey(&base, attr_list[i], attr_list[i+1]);

   if (rgba_masks && memcmp(rgba_masks, dri_masks, sizeof(dri_masks)))
      return NULL;

   base.NativeRenderable = EGL_TRUE;

   base.SurfaceType = surface_type;
   if (surface_type & (EGL_PBUFFER_BIT |
       (disp->Extensions.NOK_texture_from_pixmap ? EGL_PIXMAP_BIT : 0))) {
      base.BindToTextureRGB = bind_to_texture_rgb;
      if (base.AlphaSize > 0)
         base.BindToTextureRGBA = bind_to_texture_rgba;
   }

   base.RenderableType = disp->ClientAPIs;
   base.Conformant = disp->ClientAPIs;

   base.MinSwapInterval = dri2_dpy->min_swap_interval;
   base.MaxSwapInterval = dri2_dpy->max_swap_interval;

   if (!_eglValidateConfig(&base, EGL_FALSE)) {
      _eglLog(_EGL_DEBUG, "DRI2: failed to validate config %d", id);
      return NULL;
   }

   config_id = base.ConfigID;
   base.ConfigID    = EGL_DONT_CARE;
   base.SurfaceType = EGL_DONT_CARE;
   num_configs = _eglFilterArray(disp->Configs, (void **) &matching_config, 1,
                                 (_EGLArrayForEach) dri2_match_config, &base);

   if (num_configs == 1) {
      conf = (struct dri2_egl_config *) matching_config;

      if (double_buffer && !conf->dri_double_config[srgb])
         conf->dri_double_config[srgb] = dri_config;
      else if (!double_buffer && !conf->dri_single_config[srgb])
         conf->dri_single_config[srgb] = dri_config;
      else
         /* a similar config type is already added (unlikely) => discard */
         return NULL;
   }
   else if (num_configs == 0) {
      conf = calloc(1, sizeof *conf);
      if (conf == NULL)
         return NULL;

      if (double_buffer)
         conf->dri_double_config[srgb] = dri_config;
      else
         conf->dri_single_config[srgb] = dri_config;

      memcpy(&conf->base, &base, sizeof base);
      conf->base.SurfaceType = 0;
      conf->base.ConfigID = config_id;

      _eglLinkConfig(&conf->base);
   }
   else {
      assert(0);
      return NULL;
   }

   if (double_buffer) {
      surface_type &= ~EGL_PIXMAP_BIT;
   }

   conf->base.SurfaceType |= surface_type;

   return conf;
}

__DRIimage *
dri2_lookup_egl_image(__DRIscreen *screen, void *image, void *data)
{
   _EGLDisplay *disp = data;
   struct dri2_egl_image *dri2_img;
   _EGLImage *img;

   (void) screen;

   img = _eglLookupImage(image, disp);
   if (img == NULL) {
      _eglError(EGL_BAD_PARAMETER, "dri2_lookup_egl_image");
      return NULL;
   }

   dri2_img = dri2_egl_image(image);

   return dri2_img->dri_image;
}

const __DRIimageLookupExtension image_lookup_extension = {
   .base = { __DRI_IMAGE_LOOKUP, 1 },

   .lookupEGLImage       = dri2_lookup_egl_image
};

struct dri2_extension_match {
   const char *name;
   int version;
   int offset;
};

static struct dri2_extension_match dri3_driver_extensions[] = {
   { __DRI_CORE, 1, offsetof(struct dri2_egl_display, core) },
   { __DRI_IMAGE_DRIVER, 1, offsetof(struct dri2_egl_display, image_driver) },
   { NULL, 0, 0 }
};

static struct dri2_extension_match dri2_driver_extensions[] = {
   { __DRI_CORE, 1, offsetof(struct dri2_egl_display, core) },
   { __DRI_DRI2, 2, offsetof(struct dri2_egl_display, dri2) },
   { NULL, 0, 0 }
};

static struct dri2_extension_match dri2_core_extensions[] = {
   { __DRI2_FLUSH, 1, offsetof(struct dri2_egl_display, flush) },
   { __DRI_TEX_BUFFER, 2, offsetof(struct dri2_egl_display, tex_buffer) },
   { __DRI_IMAGE, 1, offsetof(struct dri2_egl_display, image) },
   { NULL, 0, 0 }
};

static struct dri2_extension_match swrast_driver_extensions[] = {
   { __DRI_CORE, 1, offsetof(struct dri2_egl_display, core) },
   { __DRI_SWRAST, 2, offsetof(struct dri2_egl_display, swrast) },
   { NULL, 0, 0 }
};

static struct dri2_extension_match swrast_core_extensions[] = {
   { __DRI_TEX_BUFFER, 2, offsetof(struct dri2_egl_display, tex_buffer) },
   { NULL, 0, 0 }
};

static EGLBoolean
dri2_bind_extensions(struct dri2_egl_display *dri2_dpy,
		     struct dri2_extension_match *matches,
		     const __DRIextension **extensions)
{
   int i, j, ret = EGL_TRUE;
   void *field;

   for (i = 0; extensions[i]; i++) {
      _eglLog(_EGL_DEBUG, "found extension `%s'", extensions[i]->name);
      for (j = 0; matches[j].name; j++) {
	 if (strcmp(extensions[i]->name, matches[j].name) == 0 &&
	     extensions[i]->version >= matches[j].version) {
	    field = ((char *) dri2_dpy + matches[j].offset);
	    *(const __DRIextension **) field = extensions[i];
	    _eglLog(_EGL_INFO, "found extension %s version %d",
		    extensions[i]->name, extensions[i]->version);
	 }
      }
   }

   for (j = 0; matches[j].name; j++) {
      field = ((char *) dri2_dpy + matches[j].offset);
      if (*(const __DRIextension **) field == NULL) {
         _eglLog(_EGL_WARNING, "did not find extension %s version %d",
		 matches[j].name, matches[j].version);
	 ret = EGL_FALSE;
      }
   }

   return ret;
}

static const __DRIextension **
dri2_open_driver(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = disp->DriverData;
   const __DRIextension **extensions = NULL;
   char path[PATH_MAX], *search_paths, *p, *next, *end;
   char *get_extensions_name;
   const __DRIextension **(*get_extensions)(void);

   search_paths = NULL;
   if (geteuid() == getuid()) {
      /* don't allow setuid apps to use LIBGL_DRIVERS_PATH */
      search_paths = getenv("LIBGL_DRIVERS_PATH");
   }
   if (search_paths == NULL)
      search_paths = DEFAULT_DRIVER_DIR;

   dri2_dpy->driver = NULL;
   end = search_paths + strlen(search_paths);
   for (p = search_paths; p < end; p = next + 1) {
      int len;
      next = strchr(p, ':');
      if (next == NULL)
         next = end;

      len = next - p;
#if GLX_USE_TLS
      snprintf(path, sizeof path,
	       "%.*s/tls/%s_dri.so", len, p, dri2_dpy->driver_name);
      dri2_dpy->driver = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
#endif
      if (dri2_dpy->driver == NULL) {
	 snprintf(path, sizeof path,
		  "%.*s/%s_dri.so", len, p, dri2_dpy->driver_name);
	 dri2_dpy->driver = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
	 if (dri2_dpy->driver == NULL)
	    _eglLog(_EGL_DEBUG, "failed to open %s: %s\n", path, dlerror());
      }
      /* not need continue to loop all paths once the driver is found */
      if (dri2_dpy->driver != NULL)
         break;

#ifdef ANDROID
      snprintf(path, sizeof path, "%.*s/gallium_dri.so", len, p);
      dri2_dpy->driver = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
      if (dri2_dpy->driver == NULL)
         _eglLog(_EGL_DEBUG, "failed to open %s: %s\n", path, dlerror());
      else
         break;
#endif
   }

   if (dri2_dpy->driver == NULL) {
      _eglLog(_EGL_WARNING,
	      "DRI2: failed to open %s (search paths %s)",
	      dri2_dpy->driver_name, search_paths);
      return NULL;
   }

   _eglLog(_EGL_DEBUG, "DRI2: dlopen(%s)", path);

   if (asprintf(&get_extensions_name, "%s_%s",
                __DRI_DRIVER_GET_EXTENSIONS, dri2_dpy->driver_name) != -1) {
      get_extensions = dlsym(dri2_dpy->driver, get_extensions_name);
      if (get_extensions) {
         extensions = get_extensions();
      } else {
         _eglLog(_EGL_DEBUG, "driver does not expose %s(): %s\n",
                 get_extensions_name, dlerror());
      }
      free(get_extensions_name);
   }

   if (!extensions)
      extensions = dlsym(dri2_dpy->driver, __DRI_DRIVER_EXTENSIONS);
   if (extensions == NULL) {
      _eglLog(_EGL_WARNING,
	      "DRI2: driver exports no extensions (%s)", dlerror());
      dlclose(dri2_dpy->driver);
   }

   return extensions;
}

EGLBoolean
dri2_load_driver_dri3(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = disp->DriverData;
   const __DRIextension **extensions;

   extensions = dri2_open_driver(disp);
   if (!extensions)
      return EGL_FALSE;

   if (!dri2_bind_extensions(dri2_dpy, dri3_driver_extensions, extensions)) {
      dlclose(dri2_dpy->driver);
      return EGL_FALSE;
   }
   dri2_dpy->driver_extensions = extensions;

   return EGL_TRUE;
}

EGLBoolean
dri2_load_driver(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = disp->DriverData;
   const __DRIextension **extensions;

   extensions = dri2_open_driver(disp);
   if (!extensions)
      return EGL_FALSE;

   if (!dri2_bind_extensions(dri2_dpy, dri2_driver_extensions, extensions)) {
      dlclose(dri2_dpy->driver);
      return EGL_FALSE;
   }
   dri2_dpy->driver_extensions = extensions;

   return EGL_TRUE;
}

EGLBoolean
dri2_load_driver_swrast(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = disp->DriverData;
   const __DRIextension **extensions;

   extensions = dri2_open_driver(disp);
   if (!extensions)
      return EGL_FALSE;

   if (!dri2_bind_extensions(dri2_dpy, swrast_driver_extensions, extensions)) {
      dlclose(dri2_dpy->driver);
      return EGL_FALSE;
   }
   dri2_dpy->driver_extensions = extensions;

   return EGL_TRUE;
}

static unsigned
dri2_renderer_query_integer(struct dri2_egl_display *dri2_dpy, int param)
{
   const __DRI2rendererQueryExtension *rendererQuery = dri2_dpy->rendererQuery;
   unsigned int value = 0;

   if (!rendererQuery ||
       rendererQuery->queryInteger(dri2_dpy->dri_screen, param, &value) == -1)
      return 0;

   return value;
}

void
dri2_setup_screen(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned int api_mask;

   if (dri2_dpy->image_driver) {
      api_mask = dri2_dpy->image_driver->getAPIMask(dri2_dpy->dri_screen);
   } else if (dri2_dpy->dri2) {
      api_mask = dri2_dpy->dri2->getAPIMask(dri2_dpy->dri_screen);
   } else {
      assert(dri2_dpy->swrast);
      api_mask = 1 << __DRI_API_OPENGL |
                 1 << __DRI_API_GLES |
                 1 << __DRI_API_GLES2 |
                 1 << __DRI_API_GLES3;
   }

   disp->ClientAPIs = 0;
   if ((api_mask & (1 <<__DRI_API_OPENGL)) && _eglIsApiValid(EGL_OPENGL_API))
      disp->ClientAPIs |= EGL_OPENGL_BIT;
   if ((api_mask & (1 << __DRI_API_GLES)) && _eglIsApiValid(EGL_OPENGL_ES_API))
      disp->ClientAPIs |= EGL_OPENGL_ES_BIT;
   if ((api_mask & (1 << __DRI_API_GLES2)) && _eglIsApiValid(EGL_OPENGL_ES_API))
      disp->ClientAPIs |= EGL_OPENGL_ES2_BIT;
   if ((api_mask & (1 << __DRI_API_GLES3)) && _eglIsApiValid(EGL_OPENGL_ES_API))
      disp->ClientAPIs |= EGL_OPENGL_ES3_BIT_KHR;

   assert(dri2_dpy->image_driver || dri2_dpy->dri2 || dri2_dpy->swrast);
   disp->Extensions.KHR_no_config_context = EGL_TRUE;
   disp->Extensions.KHR_surfaceless_context = EGL_TRUE;

   if (dri2_renderer_query_integer(dri2_dpy,
                                   __DRI2_RENDERER_HAS_FRAMEBUFFER_SRGB))
      disp->Extensions.KHR_gl_colorspace = EGL_TRUE;

   if (dri2_dpy->image_driver ||
       (dri2_dpy->dri2 && dri2_dpy->dri2->base.version >= 3) ||
       (dri2_dpy->swrast && dri2_dpy->swrast->base.version >= 3)) {
      disp->Extensions.KHR_create_context = EGL_TRUE;

      if (dri2_dpy->robustness)
         disp->Extensions.EXT_create_context_robustness = EGL_TRUE;
   }

   if (dri2_dpy->fence) {
      disp->Extensions.KHR_fence_sync = EGL_TRUE;
      disp->Extensions.KHR_wait_sync = EGL_TRUE;
      if (dri2_dpy->fence->get_fence_from_cl_event)
         disp->Extensions.KHR_cl_event2 = EGL_TRUE;
   }

   disp->Extensions.KHR_reusable_sync = EGL_TRUE;

   if (dri2_dpy->image) {
      if (dri2_dpy->image->base.version >= 10 &&
          dri2_dpy->image->getCapabilities != NULL) {
         int capabilities;

         capabilities = dri2_dpy->image->getCapabilities(dri2_dpy->dri_screen);
         disp->Extensions.MESA_drm_image = (capabilities & __DRI_IMAGE_CAP_GLOBAL_NAMES) != 0;

         if (dri2_dpy->image->base.version >= 11)
            disp->Extensions.MESA_image_dma_buf_export = EGL_TRUE;
      } else {
         disp->Extensions.MESA_drm_image = EGL_TRUE;
         if (dri2_dpy->image->base.version >= 11)
            disp->Extensions.MESA_image_dma_buf_export = EGL_TRUE;
      }

      disp->Extensions.KHR_image_base = EGL_TRUE;
      disp->Extensions.KHR_gl_renderbuffer_image = EGL_TRUE;
      if (dri2_dpy->image->base.version >= 5 &&
          dri2_dpy->image->createImageFromTexture) {
         disp->Extensions.KHR_gl_texture_2D_image = EGL_TRUE;
         disp->Extensions.KHR_gl_texture_cubemap_image = EGL_TRUE;
      }
      if (dri2_renderer_query_integer(dri2_dpy,
                                      __DRI2_RENDERER_HAS_TEXTURE_3D))
         disp->Extensions.KHR_gl_texture_3D_image = EGL_TRUE;
#ifdef HAVE_LIBDRM
      if (dri2_dpy->image->base.version >= 8 &&
          dri2_dpy->image->createImageFromDmaBufs) {
         disp->Extensions.EXT_image_dma_buf_import = EGL_TRUE;
      }
#endif
   }
}

/* All platforms but DRM call this function to create the screen, query the
 * dri extensions, setup the vtables and populate the driver_configs.
 * DRM inherits all that information from its display - GBM.
 */
EGLBoolean
dri2_create_screen(_EGLDisplay *disp)
{
   const __DRIextension **extensions;
   struct dri2_egl_display *dri2_dpy;
   unsigned i;

   dri2_dpy = disp->DriverData;

   if (dri2_dpy->image_driver) {
      dri2_dpy->dri_screen =
         dri2_dpy->image_driver->createNewScreen2(0, dri2_dpy->fd,
                                                  dri2_dpy->extensions,
                                                  dri2_dpy->driver_extensions,
                                                  &dri2_dpy->driver_configs,
                                                  disp);
   } else if (dri2_dpy->dri2) {
      if (dri2_dpy->dri2->base.version >= 4) {
         dri2_dpy->dri_screen =
            dri2_dpy->dri2->createNewScreen2(0, dri2_dpy->fd,
                                             dri2_dpy->extensions,
                                             dri2_dpy->driver_extensions,
                                             &dri2_dpy->driver_configs, disp);
      } else {
         dri2_dpy->dri_screen =
            dri2_dpy->dri2->createNewScreen(0, dri2_dpy->fd,
                                            dri2_dpy->extensions,
                                            &dri2_dpy->driver_configs, disp);
      }
   } else {
      assert(dri2_dpy->swrast);
      if (dri2_dpy->swrast->base.version >= 4) {
         dri2_dpy->dri_screen =
            dri2_dpy->swrast->createNewScreen2(0, dri2_dpy->extensions,
                                               dri2_dpy->driver_extensions,
                                               &dri2_dpy->driver_configs, disp);
      } else {
         dri2_dpy->dri_screen =
            dri2_dpy->swrast->createNewScreen(0, dri2_dpy->extensions,
                                              &dri2_dpy->driver_configs, disp);
      }
   }

   if (dri2_dpy->dri_screen == NULL) {
      _eglLog(_EGL_WARNING, "DRI2: failed to create dri screen");
      return EGL_FALSE;
   }

   dri2_dpy->own_dri_screen = 1;

   extensions = dri2_dpy->core->getExtensions(dri2_dpy->dri_screen);

   if (dri2_dpy->image_driver || dri2_dpy->dri2) {
      if (!dri2_bind_extensions(dri2_dpy, dri2_core_extensions, extensions))
         goto cleanup_dri_screen;
   } else {
      assert(dri2_dpy->swrast);
      if (!dri2_bind_extensions(dri2_dpy, swrast_core_extensions, extensions))
         goto cleanup_dri_screen;
   }

   for (i = 0; extensions[i]; i++) {
      if (strcmp(extensions[i]->name, __DRI2_ROBUSTNESS) == 0) {
         dri2_dpy->robustness = (__DRIrobustnessExtension *) extensions[i];
      }
      if (strcmp(extensions[i]->name, __DRI2_CONFIG_QUERY) == 0) {
         dri2_dpy->config = (__DRI2configQueryExtension *) extensions[i];
      }
      if (strcmp(extensions[i]->name, __DRI2_FENCE) == 0) {
         dri2_dpy->fence = (__DRI2fenceExtension *) extensions[i];
      }
      if (strcmp(extensions[i]->name, __DRI2_RENDERER_QUERY) == 0) {
         dri2_dpy->rendererQuery = (__DRI2rendererQueryExtension *) extensions[i];
      }
      if (strcmp(extensions[i]->name, __DRI2_INTEROP) == 0)
         dri2_dpy->interop = (__DRI2interopExtension *) extensions[i];
   }

   dri2_setup_screen(disp);

   return EGL_TRUE;

 cleanup_dri_screen:
   dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);

   return EGL_FALSE;
}

/**
 * Called via eglInitialize(), GLX_drv->API.Initialize().
 *
 * This must be guaranteed to be called exactly once, even if eglInitialize is
 * called many times (without a eglTerminate in between).
 */
static EGLBoolean
dri2_initialize(_EGLDriver *drv, _EGLDisplay *disp)
{
   EGLBoolean ret = EGL_FALSE;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   /* In the case where the application calls eglMakeCurrent(context1),
    * eglTerminate, then eglInitialize again (without a call to eglReleaseThread
    * or eglMakeCurrent(NULL) before that), dri2_dpy structure is still
    * initialized, as we need it to be able to free context1 correctly.
    *
    * It would probably be safest to forcibly release the display with
    * dri2_display_release, to make sure the display is reinitialized correctly.
    * However, the EGL spec states that we need to keep a reference to the
    * current context (so we cannot call dri2_make_current(NULL)), and therefore
    * we would leak context1 as we would be missing the old display connection
    * to free it up correctly.
    */
   if (dri2_dpy) {
      dri2_dpy->ref_count++;
      return EGL_TRUE;
   }

   /* not until swrast_dri is supported */
   if (disp->Options.UseFallback)
      return EGL_FALSE;

   /* Nothing to initialize for a test only display */
   if (disp->Options.TestOnly)
      return EGL_TRUE;

   switch (disp->Platform) {
#ifdef HAVE_SURFACELESS_PLATFORM
   case _EGL_PLATFORM_SURFACELESS:
      ret = dri2_initialize_surfaceless(drv, disp);
      break;
#endif
#ifdef HAVE_X11_PLATFORM
   case _EGL_PLATFORM_X11:
      ret = dri2_initialize_x11(drv, disp);
      break;
#endif
#ifdef HAVE_DRM_PLATFORM
   case _EGL_PLATFORM_DRM:
      ret = dri2_initialize_drm(drv, disp);
      break;
#endif
#ifdef HAVE_WAYLAND_PLATFORM
   case _EGL_PLATFORM_WAYLAND:
      ret = dri2_initialize_wayland(drv, disp);
      break;
#endif
#ifdef HAVE_ANDROID_PLATFORM
   case _EGL_PLATFORM_ANDROID:
      ret = dri2_initialize_android(drv, disp);
      break;
#endif
   default:
      _eglLog(_EGL_WARNING, "No EGL platform enabled.");
      return EGL_FALSE;
   }

   if (ret) {
      dri2_dpy = dri2_egl_display(disp);

      if (!dri2_dpy) {
         return EGL_FALSE;
      }

      dri2_dpy->ref_count++;
   }

   return ret;
}

/**
 * Decrement display reference count, and free up display if necessary.
 */
static void
dri2_display_release(_EGLDisplay *disp) {
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned i;

   assert(dri2_dpy->ref_count > 0);
   dri2_dpy->ref_count--;

   if (dri2_dpy->ref_count > 0)
      return;

   _eglCleanupDisplay(disp);

   if (dri2_dpy->own_dri_screen)
      dri2_dpy->core->destroyScreen(dri2_dpy->dri_screen);
   if (dri2_dpy->fd >= 0)
      close(dri2_dpy->fd);
   if (dri2_dpy->driver)
      dlclose(dri2_dpy->driver);
   free(dri2_dpy->driver_name);

#ifdef HAVE_WAYLAND_PLATFORM
   free(dri2_dpy->device_name);
#endif

   switch (disp->Platform) {
#ifdef HAVE_X11_PLATFORM
   case _EGL_PLATFORM_X11:
      if (dri2_dpy->own_device) {
         xcb_disconnect(dri2_dpy->conn);
      }
      break;
#endif
#ifdef HAVE_DRM_PLATFORM
   case _EGL_PLATFORM_DRM:
      if (dri2_dpy->own_device) {
         gbm_device_destroy(&dri2_dpy->gbm_dri->base.base);
      }
      break;
#endif
#ifdef HAVE_WAYLAND_PLATFORM
   case _EGL_PLATFORM_WAYLAND:
      if (dri2_dpy->wl_drm)
          wl_drm_destroy(dri2_dpy->wl_drm);
      if (dri2_dpy->wl_shm)
          wl_shm_destroy(dri2_dpy->wl_shm);
      wl_registry_destroy(dri2_dpy->wl_registry);
      wl_event_queue_destroy(dri2_dpy->wl_queue);
      if (dri2_dpy->own_device) {
         wl_display_disconnect(dri2_dpy->wl_dpy);
      }
      break;
#endif
   default:
      break;
   }

   /* The drm platform does not create the screen/driver_configs but reuses
    * the ones from the gbm device. As such the gbm itself is responsible
    * for the cleanup.
    */
   if (disp->Platform != _EGL_PLATFORM_DRM) {
      for (i = 0; dri2_dpy->driver_configs[i]; i++)
         free((__DRIconfig *) dri2_dpy->driver_configs[i]);
      free(dri2_dpy->driver_configs);
   }
   free(dri2_dpy);
   disp->DriverData = NULL;
}

/**
 * Called via eglTerminate(), drv->API.Terminate().
 *
 * This must be guaranteed to be called exactly once, even if eglTerminate is
 * called many times (without a eglInitialize in between).
 */
static EGLBoolean
dri2_terminate(_EGLDriver *drv, _EGLDisplay *disp)
{
   /* Release all non-current Context/Surfaces. */
   _eglReleaseDisplayResources(drv, disp);

   dri2_display_release(disp);

   return EGL_TRUE;
}

/**
 * Set the error code after a call to
 * dri2_egl_display::dri2::createContextAttribs.
 */
static void
dri2_create_context_attribs_error(int dri_error)
{
   EGLint egl_error;

   switch (dri_error) {
   case __DRI_CTX_ERROR_SUCCESS:
      return;

   case __DRI_CTX_ERROR_NO_MEMORY:
      egl_error = EGL_BAD_ALLOC;
      break;

  /* From the EGL_KHR_create_context spec, section "Errors":
   *
   *   * If <config> does not support a client API context compatible
   *     with the requested API major and minor version, [...] context flags,
   *     and context reset notification behavior (for client API types where
   *     these attributes are supported), then an EGL_BAD_MATCH error is
   *     generated.
   *
   *   * If an OpenGL ES context is requested and the values for
   *     attributes EGL_CONTEXT_MAJOR_VERSION_KHR and
   *     EGL_CONTEXT_MINOR_VERSION_KHR specify an OpenGL ES version that
   *     is not defined, than an EGL_BAD_MATCH error is generated.
   *
   *   * If an OpenGL context is requested, the requested version is
   *     greater than 3.2, and the value for attribute
   *     EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR has no bits set; has any
   *     bits set other than EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR and
   *     EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR; has more than
   *     one of these bits set; or if the implementation does not support
   *     the requested profile, then an EGL_BAD_MATCH error is generated.
   */
   case __DRI_CTX_ERROR_BAD_API:
   case __DRI_CTX_ERROR_BAD_VERSION:
   case __DRI_CTX_ERROR_BAD_FLAG:
      egl_error = EGL_BAD_MATCH;
      break;

  /* From the EGL_KHR_create_context spec, section "Errors":
   *
   *   * If an attribute name or attribute value in <attrib_list> is not
   *     recognized (including unrecognized bits in bitmask attributes),
   *     then an EGL_BAD_ATTRIBUTE error is generated."
   */
   case __DRI_CTX_ERROR_UNKNOWN_ATTRIBUTE:
   case __DRI_CTX_ERROR_UNKNOWN_FLAG:
      egl_error = EGL_BAD_ATTRIBUTE;
      break;

   default:
      assert(0);
      egl_error = EGL_BAD_MATCH;
      break;
   }

   _eglError(egl_error, "dri2_create_context");
}

static bool
dri2_fill_context_attribs(struct dri2_egl_context *dri2_ctx,
                          struct dri2_egl_display *dri2_dpy,
                          uint32_t *ctx_attribs,
                          unsigned *num_attribs)
{
   int pos = 0;

   assert(*num_attribs >= 8);

   ctx_attribs[pos++] = __DRI_CTX_ATTRIB_MAJOR_VERSION;
   ctx_attribs[pos++] = dri2_ctx->base.ClientMajorVersion;
   ctx_attribs[pos++] = __DRI_CTX_ATTRIB_MINOR_VERSION;
   ctx_attribs[pos++] = dri2_ctx->base.ClientMinorVersion;

   if (dri2_ctx->base.Flags != 0) {
      /* If the implementation doesn't support the __DRI2_ROBUSTNESS
       * extension, don't even try to send it the robust-access flag.
       * It may explode.  Instead, generate the required EGL error here.
       */
      if ((dri2_ctx->base.Flags & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR) != 0
            && !dri2_dpy->robustness) {
         _eglError(EGL_BAD_MATCH, "eglCreateContext");
         return false;
      }

      ctx_attribs[pos++] = __DRI_CTX_ATTRIB_FLAGS;
      ctx_attribs[pos++] = dri2_ctx->base.Flags;
   }

   if (dri2_ctx->base.ResetNotificationStrategy != EGL_NO_RESET_NOTIFICATION_KHR) {
      /* If the implementation doesn't support the __DRI2_ROBUSTNESS
       * extension, don't even try to send it a reset strategy.  It may
       * explode.  Instead, generate the required EGL error here.
       */
      if (!dri2_dpy->robustness) {
         _eglError(EGL_BAD_CONFIG, "eglCreateContext");
         return false;
      }

      ctx_attribs[pos++] = __DRI_CTX_ATTRIB_RESET_STRATEGY;
      ctx_attribs[pos++] = __DRI_CTX_RESET_LOSE_CONTEXT;
   }

   *num_attribs = pos;

   return true;
}

/**
 * Called via eglCreateContext(), drv->API.CreateContext().
 */
static _EGLContext *
dri2_create_context(_EGLDriver *drv, _EGLDisplay *disp, _EGLConfig *conf,
		    _EGLContext *share_list, const EGLint *attrib_list)
{
   struct dri2_egl_context *dri2_ctx;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_context *dri2_ctx_shared = dri2_egl_context(share_list);
   __DRIcontext *shared =
      dri2_ctx_shared ? dri2_ctx_shared->dri_context : NULL;
   struct dri2_egl_config *dri2_config = dri2_egl_config(conf);
   const __DRIconfig *dri_config;
   int api;

   (void) drv;

   dri2_ctx = malloc(sizeof *dri2_ctx);
   if (!dri2_ctx) {
      _eglError(EGL_BAD_ALLOC, "eglCreateContext");
      return NULL;
   }

   if (!_eglInitContext(&dri2_ctx->base, disp, conf, attrib_list))
      goto cleanup;

   switch (dri2_ctx->base.ClientAPI) {
   case EGL_OPENGL_ES_API:
      switch (dri2_ctx->base.ClientMajorVersion) {
      case 1:
         api = __DRI_API_GLES;
         break;
      case 2:
         api = __DRI_API_GLES2;
         break;
      case 3:
         api = __DRI_API_GLES3;
         break;
      default:
         _eglError(EGL_BAD_PARAMETER, "eglCreateContext");
         free(dri2_ctx);
         return NULL;
      }
      break;
   case EGL_OPENGL_API:
      if ((dri2_ctx->base.ClientMajorVersion >= 4
           || (dri2_ctx->base.ClientMajorVersion == 3
               && dri2_ctx->base.ClientMinorVersion >= 2))
          && dri2_ctx->base.Profile == EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR)
         api = __DRI_API_OPENGL_CORE;
      else
         api = __DRI_API_OPENGL;
      break;
   default:
      _eglError(EGL_BAD_PARAMETER, "eglCreateContext");
      free(dri2_ctx);
      return NULL;
   }

   if (conf != NULL) {
      /* The config chosen here isn't necessarily
       * used for surfaces later.
       * A pixmap surface will use the single config.
       * This opportunity depends on disabling the
       * doubleBufferMode check in
       * src/mesa/main/context.c:check_compatible()
       */
      if (dri2_config->dri_double_config[0])
         dri_config = dri2_config->dri_double_config[0];
      else
         dri_config = dri2_config->dri_single_config[0];

      /* EGL_WINDOW_BIT is set only when there is a dri_double_config.  This
       * makes sure the back buffer will always be used.
       */
      if (conf->SurfaceType & EGL_WINDOW_BIT)
         dri2_ctx->base.WindowRenderBuffer = EGL_BACK_BUFFER;
   }
   else
      dri_config = NULL;

   if (dri2_dpy->image_driver) {
      unsigned error;
      unsigned num_attribs = 8;
      uint32_t ctx_attribs[8];

      if (!dri2_fill_context_attribs(dri2_ctx, dri2_dpy, ctx_attribs,
                                        &num_attribs))
         goto cleanup;

      dri2_ctx->dri_context =
         dri2_dpy->image_driver->createContextAttribs(dri2_dpy->dri_screen,
                                                      api,
                                                      dri_config,
                                                      shared,
                                                      num_attribs / 2,
                                                      ctx_attribs,
                                                      & error,
                                                      dri2_ctx);
      dri2_create_context_attribs_error(error);
   } else if (dri2_dpy->dri2) {
      if (dri2_dpy->dri2->base.version >= 3) {
         unsigned error;
         unsigned num_attribs = 8;
         uint32_t ctx_attribs[8];

         if (!dri2_fill_context_attribs(dri2_ctx, dri2_dpy, ctx_attribs,
                                        &num_attribs))
            goto cleanup;

	 dri2_ctx->dri_context =
	    dri2_dpy->dri2->createContextAttribs(dri2_dpy->dri_screen,
                                                 api,
                                                 dri_config,
                                                 shared,
                                                 num_attribs / 2,
                                                 ctx_attribs,
                                                 & error,
                                                 dri2_ctx);
	 dri2_create_context_attribs_error(error);
      } else {
	 dri2_ctx->dri_context =
	    dri2_dpy->dri2->createNewContextForAPI(dri2_dpy->dri_screen,
						   api,
						   dri_config,
                                                   shared,
						   dri2_ctx);
      }
   } else {
      assert(dri2_dpy->swrast);
      if (dri2_dpy->swrast->base.version >= 3) {
         unsigned error;
         unsigned num_attribs = 8;
         uint32_t ctx_attribs[8];

         if (!dri2_fill_context_attribs(dri2_ctx, dri2_dpy, ctx_attribs,
                                        &num_attribs))
            goto cleanup;

         dri2_ctx->dri_context =
            dri2_dpy->swrast->createContextAttribs(dri2_dpy->dri_screen,
                                                   api,
                                                   dri_config,
                                                   shared,
                                                   num_attribs / 2,
                                                   ctx_attribs,
                                                   & error,
                                                   dri2_ctx);
         dri2_create_context_attribs_error(error);
      } else {
         dri2_ctx->dri_context =
            dri2_dpy->swrast->createNewContextForAPI(dri2_dpy->dri_screen,
                                                     api,
                                                     dri_config,
                                                     shared,
                                                     dri2_ctx);
      }
   }

   if (!dri2_ctx->dri_context)
      goto cleanup;

   return &dri2_ctx->base;

 cleanup:
   free(dri2_ctx);
   return NULL;
}

/**
 * Called via eglDestroyContext(), drv->API.DestroyContext().
 */
static EGLBoolean
dri2_destroy_context(_EGLDriver *drv, _EGLDisplay *disp, _EGLContext *ctx)
{
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   if (_eglPutContext(ctx)) {
      dri2_dpy->core->destroyContext(dri2_ctx->dri_context);
      free(dri2_ctx);
   }

   return EGL_TRUE;
}

/**
 * Called via eglMakeCurrent(), drv->API.MakeCurrent().
 */
static EGLBoolean
dri2_make_current(_EGLDriver *drv, _EGLDisplay *disp, _EGLSurface *dsurf,
		  _EGLSurface *rsurf, _EGLContext *ctx)
{
   struct dri2_egl_driver *dri2_drv = dri2_egl_driver(drv);
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
   _EGLContext *old_ctx;
   _EGLSurface *old_dsurf, *old_rsurf;
   _EGLSurface *tmp_dsurf, *tmp_rsurf;
   __DRIdrawable *ddraw, *rdraw;
   __DRIcontext *cctx;
   EGLBoolean unbind;

   if (!dri2_dpy)
      return _eglError(EGL_NOT_INITIALIZED, "eglMakeCurrent");

   /* make new bindings */
   if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf)) {
      /* _eglBindContext already sets the EGL error (in _eglCheckMakeCurrent) */
      return EGL_FALSE;
   }

   /* flush before context switch */
   if (old_ctx && dri2_drv->glFlush)
      dri2_drv->glFlush();

   ddraw = (dsurf) ? dri2_dpy->vtbl->get_dri_drawable(dsurf) : NULL;
   rdraw = (rsurf) ? dri2_dpy->vtbl->get_dri_drawable(rsurf) : NULL;
   cctx = (dri2_ctx) ? dri2_ctx->dri_context : NULL;

   if (old_ctx) {
      __DRIcontext *old_cctx = dri2_egl_context(old_ctx)->dri_context;
      dri2_dpy->core->unbindContext(old_cctx);
   }

   unbind = (cctx == NULL && ddraw == NULL && rdraw == NULL);

   if (unbind || dri2_dpy->core->bindContext(cctx, ddraw, rdraw)) {
      if (old_dsurf)
         drv->API.DestroySurface(drv, disp, old_dsurf);
      if (old_rsurf)
         drv->API.DestroySurface(drv, disp, old_rsurf);

      if (!unbind)
         dri2_dpy->ref_count++;
      if (old_ctx) {
         EGLDisplay old_disp = _eglGetDisplayHandle(old_ctx->Resource.Display);
         drv->API.DestroyContext(drv, disp, old_ctx);
         dri2_display_release(old_disp);
      }

      return EGL_TRUE;
   } else {
      /* undo the previous _eglBindContext */
      _eglBindContext(old_ctx, old_dsurf, old_rsurf, &ctx, &tmp_dsurf, &tmp_rsurf);
      assert(&dri2_ctx->base == ctx &&
             tmp_dsurf == dsurf &&
             tmp_rsurf == rsurf);

      _eglPutSurface(dsurf);
      _eglPutSurface(rsurf);
      _eglPutContext(ctx);

      _eglPutSurface(old_dsurf);
      _eglPutSurface(old_rsurf);
      _eglPutContext(old_ctx);

      /* dri2_dpy->core->bindContext failed. We cannot tell for sure why, but
       * setting the error to EGL_BAD_MATCH is surely better than leaving it
       * as EGL_SUCCESS.
       */
      return _eglError(EGL_BAD_MATCH, "eglMakeCurrent");
   }
}

__DRIdrawable *
dri2_surface_get_dri_drawable(_EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   return dri2_surf->dri_drawable;
}

/*
 * Called from eglGetProcAddress() via drv->API.GetProcAddress().
 */
static _EGLProc
dri2_get_proc_address(_EGLDriver *drv, const char *procname)
{
   struct dri2_egl_driver *dri2_drv = dri2_egl_driver(drv);

   return dri2_drv->get_proc_address(procname);
}

static _EGLSurface*
dri2_create_window_surface(_EGLDriver *drv, _EGLDisplay *dpy,
                           _EGLConfig *conf, void *native_window,
                           const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->create_window_surface(drv, dpy, conf, native_window,
                                                attrib_list);
}

static _EGLSurface*
dri2_create_pixmap_surface(_EGLDriver *drv, _EGLDisplay *dpy,
                           _EGLConfig *conf, void *native_pixmap,
                           const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->create_pixmap_surface(drv, dpy, conf, native_pixmap,
                                                attrib_list);
}

static _EGLSurface*
dri2_create_pbuffer_surface(_EGLDriver *drv, _EGLDisplay *dpy,
                           _EGLConfig *conf, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->create_pbuffer_surface(drv, dpy, conf, attrib_list);
}

static EGLBoolean
dri2_destroy_surface(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->destroy_surface(drv, dpy, surf);
}

static EGLBoolean
dri2_swap_interval(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                   EGLint interval)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->swap_interval(drv, dpy, surf, interval);
}

/**
 * Asks the client API to flush any rendering to the drawable so that we can
 * do our swapbuffers.
 */
void
dri2_flush_drawable_for_swapbuffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   __DRIdrawable *dri_drawable = dri2_dpy->vtbl->get_dri_drawable(draw);

   if (dri2_dpy->flush) {
      if (dri2_dpy->flush->base.version >= 4) {
         /* We know there's a current context because:
          *
          *     "If surface is not bound to the calling thread’s current
          *      context, an EGL_BAD_SURFACE error is generated."
         */
         _EGLContext *ctx = _eglGetCurrentContext();
         struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);

         /* From the EGL 1.4 spec (page 52):
          *
          *     "The contents of ancillary buffers are always undefined
          *      after calling eglSwapBuffers."
          */
         dri2_dpy->flush->flush_with_flags(dri2_ctx->dri_context,
                                           dri_drawable,
                                           __DRI2_FLUSH_DRAWABLE |
                                           __DRI2_FLUSH_INVALIDATE_ANCILLARY,
                                           __DRI2_THROTTLE_SWAPBUFFER);
      } else {
         dri2_dpy->flush->flush(dri_drawable);
      }
   }
}

static EGLBoolean
dri2_swap_buffers(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->swap_buffers(drv, dpy, surf);
}

static EGLBoolean
dri2_swap_buffers_with_damage(_EGLDriver *drv, _EGLDisplay *dpy,
                              _EGLSurface *surf,
                              const EGLint *rects, EGLint n_rects)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->swap_buffers_with_damage(drv, dpy, surf,
                                                   rects, n_rects);
}

static EGLBoolean
dri2_swap_buffers_region(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                         EGLint numRects, const EGLint *rects)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->swap_buffers_region(drv, dpy, surf, numRects, rects);
}

static EGLBoolean
dri2_post_sub_buffer(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                     EGLint x, EGLint y, EGLint width, EGLint height)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->post_sub_buffer(drv, dpy, surf, x, y, width, height);
}

static EGLBoolean
dri2_copy_buffers(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                  void *native_pixmap_target)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->copy_buffers(drv, dpy, surf, native_pixmap_target);
}

static EGLint
dri2_query_buffer_age(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->query_buffer_age(drv, dpy, surf);
}

static EGLBoolean
dri2_wait_client(_EGLDriver *drv, _EGLDisplay *disp, _EGLContext *ctx)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   _EGLSurface *surf = ctx->DrawSurface;
   __DRIdrawable *dri_drawable = dri2_dpy->vtbl->get_dri_drawable(surf);

   (void) drv;

   /* FIXME: If EGL allows frontbuffer rendering for window surfaces,
    * we need to copy fake to real here.*/

   if (dri2_dpy->flush != NULL)
      dri2_dpy->flush->flush(dri_drawable);

   return EGL_TRUE;
}

static EGLBoolean
dri2_wait_native(_EGLDriver *drv, _EGLDisplay *disp, EGLint engine)
{
   (void) drv;
   (void) disp;

   if (engine != EGL_CORE_NATIVE_ENGINE)
      return _eglError(EGL_BAD_PARAMETER, "eglWaitNative");
   /* glXWaitX(); */

   return EGL_TRUE;
}

static EGLBoolean
dri2_bind_tex_image(_EGLDriver *drv,
		    _EGLDisplay *disp, _EGLSurface *surf, EGLint buffer)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_context *dri2_ctx;
   _EGLContext *ctx;
   GLint format, target;
   __DRIdrawable *dri_drawable = dri2_dpy->vtbl->get_dri_drawable(surf);

   ctx = _eglGetCurrentContext();
   dri2_ctx = dri2_egl_context(ctx);

   if (!_eglBindTexImage(drv, disp, surf, buffer))
      return EGL_FALSE;

   switch (surf->TextureFormat) {
   case EGL_TEXTURE_RGB:
      format = __DRI_TEXTURE_FORMAT_RGB;
      break;
   case EGL_TEXTURE_RGBA:
      format = __DRI_TEXTURE_FORMAT_RGBA;
      break;
   default:
      assert(!"Unexpected texture format in dri2_bind_tex_image()");
      format = __DRI_TEXTURE_FORMAT_RGBA;
   }

   switch (surf->TextureTarget) {
   case EGL_TEXTURE_2D:
      target = GL_TEXTURE_2D;
      break;
   default:
      target = GL_TEXTURE_2D;
      assert(!"Unexpected texture target in dri2_bind_tex_image()");
   }

   (*dri2_dpy->tex_buffer->setTexBuffer2)(dri2_ctx->dri_context,
					  target, format,
					  dri_drawable);

   return EGL_TRUE;
}

static EGLBoolean
dri2_release_tex_image(_EGLDriver *drv,
		       _EGLDisplay *disp, _EGLSurface *surf, EGLint buffer)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_context *dri2_ctx;
   _EGLContext *ctx;
   GLint  target;
   __DRIdrawable *dri_drawable = dri2_dpy->vtbl->get_dri_drawable(surf);

   ctx = _eglGetCurrentContext();
   dri2_ctx = dri2_egl_context(ctx);

   if (!_eglReleaseTexImage(drv, disp, surf, buffer))
      return EGL_FALSE;

   switch (surf->TextureTarget) {
   case EGL_TEXTURE_2D:
      target = GL_TEXTURE_2D;
      break;
   default:
      assert(0);
   }

   if (dri2_dpy->tex_buffer->base.version >= 3 &&
       dri2_dpy->tex_buffer->releaseTexBuffer != NULL) {
      (*dri2_dpy->tex_buffer->releaseTexBuffer)(dri2_ctx->dri_context,
                                                target,
                                                dri_drawable);
   }

   return EGL_TRUE;
}

static _EGLImage*
dri2_create_image(_EGLDriver *drv, _EGLDisplay *dpy, _EGLContext *ctx,
                  EGLenum target, EGLClientBuffer buffer,
                  const EGLint *attr_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->create_image(drv, dpy, ctx, target, buffer,
                                       attr_list);
}

static _EGLImage *
dri2_create_image_from_dri(_EGLDisplay *disp, __DRIimage *dri_image)
{
   struct dri2_egl_image *dri2_img;

   if (dri_image == NULL) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_image");
      return NULL;
   }

   dri2_img = malloc(sizeof *dri2_img);
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_image");
      return NULL;
   }

   if (!_eglInitImage(&dri2_img->base, disp)) {
      free(dri2_img);
      return NULL;
   }

   dri2_img->dri_image = dri_image;

   return &dri2_img->base;
}

static _EGLImage *
dri2_create_image_khr_renderbuffer(_EGLDisplay *disp, _EGLContext *ctx,
				   EGLClientBuffer buffer,
				   const EGLint *attr_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
   GLuint renderbuffer = (GLuint) (uintptr_t) buffer;
   __DRIimage *dri_image;

   if (renderbuffer == 0) {
      _eglError(EGL_BAD_PARAMETER, "dri2_create_image_khr");
      return EGL_NO_IMAGE_KHR;
   }

   dri_image =
      dri2_dpy->image->createImageFromRenderbuffer(dri2_ctx->dri_context,
                                                   renderbuffer, NULL);

   return dri2_create_image_from_dri(disp, dri_image);
}

#ifdef HAVE_WAYLAND_PLATFORM

/* This structure describes how a wl_buffer maps to one or more
 * __DRIimages.  A wl_drm_buffer stores the wl_drm format code and the
 * offsets and strides of the planes in the buffer.  This table maps a
 * wl_drm format code to a description of the planes in the buffer
 * that lets us create a __DRIimage for each of the planes. */

static const struct wl_drm_components_descriptor {
   uint32_t dri_components;
   EGLint components;
   int nplanes;
} wl_drm_components[] = {
   { __DRI_IMAGE_COMPONENTS_RGB, EGL_TEXTURE_RGB, 1 },
   { __DRI_IMAGE_COMPONENTS_RGBA, EGL_TEXTURE_RGBA, 1 },
   { __DRI_IMAGE_COMPONENTS_Y_U_V, EGL_TEXTURE_Y_U_V_WL, 3 },
   { __DRI_IMAGE_COMPONENTS_Y_UV, EGL_TEXTURE_Y_UV_WL, 2 },
   { __DRI_IMAGE_COMPONENTS_Y_XUXV, EGL_TEXTURE_Y_XUXV_WL, 2 },
};

static _EGLImage *
dri2_create_image_wayland_wl_buffer(_EGLDisplay *disp, _EGLContext *ctx,
				    EGLClientBuffer _buffer,
				    const EGLint *attr_list)
{
   struct wl_drm_buffer *buffer;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   const struct wl_drm_components_descriptor *f;
   __DRIimage *dri_image;
   _EGLImageAttribs attrs;
   EGLint err;
   int32_t plane;

   buffer = wayland_drm_buffer_get(dri2_dpy->wl_server_drm,
                                   (struct wl_resource *) _buffer);
   if (!buffer)
       return NULL;

   err = _eglParseImageAttribList(&attrs, disp, attr_list);
   plane = attrs.PlaneWL;
   if (err != EGL_SUCCESS) {
      _eglError(EGL_BAD_PARAMETER, "dri2_create_image_wayland_wl_buffer");
      return NULL;
   }

   f = buffer->driver_format;
   if (plane < 0 || plane >= f->nplanes) {
      _eglError(EGL_BAD_PARAMETER,
                "dri2_create_image_wayland_wl_buffer (plane out of bounds)");
      return NULL;
   }

   dri_image = dri2_dpy->image->fromPlanar(buffer->driver_buffer, plane, NULL);

   if (dri_image == NULL) {
      _eglError(EGL_BAD_PARAMETER, "dri2_create_image_wayland_wl_buffer");
      return NULL;
   }

   return dri2_create_image_from_dri(disp, dri_image);
}
#endif

static EGLBoolean
dri2_get_sync_values_chromium(_EGLDisplay *dpy, _EGLSurface *surf,
                              EGLuint64KHR *ust, EGLuint64KHR *msc,
                              EGLuint64KHR *sbc)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->get_sync_values(dpy, surf, ust, msc, sbc);
}

/**
 * Set the error code after a call to
 * dri2_egl_image::dri_image::createImageFromTexture.
 */
static void
dri2_create_image_khr_texture_error(int dri_error)
{
   EGLint egl_error;

   switch (dri_error) {
   case __DRI_IMAGE_ERROR_SUCCESS:
      return;

   case __DRI_IMAGE_ERROR_BAD_ALLOC:
      egl_error = EGL_BAD_ALLOC;
      break;

   case __DRI_IMAGE_ERROR_BAD_MATCH:
      egl_error = EGL_BAD_MATCH;
      break;

   case __DRI_IMAGE_ERROR_BAD_PARAMETER:
      egl_error = EGL_BAD_PARAMETER;
      break;

   case __DRI_IMAGE_ERROR_BAD_ACCESS:
      egl_error = EGL_BAD_ACCESS;
      break;

   default:
      assert(0);
      egl_error = EGL_BAD_MATCH;
      break;
   }

   _eglError(egl_error, "dri2_create_image_khr_texture");
}

static _EGLImage *
dri2_create_image_khr_texture(_EGLDisplay *disp, _EGLContext *ctx,
				   EGLenum target,
				   EGLClientBuffer buffer,
				   const EGLint *attr_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
   struct dri2_egl_image *dri2_img;
   GLuint texture = (GLuint) (uintptr_t) buffer;
   _EGLImageAttribs attrs;
   GLuint depth;
   GLenum gl_target;
   unsigned error;

   if (texture == 0) {
      _eglError(EGL_BAD_PARAMETER, "dri2_create_image_khr");
      return EGL_NO_IMAGE_KHR;
   }

   if (_eglParseImageAttribList(&attrs, disp, attr_list) != EGL_SUCCESS)
      return EGL_NO_IMAGE_KHR;

   switch (target) {
   case EGL_GL_TEXTURE_2D_KHR:
      depth = 0;
      gl_target = GL_TEXTURE_2D;
      break;
   case EGL_GL_TEXTURE_3D_KHR:
      if (disp->Extensions.KHR_gl_texture_3D_image) {
         depth = attrs.GLTextureZOffset;
         gl_target = GL_TEXTURE_3D;
         break;
      }
      else {
         _eglError(EGL_BAD_PARAMETER, "dri2_create_image_khr");
         return EGL_NO_IMAGE_KHR;
      }
   case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_X_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Y_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Z_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_KHR:
      depth = target - EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X_KHR;
      gl_target = GL_TEXTURE_CUBE_MAP;
      break;
   default:
      _eglError(EGL_BAD_PARAMETER, "dri2_create_image_khr");
      return EGL_NO_IMAGE_KHR;
   }

   dri2_img = malloc(sizeof *dri2_img);
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_image_khr");
      return EGL_NO_IMAGE_KHR;
   }

   if (!_eglInitImage(&dri2_img->base, disp)) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_image_khr");
      free(dri2_img);
      return EGL_NO_IMAGE_KHR;
   }

   dri2_img->dri_image =
      dri2_dpy->image->createImageFromTexture(dri2_ctx->dri_context,
                                              gl_target,
                                              texture,
                                              depth,
                                              attrs.GLTextureLevel,
                                              &error,
                                              dri2_img);
   dri2_create_image_khr_texture_error(error);

   if (!dri2_img->dri_image) {
      free(dri2_img);
      return EGL_NO_IMAGE_KHR;
   }
   return &dri2_img->base;
}

static EGLBoolean
dri2_query_surface(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSurface *surf,
                   EGLint attribute, EGLint *value)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   if (!dri2_dpy->vtbl->query_surface)
      return _eglQuerySurface(drv, dpy, surf, attribute, value);
   return dri2_dpy->vtbl->query_surface(drv, dpy, surf, attribute, value);
}

static struct wl_buffer*
dri2_create_wayland_buffer_from_image(_EGLDriver *drv, _EGLDisplay *dpy,
                                      _EGLImage *img)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   return dri2_dpy->vtbl->create_wayland_buffer_from_image(drv, dpy, img);
}

#ifdef HAVE_LIBDRM
static _EGLImage *
dri2_create_image_mesa_drm_buffer(_EGLDisplay *disp, _EGLContext *ctx,
				  EGLClientBuffer buffer, const EGLint *attr_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   EGLint format, name, pitch, err;
   _EGLImageAttribs attrs;
   __DRIimage *dri_image;

   name = (EGLint) (uintptr_t) buffer;

   err = _eglParseImageAttribList(&attrs, disp, attr_list);
   if (err != EGL_SUCCESS)
      return NULL;

   if (attrs.Width <= 0 || attrs.Height <= 0 ||
       attrs.DRMBufferStrideMESA <= 0) {
      _eglError(EGL_BAD_PARAMETER,
		"bad width, height or stride");
      return NULL;
   }

   switch (attrs.DRMBufferFormatMESA) {
   case EGL_DRM_BUFFER_FORMAT_ARGB32_MESA:
      format = __DRI_IMAGE_FORMAT_ARGB8888;
      pitch = attrs.DRMBufferStrideMESA;
      break;
   default:
      _eglError(EGL_BAD_PARAMETER,
		"dri2_create_image_khr: unsupported pixmap depth");
      return NULL;
   }

   dri_image =
      dri2_dpy->image->createImageFromName(dri2_dpy->dri_screen,
					   attrs.Width,
					   attrs.Height,
					   format,
					   name,
					   pitch,
					   NULL);

   return dri2_create_image_from_dri(disp, dri_image);
}

static EGLBoolean
dri2_check_dma_buf_attribs(const _EGLImageAttribs *attrs)
{
   unsigned i;

   /**
     * The spec says:
     *
     * "Required attributes and their values are as follows:
     *
     *  * EGL_WIDTH & EGL_HEIGHT: The logical dimensions of the buffer in pixels
     *
     *  * EGL_LINUX_DRM_FOURCC_EXT: The pixel format of the buffer, as specified
     *    by drm_fourcc.h and used as the pixel_format parameter of the
     *    drm_mode_fb_cmd2 ioctl."
     *
     * and
     *
     * "* If <target> is EGL_LINUX_DMA_BUF_EXT, and the list of attributes is
     *    incomplete, EGL_BAD_PARAMETER is generated."
     */
   if (attrs->Width <= 0 || attrs->Height <= 0 ||
       !attrs->DMABufFourCC.IsPresent) {
      _eglError(EGL_BAD_PARAMETER, "attribute(s) missing");
      return EGL_FALSE;
   }

   /**
    * Also:
    *
    * "If <target> is EGL_LINUX_DMA_BUF_EXT and one or more of the values
    *  specified for a plane's pitch or offset isn't supported by EGL,
    *  EGL_BAD_ACCESS is generated."
    */
   for (i = 0; i < ARRAY_SIZE(attrs->DMABufPlanePitches); ++i) {
      if (attrs->DMABufPlanePitches[i].IsPresent &&
          attrs->DMABufPlanePitches[i].Value <= 0) {
         _eglError(EGL_BAD_ACCESS, "invalid pitch");
         return EGL_FALSE;
      }
   }

   return EGL_TRUE;
}

/* Returns the total number of file descriptors. Zero indicates an error. */
static unsigned
dri2_check_dma_buf_format(const _EGLImageAttribs *attrs)
{
   unsigned i, plane_n;

   switch (attrs->DMABufFourCC.Value) {
   case DRM_FORMAT_R8:
   case DRM_FORMAT_RG88:
   case DRM_FORMAT_GR88:
   case DRM_FORMAT_RGB332:
   case DRM_FORMAT_BGR233:
   case DRM_FORMAT_XRGB4444:
   case DRM_FORMAT_XBGR4444:
   case DRM_FORMAT_RGBX4444:
   case DRM_FORMAT_BGRX4444:
   case DRM_FORMAT_ARGB4444:
   case DRM_FORMAT_ABGR4444:
   case DRM_FORMAT_RGBA4444:
   case DRM_FORMAT_BGRA4444:
   case DRM_FORMAT_XRGB1555:
   case DRM_FORMAT_XBGR1555:
   case DRM_FORMAT_RGBX5551:
   case DRM_FORMAT_BGRX5551:
   case DRM_FORMAT_ARGB1555:
   case DRM_FORMAT_ABGR1555:
   case DRM_FORMAT_RGBA5551:
   case DRM_FORMAT_BGRA5551:
   case DRM_FORMAT_RGB565:
   case DRM_FORMAT_BGR565:
   case DRM_FORMAT_RGB888:
   case DRM_FORMAT_BGR888:
   case DRM_FORMAT_XRGB8888:
   case DRM_FORMAT_XBGR8888:
   case DRM_FORMAT_RGBX8888:
   case DRM_FORMAT_BGRX8888:
   case DRM_FORMAT_ARGB8888:
   case DRM_FORMAT_ABGR8888:
   case DRM_FORMAT_RGBA8888:
   case DRM_FORMAT_BGRA8888:
   case DRM_FORMAT_XRGB2101010:
   case DRM_FORMAT_XBGR2101010:
   case DRM_FORMAT_RGBX1010102:
   case DRM_FORMAT_BGRX1010102:
   case DRM_FORMAT_ARGB2101010:
   case DRM_FORMAT_ABGR2101010:
   case DRM_FORMAT_RGBA1010102:
   case DRM_FORMAT_BGRA1010102:
   case DRM_FORMAT_YUYV:
   case DRM_FORMAT_YVYU:
   case DRM_FORMAT_UYVY:
   case DRM_FORMAT_VYUY:
      plane_n = 1;
      break;
   case DRM_FORMAT_NV12:
   case DRM_FORMAT_NV21:
   case DRM_FORMAT_NV16:
   case DRM_FORMAT_NV61:
      plane_n = 2;
      break;
   case DRM_FORMAT_YUV410:
   case DRM_FORMAT_YVU410:
   case DRM_FORMAT_YUV411:
   case DRM_FORMAT_YVU411:
   case DRM_FORMAT_YUV420:
   case DRM_FORMAT_YVU420:
   case DRM_FORMAT_YUV422:
   case DRM_FORMAT_YVU422:
   case DRM_FORMAT_YUV444:
   case DRM_FORMAT_YVU444:
      plane_n = 3;
      break;
   default:
      _eglError(EGL_BAD_ATTRIBUTE, "invalid format");
      return 0;
   }

   /**
     * The spec says:
     *
     * "* If <target> is EGL_LINUX_DMA_BUF_EXT, and the list of attributes is
     *    incomplete, EGL_BAD_PARAMETER is generated."
     */
   for (i = 0; i < plane_n; ++i) {
      if (!attrs->DMABufPlaneFds[i].IsPresent ||
          !attrs->DMABufPlaneOffsets[i].IsPresent ||
          !attrs->DMABufPlanePitches[i].IsPresent) {
         _eglError(EGL_BAD_PARAMETER, "plane attribute(s) missing");
         return 0;
      }
   }

   /**
    * The spec also says:
    *
    * "If <target> is EGL_LINUX_DMA_BUF_EXT, and the EGL_LINUX_DRM_FOURCC_EXT
    *  attribute indicates a single-plane format, EGL_BAD_ATTRIBUTE is
    *  generated if any of the EGL_DMA_BUF_PLANE1_* or EGL_DMA_BUF_PLANE2_*
    *  attributes are specified."
    */
   for (i = plane_n; i < 3; ++i) {
      if (attrs->DMABufPlaneFds[i].IsPresent ||
          attrs->DMABufPlaneOffsets[i].IsPresent ||
          attrs->DMABufPlanePitches[i].IsPresent) {
         _eglError(EGL_BAD_ATTRIBUTE, "too many plane attributes");
         return 0;
      }
   }

   return plane_n;
}

/**
 * The spec says:
 *
 * "If eglCreateImageKHR is successful for a EGL_LINUX_DMA_BUF_EXT target, the
 *  EGL will take a reference to the dma_buf(s) which it will release at any
 *  time while the EGLDisplay is initialized. It is the responsibility of the
 *  application to close the dma_buf file descriptors."
 *
 * Therefore we must never close or otherwise modify the file descriptors.
 */
_EGLImage *
dri2_create_image_dma_buf(_EGLDisplay *disp, _EGLContext *ctx,
			  EGLClientBuffer buffer, const EGLint *attr_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   _EGLImage *res;
   EGLint err;
   _EGLImageAttribs attrs;
   __DRIimage *dri_image;
   unsigned num_fds;
   unsigned i;
   int fds[3];
   int pitches[3];
   int offsets[3];
   unsigned error;

   /**
    * The spec says:
    *
    * ""* If <target> is EGL_LINUX_DMA_BUF_EXT and <buffer> is not NULL, the
    *     error EGL_BAD_PARAMETER is generated."
    */
   if (buffer != NULL) {
      _eglError(EGL_BAD_PARAMETER, "buffer not NULL");
      return NULL;
   }

   err = _eglParseImageAttribList(&attrs, disp, attr_list);
   if (err != EGL_SUCCESS) {
      _eglError(err, "bad attribute");
      return NULL;
   }

   if (!dri2_check_dma_buf_attribs(&attrs))
      return NULL;

   num_fds = dri2_check_dma_buf_format(&attrs);
   if (!num_fds)
      return NULL;

   for (i = 0; i < num_fds; ++i) {
      fds[i] = attrs.DMABufPlaneFds[i].Value;
      pitches[i] = attrs.DMABufPlanePitches[i].Value;
      offsets[i] = attrs.DMABufPlaneOffsets[i].Value;
   }

   dri_image =
      dri2_dpy->image->createImageFromDmaBufs(dri2_dpy->dri_screen,
         attrs.Width, attrs.Height, attrs.DMABufFourCC.Value,
         fds, num_fds, pitches, offsets,
         attrs.DMABufYuvColorSpaceHint.Value,
         attrs.DMABufSampleRangeHint.Value,
         attrs.DMABufChromaHorizontalSiting.Value,
         attrs.DMABufChromaVerticalSiting.Value,
         &error,
         NULL);
   dri2_create_image_khr_texture_error(error);

   if (!dri_image)
      return EGL_NO_IMAGE_KHR;

   res = dri2_create_image_from_dri(disp, dri_image);

   return res;
}
static _EGLImage *
dri2_create_drm_image_mesa(_EGLDriver *drv, _EGLDisplay *disp,
			   const EGLint *attr_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img;
   _EGLImageAttribs attrs;
   unsigned int dri_use, valid_mask;
   int format;
   EGLint err = EGL_SUCCESS;

   (void) drv;

   dri2_img = malloc(sizeof *dri2_img);
   if (!dri2_img) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_image_khr");
      return EGL_NO_IMAGE_KHR;
   }

   if (!attr_list) {
      err = EGL_BAD_PARAMETER;
      goto cleanup_img;
   }

   if (!_eglInitImage(&dri2_img->base, disp)) {
      err = EGL_BAD_PARAMETER;
      goto cleanup_img;
   }

   err = _eglParseImageAttribList(&attrs, disp, attr_list);
   if (err != EGL_SUCCESS)
      goto cleanup_img;

   if (attrs.Width <= 0 || attrs.Height <= 0) {
      _eglLog(_EGL_WARNING, "bad width or height (%dx%d)",
            attrs.Width, attrs.Height);
      goto cleanup_img;
   }

   switch (attrs.DRMBufferFormatMESA) {
   case EGL_DRM_BUFFER_FORMAT_ARGB32_MESA:
      format = __DRI_IMAGE_FORMAT_ARGB8888;
      break;
   default:
      _eglLog(_EGL_WARNING, "bad image format value 0x%04x",
            attrs.DRMBufferFormatMESA);
      goto cleanup_img;
   }

   valid_mask =
      EGL_DRM_BUFFER_USE_SCANOUT_MESA |
      EGL_DRM_BUFFER_USE_SHARE_MESA |
      EGL_DRM_BUFFER_USE_CURSOR_MESA;
   if (attrs.DRMBufferUseMESA & ~valid_mask) {
      _eglLog(_EGL_WARNING, "bad image use bit 0x%04x",
            attrs.DRMBufferUseMESA & ~valid_mask);
      goto cleanup_img;
   }

   dri_use = 0;
   if (attrs.DRMBufferUseMESA & EGL_DRM_BUFFER_USE_SHARE_MESA)
      dri_use |= __DRI_IMAGE_USE_SHARE;
   if (attrs.DRMBufferUseMESA & EGL_DRM_BUFFER_USE_SCANOUT_MESA)
      dri_use |= __DRI_IMAGE_USE_SCANOUT;
   if (attrs.DRMBufferUseMESA & EGL_DRM_BUFFER_USE_CURSOR_MESA)
      dri_use |= __DRI_IMAGE_USE_CURSOR;

   dri2_img->dri_image =
      dri2_dpy->image->createImage(dri2_dpy->dri_screen,
				   attrs.Width, attrs.Height,
                                   format, dri_use, dri2_img);
   if (dri2_img->dri_image == NULL) {
      err = EGL_BAD_ALLOC;
      goto cleanup_img;
   }

   return &dri2_img->base;

 cleanup_img:
   free(dri2_img);
   _eglError(err, "dri2_create_drm_image_mesa");

   return EGL_NO_IMAGE_KHR;
}

static EGLBoolean
dri2_export_drm_image_mesa(_EGLDriver *drv, _EGLDisplay *disp, _EGLImage *img,
			  EGLint *name, EGLint *handle, EGLint *stride)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img = dri2_egl_image(img);

   (void) drv;

   if (name && !dri2_dpy->image->queryImage(dri2_img->dri_image,
					    __DRI_IMAGE_ATTRIB_NAME, name)) {
      _eglError(EGL_BAD_ALLOC, "dri2_export_drm_image_mesa");
      return EGL_FALSE;
   }

   if (handle)
      dri2_dpy->image->queryImage(dri2_img->dri_image,
				  __DRI_IMAGE_ATTRIB_HANDLE, handle);

   if (stride)
      dri2_dpy->image->queryImage(dri2_img->dri_image,
				  __DRI_IMAGE_ATTRIB_STRIDE, stride);

   return EGL_TRUE;
}

static EGLBoolean
dri2_export_dma_buf_image_query_mesa(_EGLDriver *drv, _EGLDisplay *disp,
                                     _EGLImage *img,
                                     EGLint *fourcc, EGLint *nplanes,
                                     EGLuint64KHR *modifiers)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img = dri2_egl_image(img);

   (void) drv;


   if (nplanes)
      dri2_dpy->image->queryImage(dri2_img->dri_image,
				  __DRI_IMAGE_ATTRIB_NUM_PLANES, nplanes);
   if (fourcc)
      dri2_dpy->image->queryImage(dri2_img->dri_image,
				  __DRI_IMAGE_ATTRIB_FOURCC, fourcc);

   if (modifiers)
      *modifiers = 0;

   return EGL_TRUE;
}

static EGLBoolean
dri2_export_dma_buf_image_mesa(_EGLDriver *drv, _EGLDisplay *disp, _EGLImage *img,
                               int *fds, EGLint *strides, EGLint *offsets)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img = dri2_egl_image(img);

   (void) drv;

   /* rework later to provide multiple fds/strides/offsets */
   if (fds)
      dri2_dpy->image->queryImage(dri2_img->dri_image,
				  __DRI_IMAGE_ATTRIB_FD, fds);

   if (strides)
      dri2_dpy->image->queryImage(dri2_img->dri_image,
				  __DRI_IMAGE_ATTRIB_STRIDE, strides);

   if (offsets)
      offsets[0] = 0;

   return EGL_TRUE;
}

#endif

_EGLImage *
dri2_create_image_khr(_EGLDriver *drv, _EGLDisplay *disp,
		      _EGLContext *ctx, EGLenum target,
		      EGLClientBuffer buffer, const EGLint *attr_list)
{
   (void) drv;

   switch (target) {
   case EGL_GL_TEXTURE_2D_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_X_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_X_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Y_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_POSITIVE_Z_KHR:
   case EGL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_KHR:
      return dri2_create_image_khr_texture(disp, ctx, target, buffer, attr_list);
   case EGL_GL_TEXTURE_3D_KHR:
      if (disp->Extensions.KHR_gl_texture_3D_image) {
         return dri2_create_image_khr_texture(disp, ctx, target, buffer, attr_list);
      }
      else {
         _eglError(EGL_BAD_PARAMETER, "dri2_create_image_khr");
         return EGL_NO_IMAGE_KHR;
      }
   case EGL_GL_RENDERBUFFER_KHR:
      return dri2_create_image_khr_renderbuffer(disp, ctx, buffer, attr_list);
#ifdef HAVE_LIBDRM
   case EGL_DRM_BUFFER_MESA:
      return dri2_create_image_mesa_drm_buffer(disp, ctx, buffer, attr_list);
   case EGL_LINUX_DMA_BUF_EXT:
      return dri2_create_image_dma_buf(disp, ctx, buffer, attr_list);
#endif
#ifdef HAVE_WAYLAND_PLATFORM
   case EGL_WAYLAND_BUFFER_WL:
      return dri2_create_image_wayland_wl_buffer(disp, ctx, buffer, attr_list);
#endif
   default:
      _eglError(EGL_BAD_PARAMETER, "dri2_create_image_khr");
      return EGL_NO_IMAGE_KHR;
   }
}

static EGLBoolean
dri2_destroy_image_khr(_EGLDriver *drv, _EGLDisplay *disp, _EGLImage *image)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_image *dri2_img = dri2_egl_image(image);

   (void) drv;

   dri2_dpy->image->destroyImage(dri2_img->dri_image);
   free(dri2_img);

   return EGL_TRUE;
}

#ifdef HAVE_WAYLAND_PLATFORM

static void
dri2_wl_reference_buffer(void *user_data, uint32_t name, int fd,
                         struct wl_drm_buffer *buffer)
{
   _EGLDisplay *disp = user_data;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   __DRIimage *img;
   int i, dri_components = 0;

   if (fd == -1)
      img = dri2_dpy->image->createImageFromNames(dri2_dpy->dri_screen,
                                                  buffer->width,
                                                  buffer->height,
                                                  buffer->format,
                                                  (int*)&name, 1,
                                                  buffer->stride,
                                                  buffer->offset,
                                                  NULL);
   else
      img = dri2_dpy->image->createImageFromFds(dri2_dpy->dri_screen,
                                                buffer->width,
                                                buffer->height,
                                                buffer->format,
                                                &fd, 1,
                                                buffer->stride,
                                                buffer->offset,
                                                NULL);

   if (img == NULL)
      return;

   dri2_dpy->image->queryImage(img, __DRI_IMAGE_ATTRIB_COMPONENTS, &dri_components);

   buffer->driver_format = NULL;
   for (i = 0; i < ARRAY_SIZE(wl_drm_components); i++)
      if (wl_drm_components[i].dri_components == dri_components)
         buffer->driver_format = &wl_drm_components[i];

   if (buffer->driver_format == NULL)
      dri2_dpy->image->destroyImage(img);
   else
      buffer->driver_buffer = img;
}

static void
dri2_wl_release_buffer(void *user_data, struct wl_drm_buffer *buffer)
{
   _EGLDisplay *disp = user_data;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   dri2_dpy->image->destroyImage(buffer->driver_buffer);
}

static struct wayland_drm_callbacks wl_drm_callbacks = {
	.authenticate = NULL,
	.reference_buffer = dri2_wl_reference_buffer,
	.release_buffer = dri2_wl_release_buffer
};

static EGLBoolean
dri2_bind_wayland_display_wl(_EGLDriver *drv, _EGLDisplay *disp,
			     struct wl_display *wl_dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   int flags = 0;
   uint64_t cap;

   (void) drv;

   if (dri2_dpy->wl_server_drm)
	   return EGL_FALSE;

   wl_drm_callbacks.authenticate =
      (int(*)(void *, uint32_t)) dri2_dpy->vtbl->authenticate;

   if (drmGetCap(dri2_dpy->fd, DRM_CAP_PRIME, &cap) == 0 &&
       cap == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT) &&
       dri2_dpy->image->base.version >= 7 &&
       dri2_dpy->image->createImageFromFds != NULL)
      flags |= WAYLAND_DRM_PRIME;

   dri2_dpy->wl_server_drm =
	   wayland_drm_init(wl_dpy, dri2_dpy->device_name,
                            &wl_drm_callbacks, disp, flags);

   if (!dri2_dpy->wl_server_drm)
	   return EGL_FALSE;

#ifdef HAVE_DRM_PLATFORM
   /* We have to share the wl_drm instance with gbm, so gbm can convert
    * wl_buffers to gbm bos. */
   if (dri2_dpy->gbm_dri)
      dri2_dpy->gbm_dri->wl_drm = dri2_dpy->wl_server_drm;
#endif

   return EGL_TRUE;
}

static EGLBoolean
dri2_unbind_wayland_display_wl(_EGLDriver *drv, _EGLDisplay *disp,
			       struct wl_display *wl_dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   (void) drv;

   if (!dri2_dpy->wl_server_drm)
	   return EGL_FALSE;

   wayland_drm_uninit(dri2_dpy->wl_server_drm);
   dri2_dpy->wl_server_drm = NULL;

   return EGL_TRUE;
}

static EGLBoolean
dri2_query_wayland_buffer_wl(_EGLDriver *drv, _EGLDisplay *disp,
                             struct wl_resource *buffer_resource,
                             EGLint attribute, EGLint *value)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct wl_drm_buffer *buffer;
   const struct wl_drm_components_descriptor *format;

   buffer = wayland_drm_buffer_get(dri2_dpy->wl_server_drm, buffer_resource);
   if (!buffer)
      return EGL_FALSE;

   format = buffer->driver_format;
   switch (attribute) {
   case EGL_TEXTURE_FORMAT:
      *value = format->components;
      return EGL_TRUE;
   case EGL_WIDTH:
      *value = buffer->width;
      return EGL_TRUE;
   case EGL_HEIGHT:
      *value = buffer->height;
      return EGL_TRUE;
   }

   return EGL_FALSE;
}
#endif

static void
dri2_egl_ref_sync(struct dri2_egl_sync *sync)
{
   p_atomic_inc(&sync->refcount);
}

static void
dri2_egl_unref_sync(struct dri2_egl_display *dri2_dpy,
                    struct dri2_egl_sync *dri2_sync)
{
   if (p_atomic_dec_zero(&dri2_sync->refcount)) {
      if (dri2_sync->base.Type == EGL_SYNC_REUSABLE_KHR)
         cnd_destroy(&dri2_sync->cond);

      if (dri2_sync->fence)
         dri2_dpy->fence->destroy_fence(dri2_dpy->dri_screen, dri2_sync->fence);

      free(dri2_sync);
   }
}

static _EGLSync *
dri2_create_sync(_EGLDriver *drv, _EGLDisplay *dpy,
                 EGLenum type, const EGLint *attrib_list,
                 const EGLAttrib *attrib_list64)
{
   _EGLContext *ctx = _eglGetCurrentContext();
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
   struct dri2_egl_sync *dri2_sync;
   EGLint ret;
   pthread_condattr_t attr;

   dri2_sync = calloc(1, sizeof(struct dri2_egl_sync));
   if (!dri2_sync) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSyncKHR");
      return NULL;
   }

   if (!_eglInitSync(&dri2_sync->base, dpy, type, attrib_list,
                     attrib_list64)) {
      free(dri2_sync);
      return NULL;
   }

   switch (type) {
   case EGL_SYNC_FENCE_KHR:
      dri2_sync->fence = dri2_dpy->fence->create_fence(dri2_ctx->dri_context);
      if (!dri2_sync->fence) {
         /* Why did it fail? DRI doesn't return an error code, so we emit
          * a generic EGL error that doesn't communicate user error.
          */
         _eglError(EGL_BAD_ALLOC, "eglCreateSyncKHR");
         free(dri2_sync);
         return NULL;
      }
      break;

   case EGL_SYNC_CL_EVENT_KHR:
      dri2_sync->fence = dri2_dpy->fence->get_fence_from_cl_event(
                                 dri2_dpy->dri_screen,
                                 dri2_sync->base.CLEvent);
      /* this can only happen if the cl_event passed in is invalid. */
      if (!dri2_sync->fence) {
         _eglError(EGL_BAD_ATTRIBUTE, "eglCreateSyncKHR");
         free(dri2_sync);
         return NULL;
      }

      /* the initial status must be "signaled" if the cl_event is signaled */
      if (dri2_dpy->fence->client_wait_sync(dri2_ctx->dri_context,
                                            dri2_sync->fence, 0, 0))
         dri2_sync->base.SyncStatus = EGL_SIGNALED_KHR;
      break;

   case EGL_SYNC_REUSABLE_KHR:
      /* intialize attr */
      ret = pthread_condattr_init(&attr);

      if (ret) {
         _eglError(EGL_BAD_ACCESS, "eglCreateSyncKHR");
         free(dri2_sync);
         return NULL;
      }

      /* change clock attribute to CLOCK_MONOTONIC */
      ret = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);

      if (ret) {
         _eglError(EGL_BAD_ACCESS, "eglCreateSyncKHR");
         free(dri2_sync);
         return NULL;
      }

      ret = pthread_cond_init(&dri2_sync->cond, &attr);

      if (ret) {
         _eglError(EGL_BAD_ACCESS, "eglCreateSyncKHR");
         free(dri2_sync);
         return NULL;
      }

      /* initial status of reusable sync must be "unsignaled" */
      dri2_sync->base.SyncStatus = EGL_UNSIGNALED_KHR;
      break;
   }

   p_atomic_set(&dri2_sync->refcount, 1);
   return &dri2_sync->base;
}

static EGLBoolean
dri2_destroy_sync(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSync *sync)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_sync *dri2_sync = dri2_egl_sync(sync);
   EGLint ret = EGL_TRUE;
   EGLint err;

   /* if type of sync is EGL_SYNC_REUSABLE_KHR and it is not signaled yet,
    * then unlock all threads possibly blocked by the reusable sync before
    * destroying it.
    */
   if (dri2_sync->base.Type == EGL_SYNC_REUSABLE_KHR &&
       dri2_sync->base.SyncStatus == EGL_UNSIGNALED_KHR) {
      dri2_sync->base.SyncStatus = EGL_SIGNALED_KHR;
      /* unblock all threads currently blocked by sync */
      err = cnd_broadcast(&dri2_sync->cond);

      if (err) {
         _eglError(EGL_BAD_ACCESS, "eglDestroySyncKHR");
         ret = EGL_FALSE;
      }
   }
   dri2_egl_unref_sync(dri2_dpy, dri2_sync);

   return ret;
}

static EGLint
dri2_client_wait_sync(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSync *sync,
                      EGLint flags, EGLTime timeout)
{
   _EGLContext *ctx = _eglGetCurrentContext();
   struct dri2_egl_driver *dri2_drv = dri2_egl_driver(drv);
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
   struct dri2_egl_sync *dri2_sync = dri2_egl_sync(sync);
   unsigned wait_flags = 0;

   /* timespecs for cnd_timedwait */
   struct timespec current;
   xtime expire;

   EGLint ret = EGL_CONDITION_SATISFIED_KHR;

   /* The EGL_KHR_fence_sync spec states:
    *
    *    "If no context is current for the bound API,
    *     the EGL_SYNC_FLUSH_COMMANDS_BIT_KHR bit is ignored.
    */
   if (dri2_ctx && flags & EGL_SYNC_FLUSH_COMMANDS_BIT_KHR)
      wait_flags |= __DRI2_FENCE_FLAG_FLUSH_COMMANDS;

   /* the sync object should take a reference while waiting */
   dri2_egl_ref_sync(dri2_sync);

   switch (sync->Type) {
   case EGL_SYNC_FENCE_KHR:
   case EGL_SYNC_CL_EVENT_KHR:
      if (dri2_dpy->fence->client_wait_sync(dri2_ctx ? dri2_ctx->dri_context : NULL,
                                         dri2_sync->fence, wait_flags,
                                         timeout))
         dri2_sync->base.SyncStatus = EGL_SIGNALED_KHR;
      else
         ret = EGL_TIMEOUT_EXPIRED_KHR;
      break;

   case EGL_SYNC_REUSABLE_KHR:
      if (dri2_ctx && dri2_sync->base.SyncStatus == EGL_UNSIGNALED_KHR &&
          (flags & EGL_SYNC_FLUSH_COMMANDS_BIT_KHR)) {
         /* flush context if EGL_SYNC_FLUSH_COMMANDS_BIT_KHR is set */
         if (dri2_drv->glFlush)
            dri2_drv->glFlush();
      }

      /* if timeout is EGL_FOREVER_KHR, it should wait without any timeout.*/
      if (timeout == EGL_FOREVER_KHR) {
         mtx_lock(&dri2_sync->mutex);
         cnd_wait(&dri2_sync->cond, &dri2_sync->mutex);
         mtx_unlock(&dri2_sync->mutex);
      } else {
         /* if reusable sync has not been yet signaled */
         if (dri2_sync->base.SyncStatus != EGL_SIGNALED_KHR) {
            clock_gettime(CLOCK_MONOTONIC, &current);

            /* calculating when to expire */
            expire.nsec = timeout % 1000000000L;
            expire.sec = timeout / 1000000000L;

            expire.nsec += current.tv_nsec;
            expire.sec += current.tv_sec;

            /* expire.nsec now is a number between 0 and 1999999998 */
            if (expire.nsec > 999999999L) {
               expire.sec++;
               expire.nsec -= 1000000000L;
            }

            mtx_lock(&dri2_sync->mutex);
            ret = cnd_timedwait(&dri2_sync->cond, &dri2_sync->mutex, &expire);
            mtx_unlock(&dri2_sync->mutex);

            if (ret == thrd_busy) {
               if (dri2_sync->base.SyncStatus == EGL_UNSIGNALED_KHR) {
                  ret = EGL_TIMEOUT_EXPIRED_KHR;
               } else {
                  _eglError(EGL_BAD_ACCESS, "eglClientWaitSyncKHR");
                  ret = EGL_FALSE;
               }
            }
         }
      }
      break;
  }
  dri2_egl_unref_sync(dri2_dpy, dri2_sync);

  return ret;
}

static EGLBoolean
dri2_signal_sync(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSync *sync,
                      EGLenum mode)
{
   struct dri2_egl_sync *dri2_sync = dri2_egl_sync(sync);
   EGLint ret;

   if (sync->Type != EGL_SYNC_REUSABLE_KHR) {
      _eglError(EGL_BAD_MATCH, "eglSignalSyncKHR");
      return EGL_FALSE;
   }

   if (mode != EGL_SIGNALED_KHR && mode != EGL_UNSIGNALED_KHR) {
      _eglError(EGL_BAD_ATTRIBUTE, "eglSignalSyncKHR");
      return EGL_FALSE;
   }

   dri2_sync->base.SyncStatus = mode;

   if (mode == EGL_SIGNALED_KHR) {
      ret = cnd_broadcast(&dri2_sync->cond);

      /* fail to broadcast */
      if (ret) {
         _eglError(EGL_BAD_ACCESS, "eglSignalSyncKHR");
         return EGL_FALSE;
      }
   }

   return EGL_TRUE;
}

static EGLint
dri2_server_wait_sync(_EGLDriver *drv, _EGLDisplay *dpy, _EGLSync *sync)
{
   _EGLContext *ctx = _eglGetCurrentContext();
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);
   struct dri2_egl_sync *dri2_sync = dri2_egl_sync(sync);

   dri2_dpy->fence->server_wait_sync(dri2_ctx->dri_context,
                                     dri2_sync->fence, 0);
   return EGL_TRUE;
}

static int
dri2_interop_query_device_info(_EGLDisplay *dpy, _EGLContext *ctx,
                               struct mesa_glinterop_device_info *out)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);

   if (!dri2_dpy->interop)
      return MESA_GLINTEROP_UNSUPPORTED;

   return dri2_dpy->interop->query_device_info(dri2_ctx->dri_context, out);
}

static int
dri2_interop_export_object(_EGLDisplay *dpy, _EGLContext *ctx,
                           struct mesa_glinterop_export_in *in,
                           struct mesa_glinterop_export_out *out)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);

   if (!dri2_dpy->interop)
      return MESA_GLINTEROP_UNSUPPORTED;

   return dri2_dpy->interop->export_object(dri2_ctx->dri_context, in, out);
}

static void
dri2_unload(_EGLDriver *drv)
{
   struct dri2_egl_driver *dri2_drv = dri2_egl_driver(drv);

   if (dri2_drv->handle)
      dlclose(dri2_drv->handle);
   free(dri2_drv);
}

static EGLBoolean
dri2_load(_EGLDriver *drv)
{
   struct dri2_egl_driver *dri2_drv = dri2_egl_driver(drv);
#ifdef HAVE_ANDROID_PLATFORM
   const char *libname = "libglapi.so";
#elif defined(__APPLE__)
   const char *libname = "libglapi.0.dylib";
#elif defined(__CYGWIN__)
   const char *libname = "cygglapi-0.dll";
#else
   const char *libname = "libglapi.so.0";
#endif
   void *handle;

   /* RTLD_GLOBAL to make sure glapi symbols are visible to DRI drivers */
   handle = dlopen(libname, RTLD_LAZY | RTLD_GLOBAL);
   if (handle) {
      dri2_drv->get_proc_address = (_EGLProc (*)(const char *))
         dlsym(handle, "_glapi_get_proc_address");
      if (!dri2_drv->get_proc_address || !libname) {
         /* no need to keep a reference */
         dlclose(handle);
         handle = NULL;
      }
   }

   /* if glapi is not available, loading DRI drivers will fail */
   if (!dri2_drv->get_proc_address) {
      _eglLog(_EGL_WARNING, "DRI2: failed to find _glapi_get_proc_address");
      return EGL_FALSE;
   }

   dri2_drv->glFlush = (void (*)(void))
      dri2_drv->get_proc_address("glFlush");

   dri2_drv->handle = handle;

   return EGL_TRUE;
}

/**
 * This is the main entrypoint into the driver, called by libEGL.
 * Create a new _EGLDriver object and init its dispatch table.
 */
_EGLDriver *
_eglBuiltInDriverDRI2(const char *args)
{
   struct dri2_egl_driver *dri2_drv;

   (void) args;

   dri2_drv = calloc(1, sizeof *dri2_drv);
   if (!dri2_drv)
      return NULL;

   if (!dri2_load(&dri2_drv->base)) {
      free(dri2_drv);
      return NULL;
   }

   _eglInitDriverFallbacks(&dri2_drv->base);
   dri2_drv->base.API.Initialize = dri2_initialize;
   dri2_drv->base.API.Terminate = dri2_terminate;
   dri2_drv->base.API.CreateContext = dri2_create_context;
   dri2_drv->base.API.DestroyContext = dri2_destroy_context;
   dri2_drv->base.API.MakeCurrent = dri2_make_current;
   dri2_drv->base.API.CreateWindowSurface = dri2_create_window_surface;
   dri2_drv->base.API.CreatePixmapSurface = dri2_create_pixmap_surface;
   dri2_drv->base.API.CreatePbufferSurface = dri2_create_pbuffer_surface;
   dri2_drv->base.API.DestroySurface = dri2_destroy_surface;
   dri2_drv->base.API.GetProcAddress = dri2_get_proc_address;
   dri2_drv->base.API.WaitClient = dri2_wait_client;
   dri2_drv->base.API.WaitNative = dri2_wait_native;
   dri2_drv->base.API.BindTexImage = dri2_bind_tex_image;
   dri2_drv->base.API.ReleaseTexImage = dri2_release_tex_image;
   dri2_drv->base.API.SwapInterval = dri2_swap_interval;
   dri2_drv->base.API.SwapBuffers = dri2_swap_buffers;
   dri2_drv->base.API.SwapBuffersWithDamageEXT = dri2_swap_buffers_with_damage;
   dri2_drv->base.API.SwapBuffersRegionNOK = dri2_swap_buffers_region;
   dri2_drv->base.API.PostSubBufferNV = dri2_post_sub_buffer;
   dri2_drv->base.API.CopyBuffers = dri2_copy_buffers,
   dri2_drv->base.API.QueryBufferAge = dri2_query_buffer_age;
   dri2_drv->base.API.CreateImageKHR = dri2_create_image;
   dri2_drv->base.API.DestroyImageKHR = dri2_destroy_image_khr;
   dri2_drv->base.API.CreateWaylandBufferFromImageWL = dri2_create_wayland_buffer_from_image;
   dri2_drv->base.API.QuerySurface = dri2_query_surface;
#ifdef HAVE_LIBDRM
   dri2_drv->base.API.CreateDRMImageMESA = dri2_create_drm_image_mesa;
   dri2_drv->base.API.ExportDRMImageMESA = dri2_export_drm_image_mesa;
   dri2_drv->base.API.ExportDMABUFImageQueryMESA = dri2_export_dma_buf_image_query_mesa;
   dri2_drv->base.API.ExportDMABUFImageMESA = dri2_export_dma_buf_image_mesa;
#endif
#ifdef HAVE_WAYLAND_PLATFORM
   dri2_drv->base.API.BindWaylandDisplayWL = dri2_bind_wayland_display_wl;
   dri2_drv->base.API.UnbindWaylandDisplayWL = dri2_unbind_wayland_display_wl;
   dri2_drv->base.API.QueryWaylandBufferWL = dri2_query_wayland_buffer_wl;
#endif
   dri2_drv->base.API.GetSyncValuesCHROMIUM = dri2_get_sync_values_chromium;
   dri2_drv->base.API.CreateSyncKHR = dri2_create_sync;
   dri2_drv->base.API.ClientWaitSyncKHR = dri2_client_wait_sync;
   dri2_drv->base.API.SignalSyncKHR = dri2_signal_sync;
   dri2_drv->base.API.WaitSyncKHR = dri2_server_wait_sync;
   dri2_drv->base.API.DestroySyncKHR = dri2_destroy_sync;
   dri2_drv->base.API.GLInteropQueryDeviceInfo = dri2_interop_query_device_info;
   dri2_drv->base.API.GLInteropExportObject = dri2_interop_export_object;

   dri2_drv->base.Name = "DRI2";
   dri2_drv->base.Unload = dri2_unload;

   return &dri2_drv->base;
}
