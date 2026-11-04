/* Configuration is local to these units, never added to global CFLAGS. */
#ifndef ROBOT_VOICE_AFE_VENDOR_H
#define ROBOT_VOICE_AFE_VENDOR_H
#undef HAVE_CONFIG_H
#undef FIXED_POINT
#define FLOATING_POINT 1
#define USE_KISS_FFT 1
#define EXPORT
#include "vendor/namespace.h"
#endif
