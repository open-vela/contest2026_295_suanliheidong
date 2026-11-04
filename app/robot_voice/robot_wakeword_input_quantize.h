#ifndef ROBOT_WAKEWORD_INPUT_QUANTIZE_H
#define ROBOT_WAKEWORD_INPUT_QUANTIZE_H
#include <stdint.h>
#include <stddef.h>
#include <math.h>
/* Independent of ambient rounding mode. Float32 divide then add, nearest even. */
static inline int RobotWakewordQuantizeInput(const float *x, size_t n,
                                             float scale, int zero, int8_t *out) {
  if (!x || !out || !(scale>0) || !isfinite(scale) || zero < -128 || zero > 127) return -1;
  for (size_t i=0;i<n;i++) {
    if (!isfinite(x[i])) return -1;
    volatile float divided=x[i]/scale;
    float value=divided+(float)zero;
    if (value<=-128) out[i]=-128;
    else if (value>=127) out[i]=127;
    else {
      float base=floorf(value), delta=value-base;
      int rounded=(int)base;
      if (delta>.5f || (delta==.5f && (rounded%2)!=0)) rounded++;
      out[i]=(int8_t)rounded;
    }
  }
  return 0;
}
static inline int robot_wakeword_quantize_input(const float *x, size_t n,
                                                float scale, int zero, int8_t *out) {
  return RobotWakewordQuantizeInput(x, n, scale, zero, out);
}
#endif
