/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Register the MiMo ASR/TTS backends with the generic voice framework. */
int mimo_asr_register(void);
int mimo_tts_register(void);

#ifdef __cplusplus
}
#endif
