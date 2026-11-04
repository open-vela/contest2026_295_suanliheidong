/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Run the contest boot sequence.  Each step retries until it succeeds, so
 * later services are never started with a partially configured runtime.
 */
int agent_autostartup_run(void);

/* Network services wait for the credentials/configuration transaction. */
void agent_autostartup_release_network(void);
void agent_autostartup_wait_network_release(void);
void agent_autostartup_mark_network_ready(void);
void agent_autostartup_wait_network_ready(void);

#ifdef __cplusplus
}
#endif
