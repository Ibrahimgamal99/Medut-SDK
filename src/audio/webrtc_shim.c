/* Lazy-loads libwebrtc-audio-processing-1 via dlopen so medut.so has no
 * hard DT_NEEDED entry for it.  If the library is absent at runtime,
 * AudioProcessingBuilder::Create returns NULL and bsdk_webrtc_lib_available()
 * says so at init, so the SDK falls back to its own suppressor instead.
 * Only built with MEDUT_WITH_WEBRTC_AEC (webrtc_apm.cpp is the one caller).
 *
 * Only the SONAME found at build time is loaded.  webrtc_apm.cpp is compiled
 * against that version's headers, and an older .so has a different vtable
 * order and Config layout — loading it would not fail, it would misbehave.
 *
 * The shims are hidden and the library is opened RTLD_LOCAL: they exist for
 * webrtc_apm.o alone, and exported they would also intercept the library's
 * own internal calls to the same symbols.
 */
#ifdef MEDUT_HAS_WEBRTC_AEC

#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include "webrtc_apm.h"

#ifndef MEDUT_WEBRTC_SONAME
#error "MEDUT_WEBRTC_SONAME must name the library the headers came from"
#endif

#define SHIM __attribute__((visibility("hidden")))

#define SYM_CTOR   "_ZN6webrtc22AudioProcessingBuilderC1Ev"
#define SYM_DTOR   "_ZN6webrtc22AudioProcessingBuilderD1Ev"
#define SYM_CREATE "_ZN6webrtc22AudioProcessingBuilder6CreateEv"

static void          *s_lib;
static pthread_once_t s_once = PTHREAD_ONCE_INIT;

static void load_lib(void)
{
	s_lib = dlopen(MEDUT_WEBRTC_SONAME, RTLD_NOW | RTLD_LOCAL);
}

static void *get_sym(const char *sym)
{
	pthread_once(&s_once, load_lib);
	return s_lib ? dlsym(s_lib, sym) : NULL;
}

bool bsdk_webrtc_lib_available(void)
{
	return get_sym(SYM_CTOR) && get_sym(SYM_DTOR) && get_sym(SYM_CREATE);
}

/* ---- webrtc::AudioProcessingBuilder --------------------------------------- */

SHIM void webrtc_apb_ctor(void *self) __asm__(SYM_CTOR);
SHIM void webrtc_apb_ctor(void *self)
{
	typedef void (*fn_t)(void *);
	fn_t f = (fn_t)get_sym(SYM_CTOR);
	if (f) f(self);
}

SHIM void webrtc_apb_dtor(void *self) __asm__(SYM_DTOR);
SHIM void webrtc_apb_dtor(void *self)
{
	typedef void (*fn_t)(void *);
	fn_t f = (fn_t)get_sym(SYM_DTOR);
	if (f) f(self);
}

SHIM void *webrtc_apb_create(void *self) __asm__(SYM_CREATE);
SHIM void *webrtc_apb_create(void *self)
{
	typedef void *(*fn_t)(void *);
	fn_t f = (fn_t)get_sym(SYM_CREATE);
	return f ? f(self) : NULL;
}

#endif /* MEDUT_HAS_WEBRTC_AEC */
