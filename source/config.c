/* config.c -- load/save the user config for the BTD5 Switch port.
 *
 * A tiny "name value" text file (config.h: CONFIG_NAME), created with defaults
 * on first run. Populates the global `config` and initialises the runtime
 * render size (`screen_width`/`screen_height`); the platform layer then tracks
 * dock/undock and re-resizes live.
 *
 * MIT license -- see LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

static void set_defaults(Config *c) {
  c->screen_width  = 0;      /* 0 = automatic (per dock state) */
  c->screen_height = 0;
  c->docked_width  = 1920;
  c->docked_height = 1080;
  strcpy(c->language, "auto");
}

/* Fixed render size: always 1080p, in every mode. The Switch compositor scales
 * the 1080p buffer down to the 720p panel in handheld, so there's no separate
 * handheld path and no dock-change resize (which is simpler and avoids resizing
 * the engine's live GL surface). */
static void apply_runtime_size(const Config *c) {
  (void)c;
  screen_width  = 1920;
  screen_height = 1080;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (!f) return -1;
  fprintf(f,
    "# Bloons TD 5 (Switch) config -- 'name value' per line, '#' comments.\n"
    "# 0 = automatic: 1920x1080 docked, 1280x720 handheld.\n"
    "# Set both to force a fixed resolution in every mode.\n"
    "screen_width %d\n"
    "screen_height %d\n"
    "# resolution used when docked while the above are automatic\n"
    "docked_width %d\n"
    "docked_height %d\n"
    "# 'auto' follows the Switch system language, or a 2-letter code\n"
    "# (en de fr es it pt ru ja ko zh nl sv da no fi ...)\n"
    "language %s\n",
    config.screen_width, config.screen_height,
    config.docked_width, config.docked_height, config.language);
  fclose(f);
  return 0;
}

int read_config(const char *file) {
  set_defaults(&config);

  FILE *f = fopen(file, "r");
  if (!f) {                         /* first run: write defaults, use them */
    write_config(file);
    apply_runtime_size(&config);
    return -1;
  }

  char line[128], key[64], val[64];
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
    if (sscanf(line, "%63s %63s", key, val) != 2) continue;
    if      (!strcmp(key, "screen_width"))  config.screen_width  = atoi(val);
    else if (!strcmp(key, "screen_height")) config.screen_height = atoi(val);
    else if (!strcmp(key, "docked_width"))  config.docked_width  = atoi(val);
    else if (!strcmp(key, "docked_height")) config.docked_height = atoi(val);
    else if (!strcmp(key, "language")) {
      strncpy(config.language, val, sizeof(config.language) - 1);
      config.language[sizeof(config.language) - 1] = '\0';
    }
  }
  fclose(f);

  apply_runtime_size(&config);
  return 0;
}
