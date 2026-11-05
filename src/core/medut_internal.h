/**
 * @file medut_internal.h  Plug-in header: SDK internals + the plug-in
 *                           interfaces of the Medut stack.
 *
 * Included by the code that plugs *into* the stack rather than sitting above
 * it: the platform audio drivers (platform/android/sles_vc.c, ...), the
 * app-owned and null devices and the filters (src/audio/), and the ICE
 * media-NAT (src/ice/ice_mod.c).  They register through the module
 * interfaces of core/mdt_mod.h — mdt_ausrc / mdt_auplay / mdt_aufilt /
 * mdt_mnat — whose contract test/compat_contract_test.c pins.
 *
 * Everything else includes bsdk.h.
 */

#ifndef MEDUT_INTERNAL_H
#define MEDUT_INTERNAL_H

#include <re.h>
#include "../api/bsdk.h"
#include "mdt_mod.h"

#endif /* MEDUT_INTERNAL_H */
