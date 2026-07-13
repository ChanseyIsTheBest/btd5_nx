/* ninjakiwi.h -- interface to the emulated com.ninjakiwi.* shell.
 * MIT license -- see LICENSE. */
#ifndef __NINJAKIWI_H__
#define __NINJAKIWI_H__

#include <stdarg.h>
#include "jni.h"

/* The single native->Java up-call entry point. jni_fake.c's Call*Method[V]
 * forms funnel here with the resolved (class,name,sig), the receiver object,
 * and a va_list positioned at the first Java argument. We parse the args per
 * `sig`, answer offline-safe, and return the result in the matching jvalue
 * field (jni_fake picks the field for the Call* form the engine used).
 * Unhandled calls are logged as "JNI call unhandled: <cls>.<name> <sig>". */
jvalue nk_upcall(const char *cls, const char *name, const char *sig,
                 jobject self, va_list ap);

/* Ask the Switch software keyboard to open (backs MainActivity.showKeyboard).
 * The typed string is later delivered through nativeInputTextChanged. */
void nk_request_keyboard(jobject prompt);

#endif
