/* nx_savetool.h -- edit Bloons TD 5's profile from save.txt at boot.
 *
 * Reads save.txt in the game folder and writes the values it names into
 * Profile.save and OldProfile.save, in the game's own obfuscated and
 * checksummed format, before the engine loads them. On first run it writes a
 * fully commented save.txt with everything switched off, so the file documents
 * itself. Modelled on smk2_nx's savetool (same engine and save format).
 *
 * Call once from main(), after the game folder is known and before the engine
 * is loaded (no engine threads may be running).
 *
 * MIT license -- see LICENSE.
 */
#ifndef NX_SAVETOOL_H
#define NX_SAVETOOL_H

void nx_savetool_apply(void);

#endif
