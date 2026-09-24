/* nx_savetool.c -- see nx_savetool.h.
 *
 * WHAT IS VERIFIED, AND HOW
 * -------------------------
 * Format: Profile.save is DGDATA + checksum + shifted JSON (nk_save.h); every
 * stored checksum in real 4.7 saves matches, and re-encoding reproduces them
 * byte for byte. OldProfile.save is the engine's rolling backup of the same
 * profile (identical apart from its timestamps), so both are edited.
 *
 * Fields: each one below was seen changing in real saves between two sessions
 * (MonkeyMoney 100 -> 150, Rank 1 -> 2, RankXP 0 -> 50, DartMonkey XP 50, an
 * UnlockedTowers entry and a DartMonkey upgrade flag flipping to true).
 *
 * Rank: CPlayerProfileV1::InternalLoad (libnative.so 0x47d7ec) reads "Rank" and
 * trusts it -- it is NOT recomputed from RankXP -- then looks the XP-bar
 * thresholds up in the rank table (Assets/JSON/ProfileDefinitions/
 * RankGateway.json). A rank past the end of that table is handled by the
 * loader itself: the next threshold becomes ~1e37, i.e. no further rank-ups.
 *
 * Left out on purpose: the save's "UnlockAll" and "BypassRanks" flags. They
 * belong to the developers' debug menu (0x64af00, beside ForceCrash and
 * GiveAllSkins) and what they switch on is not visible -- possibly paid
 * content, possibly progress tracking -- so they are not offered.
 *
 * SAFETY
 * ------
 *  - Only the exact bytes of each named value change (nk_save.h), and only if
 *    the existing value has the expected type.
 *  - The edited document is re-validated as JSON before anything is written.
 *  - The new file is written beside the old one, read back and decoded to
 *    check it, and only then swapped in.
 *  - <name>.bak keeps the untouched original: written once, never overwritten.
 *
 * MIT license -- see LICENSE.
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "nk_save.h"
#include "nx_paths.h"
#include "nx_savetool.h"

extern int debugPrintf(char *text, ...);

#define SAVE_TXT   "save.txt"
#define MAX_SAVE   (1024 * 1024)            /* real profiles are ~7 KB */
#define MAX_JSON   (MAX_SAVE + 64 * 1024)   /* room for edits to grow the text */

static const char *PROFILES[] = { "Profile.save", "OldProfile.save" };

/* Tower types, as Levels.Towers lists them in 4.7 saves. */
static const char *TOWERS[] = {
  "DartMonkey", "TackTower", "SniperMonkey", "BoomerangThrower", "NinjaMonkey",
  "BombTower", "IceTower", "GlueGunner", "MonkeyBuccaneer", "MonkeyAce",
  "SuperMonkey", "MonkeyApprentice", "MonkeyVillage", "BananaFarm", "MortarTower",
  "DartlingGun", "SpikeFactory", "HeliPilot", "RoadSpikes", "ExplodingPineapple",
  "MonkeyEngineer", "Bloonchipper", "MonkeySub", "SuperMonkeyStorm", "BloonberryBush",
};
#define N_TOWERS ((int)(sizeof TOWERS / sizeof *TOWERS))

/* Google Play products, from the game's own catalogue (libnative.so 0xb06ca8:
 * product id, price, consumable and subscription flags) paired with its
 * internal names (0xb06ad8). CBloonsTD5Game::ProductPurchased records a
 * permanent purchase by adding the PRODUCT ID to the owned list -- for a skin,
 * the regular id even when bought at the sale price -- and that list is saved
 * as Items.PremiumUpgrades / Upgrades.PremiumUpgrades. The editor records them
 * exactly the same way. Consumables (Monkey Money and token packs, rank-ups)
 * and the two time-limited subscriptions are not listed: nothing to own. */
typedef struct { const char *name, *product; int skin; } Product;
static const Product PRODUCTS[] = {
  { "DoubleCash",               "btd5doublecashmode", 0 },
  { "HealthyBananas",           "btd5healthybananas", 0 },
  { "BiggerBeacons",            "btd5biggerbeacons", 0 },
  { "SuperComboPack",           "btd5supercombopack", 0 },
  { "MedievalDartMonkey",       "btd5_towerskin_dartmonkey_medieval", 1 },
  { "HalloweenDartMonkey",      "btd5_towerskin_dartmonkey_halloween", 1 },
  { "ClassicDartMonkey",        "btd5_towerskin_dartmonkey_classic", 1 },
  { "ClassicTackTower",         "btd5_towerskin_tacktower_classic", 1 },
  { "HunterSniperMonkey",       "btd5_towerskin_snipermonkey_hunter", 1 },
  { "TribalBoomerangThrower",   "btd5_towerskin_boomerangthrower_tribal", 1 },
  { "SamuraiNinjaMonkey",       "btd5_towerskin_ninjamonkey_samurai", 1 },
  { "MilitaryBombTower",        "btd5_towerskin_bombtower_military", 1 },
  { "ClassicBombTower",         "btd5_towerskin_bombtower_classic", 1 },
  { "ClassicIceTower",          "btd5_towerskin_icetower_classic", 1 },
  { "NavyMonkeyBuccaneer",      "btd5_towerskin_monkeybuccaneer_navy", 1 },
  { "TopgunMonkeyAce",          "btd5_towerskin_monkeyace_topgun", 1 },
  { "ClassicSuperMonkey",       "btd5_towerskin_supermonkey_classic", 1 },
  { "SorcererMonkeyApprentice", "btd5_towerskin_monkeyapprentice_sorcerer", 1 },
  { "CandyBananaFarm",          "btd5_towerskin_bananafarm_candy", 1 },
  { "FireworksMortarTower",     "btd5_towerskin_mortartower_fireworks", 1 },
  { "UFOHeliPilot",             "btd5_towerskin_helipilot_ufo", 1 },
  { "SteampunkMonkeySub",       "btd5_towerskin_monkeysub_steampunk", 1 },
};
#define N_PRODUCTS ((int)(sizeof PRODUCTS / sizeof *PRODUCTS))
static const char *OWNED_LISTS[] = { "Items.PremiumUpgrades", "Upgrades.PremiumUpgrades" };

/* -1 = not set: leave whatever the game has. */
typedef struct {
  long long monkey_money, tokens, rank, rank_xp, xp_all;
  long long xp[N_TOWERS];
  int unlock_towers, unlock_upgrades, unlock_tier4, sandbox, fast_track;
  int music, sfx, hints, auto_round;
  int own[N_PRODUCTS];                  /* 1 = add to the owned list */
  long long special_items;
  long long mastery_level;
  int mastery;
  int any;
} Settings;

/* ------------------------------------------------------------------ */
/* save.txt                                                            */
/* ------------------------------------------------------------------ */

static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { debugPrintf("savetool: cannot create %s\n", path); return; }
  fputs(
    "# Bloons TD 5 -- save editor\n"
    "#\n"
    "# Everything here is commented out, so by default this file does nothing.\n"
    "# Remove the '#' in front of a line and set a value, and it is written\n"
    "# into your profile the next time the game starts.\n"
    "#\n"
    "# The edit happens once per launch, before the game loads your profile.\n"
    "# After that the game owns the value again: spend Monkey Money and it goes\n"
    "# down as usual. A line left uncommented is applied again on EVERY launch,\n"
    "# so put the '#' back once you have what you want.\n"
    "#\n"
    "# Both Profile.save and OldProfile.save (the game's own backup) are edited.\n"
    "# Before the first edit each original is copied to <name>.bak, which is\n"
    "# never overwritten. To undo everything, quit the game, delete\n"
    "# Profile.save and OldProfile.save, and rename the two .bak files back.\n"
    "#\n"
    "# Format: 'name = value'. '#' starts a comment anywhere on a line.\n"
    "# The results of each launch are in btd5_nx.log (lines starting 'savetool:').\n"
    "\n"
    "# --- currency ----------------------------------------------------------\n"
    "#monkey_money = 10000\n"
    "#tokens = 100\n"
    "\n"
    "# --- rank --------------------------------------------------------------\n"
    "# The game trusts the stored rank. Ranks above the game's own maximum are\n"
    "# accepted; the XP bar simply stops there. rank_xp is your rank\n"
    "# experience; if it is below what your new rank needs, the bar starts\n"
    "# empty and fills from there.\n"
    "#rank = 30\n"
    "#rank_xp = 0\n"
    "\n"
    "# --- tower experience --------------------------------------------------\n"
    "# Each tower's XP, which unlocks its upgrades. tower_xp.all sets every\n"
    "# tower; a single tower's line overrides it. A tower only appears in your\n"
    "# save once the game has added it, so a tower you have never had cannot\n"
    "# be set yet (the log says so).\n"
    "#tower_xp.all = 100000\n", f);
  for (int i = 0; i < N_TOWERS; i++) fprintf(f, "#tower_xp.%s = 0\n", TOWERS[i]);
  fputs(
    "\n"
    "# --- unlocks -----------------------------------------------------------\n"
    "# 'on' unlocks. These cannot be switched back off from here -- use the\n"
    "# .bak files to go back.\n"
    "#unlock_towers = on          # every tower, regardless of rank\n"
    "#unlock_upgrades = on        # every tower's upgrade paths\n"
    "#unlock_tier4 = on           # every tower's tier-4 upgrades\n"
    "#sandbox = on                # sandbox mode\n"
    "#fast_track = on             # fast track\n"
    "\n"
    "# --- mastery mode ------------------------------------------------------\n"
    "# Mastery mode makes every match harder, in five levels. The game\n"
    "# normally unlocks each level as you earn medals across its tracks; 5 is\n"
    "# its maximum. mastery switches it on or off, like the in-game toggle, and\n"
    "# needs at least level 1 unlocked.\n"
    "#mastery_level = 5\n"
    "#mastery = on\n"
    "\n"
    "# --- Google Play purchases ---------------------------------------------\n"
    "# Restores content bought on Google Play, recorded exactly the way the\n"
    "# game records a purchase. The two group lines add everything in their\n"
    "# group; the lines below them add one item each. Specialty buildings and\n"
    "# Monkey Lab items are not Google Play purchases -- buy those in the game\n"
    "# with Monkey Money (set monkey_money above).\n"
    "#premium_upgrades = all      # Double Cash, Healthy Bananas, Bigger Beacons, Super Combo Pack\n"
    "#tower_skins = all           # all 18 tower skins\n", f);
  for (int i = 0; i < N_PRODUCTS; i++)
    fprintf(f, "#%s.%s = on\n", PRODUCTS[i].skin ? "skin" : "premium", PRODUCTS[i].name);
  fputs(
    "\n"
    "# --- special items (EXPERIMENTAL) --------------------------------------\n"
    "# A set of flags that event rewards switch on. This version of the game\n"
    "# only ever checks the first one (value 1), which enables something in\n"
    "# matches near its boss-event code -- exactly what is not known. 0 clears.\n"
    "#special_items = 1\n"
    "\n"
    "# --- options (the same switches as the in-game menu) --------------------\n"
    "#music = on\n"
    "#sfx = on\n"
    "#hints = on\n"
    "#auto_round = off\n", f);
  fclose(f);
  debugPrintf("savetool: wrote a commented template to %s\n", path);
}

static void trim(char *s) {
  char *p = s;
  while (*p && isspace((unsigned char)*p)) p++;
  if (p != s) memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

/* on/off -> 1/0, anything else -> -1 */
static int parse_bool(const char *v) {
  static const char *yes[] = { "on", "yes", "true", "1", "all" };
  static const char *no[]  = { "off", "no", "false", "0" };
  for (size_t i = 0; i < sizeof yes / sizeof *yes; i++) if (!strcasecmp(v, yes[i])) return 1;
  for (size_t i = 0; i < sizeof no / sizeof *no; i++)   if (!strcasecmp(v, no[i]))  return 0;
  return -1;
}

/* whole-string integer in [lo, hi]; -1 on error (all our values are >= 0) */
static long long parse_num(const char *key, const char *v, long long lo, long long hi) {
  char *end = NULL;
  errno = 0;
  const long long x = strtoll(v, &end, 10);
  if (errno || end == v || *end) { debugPrintf("savetool: %s = \"%s\" is not a number -- ignored\n", key, v); return -1; }
  if (x < lo) { debugPrintf("savetool: %s raised to %lld (minimum)\n", key, lo); return lo; }
  if (x > hi) { debugPrintf("savetool: %s lowered to %lld (maximum)\n", key, hi); return hi; }
  return x;
}

#define INT_MAXV 2147483647LL
#define MASTERY_MAX 5          /* CMasteryManager's own unlock loop stops at 5 (0x418420) */
#define XP_MAXV  2000000000LL

static int read_settings(Settings *s) {
  memset(s, 0, sizeof *s);
  s->monkey_money = s->tokens = s->rank = s->rank_xp = s->xp_all = -1;
  for (int i = 0; i < N_TOWERS; i++) s->xp[i] = -1;
  s->unlock_towers = s->unlock_upgrades = s->unlock_tier4 = s->sandbox = s->fast_track = -1;
  s->music = s->sfx = s->hints = s->auto_round = -1;
  s->special_items = -1;
  s->mastery_level = -1;
  s->mastery = -1;

  char path[600], line[512];
  if (!nx_data_file(SAVE_TXT, path, sizeof path)) return 0;
  FILE *f = fopen(path, "r");
  if (!f) { write_template(path); return 0; }

  int lineno = 0;
  while (fgets(line, sizeof line, f)) {
    lineno++;
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    char *eq = strchr(line, '=');
    if (!eq) { trim(line); if (*line) debugPrintf("savetool: line %d has no '=' -- ignored\n", lineno); continue; }
    *eq = 0;
    char *key = line, *val = eq + 1;
    trim(key); trim(val);
    if (!*key || !*val) continue;

    struct { const char *name; int *dst; } bools[] = {
      { "unlock_towers", &s->unlock_towers }, { "unlock_upgrades", &s->unlock_upgrades },
      { "unlock_tier4", &s->unlock_tier4 },   { "sandbox", &s->sandbox },
      { "fast_track", &s->fast_track },       { "music", &s->music },
      { "sfx", &s->sfx },                     { "hints", &s->hints },
      { "auto_round", &s->auto_round },       { "mastery", &s->mastery },
    };
    int done = 0;
    for (size_t i = 0; i < sizeof bools / sizeof *bools && !done; i++) {
      if (strcasecmp(key, bools[i].name)) continue;
      done = 1;
      const int b = parse_bool(val);
      if (b < 0) { debugPrintf("savetool: %s = \"%s\" -- expected on or off, ignored\n", key, val); break; }
      *bools[i].dst = b;
      s->any = 1;
    }
    if (done) continue;

    /* Google Play purchases: groups, or one item by the game's own name */
    {
      int group = -1, one = -1;
      if (!strcasecmp(key, "premium_upgrades")) group = 0;
      else if (!strcasecmp(key, "tower_skins")) group = 1;
      else if (!strncasecmp(key, "premium.", 8) || !strncasecmp(key, "skin.", 5)) {
        const int is_skin = !strncasecmp(key, "skin.", 5);
        const char *nm = key + (is_skin ? 5 : 8);
        for (int i = 0; i < N_PRODUCTS; i++)
          if (PRODUCTS[i].skin == is_skin && !strcasecmp(nm, PRODUCTS[i].name)) one = i;
        if (one < 0) { debugPrintf("savetool: unknown %s \"%s\" -- ignored\n", is_skin ? "skin" : "premium upgrade", nm); continue; }
      }
      if (group >= 0 || one >= 0) {
        const int b = parse_bool(val);
        if (b < 0) { debugPrintf("savetool: %s = \"%s\" -- expected on (or all), ignored\n", key, val); continue; }
        if (b == 0) { debugPrintf("savetool: %s = off: purchases cannot be removed from save.txt -- restore the .bak files instead\n", key); continue; }
        for (int i = 0; i < N_PRODUCTS; i++)
          if ((group >= 0 && PRODUCTS[i].skin == group) || i == one) s->own[i] = 1;
        s->any = 1;
        continue;
      }
    }

    long long *num = NULL, lo = 0, hi = INT_MAXV;
    if      (!strcasecmp(key, "monkey_money")) num = &s->monkey_money;
    else if (!strcasecmp(key, "tokens"))       num = &s->tokens;
    else if (!strcasecmp(key, "rank"))       { num = &s->rank; lo = 1; hi = 99999; }
    else if (!strcasecmp(key, "rank_xp"))    { num = &s->rank_xp; hi = XP_MAXV; }
    else if (!strcasecmp(key, "special_items")) { num = &s->special_items; hi = 0x7fffffffffffffffLL; }
    else if (!strcasecmp(key, "mastery_level")) { num = &s->mastery_level; hi = MASTERY_MAX; }
    else if (!strncasecmp(key, "tower_xp.", 9)) {
      const char *t = key + 9;
      hi = XP_MAXV;
      if (!strcasecmp(t, "all")) num = &s->xp_all;
      else for (int i = 0; i < N_TOWERS; i++) if (!strcasecmp(t, TOWERS[i])) { num = &s->xp[i]; break; }
      if (!num) { debugPrintf("savetool: unknown tower \"%s\" -- ignored\n", t); continue; }
    } else {
      debugPrintf("savetool: unknown setting \"%s\" (line %d) -- ignored\n", key, lineno);
      continue;
    }
    const long long x = parse_num(key, val, lo, hi);
    if (x >= 0) { *num = x; s->any = 1; }
  }
  fclose(f);
  if (s->unlock_towers == 0 || s->unlock_upgrades == 0 || s->unlock_tier4 == 0)
    debugPrintf("savetool: unlocks cannot be switched off from save.txt -- restore the .bak files instead\n");
  return s->any;
}

/* ------------------------------------------------------------------ */
/* applying to one profile                                             */
/* ------------------------------------------------------------------ */

static int apply_to_json(const Settings *s, char *j, size_t *n, size_t cap, const char *file) {
  int c = 0;
  if (s->monkey_money >= 0) c += nkj_set_int(j, n, cap, "Items.MonkeyMoney", s->monkey_money);
  if (s->tokens >= 0)       c += nkj_set_int(j, n, cap, "Items.Tokens", s->tokens);
  if (s->rank >= 0)         c += nkj_set_int(j, n, cap, "Levels.Rank", s->rank);
  if (s->rank_xp >= 0)      c += nkj_set_int(j, n, cap, "Levels.RankXP", s->rank_xp);

  char path[160];
  if (s->xp_all >= 0) {
    /* every listed tower -- except those with their own line, which follow
     * (setting them twice would rewrite the file on every launch for nothing) */
    const int k = nkj_count(j, *n, "Levels.Towers");
    for (int i = 0; i < k; i++) {
      size_t vs, ve;
      int own = 0;
      snprintf(path, sizeof path, "Levels.Towers[%d].Type", i);
      if (nkj_find(j, *n, path, &vs, &ve) && ve - vs >= 2)
        for (int t = 0; t < N_TOWERS && !own; t++)
          own = s->xp[t] >= 0 && strlen(TOWERS[t]) == ve - vs - 2 &&
                !memcmp(j + vs + 1, TOWERS[t], ve - vs - 2);
      if (own) continue;
      snprintf(path, sizeof path, "Levels.Towers[%d].XP", i);
      c += nkj_set_int(j, n, cap, path, s->xp_all);
    }
  }
  for (int i = 0; i < N_TOWERS; i++) {
    if (s->xp[i] < 0) continue;
    snprintf(path, sizeof path, "Levels.Towers[Type=%s].XP", TOWERS[i]);
    size_t vs, ve;
    if (!nkj_find(j, *n, path, &vs, &ve))
      debugPrintf("savetool: %s: %s is not in this save yet -- skipped\n", file, TOWERS[i]);
    else c += nkj_set_int(j, n, cap, path, s->xp[i]);
  }

  if (s->unlock_towers == 1) c += nkj_set_array_bools(j, n, cap, "UnlockedTowers", 1);
  if (s->unlock_tier4 == 1)  c += nkj_set_array_bools(j, n, cap, "UnlockedLevel4Upgrades", 1);
  if (s->unlock_upgrades == 1) {
    const int k = nkj_count(j, *n, "UnlockedTowerLevelUps");
    char name[96];
    for (int i = 0; i < k; i++) {
      if (!nkj_member_name(j, *n, "UnlockedTowerLevelUps", i, name, sizeof name)) continue;
      if (!strcmp(name, "TestTower")) continue;              /* the developers' own */
      snprintf(path, sizeof path, "UnlockedTowerLevelUps.%s.PathA", name);
      c += nkj_set_array_bools(j, n, cap, path, 1);
      snprintf(path, sizeof path, "UnlockedTowerLevelUps.%s.PathB", name);
      c += nkj_set_array_bools(j, n, cap, path, 1);
    }
  }
  if (s->sandbox == 1)    c += nkj_set_bool(j, n, cap, "SandboxUnlocked", 1);
  if (s->fast_track == 1) c += nkj_set_bool(j, n, cap, "FastTrackUnlocked", 1);

  for (int i = 0; i < N_PRODUCTS; i++) {
    if (!s->own[i]) continue;
    for (size_t l = 0; l < sizeof OWNED_LISTS / sizeof *OWNED_LISTS; l++)
      c += nkj_array_add_string(j, n, cap, OWNED_LISTS[l], PRODUCTS[i].product);
  }
  if (s->special_items >= 0) c += nkj_set_int(j, n, cap, "Items.SpecialItems", s->special_items);

  /* Mastery: the level first, so switching mastery on can check it. With no
   * level unlocked the game would look for a Mastery0.mode that does not
   * exist, so "on" needs a level of at least 1. */
  if (s->mastery_level >= 0) c += nkj_set_int(j, n, cap, "Mastery.UnlockedLevel", s->mastery_level);
  if (s->mastery == 1) {
    long long lvl = 0;
    if (!nkj_get_int(j, *n, "Mastery.UnlockedLevel", &lvl) || lvl < 1)
      debugPrintf("savetool: %s: mastery = on needs mastery_level 1 or more -- not switched on\n", file);
    else c += nkj_set_bool(j, n, cap, "Mastery.Enabled", 1);
  } else if (s->mastery == 0) {
    c += nkj_set_bool(j, n, cap, "Mastery.Enabled", 0);
  }

  if (s->music >= 0)      c += nkj_set_bool(j, n, cap, "Items.MusicOn", s->music);
  if (s->sfx >= 0)        c += nkj_set_bool(j, n, cap, "Items.SFXOn", s->sfx);
  if (s->hints >= 0)      c += nkj_set_bool(j, n, cap, "Items.ShowHints", s->hints);
  if (s->auto_round >= 0) c += nkj_set_bool(j, n, cap, "Items.AutoRoundOn", s->auto_round);
  return c;
}

static unsigned char *read_file(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  unsigned char *b = malloc(MAX_SAVE + 1);
  if (b) {
    *n = fread(b, 1, MAX_SAVE + 1, f);
    if (*n > MAX_SAVE) { free(b); b = NULL; }                 /* not a profile */
  }
  fclose(f);
  return b;
}

static int write_file(const char *path, const unsigned char *d, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  const int ok = fwrite(d, 1, n, f) == n;
  return (fclose(f) == 0) && ok;
}

static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static void apply_to_profile(const Settings *s, const char *name) {
  char path[600], bak[700], tmp[700];
  if (!nx_data_file(name, path, sizeof path)) return;
  size_t rn = 0;
  unsigned char *raw = read_file(path, &rn);
  if (!raw) { debugPrintf("savetool: %s not found -- nothing to edit yet\n", name); return; }

  char *json = malloc(MAX_JSON);
  unsigned char *out = malloc(MAX_JSON + NK_HDR);
  if (!json || !out) { free(raw); free(json); free(out); return; }

  int crc_ok = 0;
  const long jl = nk_decode(raw, rn, json, MAX_JSON, &crc_ok);
  if (jl < 0 || !nkj_valid(json, (size_t)jl)) {
    debugPrintf("savetool: %s is not a readable profile -- left untouched\n", name);
    goto done;
  }
  if (!crc_ok) debugPrintf("savetool: %s: its checksum was already wrong before editing\n", name);

  size_t n = (size_t)jl;
  char *orig = malloc((size_t)jl + 1);
  if (!orig) goto done;
  memcpy(orig, json, (size_t)jl + 1);
  const int changed = apply_to_json(s, json, &n, MAX_JSON, name);
  const int same = n == (size_t)jl && !memcmp(json, orig, n);   /* the final text decides */
  free(orig);
  if (changed == 0 || same) { debugPrintf("savetool: %s already has these values -- not rewritten\n", name); goto done; }
  if (!nkj_valid(json, n)) {                        /* cannot happen; never write if it does */
    debugPrintf("savetool: %s: edit produced invalid data -- NOT written\n", name);
    goto done;
  }

  const long on = nk_encode(json, n, out, MAX_JSON + NK_HDR);
  snprintf(bak, sizeof bak, "%s.bak", path);
  snprintf(tmp, sizeof tmp, "%s.new", path);
  if (on < 0) goto done;

  if (!file_exists(bak) && !write_file(bak, raw, rn)) {
    debugPrintf("savetool: cannot back up %s -- NOT edited\n", name);
    goto done;
  }
  /* Write beside the original, read it back and decode it, then swap it in.
   * (FAT's rename will not replace an existing file, hence remove first.) */
  size_t vn = 0;
  unsigned char *verify = NULL;
  int good = write_file(tmp, out, (size_t)on);
  if (good) {
    verify = read_file(tmp, &vn);
    int vcrc = 0;
    good = verify && vn == (size_t)on && !memcmp(verify, out, vn) &&
           nk_decode(verify, vn, json, MAX_JSON, &vcrc) == (long)n && vcrc;
  }
  free(verify);
  if (!good) {
    remove(tmp);
    debugPrintf("savetool: writing %s failed -- the original is untouched\n", name);
    goto done;
  }
  remove(path);
  if (rename(tmp, path) != 0) {
    debugPrintf("savetool: could not move %s.new into place -- your edited profile is in %s.new "
                "and the original in %s.bak\n", name, name, name);
    goto done;
  }
  debugPrintf("savetool: %s: %d value(s) changed\n", name, changed);

done:
  free(raw); free(json); free(out);
}

void nx_savetool_apply(void) {
  Settings s;
  if (!read_settings(&s)) return;              /* no save.txt yet, or nothing uncommented */
  for (size_t i = 0; i < sizeof PROFILES / sizeof *PROFILES; i++)
    apply_to_profile(&s, PROFILES[i]);
}
