/* nx_config.h -- config.txt in the game folder: one setting, online = on|off.
 *
 * Plain C (tools/config_test builds it on a PC). Writes the file on first
 * launch, and rewrites anything that is not exactly the current one-line form
 * (an older build's extra keys, online=1/0, stray text), keeping the online
 * choice it found.
 *
 * MIT license -- see LICENSE.
 */
#ifndef NX_CONFIG_H
#define NX_CONFIG_H

/* 1 = online (the default), 0 = off. Main thread, before engine threads. */
int nx_config_online(void);

#endif
