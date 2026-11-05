/**
 * @file mdt_stack.h  Private header of the Medut stack's core
 *
 * Implements core/mdt.h for the SDK above and core/mdt_mod.h for the
 * plug-ins beside it.  Everything here runs on re_main unless a comment says
 * otherwise.
 */

#ifndef MDT_STACK_H
#define MDT_STACK_H

#include <re.h>
#include "../api/bsdk.h"
#include "mdt_mod.h"

/* stack.c: the stack's state between mdt_stack_init() and mdt_stack_close() */
struct mdt_stack {
	char         conf_dir[512]; /* writable state directory, may be ""   */
	struct dnsc *dnsc;
	struct list  laddrs;      /* struct mdt_laddr: usable local addresses */
	bool         up;
};

extern struct mdt_stack g_mdt;

/* One local address the engine may bind to. */
struct mdt_laddr {
	struct le le;
	char      ifname[64];
	struct sa sa;
};

/* stack.c: (re)read the box's usable addresses into g_mdt.laddrs */
void mdt_net_scan(void);
/* stack.c: may media use address family af? */
bool mdt_net_af_enabled(int af);
/* stack.c: our address the kernel routes dst out of, NULL if none */
const struct sa *mdt_net_laddr_for(const struct sa *dst);

/* audio/device.c: the driver every stream opens on, and the device names
 * it asks that driver for ("" = the driver's default). */
const char *mdt_audio_driver(void);
const char *mdt_audio_dev(bool input);
/* platform/<os>/: the platform's own audio driver, registered under its
 * name by device.c on the build that has it. */
int  mdt_pulse_init(void);        /* Linux: "pulse"     */
void mdt_pulse_close(void);
int  mdt_coreaudio_init(void);    /* macOS: "coreaudio" */
void mdt_coreaudio_close(void);
int  mdt_wasapi_init(void);       /* Windows: "wasapi"  */
void mdt_wasapi_close(void);
int  mdt_audiounit_init(void);    /* iOS: "audiounit"   */
void mdt_audiounit_close(void);

/* audio/device.c: register the build's drivers, select the platform's */
int mdt_audio_drivers_init(void);
void mdt_audio_drivers_close(void);

/* audio/null.c: the silent device (mdt_ausrc + mdt_auplay "null") */
int  mdt_null_init(void);
void mdt_null_close(void);

#endif /* MDT_STACK_H */
