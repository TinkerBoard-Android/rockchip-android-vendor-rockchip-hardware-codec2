/*
 * Copyright (C) 2020 Rockchip Electronics Co. LTD
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

#ifndef ANDROID_C2_RK_RGA_PROCESS_H__
#define ANDROID_C2_RK_RGA_PROCESS_H__

#include <log/log.h>
#include <string.h>
#include "C2RKMediaDefs.h"

int32_t rga_dev_open(void **rga_ctx);
int32_t rga_dev_close(void *rga_ctx);
void rga_rgb2nv12(RKVideoPlane *plane, RKMemLinear *vpumem,
            uint32_t Width, uint32_t Height, uint32_t dstWidth, uint32_t dstHeight, void *rga_ctx);
void rga_nv12_copy(RKVideoPlane *plane, RKMemLinear *vpumem,
            uint32_t Width, uint32_t Height, void* rga_ctx);
#endif  // ANDROID_C2_RK_RGA_PROCESS_H__

