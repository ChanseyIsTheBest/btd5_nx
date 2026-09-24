/* nx_keyboard.h -- the Switch software keyboard behind the engine's text entry.
 *
 * Bloons TD 5's text fields (the co-op join-code screen, rename dialogs) use
 * CDroidKeyboard (Platform/Droid/DroidKeyboard.cpp), which on Android talks to
 * MainActivity over JNI. The contract, read from libnative.so:
 *
 *   engine -> Java   SetKeyboardInputType(I)V      before every show
 *                    SetKeyboardMaxCharacters(I)V  before every show
 *                    ShowKeyboard(Z)V              true = show, false = hide
 *   Java -> engine   nativeInputTextChanged(String)  the field's WHOLE text:
 *                    CCoopEnterCodeScreen assigns it (code = text), it does
 *                    not append
 *                    nativeKeyboardHidden()          keyboard dismissed
 *                    nativeBackSpace()               (unused: we send whole text)
 *
 * The Switch keyboard is a blocking system applet, so it cannot run inside the
 * engine's ShowKeyboard call: CDroidKeyboard::Show sets its "visible" flag
 * AFTER that call returns, so reporting "hidden" from inside it would leave the
 * engine believing the keyboard is still open. Requests are queued here and
 * the main loop runs the applet between frames (main.c pump_keyboard()), the
 * way Android's UI thread answers asynchronously.
 *
 * MIT license -- see LICENSE.
 */
#ifndef NX_KEYBOARD_H
#define NX_KEYBOARD_H

#include <stddef.h>

/* JNI side (ninjakiwi.c) */
void nxk_set_input_type(int type);
void nxk_set_max_chars(int max);
void nxk_request(int show);

/* main loop side (main.c) */
void nxk_note_tap(void);         /* a touch/click reached the game             */
int  nxk_pending(void);          /* a show request is waiting                  */
/* Runs the Switch keyboard (blocks until closed). Returns 1 when the player
 * confirmed (text in out), 0 when cancelled or it could not be shown. */
int  nxk_run(char *out, size_t cap);

#endif
