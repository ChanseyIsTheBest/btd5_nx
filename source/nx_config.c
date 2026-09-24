/* nx_config.c -- see nx_config.h.
 *
 * MIT license -- see LICENSE.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "nx_config.h"
#include "nx_paths.h"

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

static void write_config(const char *path, int online) {
  FILE *f = fopen(path, "w");
  if (!f) return;
  fputs("# Bloons TD 5 (Switch) -- settings\n"
        "#\n"
        "# online = on    co-op, events and news through Ninja Kiwi's servers\n"
        "# online = off   keep the game off the network entirely\n", f);
  fprintf(f, "online = %s\n", online ? "on" : "off");
  fclose(f);
}

int nx_config_online(void) {
  char path[600];
  int online = 1;
  if (!nx_data_file(CONFIG_FILE, path, sizeof path)) return online;
  FILE *f = fopen(path, "r");
  if (!f) { write_config(path, online); return online; }

  int tidy = 1, seen = 0;
  char line[256];
  while (fgets(line, sizeof line, f)) {
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *eq = strchr(line, '=');
    if (!eq) { if (*trim(line)) tidy = 0; continue; }
    *eq = 0;
    char *k = trim(line), *v = trim(eq + 1);
    if (strcasecmp(k, "online") || seen) { tidy = 0; if (strcasecmp(k, "online")) continue; }
    seen = 1;
    if (!strcasecmp(v, "on"))       online = 1;
    else if (!strcasecmp(v, "off")) online = 0;
    else {
      tidy = 0;                                     /* older spellings, still honoured */
      if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1")) online = 1;
      else if (!strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcmp(v, "0")) online = 0;
    }
  }
  fclose(f);
  if (!tidy || !seen) write_config(path, online);
  return online;
}
