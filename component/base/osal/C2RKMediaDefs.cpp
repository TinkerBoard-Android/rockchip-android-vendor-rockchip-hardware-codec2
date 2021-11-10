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
#define ROCKCHIP_LOG_TAG    "C2RKMediaDefs"

#include <string.h>
#include <C2Component.h>

#include "hardware/hardware_rockchip.h"
#include "C2RKMediaDefs.h"
#include "C2RKLog.h"

namespace android {

const char *C2_RK_AVC_DEC_COMPONENT_NAME = "c2.rk.avc.decoder";
const char *C2_RK_VP9_DEC_COMPONENT_NAME = "c2.rk.vp9.decoder";
const char *C2_RK_HEVC_DEC_COMPONENT_NAME = "c2.rk.hevc.decoder";
const char *C2_RK_VP8_DEC_COMPONENT_NAME = "c2.rk.vp8.decoder";
const char *C2_RK_MPEG2_DEC_COMPONENT_NAME = "c2.rk.mpeg2.decoder";
const char *C2_RK_MPEG4_DEC_COMPONENT_NAME = "c2.rk.m4v.decoder";
const char *C2_RK_H263_DEC_COMPONENT_NAME = "c2.rk.h263.decoder";
const char *C2_RK_AVC_ENC_COMPONENT_NAME = "c2.rk.avc.encoder";
const char *C2_RK_HEVC_ENC_COMPONENT_NAME = "c2.rk.hevc.encoder";

int getCodingTypeFromComponentName(
        C2String componentName, MppCodingType *codingType) {
    FunctionIn();

    for (size_t i = 0;
         i < sizeof(kCodingNameMapEntry) / sizeof(kCodingNameMapEntry[0]);
         ++i) {
        if (!strcasecmp(componentName.c_str(), kCodingNameMapEntry[i].componentName.c_str())) {
            *codingType = kCodingNameMapEntry[i].codingType;
            return 0;
        }
    }

    *codingType = MPP_VIDEO_CodingUnused;

    FunctionOut();

    return -1;
}

int getMimeFromComponentName(C2String componentName, C2String *mime) {
    FunctionIn();

    for (size_t i = 0;
         i < sizeof(kCodingNameMapEntry) / sizeof(kCodingNameMapEntry[0]);
         ++i) {
        if (!strcasecmp(componentName.c_str(), kCodingNameMapEntry[i].componentName.c_str())) {
            *mime = kCodingNameMapEntry[i].mime;
            return 0;
        }
    }

    FunctionOut();

    return -1;
}
int getKindFromComponentName(C2String componentName, C2Component::kind_t *kind) {
    FunctionIn();

    C2Component::kind_t tmp_kind = C2Component::KIND_OTHER;
    if (componentName.find("encoder") != std::string::npos) {
        tmp_kind = C2Component::KIND_ENCODER;
    } else if (componentName.find("decoder") != std::string::npos) {
        tmp_kind = C2Component::KIND_DECODER;
    } else {
        return -1;
    }

    *kind = tmp_kind;

    FunctionOut();

    return 0;
}

int getDomainFromComponentName(C2String componentName, C2Component::domain_t *domain) {
    FunctionIn();

    int ret = 0;
    MppCodingType codingType;
    C2Component::domain_t tmp_domain;
    ret = getCodingTypeFromComponentName(componentName, &codingType);
    if (ret) {
        c2_err("get coding type from component name failed");
        return ret;
    }

    switch (codingType) {
        case MPP_VIDEO_CodingAVC:
        case MPP_VIDEO_CodingVP9:
        case MPP_VIDEO_CodingHEVC:
        case MPP_VIDEO_CodingVP8:
        case MPP_VIDEO_CodingMPEG2:
        case MPP_VIDEO_CodingMPEG4:
        case MPP_VIDEO_CodingH263: {
            tmp_domain = C2Component::DOMAIN_VIDEO;
        } break;
        default: {
            c2_err("unsupport coding type: %d", codingType);
            ret = -1;
            return ret;
        }
    }

    *domain = tmp_domain;

    FunctionOut();

    return ret;
}


uint32_t colorFormatMpiToAndroid(const uint32_t format) {
    FunctionIn();

    uint32_t androidFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
    switch (format) {
        case MPP_FMT_YUV422SP:
        case MPP_FMT_YUV422P: {
            androidFormat = HAL_PIXEL_FORMAT_YCbCr_422_SP;
        } break;
        case MPP_FMT_YUV420SP:
        case MPP_FMT_YUV420P: {
            androidFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
        } break;
        case MPP_FMT_YUV420SP_10BIT: {
            androidFormat = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
        } break;
        case MPP_FMT_YUV422SP_10BIT: {
            androidFormat = HAL_PIXEL_FORMAT_YCbCr_422_SP_10;
        } break;
        default: {
            c2_err("unsupport color format: %d", format);
        }
    }

    FunctionOut();

    return androidFormat;
}

int32_t VPUMallocLinear(VPUMemLinear_t *p, uint32_t size)
{
    int ret = 0;
    MppBuffer buffer = NULL;
    ret = mpp_buffer_get(NULL, &buffer, size);
    if (ret != MPP_OK) {
        return -1;
    }
    p->phy_addr = (uint32_t)mpp_buffer_get_fd(buffer);
    p->vir_addr = (uint32_t*)mpp_buffer_get_ptr(buffer);
    p->size = size;
    p->offset = (uint32_t*)buffer;
    return 0;
}

int32_t VPUFreeLinear(VPUMemLinear_t *p)
{
    if (p->offset != NULL) {
        MppBuffer buf = (MppBuffer)p->offset;
        c2_trace("p %p buffer %p\n", p, buf);
        if (buf != NULL) {
            mpp_buffer_put(buf);
            memset(p, 0, sizeof(VPUMemLinear_t));
        }
    }
    return 0;
}

}

