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

#ifndef ANDROID_C2_RK_MEDIA_DEFS_H_
#define ANDROID_C2_RK_MEDIA_DEFS_H_

#include <media/stagefright/foundation/MediaDefs.h>

#include "mpp/rk_mpi.h"
#include <C2.h>
#include <C2RKComponent.h>

enum C2OperatorType {
    C2_OP_INTERNAL      =0,
    C2_OP_UI
};

typedef struct _RKMemLinear {
    int32_t  phyAddr;
    int32_t  size;
    void    *windowBuf;
} RKMemLinear;

typedef struct _RKVideoPlane {
    void        *addr;
    uint32_t    allocSize;
    uint32_t    dataSize;
    uint32_t    offset;
    int32_t     fd;
    int32_t     type;
    uint32_t    stride;
} RKVideoPlane;

namespace android {

#define C2_SAFE_FREE(p) { if (p) {free(p); (p)=NULL;} }
#define C2_ALIGN(x, a)         (((x)+(a)-1)&~((a)-1))

extern const char *C2_RK_AVC_DEC_COMPONENT_NAME;
extern const char *C2_RK_VP9_DEC_COMPONENT_NAME;
extern const char *C2_RK_HEVC_DEC_COMPONENT_NAME;
extern const char *C2_RK_VP8_DEC_COMPONENT_NAME;
extern const char *C2_RK_MPEG2_DEC_COMPONENT_NAME;
extern const char *C2_RK_MPEG4_DEC_COMPONENT_NAME;
extern const char *C2_RK_H263_DEC_COMPONENT_NAME;
extern const char *C2_RK_AVC_ENC_COMPONENT_NAME;
extern const char *C2_RK_HEVC_ENC_COMPONENT_NAME;

static const struct CodingNameMapEntry {
    C2String        componentName;
    MppCodingType   codingType;
    C2String        mime;
} kCodingNameMapEntry[] = {
    { C2_RK_AVC_DEC_COMPONENT_NAME, MPP_VIDEO_CodingAVC, MEDIA_MIMETYPE_VIDEO_AVC },
    { C2_RK_VP9_DEC_COMPONENT_NAME, MPP_VIDEO_CodingVP9, MEDIA_MIMETYPE_VIDEO_VP9 },
    { C2_RK_HEVC_DEC_COMPONENT_NAME, MPP_VIDEO_CodingHEVC, MEDIA_MIMETYPE_VIDEO_HEVC },
    { C2_RK_VP8_DEC_COMPONENT_NAME, MPP_VIDEO_CodingVP8, MEDIA_MIMETYPE_VIDEO_VP8 },
    { C2_RK_MPEG2_DEC_COMPONENT_NAME, MPP_VIDEO_CodingMPEG2, MEDIA_MIMETYPE_VIDEO_MPEG2 },
    { C2_RK_MPEG4_DEC_COMPONENT_NAME, MPP_VIDEO_CodingMPEG4, MEDIA_MIMETYPE_VIDEO_MPEG4 },
    { C2_RK_H263_DEC_COMPONENT_NAME, MPP_VIDEO_CodingH263, MEDIA_MIMETYPE_VIDEO_H263 },
    { C2_RK_AVC_ENC_COMPONENT_NAME, MPP_VIDEO_CodingAVC, MEDIA_MIMETYPE_VIDEO_AVC },
    { C2_RK_HEVC_ENC_COMPONENT_NAME, MPP_VIDEO_CodingHEVC, MEDIA_MIMETYPE_VIDEO_HEVC }
};

int getCodingTypeFromComponentName(C2String componentName, MppCodingType *codingType);
int getMimeFromComponentName(C2String componentName, C2String *mime);
int getKindFromComponentName(C2String componentName, C2Component::kind_t *kind);
int getDomainFromComponentName(C2String componentName, C2Component::domain_t *domain);

uint32_t colorFormatMpiToAndroid(const uint32_t format);

}  // namespace android

#endif  // ANDROID_C2_RK_MEDIA_DEFS_H_

