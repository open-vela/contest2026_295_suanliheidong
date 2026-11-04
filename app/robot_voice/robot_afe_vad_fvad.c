/* Portable, application-local vendor unit; see vendor/README.md. */
#include "robot_voice_afe_vendor.h"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "vendor/libfvad/src/fvad.c"

/* Preserve the GMM's actual evidence (1); values >1 are hangover only.
 * Counting hangover as independent onset votes lets one impact open VAD. */
int robot_afe_vad_evidence(Fvad *inst, const int16_t *frame, size_t length)
{
  if (!inst || !frame || !valid_length(inst->rate_idx, length)) return -1;
  return process_funcs[inst->rate_idx](&inst->core, frame, length);
}
