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
#define ROCKCHIP_LOG_TAG    "C2RKRgaDef"

#include <string.h>

#include "C2RKRgaDef.h"
#include "C2RKLog.h"
#include "RockchipRga.h"
#include "hardware/hardware_rockchip.h"

using namespace android;

void C2RKRgaDef::paramInit(RgaParam *param, int32_t fd,
                           int32_t width, int32_t height,
                           int32_t wstride, int32_t hstride) {
    memset(param, 0, sizeof(RgaParam));

    param->fd = fd;
    param->width = width;
    param->height = height;
    param->wstride = (wstride > 0) ? wstride : width;
    param->hstride = (hstride > 0) ? hstride : height;
}

bool C2RKRgaDef::rgbToNv12(RgaParam srcParam, RgaParam dstParam) {
    rga_info_t src;
    rga_info_t dst;

    RockchipRga& rkRga(RockchipRga::get());

    c2_trace("rga src fd %d rect[%d, %d, %d, %d]", srcParam.fd,
             srcParam.width, srcParam.height, srcParam.wstride, srcParam.hstride);
    c2_trace("rga dst fd %d rect[%d, %d, %d, %d]", dstParam.fd,
             dstParam.width, dstParam.height, dstParam.wstride, dstParam.hstride);

    if ((srcParam.wstride % 4) != 0) {
        c2_warn("err yuv not align to 4");
        return true;
    }

    memset((void*)&src, 0, sizeof(rga_info_t));
    memset((void*)&dst, 0, sizeof(rga_info_t));

    src.fd = srcParam.fd;
    dst.fd = dstParam.fd;
    rga_set_rect(&src.rect, 0, 0,srcParam.width, srcParam.height,
                 srcParam.wstride, srcParam.hstride, HAL_PIXEL_FORMAT_RGBA_8888);
    rga_set_rect(&dst.rect, 0, 0,dstParam.width, dstParam.height,
                 dstParam.wstride, dstParam.hstride, HAL_PIXEL_FORMAT_YCrCb_NV12);

    if (rkRga.RkRgaBlit(&src, &dst, NULL)) {
        c2_err("RgaBlit fail, rgbToNv12");
        return false;
    }

    return true;
}

bool C2RKRgaDef::nv12Copy(RgaParam srcParam, RgaParam dstParam) {
    rga_info_t src;
    rga_info_t dst;

    RockchipRga& rkRga(RockchipRga::get());

    c2_trace("rga src fd %d rect[%d, %d, %d, %d]", srcParam.fd,
             srcParam.width, srcParam.height, srcParam.wstride, srcParam.hstride);
    c2_trace("rga dst fd %d rect[%d, %d, %d, %d]", dstParam.fd,
             dstParam.width, dstParam.height, dstParam.wstride, dstParam.hstride);

    if ((srcParam.wstride % 4) != 0) {
        c2_warn("err yuv not align to 4");
        return true;
    }

    memset((void*)&src, 0, sizeof(rga_info_t));
    memset((void*)&dst, 0, sizeof(rga_info_t));

    src.fd = srcParam.fd;
    dst.fd = dstParam.fd;
    rga_set_rect(&src.rect, 0, 0,srcParam.width, srcParam.height,
                 srcParam.wstride, srcParam.hstride, HAL_PIXEL_FORMAT_YCrCb_NV12);
    rga_set_rect(&dst.rect, 0, 0,dstParam.width, dstParam.height,
                 dstParam.wstride, dstParam.hstride, HAL_PIXEL_FORMAT_YCrCb_NV12);

    if (rkRga.RkRgaBlit(&src, &dst, NULL)) {
        c2_err("RgaBlit fail, nv12Copy");
        return false;
    }

    return true;
}
