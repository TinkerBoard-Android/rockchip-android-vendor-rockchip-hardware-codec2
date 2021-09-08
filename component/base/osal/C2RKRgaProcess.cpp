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

#undef  ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "C2RKRgaProcess"

#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <dlfcn.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "hardware/hardware_rockchip.h"
#include "hardware/rga.h"
#include "RgaApi.h"
#include "C2RKRgaProcess.h"
#include "C2RKLog.h"

int32_t rga_dev_open(void **rga_ctx)
{
    FunctionIn();

    RgaInit(rga_ctx);
    if (*rga_ctx == NULL) {
        c2_err("rga init fail!!!");
        return -1;
    }

    FunctionOut();

    return 0;
}

int32_t rga_dev_close(void *rga_ctx)
{
    FunctionIn();

    RgaDeInit(rga_ctx);

    FunctionOut();

    return 0;
}

void rga_rgb2nv12(RKVideoPlane *plane, VPUMemLinear_t *vpumem,
                  uint32_t Width, uint32_t Height, uint32_t dstWidth, uint32_t dstHeight,  void* rga_ctx)
{
    FunctionIn();

    rga_info_t src;
    rga_info_t dst;
    (void) rga_ctx;
    memset((void*)&src, 0, sizeof(rga_info_t));
    memset((void*)&dst, 0, sizeof(rga_info_t));
    c2_trace(" plane->stride %d", plane->stride);
    rga_set_rect(&src.rect, 0, 0, Width, Height, plane->stride, Height, HAL_PIXEL_FORMAT_RGBA_8888);
    rga_set_rect(&dst.rect, 0, 0, Width, Height, dstWidth, dstHeight, HAL_PIXEL_FORMAT_YCrCb_NV12);
    src.fd = plane->fd;
    dst.fd = vpumem->phy_addr;
    c2_trace("RgaBlit in src.fd = 0x%x, dst.fd = 0x%x", src.fd, dst.fd);
    if (RgaBlit(&src, &dst, NULL)) {
        c2_err("RgaBlit fail");
    }

    FunctionOut();
}

void rga_nv12_copy(RKVideoPlane *plane, VPUMemLinear_t *vpumem,
            uint32_t Width, uint32_t Height, void* rga_ctx) {
    FunctionIn();

    rga_info_t src;
    rga_info_t dst;
    (void) rga_ctx;
    memset((void*)&src, 0, sizeof(rga_info_t));
    memset((void*)&dst, 0, sizeof(rga_info_t));
    rga_set_rect(&src.rect, 0, 0, Width, Height, plane->stride, Height, HAL_PIXEL_FORMAT_YCrCb_NV12);
    rga_set_rect(&dst.rect, 0, 0, Width, Height, Width, Height, HAL_PIXEL_FORMAT_YCrCb_NV12);
    src.fd = plane->fd;
    dst.fd = vpumem->phy_addr;
    if (RgaBlit(&src, &dst, NULL)) {
        c2_err("RgaBlit fail");
    }

    FunctionOut();
}
