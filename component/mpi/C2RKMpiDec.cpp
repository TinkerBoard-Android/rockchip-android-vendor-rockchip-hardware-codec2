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
#define ROCKCHIP_LOG_TAG    "C2RKMpiDec"

#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <C2AllocatorGralloc.h>
#include <Codec2Mapper.h>
#include <ui/GraphicBufferMapper.h>
#include <gralloc_priv_omx.h>

#include "hardware/hardware_rockchip.h"
#include "hardware/gralloc_rockchip.h"
#include "C2RKMpiDec.h"
#include "C2RKLog.h"
#include "C2RKMediaUtils.h"
#include "C2RKRgaDef.h"
#include "C2RKFbcDef.h"
#include "C2RKColorAspects.h"
#include "C2RKVersion.h"
#include "C2RKEnv.h"
#include <sys/syscall.h>

#define FLAG_NON_DISPLAY_FRAME (1u << 15)

namespace android {

constexpr uint32_t kDefaultOutputDelay = 16;
constexpr uint32_t kMaxOutputDelay = 16;

/* max support video resolution */
constexpr uint32_t kMaxVideoWidth = 8192;
constexpr uint32_t kMaxVideoHeight = 4320;

constexpr uint32_t kMaxReferenceCount = 16;
constexpr size_t kMinInputBufferSize = 2 * 1024 * 1024;

constexpr uint32_t kMaxGegerationClearCount = 100;

class C2RKMpiDec::IntfImpl : public C2RKInterface<void>::BaseParams {
public:
    explicit IntfImpl(
            const std::shared_ptr<C2ReflectorHelper> &helper,
            C2String name,
            C2Component::kind_t kind,
            C2Component::domain_t domain,
            C2String mediaType)
        : C2RKInterface<void>::BaseParams(helper, name, kind, domain, mediaType) {
        addParameter(
                DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::output(kDefaultOutputDelay))
                .withFields({C2F(mActualOutputDelay, value).inRange(0, kMaxOutputDelay)})
                .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
                .build());

        // input picture frame size
        addParameter(
                DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
                .withFields({
                    C2F(mSize, width).inRange(2, kMaxVideoWidth, 2),
                    C2F(mSize, height).inRange(2, kMaxVideoWidth, 2),
                })
                .withSetter(SizeSetter)
                .build());

        addParameter(
                DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
                .withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
                .withFields({
                    C2F(mSize, width).inRange(2, kMaxVideoWidth, 2),
                    C2F(mSize, height).inRange(2, kMaxVideoWidth, 2),
                })
                .withSetter(MaxPictureSizeSetter, mSize)
                .build());

        addParameter(
                DefineParam(mBlockSize, C2_PARAMKEY_BLOCK_SIZE)
                .withDefault(new C2StreamBlockSizeInfo::output(0u, 320, 240))
                .withFields({
                    C2F(mBlockSize, width).inRange(2, kMaxVideoWidth, 2),
                    C2F(mBlockSize, height).inRange(2, kMaxVideoWidth, 2),
                })
                .withSetter(BlockSizeSetter)
                .build());

        // TODO: support more formats?
        addParameter(
                DefineParam(mPixelFormat, C2_PARAMKEY_PIXEL_FORMAT)
                .withConstValue(new C2StreamPixelFormatInfo::output(
                                    0u, HAL_PIXEL_FORMAT_YCBCR_420_888))
                .build());

        // profile and level
        if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_AVC_BASELINE, C2Config::LEVEL_AVC_5_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_AVC_CONSTRAINED_BASELINE,
                                C2Config::PROFILE_AVC_BASELINE,
                                C2Config::PROFILE_AVC_MAIN,
                                C2Config::PROFILE_AVC_CONSTRAINED_HIGH,
                                C2Config::PROFILE_AVC_PROGRESSIVE_HIGH,
                                C2Config::PROFILE_AVC_HIGH}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
                                C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
                                C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                                C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2,
                                C2Config::LEVEL_AVC_4, C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                                C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1, C2Config::LEVEL_AVC_5_2})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_HEVC) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_HEVC_MAIN, C2Config::LEVEL_HEVC_MAIN_5_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_HEVC_MAIN,
                                C2Config::PROFILE_HEVC_MAIN_10}),
                        C2F(mProfileLevel, level).oneOf({
                               C2Config::LEVEL_HEVC_MAIN_1,
                               C2Config::LEVEL_HEVC_MAIN_2, C2Config::LEVEL_HEVC_MAIN_2_1,
                               C2Config::LEVEL_HEVC_MAIN_3, C2Config::LEVEL_HEVC_MAIN_3_1,
                               C2Config::LEVEL_HEVC_MAIN_4, C2Config::LEVEL_HEVC_MAIN_4_1,
                               C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1,
                               C2Config::LEVEL_HEVC_MAIN_5_2, C2Config::LEVEL_HEVC_HIGH_4,
                               C2Config::LEVEL_HEVC_HIGH_4_1, C2Config::LEVEL_HEVC_HIGH_5,
                               C2Config::LEVEL_HEVC_HIGH_5_1})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_MP2V_SIMPLE, C2Config::LEVEL_MP2V_HIGH))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_MP2V_SIMPLE,
                                C2Config::PROFILE_MP2V_MAIN}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_MP2V_LOW,
                                C2Config::LEVEL_MP2V_MAIN,
                                C2Config::LEVEL_MP2V_HIGH_1440,
                                C2Config::LEVEL_MP2V_HIGH})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_MPEG4) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_MP4V_SIMPLE, C2Config::LEVEL_MP4V_3))
                   .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_MP4V_SIMPLE}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_MP4V_0,
                                C2Config::LEVEL_MP4V_0B,
                                C2Config::LEVEL_MP4V_1,
                                C2Config::LEVEL_MP4V_2,
                                C2Config::LEVEL_MP4V_3})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_H263) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_H263_BASELINE, C2Config::LEVEL_H263_30))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_H263_BASELINE,
                                C2Config::PROFILE_H263_ISWV2}),
                       C2F(mProfileLevel, level).oneOf({
                               C2Config::LEVEL_H263_10,
                               C2Config::LEVEL_H263_20,
                               C2Config::LEVEL_H263_30,
                               C2Config::LEVEL_H263_40,
                               C2Config::LEVEL_H263_45})
                    })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_VP9) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::input(0u,
                            C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_5))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                                C2Config::PROFILE_VP9_0,
                                C2Config::PROFILE_VP9_2}),
                        C2F(mProfileLevel, level).oneOf({
                                C2Config::LEVEL_VP9_1,
                                C2Config::LEVEL_VP9_1_1,
                                C2Config::LEVEL_VP9_2,
                                C2Config::LEVEL_VP9_2_1,
                                C2Config::LEVEL_VP9_3,
                                C2Config::LEVEL_VP9_3_1,
                                C2Config::LEVEL_VP9_4,
                                C2Config::LEVEL_VP9_4_1,
                                C2Config::LEVEL_VP9_5})
                     })
                    .withSetter(ProfileLevelSetter, mSize)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_VP8) {
            // TODO
         }

        // max input buffer size
        addParameter(
                DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
                .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kMinInputBufferSize))
                .withFields({
                    C2F(mMaxInputSize, value).any(),
                })
                .calculatedAs(MaxInputSizeSetter, mMaxSize)
                .build());

        // ColorInfo
        C2ChromaOffsetStruct locations[1] = { C2ChromaOffsetStruct::ITU_YUV_420_0() };
        std::shared_ptr<C2StreamColorInfo::output> defaultColorInfo =
            C2StreamColorInfo::output::AllocShared(
                    1u, 0u, 8u /* bitDepth */, C2Color::YUV_420);
        memcpy(defaultColorInfo->m.locations, locations, sizeof(locations));

        defaultColorInfo =
            C2StreamColorInfo::output::AllocShared(
                   { C2ChromaOffsetStruct::ITU_YUV_420_0() },
                   0u, 8u /* bitDepth */, C2Color::YUV_420);
        helper->addStructDescriptors<C2ChromaOffsetStruct>();

        addParameter(
                DefineParam(mColorInfo, C2_PARAMKEY_CODED_COLOR_INFO)
                .withConstValue(defaultColorInfo)
                .build());

        // colorAspects
        addParameter(
                DefineParam(mDefaultColorAspects, C2_PARAMKEY_DEFAULT_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsTuning::output(
                        0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                        C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                .withFields({
                    C2F(mDefaultColorAspects, range).inRange(
                            C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                    C2F(mDefaultColorAspects, primaries).inRange(
                            C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                    C2F(mDefaultColorAspects, transfer).inRange(
                            C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                    C2F(mDefaultColorAspects, matrix).inRange(
                            C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
                })
                .withSetter(DefaultColorAspectsSetter)
                .build());

        // vui colorAspects
        if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC ||
            mediaType == MEDIA_MIMETYPE_VIDEO_HEVC ||
            mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2) {
            addParameter(
                    DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
                    .withDefault(new C2StreamColorAspectsInfo::input(
                            0u, C2Color::RANGE_LIMITED, C2Color::PRIMARIES_UNSPECIFIED,
                            C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                    .withFields({
                        C2F(mCodedColorAspects, range).inRange(
                                C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                        C2F(mCodedColorAspects, primaries).inRange(
                                C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                        C2F(mCodedColorAspects, transfer).inRange(
                                C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                        C2F(mCodedColorAspects, matrix).inRange(
                                C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
                    })
                    .withSetter(CodedColorAspectsSetter)
                    .build());

           addParameter(
                    DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                    .withDefault(new C2StreamColorAspectsInfo::output(
                            0u, C2Color::RANGE_UNSPECIFIED, C2Color::PRIMARIES_UNSPECIFIED,
                            C2Color::TRANSFER_UNSPECIFIED, C2Color::MATRIX_UNSPECIFIED))
                    .withFields({
                        C2F(mColorAspects, range).inRange(
                                C2Color::RANGE_UNSPECIFIED,     C2Color::RANGE_OTHER),
                        C2F(mColorAspects, primaries).inRange(
                                C2Color::PRIMARIES_UNSPECIFIED, C2Color::PRIMARIES_OTHER),
                        C2F(mColorAspects, transfer).inRange(
                                C2Color::TRANSFER_UNSPECIFIED,  C2Color::TRANSFER_OTHER),
                        C2F(mColorAspects, matrix).inRange(
                                C2Color::MATRIX_UNSPECIFIED,    C2Color::MATRIX_OTHER)
                    })
                    .withSetter(ColorAspectsSetter, mDefaultColorAspects, mCodedColorAspects)
                    .build());

            addParameter(
                    DefineParam(mLowLatency, C2_PARAMKEY_LOW_LATENCY_MODE)
                    .withDefault(new C2GlobalLowLatencyModeTuning(false))
                    .withFields({C2F(mLowLatency, value)})
                    .withSetter(Setter<decltype(*mLowLatency)>::NonStrictValueWithNoDeps)
                    .build());
        }
    }

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::output> &oldMe,
                          C2P<C2StreamPictureSizeInfo::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        if (me.set().width * me.set().height > kMaxVideoWidth * kMaxVideoHeight) {
            c2_warn("max support video resolution %dx%d, cur %dx%d",
                    kMaxVideoWidth, kMaxVideoHeight, me.set().width, me.set().height);
        }
        return res;
    }

    static C2R MaxPictureSizeSetter(bool mayBlock, C2P<C2StreamMaxPictureSizeTuning::output> &me,
                                    const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        // TODO: get max width/height from the size's field helpers vs. hardcoding
        me.set().width = c2_min(c2_max(me.v.width, size.v.width), kMaxVideoWidth);
        me.set().height = c2_min(c2_max(me.v.height, size.v.height), kMaxVideoWidth);
        if (me.set().width * me.set().height > kMaxVideoWidth * kMaxVideoHeight) {
            c2_warn("max support video resolution %dx%d, cur %dx%d",
                    kMaxVideoWidth, kMaxVideoHeight, me.set().width, me.set().height);
        }
        return C2R::Ok();
    }

    static C2R BlockSizeSetter(bool mayBlock, const C2P<C2StreamBlockSizeInfo::output> &oldMe,
                          C2P<C2StreamBlockSizeInfo::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (!me.F(me.v.width).supportsAtAll(me.v.width)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.width)));
            me.set().width = oldMe.v.width;
        }
        if (!me.F(me.v.height).supportsAtAll(me.v.height)) {
            res = res.plus(C2SettingResultBuilder::BadValue(me.F(me.v.height)));
            me.set().height = oldMe.v.height;
        }
        return res;
    }

    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input> &me,
                                  const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        (void)size;
        (void)me;  // TODO: validate
        return C2R::Ok();
    }

    static C2R MaxInputSizeSetter(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input> &me,
                                const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize) {
        (void)mayBlock;
        // assume compression ratio of 2
        me.set().value = c2_max((((maxSize.v.width + 63) / 64)
                * ((maxSize.v.height + 63) / 64) * 3072), kMinInputBufferSize);
        return C2R::Ok();
    }


    static C2R DefaultColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsTuning::output> &me) {
        (void)mayBlock;
        if (me.v.range > C2Color::RANGE_OTHER) {
                me.set().range = C2Color::RANGE_OTHER;
        }
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
                me.set().primaries = C2Color::PRIMARIES_OTHER;
        }
        if (me.v.transfer > C2Color::TRANSFER_OTHER) {
                me.set().transfer = C2Color::TRANSFER_OTHER;
        }
        if (me.v.matrix > C2Color::MATRIX_OTHER) {
                me.set().matrix = C2Color::MATRIX_OTHER;
        }
        return C2R::Ok();
    }

    static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me) {
        (void)mayBlock;
        if (me.v.range > C2Color::RANGE_OTHER) {
                me.set().range = C2Color::RANGE_OTHER;
        }
        if (me.v.primaries > C2Color::PRIMARIES_OTHER) {
                me.set().primaries = C2Color::PRIMARIES_OTHER;
        }
        if (me.v.transfer > C2Color::TRANSFER_OTHER) {
                me.set().transfer = C2Color::TRANSFER_OTHER;
        }
        if (me.v.matrix > C2Color::MATRIX_OTHER) {
                me.set().matrix = C2Color::MATRIX_OTHER;
        }
        return C2R::Ok();
    }

    static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                                  const C2P<C2StreamColorAspectsTuning::output> &def,
                                  const C2P<C2StreamColorAspectsInfo::input> &coded) {
        (void)mayBlock;
        // take default values for all unspecified fields, and coded values for specified ones
        me.set().range = coded.v.range == RANGE_UNSPECIFIED ? def.v.range : coded.v.range;
        me.set().primaries = coded.v.primaries == PRIMARIES_UNSPECIFIED
                ? def.v.primaries : coded.v.primaries;
        me.set().transfer = coded.v.transfer == TRANSFER_UNSPECIFIED
                ? def.v.transfer : coded.v.transfer;
        me.set().matrix = coded.v.matrix == MATRIX_UNSPECIFIED ? def.v.matrix : coded.v.matrix;
        return C2R::Ok();
    }

    std::shared_ptr<C2StreamPictureSizeInfo::output> getSize_l() {
        return mSize;
    }

    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects_l() {
        return mColorAspects;
    }

    std::shared_ptr<C2StreamColorAspectsTuning::output> getDefaultColorAspects_l() {
        return mDefaultColorAspects;
    }

    std::shared_ptr<C2GlobalLowLatencyModeTuning> getLowLatency_l() {
        return mLowLatency;
    }

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamBlockSizeInfo::output> mBlockSize;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
    std::shared_ptr<C2GlobalLowLatencyModeTuning> mLowLatency;
};

C2RKMpiDec::C2RKMpiDec(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mMppCtx(nullptr),
      mMppMpi(nullptr),
      mCodingType(MPP_VIDEO_CodingUnused),
      mColorFormat(MPP_FMT_YUV420SP),
      mFrmGrp(nullptr),
      mWidth(0),
      mHeight(0),
      mHorStride(0),
      mVerStride(0),
      mLastPts(-1),
      mGeneration(0),
      mGenerationChange(false),
      mGenerationCount(0),
      mStarted(false),
      mFlushed(false),
      mOutputEos(false),
      mSignalledInputEos(false),
      mSignalledError(false),
      mLowLatencyMode(false),
      mBufferMode(false),
      mOutFile(nullptr),
      mInFile(nullptr) {
    c2_info("version: %s", C2_GIT_BUILD_VERSION);

    if (!C2RKMediaUtils::getCodingTypeFromComponentName(name, &mCodingType)) {
        c2_err("failed to get codingType from component %s", name);
    }

    Rockchip_C2_GetEnvU32("vendor.c2.vdec.debug", &c2_vdec_debug, 0);
    c2_info("vdec_debug: 0x%x", c2_vdec_debug);

    if (c2_vdec_debug & VIDEO_DBG_RECORD_OUT) {
        char fileName[128];
        memset(fileName, 0, 128);

        sprintf(fileName, "/data/video/dec_out_%ld.bin", syscall(SYS_gettid));
        mOutFile = fopen(fileName, "wb");
        if (mOutFile == nullptr) {
            c2_err("failed to open output file, err %s", strerror(errno));
        } else {
            c2_info("recording output to %s", fileName);
        }
    }

    if (c2_vdec_debug & VIDEO_DBG_RECORD_IN) {
        char fileName[128];
        memset(fileName, 0, 128);

        sprintf(fileName, "/data/video/dec_in_%ld.bin", syscall(SYS_gettid));
        mInFile = fopen(fileName, "wb");
        if (mInFile == nullptr) {
            c2_err("failed to open output file, err %s", strerror(errno));
        } else {
            c2_info("recording output to %s", fileName);
        }
    }
}

C2RKMpiDec::~C2RKMpiDec() {
    c2_info_f("in");
    onRelease();
}

c2_status_t C2RKMpiDec::onInit() {
    c2_info_f("in");
    return C2_OK;
}

c2_status_t C2RKMpiDec::onStop() {
    c2_info_f("in");
    if (!mFlushed) {
        return onFlush_sm();
    }

    return C2_OK;
}

void C2RKMpiDec::onReset() {
    c2_info_f("in");
    onStop();
}

void C2RKMpiDec::onRelease() {
    c2_info_f("in");

    mStarted = false;

    if (!mFlushed) {
        onFlush_sm();
    }

    if (mOutBlock) {
        mOutBlock.reset();
    }

    if (mFrmGrp != nullptr) {
        mpp_buffer_group_put(mFrmGrp);
        mFrmGrp = nullptr;
    }

    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    if (mOutFile != nullptr) {
        fclose(mOutFile);
        mOutFile = nullptr;
    }
    if (mInFile != nullptr) {
        fclose(mInFile);
        mInFile = nullptr;
    }
}

c2_status_t C2RKMpiDec::onFlush_sm() {
    c2_status_t ret = C2_OK;

    c2_info_f("in");

    mOutputEos = false;
    mSignalledInputEos = false;
    mSignalledError = false;
    mGeneration = 0;

    mWorkQueue.clear();
    clearOutBuffers();

    if (mFrmGrp) {
        mpp_buffer_group_clear(mFrmGrp);
    }

    if (mMppMpi) {
        mMppMpi->reset(mMppCtx);
    }

    mFlushed = true;

    return ret;
}

c2_status_t C2RKMpiDec::initDecoder() {
    MPP_RET err = MPP_OK;

    c2_info_f("in");

    {
        IntfImpl::Lock lock = mIntf->lock();
        mWidth = mIntf->getSize_l()->width;
        mHeight = mIntf->getSize_l()->height;
        mTransfer = (uint32_t)mIntf->getDefaultColorAspects_l()->transfer;
        if (mIntf->getLowLatency_l() != nullptr) {
            mLowLatencyMode = (mIntf->getLowLatency_l()->value > 0) ? true : false ;
        }
    }

    c2_info("init: w %d h %d coding %d", mWidth, mHeight, mCodingType);

    err = mpp_create(&mMppCtx, &mMppMpi);
    if (err != MPP_OK) {
        c2_err("failed to mpp_create, ret %d", err);
        goto error;
    }

    // TODO: workround: CTS-CodecDecoderTest
    // testFlushNative[15(c2.rk.mpeg2.decoder_video/mpeg2)
    if (mCodingType == MPP_VIDEO_CodingMPEG2) {
        uint32_t vmode = 0, split = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &vmode);
        mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_SPLIT_MODE, &split);
    } else {
        // enable deinterlace, but not decting
        uint32_t vmode = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &vmode);
    }

    {
        // enable fast mode,
        uint32_t fastParser = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_PARSER_FAST_MODE, &fastParser);
    }

    err = mpp_init(mMppCtx, MPP_CTX_DEC, mCodingType);
    if (err != MPP_OK) {
        c2_err("failed to mpp_init, ret %d", err);
        goto error;
    }

    {
        // enable fast-play mode, ignore the effect of B-frame.
        uint32_t fastPlay = 1;
        mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_FAST_PLAY, &fastPlay);

        if (mLowLatencyMode) {
            uint32_t deinterlace = 0, immediate = 1;
            c2_info("enable lowLatency, enable mpp immediate-out mode");
            mMppMpi->control(mMppCtx, MPP_DEC_SET_ENABLE_DEINTERLACE, &deinterlace);
            mMppMpi->control(mMppCtx, MPP_DEC_SET_IMMEDIATE_OUT, &immediate);
        }
    }

    {
        MppFrame frame  = nullptr;
        uint32_t mppFmt = mColorFormat;

        /* user can't process fbc output on bufferMode */
        /* SMPTEST2084 = 6*/
        if ((mTransfer == 6) || (!mBufferMode && (mWidth * mHeight > 1920 * 1080))) {
            mFbcCfg.mode = C2RKFbcDef::getFbcOutputMode(mCodingType);
            if (mFbcCfg.mode) {
                c2_info("use mpp fbc output mode");
                mppFmt |= MPP_FRAME_FBC_AFBC_V2;
                mMppMpi->control(mMppCtx, MPP_DEC_SET_OUTPUT_FORMAT, (MppParam)&mppFmt);
            }
        } else {
            mFbcCfg.mode = 0;
        }

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, mWidth);
        mpp_frame_set_height(frame, mHeight);
        mpp_frame_set_fmt(frame, (MppFrameFormat)mppFmt);
        mMppMpi->control(mMppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);

        /*
         * Command "set-frame-info" may failed to provide stride info in old
         * mpp version, so config unaligned resolution for stride and then
         * info-change will sent to transmit correct stride.
         */
        if (mpp_frame_get_hor_stride(frame) <= 0 ||
            mpp_frame_get_ver_stride(frame) <= 0)
        {
            mpp_frame_set_hor_stride(frame, mWidth);
            mpp_frame_set_ver_stride(frame, mHeight);
            mMppMpi->control(mMppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);
        }

        mHorStride = mpp_frame_get_hor_stride(frame);
        mVerStride = mpp_frame_get_ver_stride(frame);

        c2_info("init: get stride [%d:%d]", mHorStride, mVerStride);

        mpp_frame_deinit(&frame);
    }

    /*
     * For buffer mode, since we don't konw when the last buffer will use
     * up by user, so we use MPP internal buffer group, and copy output to
     * dst block(mOutBlock).
     */
    if (!mBufferMode) {
        err = mpp_buffer_group_get_external(&mFrmGrp, MPP_BUFFER_TYPE_ION);
        if (err != MPP_OK) {
            c2_err_f("failed to get buffer_group, err %d", err);
            goto error;
        }
        mMppMpi->control(mMppCtx, MPP_DEC_SET_EXT_BUF_GROUP, mFrmGrp);
    }

    /* fbc decode output has padding inside, set crop before display */
    if (mFbcCfg.mode) {
        C2RKFbcDef::getFbcOutputOffset(mCodingType,
                                       &mFbcCfg.paddingX,
                                       &mFbcCfg.paddingY);
        c2_info("fbc padding offset(%d, %d)", mFbcCfg.paddingX, mFbcCfg.paddingY);
    }

    mStarted = true;

    return C2_OK;

error:
    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    return C2_CORRUPTED;
}

void C2RKMpiDec::fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    uint32_t flags = 0;

    c2_trace_f("in");

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        c2_info("signalling eos");
    }

    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2RKMpiDec::finishWork(
        uint64_t index,
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2GraphicBlock> block,
        bool delayOutput) {
    if (!block) {
        c2_err("empty block index %d", index);
        return;
    }

    uint32_t left = mFbcCfg.mode ? mFbcCfg.paddingX : 0;
    uint32_t top  = mFbcCfg.mode ? mFbcCfg.paddingY : 0;

    std::shared_ptr<C2Buffer> buffer
            = createGraphicBuffer(std::move(block),
                                  C2Rect(mWidth, mHeight).at(left, top));

    mOutBlock = nullptr;

    {
        if (mCodingType == MPP_VIDEO_CodingAVC ||
            mCodingType == MPP_VIDEO_CodingHEVC ||
            mCodingType == MPP_VIDEO_CodingMPEG2) {
            IntfImpl::Lock lock = mIntf->lock();
            buffer->setInfo(mIntf->getColorAspects_l());
        }
    }

    class FillWork {
       public:
        FillWork(uint32_t flags, C2WorkOrdinalStruct ordinal,
                 const std::shared_ptr<C2Buffer>& buffer)
            : mFlags(flags), mOrdinal(ordinal), mBuffer(buffer) {}
        ~FillWork() = default;

        void operator()(const std::unique_ptr<C2Work>& work) {
            work->worklets.front()->output.flags = (C2FrameData::flags_t)mFlags;
            work->worklets.front()->output.buffers.clear();
            work->worklets.front()->output.ordinal = mOrdinal;
            work->workletsProcessed = 1u;
            work->result = C2_OK;
            if (mBuffer) {
                work->worklets.front()->output.buffers.push_back(mBuffer);
            }
            c2_trace("timestamp = %lld, index = %lld, w/%s buffer",
                     mOrdinal.timestamp.peekll(), mOrdinal.frameIndex.peekll(),
                     mBuffer ? "" : "o");
        }

       private:
        const uint32_t mFlags;
        const C2WorkOrdinalStruct mOrdinal;
        const std::shared_ptr<C2Buffer> mBuffer;
    };

    auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };

    if (work && c2_cntr64_t(index) == work->input.ordinal.frameIndex) {
        bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
        if (eos) {
            if (buffer) {
                C2WorkOrdinalStruct outOrdinal = work->input.ordinal;
                cloneAndSend(
                    index, work,
                    FillWork(C2FrameData::FLAG_INCOMPLETE, outOrdinal, buffer));
                buffer.reset();
            }
        } else {
            fillWork(work);
        }
    } else {
        finish(index, fillWork, delayOutput);
    }
}

c2_status_t C2RKMpiDec::drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work) {
    c2_info_f("in");

    if (drainMode == NO_DRAIN) {
        c2_warn("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        c2_warn("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    c2_status_t ret = C2_OK;
    OutWorkEntry entry;
    uint32_t kMaxRetryNum = 20;
    uint32_t retry = 0;

    while (true){
        ret = ensureDecoderState(pool);
        if (ret != C2_OK && work) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return C2_CORRUPTED;
        }

        ret = getoutframe(&entry, false);
        if (ret == C2_OK && entry.outblock) {
            finishWork(entry.frameIndex, work, entry.outblock);
        } else if (drainMode == DRAIN_COMPONENT_NO_EOS && !work) {
            c2_info_f("drain without wait eos, done.");
            break;
        }

        if (mOutputEos) {
            fillEmptyWork(work);
            break;
        }

        if ((++retry) > kMaxRetryNum) {
            mOutputEos = true;
            c2_warn("drain: eos not found, force set output EOS.");
        } else {
            usleep(5 * 1000);
        }
    }

    c2_info_f("out");

    return C2_OK;
}

c2_status_t C2RKMpiDec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    return drainInternal(drainMode, pool, nullptr);
}

void C2RKMpiDec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err = C2_OK;

    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    mFlushed = false;
    mBufferMode = (pool->getLocalId() <= C2BlockPool::PLATFORM_START);

    // Initialize decoder if not already initialized
    if (!mStarted) {
        err = initDecoder();
        if (err != C2_OK) {
            work->result = C2_BAD_VALUE;
            c2_info("failed to initialize, signalled Error");
            return;
        }
    }

    if (mSignalledInputEos || mSignalledError) {
        work->result = C2_BAD_VALUE;
        return;
    }

    uint8_t *inData = nullptr;
    size_t inSize = 0u;
    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inData = const_cast<uint8_t *>(rView.data());
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            c2_err("failed to read rWiew, error %d", rView.error());
            work->result = rView.error();
            return;
        }
    }

    uint32_t flags = work->input.flags;
    uint64_t frameIndex = work->input.ordinal.frameIndex.peekull();
    uint64_t timestamp = work->input.ordinal.timestamp.peekll();

    c2_trace("in buffer attr. size %zu timestamp %lld frameindex %lld, flags %x",
             inSize, timestamp, frameIndex, flags);

    bool eos = ((flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    bool hasPicture = false;
    bool delayOutput = false;
    bool needGetFrame = false;
    bool sendPacketFlag = true;
    uint32_t outfrmCnt = 0;
    OutWorkEntry entry;

    err = ensureDecoderState(pool);
    if (err != C2_OK) {
        mSignalledError = true;
        work->workletsProcessed = 1u;
        work->result = C2_CORRUPTED;
        return;
    }

inPacket:
    needGetFrame   = false;
    sendPacketFlag = true;
    // may block, quit util enqueue success.
    err = sendpacket(inData, inSize, frameIndex, timestamp, flags);
    if (err != C2_OK) {
        c2_warn("failed to enqueue packet, pts %lld", timestamp);
        needGetFrame = true;
        sendPacketFlag = false;
    } else if (flags & (C2FrameData::FLAG_CODEC_CONFIG | FLAG_NON_DISPLAY_FRAME)) {
        fillEmptyWork(work);
    } else {
        if (inSize == 0 && !eos) {
            fillEmptyWork(work);
        }

        // TODO workround: CTS-CodecDecoderTest
        // testFlushNative[15(c2.rk.mpeg2.decoder_video/mpeg2)
        if (mLastPts != timestamp) {
            mLastPts = timestamp;
        } else if (mCodingType == MPP_VIDEO_CodingMPEG2) {
            if (!eos) {
                fillEmptyWork(work);
            }
        }
    }

outframe:
    if (!eos) {
        err = getoutframe(&entry, needGetFrame);
        if (err == C2_OK) {
            outfrmCnt++;
            needGetFrame = false;
            hasPicture = true;
        } else if (err == C2_CORRUPTED) {
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        }
    }

    if (eos) {
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
        mSignalledInputEos = true;
    } else if (hasPicture) {
        finishWork(entry.frameIndex, work, entry.outblock, delayOutput);
        /* Avoid stock frame, continue to search available output */
        ensureDecoderState(pool);
        hasPicture = false;

        /* output pending work after the C2Work in process return. It is
           neccessary to output work sequentially, otherwise the output
           captured by the user may be discontinuous */
        if (entry.frameIndex == frameIndex) {
            delayOutput = true;
        }
        if (sendPacketFlag == false) {
            goto inPacket;
        }
        goto outframe;
    } else if (err == C2_NO_MEMORY) {
        ensureDecoderState(pool);
        // update new config and feekback to framework
        C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        err = mIntf->config({&size}, C2_MAY_BLOCK, &failures);
        if (err == OK) {
            work->worklets.front()->output.configUpdate.push_back(
                C2Param::Copy(size));
        } else {
            c2_err("failed to set width and height");
            mSignalledError = true;
            work->workletsProcessed = 1u;
            work->result = C2_CORRUPTED;
            return;
        }

        goto outframe;
    } else if (outfrmCnt == 0) {
        usleep(1000);
        if (mLowLatencyMode && timestamp > 0) {
            goto outframe;
        }
    }
}

void C2RKMpiDec::getVuiParams(MppFrame frame) {
    VuiColorAspects aspects;

    aspects.primaries = mpp_frame_get_color_primaries(frame);
    aspects.transfer  = mpp_frame_get_color_trc(frame);
    aspects.coeffs    = mpp_frame_get_colorspace(frame);
    if (mCodingType == MPP_VIDEO_CodingMPEG2) {
        aspects.fullRange = 0;
    } else {
        aspects.fullRange =
            (mpp_frame_get_color_range(frame) == MPP_FRAME_RANGE_JPEG);
    }

    // convert vui aspects to C2 values if changed
    if (!(aspects == mBitstreamColorAspects)) {
        mBitstreamColorAspects = aspects;
        ColorAspects sfAspects;
        C2StreamColorAspectsInfo::input codedAspects = { 0u };

        ColorUtils::convertIsoColorAspectsToCodecAspects(
                aspects.primaries, aspects.transfer, aspects.coeffs,
                aspects.fullRange, sfAspects);

        if (!C2Mapper::map(sfAspects.mPrimaries, &codedAspects.primaries)) {
            codedAspects.primaries = C2Color::PRIMARIES_UNSPECIFIED;
        }
        if (!C2Mapper::map(sfAspects.mRange, &codedAspects.range)) {
            codedAspects.range = C2Color::RANGE_UNSPECIFIED;
        }
        if (!C2Mapper::map(sfAspects.mMatrixCoeffs, &codedAspects.matrix)) {
            codedAspects.matrix = C2Color::MATRIX_UNSPECIFIED;
        }
        if (!C2Mapper::map(sfAspects.mTransfer, &codedAspects.transfer)) {
            codedAspects.transfer = C2Color::TRANSFER_UNSPECIFIED;
        }
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        mIntf->config({&codedAspects}, C2_MAY_BLOCK, &failures);

        c2_trace("VuiColorAspects: pri %d tra %d coeff %d range %d",
                 aspects.primaries, aspects.transfer,
                 aspects.coeffs, aspects.fullRange);
    }
}

c2_status_t C2RKMpiDec::sendpacket(
        uint8_t *data, size_t size, uint64_t frmIndex, uint64_t pts, uint32_t flags) {
    c2_status_t ret = C2_OK;
    MppPacket packet = nullptr;

    mpp_packet_init(&packet, data, size);
    mpp_packet_set_pts(packet, pts);
    mpp_packet_set_pos(packet, data);
    mpp_packet_set_length(packet, size);

    if (mInFile != nullptr) {
        fwrite(data, 1, size, mInFile);
        fflush(mInFile);
    }

    if (flags & C2FrameData::FLAG_END_OF_STREAM) {
        c2_info("send input eos");
        mpp_packet_set_eos(packet);
    }

    if (flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mpp_packet_set_extra_data(packet);
    }

    MPP_RET err = MPP_OK;
    uint32_t kMaxRetryNum = 20;
    uint32_t retry = 0;

    while (true) {
        err = mMppMpi->decode_put_packet(mMppCtx, packet);
        if (err == MPP_OK) {
            c2_trace("send packet pts %lld size %d", pts, size);
            if (!(flags & (C2FrameData::FLAG_CODEC_CONFIG | FLAG_NON_DISPLAY_FRAME))) {
                mWorkQueue.insert(std::make_pair(frmIndex, pts));
            }
            break;
        }

        if ((++retry) > kMaxRetryNum) {
            ret = C2_CORRUPTED;
            break;
        }
        usleep(5 * 1000);
    }

    mpp_packet_deinit(&packet);

    return ret;
}

c2_status_t C2RKMpiDec::getoutframe(OutWorkEntry *entry, bool needGetFrame) {
    c2_status_t ret = C2_OK;
    MPP_RET err = MPP_OK;
    MppFrame frame = nullptr;

    uint64_t outIndex = 0;
    uint32_t tryCount = 0;
    std::shared_ptr<C2GraphicBlock> outblock = nullptr;

REDO:
    err = mMppMpi->decode_get_frame(mMppCtx, &frame);
    tryCount++;
    if (MPP_OK != err || !frame) {
        if (needGetFrame == true && tryCount < 10) {
            c2_info("need to get frame");
            usleep(5*1000);
            goto REDO;
        }
        return C2_NOT_FOUND;
    }

    uint32_t width  = mpp_frame_get_width(frame);
    uint32_t height = mpp_frame_get_height(frame);
    uint32_t hstride = mpp_frame_get_hor_stride(frame);
    uint32_t vstride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat format = mpp_frame_get_fmt(frame);

    if (mpp_frame_get_info_change(frame)) {
        c2_info("info-change with old dimensions(%dx%d) stride(%dx%d) fmt %d", \
                mWidth, mHeight, mHorStride, mVerStride, mColorFormat);
        c2_info("info-change with new dimensions(%dx%d) stride(%dx%d) fmt %d", \
                width, height, hstride, vstride, format);

        if (!mBufferMode) {
            clearOutBuffers();
            mpp_buffer_group_clear(mFrmGrp);
        }

        /*
         * All buffer group config done. Set info change ready to let
         * decoder continue decoding
         */
        err = mMppMpi->control(mMppCtx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
        if (err) {
            c2_err_f("failed to set info-change ready, ret %d", ret);
            ret = C2_CORRUPTED;
            goto exit;
        }

        mWidth = width;
        mHeight = height;
        mHorStride = hstride;
        mVerStride = vstride;
        mColorFormat = format;

        ret = C2_NO_MEMORY;
    } else {
        uint32_t err  = mpp_frame_get_errinfo(frame);
        int64_t  pts  = mpp_frame_get_pts(frame);
        uint32_t eos  = mpp_frame_get_eos(frame);
        uint32_t mode = mpp_frame_get_mode(frame);

        MppBuffer mppBuffer = mpp_frame_get_buffer(frame);
        bool isI4O2 = (mode & MPP_FRAME_FLAG_IEP_DEI_MASK) == MPP_FRAME_FLAG_IEP_DEI_I4O2;

        // find frameIndex from pts map.
        for (auto it = mWorkQueue.begin(); it != mWorkQueue.end(); it++) {
            if (pts == it->second) {
                outIndex = it->first;
                mWorkQueue.erase(it);
                break;
            }
        }

        c2_trace("get one frame [%d:%d] stride [%d:%d] pts %lld err %d eos %d frameIndex %d",
                 width, height, hstride, vstride, pts, err, eos, outIndex);

        if (eos) {
            c2_info("get output eos.");
            mOutputEos = true;
            // ignore null frame with eos
            if (!mppBuffer) goto exit;
        }

        if (mBufferMode) {
            bool useRga = (width * height >= 1280 * 720);

            if (useRga) {
                RgaParam src, dst;

                int32_t srcFd = mpp_buffer_get_fd(mppBuffer);
                auto c2Handle = mOutBlock->handle();
                int32_t dstFd = c2Handle->data[0];

                C2RKRgaDef::paramInit(&src, srcFd, width, height, hstride, vstride);
                C2RKRgaDef::paramInit(&dst, dstFd, width, height, hstride, vstride);
                if (!C2RKRgaDef::nv12Copy(src, dst)) {
                    c2_err("faild to copy output to dstBlock on buffer mode.");
                    ret = C2_CORRUPTED;
                    goto exit;
                }
            } else {
                C2GraphicView wView = mOutBlock->map().get();
                uint8_t *dst = wView.data()[C2PlanarLayout::PLANE_Y];
                uint8_t *src = (uint8_t*)mpp_buffer_get_ptr(mppBuffer);

                memcpy(dst, src, hstride * vstride * 3 / 2);
            }

            outblock = mOutBlock;
        } else {
            OutBuffer *outBuffer = findOutBuffer(mppBuffer);
            if (!outBuffer) {
                c2_err("failed to find output buffer %p", mppBuffer);
                if (frame) {
                    mpp_frame_deinit(&frame);
                    frame = nullptr;
                }
                goto REDO;

            }
            mpp_buffer_inc_ref(mppBuffer);
            outBuffer->site = BUFFER_SITE_BY_C2;

            outblock = outBuffer->block;
            if (mOutFile != nullptr) {
                uint8_t *src = (uint8_t*)mpp_buffer_get_ptr(mppBuffer);
                fwrite(src, 1, hstride * vstride * 3 / 2, mOutFile);
                fflush(mOutFile);
            }
        }

        if (mCodingType == MPP_VIDEO_CodingAVC ||
            mCodingType == MPP_VIDEO_CodingHEVC ||
            mCodingType == MPP_VIDEO_CodingMPEG2)
        {
            getVuiParams(frame);
        }

        if (outIndex == 0) {
            if (isI4O2) {
                outIndex = I2O4INDEX;
            } else {
                c2_warn("get unexpect pts %lld, skip this frame", pts);
                if (mppBuffer) {
                    mpp_buffer_put(mppBuffer);
                }
            }
        }

        ret = C2_OK;
    }

exit:
    if (frame) {
        mpp_frame_deinit(&frame);
        frame = nullptr;
    }

    entry->outblock = outblock;
    entry->frameIndex = outIndex;

    return ret;
}

c2_status_t C2RKMpiDec::commitBufferToMpp(std::shared_ptr<C2GraphicBlock> block) {
    if (!block.get()) {
        c2_err_f("failed to get block");
        return C2_CORRUPTED;
    }

    auto c2Handle = block->handle();
    uint32_t fd = c2Handle->data[0];

    uint32_t bqSlot, width, height, format, stride, generation;
    uint64_t usage, bqId;

    android::_UnwrapNativeCodec2GrallocMetadata(
                c2Handle, &width, &height, &format, &usage,
                &stride, &generation, &bqId, &bqSlot);
    if (mGeneration == 0) {
        mGeneration = generation;
        mGenerationCount = 1;	
    } else if (mGeneration != generation) {
        c2_info("change generation");
        mGenerationChange = true;
        mGeneration = generation;
        mGenerationCount = 1;
    } else {
        mGenerationCount++;
    }
    auto GetC2BlockSize
            = [c2Handle, width, height, format, usage, stride]() -> uint32_t {
        gralloc_private_handle_t pHandle;
        buffer_handle_t bHandle;
        native_handle_t *nHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

        GraphicBufferMapper &gm(GraphicBufferMapper::get());
        gm.importBuffer(const_cast<native_handle_t *>(nHandle),
                        width, height, 1, format, usage,
                        stride, &bHandle);

        Rockchip_get_gralloc_private((uint32_t *)bHandle, &pHandle);

        gm.freeBuffer(bHandle);
        native_handle_delete(nHandle);

        return pHandle.size;
    };


    if (mGenerationCount > kMaxGegerationClearCount && mGenerationChange) {
        c2_info("clear old generation buffer");
        mGenerationChange = false;
        clearOldGenerationOutBuffers(generation);
    }

    OutBuffer *buffer = findOutBuffer(bqSlot);
    if (buffer && buffer->generation == generation) {
        /* commit this buffer back to mpp */
        MppBuffer mppBuffer = buffer->mppBuffer;
        if (mppBuffer) {
            mpp_buffer_put(mppBuffer);
        }
        buffer->block = block;
        buffer->site = BUFFER_SITE_BY_MPI;

        c2_trace("put this buffer: generation %d bpId 0x%llx slot %d fd %d buf %p", generation, bqId, bqSlot, fd, mppBuffer);
    } else {
        /* register this buffer to mpp group */
        MppBuffer mppBuffer;
        MppBufferInfo info;
        memset(&info, 0, sizeof(info));

        info.type = MPP_BUFFER_TYPE_ION;
        info.fd = fd;
        info.ptr = nullptr;
        info.hnd = nullptr;
        info.size = GetC2BlockSize();

        mpp_buffer_import_with_tag(mFrmGrp, &info,
                                   &mppBuffer, "codec2", __FUNCTION__);

        OutBuffer *buffer = new OutBuffer;
        buffer->index = bqSlot;
        buffer->mppBuffer = mppBuffer;
        buffer->block = block;
        buffer->site = BUFFER_SITE_BY_MPI;
        buffer->generation = generation;
        mpp_buffer_put(mppBuffer);

        mOutBuffers.push(buffer);

        c2_trace("import this buffer: slot %d fd %d size %d buf %p", bqSlot,
                 fd, info.size, mppBuffer);
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::ensureDecoderState(
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t ret = C2_OK;

    uint32_t blockW = mHorStride;
    uint32_t blockH = mVerStride;

    uint64_t usage  = RK_GRALLOC_USAGE_SPECIFY_STRIDE;
    uint32_t format = C2RKMediaUtils::colorFormatMpiToAndroid(mColorFormat, mFbcCfg.mode);

    std::lock_guard<std::mutex> lock(mPoolMutex);

    // workround for tencent-video, the application can not deal with crop
    // correctly, so use actual dimention when fetch block, make sure that
    // the output buffer carries all info needed.
    if (format == HAL_PIXEL_FORMAT_YCrCb_NV12 && mWidth != mHorStride) {
        blockW = mWidth;
        usage = C2RKMediaUtils::getStrideUsage(mWidth, mHorStride);
    }

    if (mFbcCfg.mode) {
        // NOTE: FBC case may have offset y on top and vertical stride
        // should aligned to 16.
        blockH = C2_ALIGN(mVerStride + mFbcCfg.paddingY, 16);

        // In fbc 10bit mode, treat width of buffer as pixer_stride.
        if (format == HAL_PIXEL_FORMAT_YUV420_10BIT_I ||
            format == HAL_PIXEL_FORMAT_Y210) {
            blockW = C2_ALIGN(mWidth, 64);
        }
    }

    switch(mTransfer) {
        case ColorTransfer::kColorTransferST2084:
            usage |= ((GRALLOC_NV12_10_HDR_10 << 24) & GRALLOC_COLOR_SPACE_MASK);  // hdr10;
            break;
        case ColorTransfer::kColorTransferHLG:
            usage |= ((GRALLOC_NV12_10_HDR_HLG << 24) & GRALLOC_COLOR_SPACE_MASK);  // hdr-hlg
            break;
    }

    /*
     * For buffer mode, since we don't konw when the last buffer will use
     * up by user, so we use MPP internal buffer group, and copy output to
     * dst block(mOutBlock).
     */
    if (mBufferMode) {
        if (mOutBlock &&
                (mOutBlock->width() != blockW || mOutBlock->height() != blockH)) {
            mOutBlock.reset();
        }
        if (!mOutBlock) {
            ret = pool->fetchGraphicBlock(blockW, blockH, format,
                                          C2AndroidMemoryUsage::FromGrallocUsage(usage),
                                          &mOutBlock);
            if (ret != C2_OK) {
                c2_err("failed to fetchGraphicBlock, err %d", ret);
                return ret;
            }
            c2_trace("required (%dx%d) usage 0x%llx format 0x%x , fetch done",
                     blockW, blockH, usage, format);
        }
    } else {
        std::shared_ptr<C2GraphicBlock> outblock;
        uint32_t count = kMaxReferenceCount - getOutBufferCountOwnByMpi();

        uint32_t i = 0;
        for (i = 0; i < count; i++) {
            ret = pool->fetchGraphicBlock(blockW, blockH, format,
                                          C2AndroidMemoryUsage::FromGrallocUsage(usage),
                                          &outblock);
            if (ret != C2_OK) {
                c2_err("failed to fetchGraphicBlock, err %d", ret);
                break;
            }

            ret = commitBufferToMpp(outblock);
            if (ret != C2_OK) {
                c2_err("register buffer to mpp failed with status %d", ret);
                break;
            }
        }
        c2_trace("required (%dx%d) usage 0x%llx format 0x%x, fetch %d/%d",
                 blockW, blockH, usage, format, i, count);
    }

    return ret;
}

class C2RKMpiDecFactory : public C2ComponentFactory {
public:
    C2RKMpiDecFactory(std::string componentName)
            : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
                  GetCodec2PlatformComponentStore()->getParamReflector())),
              mComponentName(componentName) {
        if (!C2RKMediaUtils::getMimeFromComponentName(componentName, &mMime)) {
            c2_err("failed to get mime from component %s", componentName.c_str());
        }
        if (!C2RKMediaUtils::getDomainFromComponentName(componentName, &mDomain)) {
            c2_err("failed to get domain from component %s", componentName.c_str());
        }
        if (!C2RKMediaUtils::getKindFromComponentName(componentName, &mKind)) {
            c2_err("failed to get kind from component %s", componentName.c_str());
        }
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        c2_trace_f("in");
        *component = std::shared_ptr<C2Component>(
                new C2RKMpiDec(
                        mComponentName.c_str(),
                        id,
                        std::make_shared<C2RKMpiDec::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        c2_trace_f("in");
        *interface = std::shared_ptr<C2ComponentInterface>(
                new C2RKInterface<C2RKMpiDec::IntfImpl>(
                        mComponentName.c_str(),
                        id,
                        std::make_shared<C2RKMpiDec::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual ~C2RKMpiDecFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
    std::string mComponentName;
    std::string mMime;
    C2Component::kind_t mKind;
    C2Component::domain_t mDomain;
};

C2ComponentFactory* CreateRKMpiDecFactory(std::string componentName) {
    c2_trace_f("in");
    return new ::android::C2RKMpiDecFactory(componentName);
}

}  // namespace android
