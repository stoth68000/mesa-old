/**************************************************************************
 *
 * Copyright 2013 Marek Olšák <maraeo@gmail.com>
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef HUD_PRIVATE_H
#define HUD_PRIVATE_H

#include "pipe/p_context.h"
#include "util/list.h"

struct hud_graph {
   /* initialized by common code */
   struct list_head head;
   struct hud_pane *pane;
   float color[3];
   float *vertices; /* ring buffer of vertices */

   /* name and query */
   char name[128];
   void *query_data;
   void (*query_new_value)(struct hud_graph *gr);
   void (*free_query_data)(void *ptr); /**< do not use ordinary free() */

   /* mutable variables */
   unsigned num_vertices;
   unsigned index; /* vertex index being updated */
   uint64_t current_value;
};

struct hud_pane {
   struct list_head head;
   unsigned x1, y1, x2, y2;
   unsigned inner_x1;
   unsigned inner_y1;
   unsigned inner_x2;
   unsigned inner_y2;
   unsigned inner_width;
   unsigned inner_height;
   float yscale;
   unsigned max_num_vertices;
   unsigned last_line; /* index of the last describing line in the graph */
   uint64_t max_value;
   uint64_t initial_max_value;
   uint64_t ceiling;
   unsigned dyn_ceil_last_ran;
   boolean dyn_ceiling;
   enum pipe_driver_query_type type;
   uint64_t period; /* in microseconds */

   struct list_head graph_list;
   unsigned num_graphs;
};


/* core */
void hud_pane_add_graph(struct hud_pane *pane, struct hud_graph *gr);
void hud_pane_set_max_value(struct hud_pane *pane, uint64_t value);
void hud_graph_add_value(struct hud_graph *gr, uint64_t value);

/* graphs/queries */
struct hud_batch_query_context;

#define ALL_CPUS ~0 /* optionally set as cpu_index */

int hud_get_num_cpus(void);

void hud_fps_graph_install(struct hud_pane *pane);
void hud_cpu_graph_install(struct hud_pane *pane, unsigned cpu_index);
void hud_pipe_query_install(struct hud_batch_query_context **pbq,
                            struct hud_pane *pane, struct pipe_context *pipe,
                            const char *name, unsigned query_type,
                            unsigned result_index,
                            uint64_t max_value,
                            enum pipe_driver_query_type type,
                            enum pipe_driver_query_result_type result_type,
                            unsigned flags);
boolean hud_driver_query_install(struct hud_batch_query_context **pbq,
                                 struct hud_pane *pane,
                                 struct pipe_context *pipe, const char *name);
void hud_batch_query_update(struct hud_batch_query_context *bq);
void hud_batch_query_cleanup(struct hud_batch_query_context **pbq);

#if HAVE_GALLIUM_EXTRA_HUD
int hud_get_num_nics(int displayhelp);
#define NIC_DIRECTION_RX 1
#define NIC_DIRECTION_TX 2
#define NIC_RSSI_DBM     3
void hud_nic_graph_install(struct hud_pane *pane, char *nic_index,
                           unsigned int mode);

int hud_get_num_disks(int displayhelp);
#define DISKSTAT_RD 1
#define DISKSTAT_WR 2
void hud_diskstat_graph_install(struct hud_pane *pane, char *dev_name,
                                unsigned int mode);
#endif

#if HAVE_LIBSENSORS
int hud_get_num_sensors(int displayhelp);
#define SENSORS_TEMP_CURRENT     1
#define SENSORS_TEMP_CRITICAL    2
#define SENSORS_VOLTAGE_CURRENT  3
#define SENSORS_CURRENT_CURRENT  4
void hud_sensors_temp_graph_install(struct hud_pane *pane, char *dev_name,
                                    unsigned int mode);
#endif

#endif
