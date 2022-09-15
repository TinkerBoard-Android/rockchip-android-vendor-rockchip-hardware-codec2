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
#define ROCKCHIP_LOG_TAG    "C2RKMediaUtils"

#include <string.h>

#include "hardware/hardware_rockchip.h"
#include "hardware/gralloc_rockchip.h"
#include "C2RKMediaUtils.h"
#include "C2RKLog.h"
#include "mpp/mpp_soc.h"


using namespace android;

C2_U32 c2_vdec_debug = 0;
C2_U32 c2_venc_debug = 0;

bool C2RKMediaUtils::getCodingTypeFromComponentName(
        C2String componentName, MppCodingType *codingType) {
    FunctionIn();

    for (int i = 0; i < C2_RK_ARRAY_ELEMS(kComponentMapEntry); ++i) {
        if (!strcasecmp(componentName.c_str(), kComponentMapEntry[i].componentName.c_str())) {
            *codingType = kComponentMapEntry[i].codingType;
            return true;
        }
    }

    *codingType = MPP_VIDEO_CodingUnused;

    FunctionOut();

    return false;
}

bool C2RKMediaUtils::getMimeFromComponentName(C2String componentName, C2String *mime) {
    FunctionIn();

    for (int i = 0; i < C2_RK_ARRAY_ELEMS(kComponentMapEntry); ++i) {
        if (!strcasecmp(componentName.c_str(), kComponentMapEntry[i].componentName.c_str())) {
            *mime = kComponentMapEntry[i].mime;
            return true;
        }
    }

    FunctionOut();

    return false;
}
bool C2RKMediaUtils::getKindFromComponentName(C2String componentName, C2Component::kind_t *kind) {
    FunctionIn();

    C2Component::kind_t tmp_kind = C2Component::KIND_OTHER;
    if (componentName.find("encoder") != std::string::npos) {
        tmp_kind = C2Component::KIND_ENCODER;
    } else if (componentName.find("decoder") != std::string::npos) {
        tmp_kind = C2Component::KIND_DECODER;
    } else {
        return false;
    }

    *kind = tmp_kind;

    FunctionOut();

    return true;
}

bool C2RKMediaUtils::getDomainFromComponentName(C2String componentName, C2Component::domain_t *domain) {
    FunctionIn();

    MppCodingType codingType;
    C2Component::domain_t tmp_domain;

    if (!getCodingTypeFromComponentName(componentName, &codingType)) {
        c2_err("get coding type from component name failed");
        return false;
    }

    switch (codingType) {
        case MPP_VIDEO_CodingAVC:
        case MPP_VIDEO_CodingVP9:
        case MPP_VIDEO_CodingHEVC:
        case MPP_VIDEO_CodingVP8:
        case MPP_VIDEO_CodingMPEG2:
        case MPP_VIDEO_CodingMPEG4:
        case MPP_VIDEO_CodingH263:
        case MPP_VIDEO_CodingAV1: {
            tmp_domain = C2Component::DOMAIN_VIDEO;
        } break;
        default: {
            c2_err("unsupport coding type: %d", codingType);
            return false;
        }
    }

    *domain = tmp_domain;

    FunctionOut();

    return true;
}


int32_t C2RKMediaUtils::colorFormatMpiToAndroid(uint32_t format, bool fbcMode) {
    FunctionIn();

    int32_t aFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;

    switch (format & MPP_FRAME_FMT_MASK) {
        case MPP_FMT_YUV422SP:
        case MPP_FMT_YUV422P: {
            if (fbcMode) {
                aFormat = HAL_PIXEL_FORMAT_YCbCr_422_I;
            } else {
                aFormat = HAL_PIXEL_FORMAT_YCbCr_422_SP;
            }
        } break;
        case MPP_FMT_YUV420SP:
        case MPP_FMT_YUV420P: {
            if (fbcMode) {
                aFormat = HAL_PIXEL_FORMAT_YUV420_8BIT_I;
            } else {
                aFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
            }
        } break;
        case MPP_FMT_YUV420SP_10BIT: {
            if (fbcMode) {
                aFormat = HAL_PIXEL_FORMAT_YUV420_10BIT_I;
            } else {
                aFormat = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
            }
        } break;
        case MPP_FMT_YUV422SP_10BIT: {
            if (fbcMode) {
                aFormat = HAL_PIXEL_FORMAT_Y210;
            } else {
                aFormat = HAL_PIXEL_FORMAT_YCbCr_422_SP_10;
            }
        } break;
        default: {
            c2_err("unsupport color format: 0x%x", format);
        }
    }

    FunctionOut();

    return aFormat;
}

bool C2RKMediaUtils::checkHWSupport(MppCtxType type, MppCodingType codingType) {
    c2_info("type:%d codingType:%d", type, codingType);

    if (!mpp_check_soc_cap(type, codingType)) {
        return false;
    }

    return true;
}

uint64_t C2RKMediaUtils::getStrideUsage(int32_t width, int32_t stride) {
    if (stride == C2_ALIGN_ODD(width, 256)) {
        return RK_GRALLOC_USAGE_STRIDE_ALIGN_256_ODD_TIMES;
    } else if (stride == C2_ALIGN(width, 128)) {
        return  RK_GRALLOC_USAGE_STRIDE_ALIGN_128;
    } else if (stride == C2_ALIGN(width, 64)) {
        return RK_GRALLOC_USAGE_STRIDE_ALIGN_64;
    } else {
        return RK_GRALLOC_USAGE_STRIDE_ALIGN_16;
    }
}
