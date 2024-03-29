/**************************************************************************
 *
 * Copyright (C) 2016 Steven Toth <stoth@kernellabs.com>
 * Copyright (C) 2016 Zodiac Inflight Innovations
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

#if HAVE_LIBSENSORS
/* Purpose: Extract lm-sensors data, expose temperature, power, voltage. */

#include "hud/hud_private.h"
#include "util/list.h"
#include "os/os_time.h"
#include "util/u_memory.h"
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sensors/sensors.h>

#define LOCAL_DEBUG 0

/* TODO: We don't handle dynamic sensor discovery / arrival or removal.
 * Static globals specific to this HUD category.
 */
static int gsensors_temp_count = 0;
static struct list_head gsensors_temp_list;

struct sensors_temp_info
{
   struct list_head list;

   /* Combined chip and feature name, human readable. */
   char name[64];

   /* The type of measurement, critical or current. */
   unsigned int mode;

   uint64_t last_time;

   char chipname[64];
   char featurename[128];

   sensors_chip_name *chip;
   const sensors_feature *feature;
   double current, min, max, critical;
};

static double
get_value(const sensors_chip_name * name, const sensors_subfeature * sub)
{
   double val;
   int err;

   err = sensors_get_value(name, sub->number, &val);
   if (err) {
      fprintf(stderr, "ERROR: Can't get value of subfeature %s\n", sub->name);
      val = 0;
   }
   return val;
}

static void
get_sensor_values(struct sensors_temp_info *sti)
{
   const sensors_subfeature *sf;

   if (sti->mode == SENSORS_VOLTAGE_CURRENT) {
      sf = sensors_get_subfeature(sti->chip, sti->feature,
                                  SENSORS_SUBFEATURE_IN_INPUT);
      if (sf)
         sti->current = get_value(sti->chip, sf);
   }

   if (sti->mode == SENSORS_CURRENT_CURRENT) {
      sf = sensors_get_subfeature(sti->chip, sti->feature,
                                  SENSORS_SUBFEATURE_CURR_INPUT);
      if (sf) {
         /* Sensors API returns in AMPs, even though driver is reporting mA,
          * convert back to mA */
         sti->current = get_value(sti->chip, sf) * 1000;
      }
   }

   if (sti->mode == SENSORS_TEMP_CURRENT) {
      sf = sensors_get_subfeature(sti->chip, sti->feature,
                                  SENSORS_SUBFEATURE_TEMP_INPUT);
      if (sf)
         sti->current = get_value(sti->chip, sf);
   }

   if (sti->mode == SENSORS_TEMP_CRITICAL) {
      sf = sensors_get_subfeature(sti->chip, sti->feature,
                                  SENSORS_SUBFEATURE_TEMP_CRIT);
      if (sf)
         sti->critical = get_value(sti->chip, sf);
   }

   sf = sensors_get_subfeature(sti->chip, sti->feature,
                               SENSORS_SUBFEATURE_TEMP_MIN);
   if (sf)
      sti->min = get_value(sti->chip, sf);

   sf = sensors_get_subfeature(sti->chip, sti->feature,
                               SENSORS_SUBFEATURE_TEMP_MAX);
   if (sf)
      sti->max = get_value(sti->chip, sf);
#if LOCAL_DEBUG
   printf("%s.%s.current = %.1f\n", sti->chipname, sti->featurename,
          sti->current);
   printf("%s.%s.critical = %.1f\n", sti->chipname, sti->featurename,
          sti->critical);
#endif
}

static struct sensors_temp_info *
find_sti_by_name(char *n, unsigned int mode)
{
   list_for_each_entry(struct sensors_temp_info, sti, &gsensors_temp_list, list) {
      if (sti->mode != mode)
         continue;
      if (strcasecmp(sti->name, n) == 0)
         return sti;
   }
   return 0;
}

static void
query_sti_load(struct hud_graph *gr)
{
   struct sensors_temp_info *sti = gr->query_data;
   uint64_t now = os_time_get();

   if (sti->last_time) {
      if (sti->last_time + gr->pane->period <= now) {
         get_sensor_values(sti);

         switch (sti->mode) {
         case SENSORS_TEMP_CURRENT:
            hud_graph_add_value(gr, (uint64_t) sti->current);
            break;
         case SENSORS_TEMP_CRITICAL:
            hud_graph_add_value(gr, (uint64_t) sti->critical);
            break;
         case SENSORS_VOLTAGE_CURRENT:
            hud_graph_add_value(gr, (uint64_t) sti->current);
            break;
         case SENSORS_CURRENT_CURRENT:
            hud_graph_add_value(gr, (uint64_t) sti->current);
            break;
         }

         sti->last_time = now;
      }
   }
   else {
      /* initialize */
      get_sensor_values(sti);
      sti->last_time = now;
   }
}

static void
free_query_data(void *p)
{
   struct sensors_temp_info *sti = (struct sensors_temp_info *) p;
   list_del(&sti->list);
   if (sti->chip)
      sensors_free_chip_name(sti->chip);
   FREE(sti);
   sensors_cleanup();
}

/**
  * Create and initialize a new object for a specific sensor interface dev.
  * \param  pane  parent context.
  * \param  dev_name  device name, EG. 'coretemp-isa-0000.Core 1'
  * \param  mode  query type (NIC_DIRECTION_RX/WR/RSSI) statistics.
  */
void
hud_sensors_temp_graph_install(struct hud_pane *pane, char *dev_name,
                               unsigned int mode)
{
   struct hud_graph *gr;
   struct sensors_temp_info *sti;

   int num_devs = hud_get_num_sensors(0);
   if (num_devs <= 0)
      return;
#if LOCAL_DEBUG
   printf("%s(%s, %s) - Creating HUD object\n", __func__, dev_name,
          mode == SENSORS_VOLTAGE_CURRENT ? "VOLTS" :
          mode == SENSORS_CURRENT_CURRENT ? "AMPS" :
          mode == SENSORS_TEMP_CURRENT ? "CU" :
          mode == SENSORS_TEMP_CRITICAL ? "CR" : "UNDEFINED");
#endif

   sti = find_sti_by_name(dev_name, mode);
   if (!sti)
      return;

   gr = CALLOC_STRUCT(hud_graph);
   if (!gr)
      return;

   sprintf(gr->name, "%.6s..%s (%s)",
           sti->chipname,
           sti->featurename,
           sti->mode == SENSORS_VOLTAGE_CURRENT ? "Volts" :
           sti->mode == SENSORS_CURRENT_CURRENT ? "Amps" :
           sti->mode == SENSORS_TEMP_CURRENT ? "Curr" :
           sti->mode == SENSORS_TEMP_CRITICAL ? "Crit" : "Unkn");

   gr->query_data = sti;
   gr->query_new_value = query_sti_load;

   /* Don't use free() as our callback as that messes up Gallium's
    * memory debugger.  Use simple free_query_data() wrapper.
    */
   gr->free_query_data = free_query_data;

   hud_pane_add_graph(pane, gr);
   switch (sti->mode) {
   case SENSORS_TEMP_CURRENT:
   case SENSORS_TEMP_CRITICAL:
      hud_pane_set_max_value(pane, 120);
      break;
   case SENSORS_VOLTAGE_CURRENT:
      hud_pane_set_max_value(pane, 12);
      break;
   case SENSORS_CURRENT_CURRENT:
      hud_pane_set_max_value(pane, 5000);
      break;
   }
}

static void
create_object(char *chipname, char *featurename,
             const sensors_chip_name * chip, const sensors_feature * feature,
             int mode)
{
#if LOCAL_DEBUG
   printf("%03d: %s.%s\n", gsensors_temp_count, chipname, featurename);
#endif
   struct sensors_temp_info *sti = CALLOC_STRUCT(sensors_temp_info);

   sti->mode = mode;
   sti->chip = (sensors_chip_name *) chip;
   sti->feature = feature;
   strcpy(sti->chipname, chipname);
   strcpy(sti->featurename, featurename);
   sprintf(sti->name, "%s.%s", sti->chipname, sti->featurename);

   list_addtail(&sti->list, &gsensors_temp_list);
   gsensors_temp_count++;
}

static void
build_sensor_list(void)
{
   const sensors_chip_name *chip;
   const sensors_chip_name *match = 0;
   const sensors_feature *feature;
   int chip_nr = 0;

   char name[256];
   while ((chip = sensors_get_detected_chips(match, &chip_nr))) {
      sensors_snprintf_chip_name(name, sizeof(name), chip);

      /* Get all features and filter accordingly. */
      int fnr = 0;
      while ((feature = sensors_get_features(chip, &fnr))) {
         char *featurename = sensors_get_label(chip, feature);
         if (!featurename)
            continue;

         /* Create a 'current' and 'critical' object pair.
          * Ignore sensor if its not temperature based.
          */
         if (feature->type == SENSORS_FEATURE_TEMP) {
            create_object(name, featurename, chip, feature,
                          SENSORS_TEMP_CURRENT);
            create_object(name, featurename, chip, feature,
                          SENSORS_TEMP_CRITICAL);
         }
         if (feature->type == SENSORS_FEATURE_IN) {
            create_object(name, featurename, chip, feature,
                          SENSORS_VOLTAGE_CURRENT);
         }
         if (feature->type == SENSORS_FEATURE_CURR) {
            create_object(name, featurename, chip, feature,
                          SENSORS_CURRENT_CURRENT);
         }
         free(featurename);
      }
   }
}

/**
  * Initialize internal object arrays and display lmsensors HUD help.
  * \param  displayhelp  true if the list of detected devices should be
                         displayed on the console.
  * \return  number of detected lmsensor devices.
  */
int
hud_get_num_sensors(int displayhelp)
{
   /* Return the number of sensors detected. */
   if (gsensors_temp_count)
      return gsensors_temp_count;

   int ret = sensors_init(NULL);
   if (ret)
      return 0;

   list_inithead(&gsensors_temp_list);

   /* Scan /sys/block, for every object type we support, create and
    * persist an object to represent its different statistics.
    */
   build_sensor_list();

   if (displayhelp) {
      list_for_each_entry(struct sensors_temp_info, sti, &gsensors_temp_list, list) {
         char line[64];
         switch (sti->mode) {
         case SENSORS_TEMP_CURRENT:
            sprintf(line, "    sensors_temp_cu-%s", sti->name);
            break;
         case SENSORS_TEMP_CRITICAL:
            sprintf(line, "    sensors_temp_cr-%s", sti->name);
            break;
         case SENSORS_VOLTAGE_CURRENT:
            sprintf(line, "    sensors_volt_cu-%s", sti->name);
            break;
         case SENSORS_CURRENT_CURRENT:
            sprintf(line, "    sensors_curr_cu-%s", sti->name);
            break;
         }

         puts(line);
      }
   }

   return gsensors_temp_count;
}

#endif /* HAVE_LIBSENSORS */
