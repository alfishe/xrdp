/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) 2026 alfishe
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * H.264 encoder for the EGFX pipeline based on NVIDIA NVENC
 */

#ifndef _XRDP_ENCODER_NVENC_H
#define _XRDP_ENCODER_NVENC_H

#include "arch.h"

/**
 * Checks that the CUDA driver and the NVENC runtime library can be loaded
 * and that an encoder session can be opened on this machine.
 * @return non-zero if NVENC can be used
 */
int
xrdp_encoder_nvenc_install_ok(void);

void *
xrdp_encoder_nvenc_create(void);
int
xrdp_encoder_nvenc_delete(void *handle);
int
xrdp_encoder_nvenc_encode(void *handle, int session, int left, int top,
                          int width, int height, int twidth, int theight,
                          int format, const char *data,
                          short *crects, int num_crects,
                          char *cdata, int *cdata_bytes, int connection_type,
                          int *flags_ptr);

#endif
