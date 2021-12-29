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

#include "hardware/hardware_rockchip.h"
#include "hardware/gralloc_rockchip.h"
#include "C2RKMpiDec.h"
#include "C2RKLog.h"
#include "C2RKMediaUtils.h"
#include "C2RKRgaDef.h"
#include "C2RKVersion.h"
#include "C2RKEnv.h"

#define FLAG_NON_DISPLAY_FRAME (1u << 15)

namespace android {

constexpr uint32_t kDefaultOutputDelay = 16;
constexpr uint32_t kMaxOutputDelay = 16;

/* max support video resolution */
constexpr uint32_t kMaxVideoWidth = 8192;
constexpr uint32_t kMaxVideoHeight = 4320;

constexpr uint32_t kMaxReferenceCount = 16;
constexpr size_t kMinInputBufferSize = 2 * 1024 * 1024;

constexpr uint32_t kMaxRetryNum = 20;

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

        addParameter(
                DefineParam(mBlockCount, C2_PARAMKEY_BLOCK_COUNT)
                .withDefault(new C2StreamBlockCountInfo::output(0u, 0u))
                .withFields({C2F(mBlockCount, value).inRange(0, kMaxReferenceCount)})
                .withSetter(Setter<decltype(*mBlockCount)>::StrictValueWithNoDeps)
                .build());

        // max output reference buffer count
        addParameter(
               DefineParam(mMaxRefCount, C2_PARAMKEY_OUTPUT_MAX_REFERENCE_COUNT)
               .withDefault(new C2StreamMaxReferenceCountTuning::output(0, kMaxReferenceCount))
               .withFields({C2F(mMaxRefCount, value).inRange(0, kMaxReferenceCount)})
               .withSetter(Setter<C2StreamMaxReferenceCountTuning::output>::NonStrictValueWithNoDeps)
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

private:
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamBlockSizeInfo::output> mBlockSize;
    std::shared_ptr<C2StreamBlockCountInfo::output> mBlockCount;
    std::shared_ptr<C2StreamMaxReferenceCountTuning::output> mMaxRefCount;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
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
      mWidth(320),
      mHeight(240),
      mLastPts(-1),
      mStarted(false),
      mFlushed(false),
      mOutputEos(false),
      mSignalledOutputEos(false),
      mBufferMode(false) {
    c2_info("version: %s", C2_GIT_BUILD_VERSION);

    if (!C2RKMediaUtils::getCodingTypeFromComponentName(name, &mCodingType)) {
        c2_err("failed to get codingType from component %s", name);
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
    c2_status_t ret = C2_OK;

    c2_info_f("in");

    if (!mFlushed)
        ret = flush();

    return ret;
}

void C2RKMpiDec::onReset() {
    c2_info_f("in");
    onStop();
}

void C2RKMpiDec::onRelease() {
    c2_info_f("in");

    mStarted = false;

    if (mOutBlock) {
        mOutBlock.reset();
    }

    if (!mFlushed) {
        flush();
    }

    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    if (mFrmGrp != nullptr) {
        mpp_buffer_group_put(mFrmGrp);
        mFrmGrp = nullptr;
    }
}

c2_status_t C2RKMpiDec::onFlush_sm() {
    return flush();
}

c2_status_t C2RKMpiDec::flush() {
    c2_status_t ret = C2_OK;

    c2_info_f("in");

    clearMyBuffers();
    mPtsMaps.clear();

    mOutputEos = false;
    mSignalledOutputEos = false;

    C2StreamBlockCountInfo::output blockCount(0u);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ret = mIntf->config({&blockCount}, C2_MAY_BLOCK, &failures);
    if (ret != C2_OK) {
        c2_err_f("failed to config block count, err %d", ret);
        return C2_CORRUPTED;
    }

    mpp_buffer_group_clear(mFrmGrp);
    mMppMpi->reset(mMppCtx);

    mFlushed = true;
    return ret;
}

c2_status_t C2RKMpiDec::initDecoder() {
    c2_status_t ret = C2_OK;
    MPP_RET     err = MPP_OK;

    c2_info_f("in");

    {
        IntfImpl::Lock lock = mIntf->lock();
        mWidth = mIntf->getSize_l()->width;
        mHeight = mIntf->getSize_l()->height;
    }

    c2_info("init: w %d h %d coding %d", mWidth, mHeight, mCodingType);

    err = mpp_create(&mMppCtx, &mMppMpi);
    if (err != MPP_OK) {
        c2_err("failed to mpp_create, ret %d", err);
        goto __FAILED;
    }

    err = mpp_init(mMppCtx, MPP_CTX_DEC, mCodingType);
    if (err != MPP_OK) {
        c2_err("failed to mpp_init, ret %d", err);
        goto __FAILED;
    }

    {
        MppFrame frame = nullptr;
        uint32_t wstride = 0, hstride = 0;

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, mWidth);
        mpp_frame_set_height(frame, mHeight);
        mpp_frame_set_fmt(frame, mColorFormat);
        mMppMpi->control(mMppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);

        /*
         * command "set-frame-info" may failed to provide stride info in old
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

        wstride = mpp_frame_get_hor_stride(frame);
        hstride = mpp_frame_get_ver_stride(frame);

        c2_info("init: get stride [%d:%d]", wstride, hstride);

        /* config output block size */
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        C2StreamBlockSizeInfo::output blockSize(0u, wstride, hstride);
        ret = mIntf->config({&blockSize}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err_f("failed to config output block size, ret %d", ret);
            goto __FAILED;
        }

        /* config output reference buffer count */
        C2StreamMaxReferenceCountTuning::output maxRefCount(0, kMaxReferenceCount);
        ret = mIntf->config({&maxRefCount}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err_f("failed to config max referent count, ret 0x%x", ret);
            goto __FAILED;
        }

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
            goto __FAILED;
        }
        mMppMpi->control(mMppCtx, MPP_DEC_SET_EXT_BUF_GROUP, mFrmGrp);
    }

    mStarted = true;

    return C2_OK;

__FAILED:
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
        const std::shared_ptr<C2GraphicBlock> block) {
    if (!block) {
        c2_err("empty block index %d", index);
        return;
    }

    std::shared_ptr<C2Buffer> buffer
            = createGraphicBuffer(std::move(block), C2Rect(mWidth, mHeight));
    mOutBlock = nullptr;

    {
        if (mCodingType == MPP_VIDEO_CodingAVC ||
            mCodingType == MPP_VIDEO_CodingHEVC ||
            mCodingType == MPP_VIDEO_CodingMPEG2) {
            IntfImpl::Lock lock = mIntf->lock();
            if (buffer)
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
        if(buffer)
            work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };

    if (!mBufferMode) {
        C2StreamBlockCountInfo::output blockCount(0u);
        std::vector<std::unique_ptr<C2Param>> params;
        c2_status_t err = intf()->query_vb({ &blockCount }, {},
                                           C2_DONT_BLOCK, &params);

        blockCount.value--;
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        err = mIntf->config({&blockCount}, C2_MAY_BLOCK, &failures);
        if (err != C2_OK) {
            c2_err_f("failed to config blockCount, ret 0x%x", err);
        } else {
            c2_trace_f("config block count %d", blockCount.value);
        }
    }

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
        finish(index, fillWork);
    }
}

c2_status_t C2RKMpiDec::drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work) {
    c2_status_t ret = C2_OK;
    uint32_t retry = 0;

    c2_info_f("in");

    if (drainMode == NO_DRAIN) {
        c2_warn("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        c2_warn("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    while (!mOutputEos){
        ret = ensureBlockState(pool);
        if (ret != C2_OK) {
            c2_warn_f("failed to ensure block buffer, ret %d", ret);
        }
        ret = decode_getoutframe(work);
        usleep(5 * 1000);

        // avoid infinite loop
        if ((retry++) > kMaxRetryNum)
            mOutputEos = true;

        if (mOutputEos)
            fillEmptyWork(work);
    }

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

    if (mSignalledOutputEos) {
        c2_err("mSignalledOutputEos is true, return!");
        work->result = C2_BAD_VALUE;
        return;
    }

    size_t inSize = 0u;
    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            c2_err("failed to read rWiew, error %d", rView.error());
            work->result = rView.error();
            return;
        }
    }

    c2_trace("in buffer attr. size %zu timestamp %d frameindex %d, flags %x",
             inSize, (int)work->input.ordinal.timestamp.peeku(),
             (int)work->input.ordinal.frameIndex.peeku(), work->input.flags);

    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    if (inSize == 0 && !eos) {
        fillEmptyWork(work);
        return;
    } else if (inSize == 0 && eos) {
        c2_info("get empty input work with eos");
        mSignalledOutputEos = true;
        fillEmptyWork(work);
    }

    uint32_t pkt_done = 0;
    uint32_t retry = 0;
    //bool hasPicture = false;

    err = ensureBlockState(pool);
    if (err != C2_OK) {
        // TODO failed to ensure bloclk state
        c2_warn_f("failed to ensureBlockState, err %d", err);
    }

    do {
        err = decode_sendstream(work);
        if (err != C2_OK) {
            // the work should be try again
            c2_trace("decode_sendstream failed, retry count %d", retry);
            if((retry++) > kMaxRetryNum) pkt_done = 1;
            usleep(5 * 1000);
        } else {
            pkt_done = 1;
        }
    } while (!pkt_done);

    err = decode_getoutframe(work);
    if (err != C2_OK) {
        // nothing to do
        c2_trace_f("decode_getoutframe failed!");
    }

    if (eos && !mOutputEos) {
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
        mSignalledOutputEos = true;
    } else if (eos && mOutputEos) {
        fillEmptyWork(work);
    } else if (retry > kMaxRetryNum) {
        fillEmptyWork(work);
    }

    c2_trace_f("out");
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

c2_status_t C2RKMpiDec::decode_sendstream(const std::unique_ptr<C2Work> &work) {
    c2_status_t ret = C2_OK;
    MPP_RET err = MPP_OK;

    MppPacket packet = nullptr;
    uint8_t *inData = nullptr;
    size_t inSize = 0u;

    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inData = const_cast<uint8_t *>(rView.data());
        inSize = rView.capacity();
    }

    uint64_t frmIndex = work->input.ordinal.frameIndex.peekull();
    uint64_t pts = work->input.ordinal.timestamp.peekll();

    mpp_packet_init(&packet, inData, inSize);
    mpp_packet_set_pts(packet, pts);
    mpp_packet_set_pos(packet, inData);
    mpp_packet_set_length(packet, inSize);

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        c2_info("send input eos");
        mpp_packet_set_eos(packet);
    }

    if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mpp_packet_set_extra_data(packet);
    }

    err = mMppMpi->decode_put_packet(mMppCtx, packet);
    if (MPP_OK != err) {
        ret = C2_NOT_FOUND;
    } else {
        c2_trace("send pkt index %lld pts %lld size %d", frmIndex, pts, inSize);

        // TODO: CtsMediaV2TestCases: return this work when timestamps repeat
        if (!(work->input.flags &
                (C2FrameData::FLAG_CODEC_CONFIG | FLAG_NON_DISPLAY_FRAME))) {
            mPtsMaps[frmIndex] = pts;
            if (mLastPts != pts) {
                mLastPts = pts;
            } else {
                if (mCodingType == MPP_VIDEO_CodingMPEG2) {
                    fillEmptyWork(work);
                }
            }
        }
        if ((work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) != 0) {
            fillEmptyWork(work);
        }
        if (mCodingType == MPP_VIDEO_CodingVP9) {
            if ((work->input.flags & FLAG_NON_DISPLAY_FRAME) != 0) {
                fillEmptyWork(work);
            }
        }
    }

    if (packet) {
        mpp_packet_deinit(&packet);
        packet = nullptr;
    }

    return ret;
}

c2_status_t C2RKMpiDec::decode_getoutframe(const std::unique_ptr<C2Work> &work) {
    c2_status_t ret = C2_OK;
    MPP_RET err = MPP_OK;
    MppFrame frame = nullptr;

    err = mMppMpi->decode_get_frame(mMppCtx, &frame);
    if (MPP_OK != err || !frame) {
        c2_trace("failed to get output frame, ret %d", err);
        return C2_NOT_FOUND;
    }

    uint32_t width   = mpp_frame_get_width(frame);
    uint32_t height  = mpp_frame_get_height(frame);
    uint32_t wstride = mpp_frame_get_hor_stride(frame);
    uint32_t hstride = mpp_frame_get_ver_stride(frame);
    uint32_t errInfo = mpp_frame_get_errinfo(frame);
    int64_t  pts     = mpp_frame_get_pts(frame);
    uint32_t eos     = mpp_frame_get_eos(frame);

    if (mpp_frame_get_info_change(frame)) {
        MppFrameFormat format = mpp_frame_get_fmt(frame);

        c2_info("info-change with old dimens [%d:%d] fmt %d",
                mWidth, mHeight, mColorFormat);
        c2_info("info-change with new dimens [%d:%d] stride [%d:%d] fmt %d",
                width, height, wstride, hstride, format);

        mWidth = width;
        mHeight = height;
        mColorFormat = format;

        clearMyBuffers();

        if (!mBufferMode) {
            C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
            C2StreamBlockSizeInfo::output blockSize(0u, wstride, hstride);
            C2StreamBlockCountInfo::output blockCount(0u);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            ret = mIntf->config({ &size, &blockSize, &blockCount },
                                C2_MAY_BLOCK, &failures);
            if (ret != C2_OK) {
                c2_err_f("failed to config block info, err %d", ret);
                goto _EXIT;
            }

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
            goto _EXIT;
        }

        ret = C2_NO_MEMORY;
    } else {
        c2_trace("get one frm info [%d:%d] stride [%d:%d] pts %lld err %d eos %d",
                 width, height, wstride, hstride, pts, errInfo, eos);

        int64_t workIndex = -1ll;
        std::shared_ptr<C2GraphicBlock> block = nullptr;

        if (eos) {
            c2_info("get output eos.");
            mOutputEos = true;
            goto _EXIT;
        }

        for (auto it = mPtsMaps.begin(); it != mPtsMaps.end(); it++) {
            if (pts == it->second) {
                workIndex = it->first;
                mPtsMaps.erase(it);
                break;
            }
        }
        if (workIndex == -1){
            c2_err("can't find timestamp %lld", pts);
            for (auto it = mPtsMaps.begin(); it != mPtsMaps.end(); it++) {
                if (0 == it->second) {
                    workIndex = it->first;
                    mPtsMaps.erase(it);
                    break;
                }
            }
        }

        if (mBufferMode) {
            RgaParam srcParam, dstParam;
            auto c2Handle = mOutBlock->handle();
            int32_t dstFd = c2Handle->data[0];
            int32_t srcFd = mpp_buffer_get_fd(mpp_frame_get_buffer(frame));

            if (dstFd == -1) {
                c2_err("get empty output block in bufferMode");
                goto _EXIT;
            }

            C2RKRgaDef::paramInit(&srcParam, srcFd, width, height, wstride, hstride);
            C2RKRgaDef::paramInit(&dstParam, dstFd, width, height, wstride, hstride);
            if (!C2RKRgaDef::nv12Copy(srcParam, dstParam)) {
                c2_err("faild to convert nv12");
            }
            block = mOutBlock;
        } else {
            MyC2Buffer *buffer = findMyBuffer(mpp_frame_get_buffer(frame));
            if (!buffer || !buffer->block){
                c2_err("failed to find blockBuffer");
                goto _EXIT;
            }
            buffer->site = MY_BUFFER_SITE_BY_C2;
            mpp_buffer_inc_ref(mpp_frame_get_buffer(frame));
            block = buffer->block;
        }

        if (mCodingType == MPP_VIDEO_CodingAVC ||
            mCodingType == MPP_VIDEO_CodingHEVC ||
            mCodingType == MPP_VIDEO_CodingMPEG2)
        {
            getVuiParams(frame);
        }

        c2_trace("frame index %lld", workIndex);
        finishWork(workIndex, work, block);
    }

_EXIT:
    if (frame) {
        mpp_frame_deinit(&frame);
        frame = nullptr;
    }

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

    MyC2Buffer *buffer = findMyBuffer(bqSlot);
    if (buffer) {
        /* commit this buffer back to mpp */
        MppBuffer mppBuffer = buffer->mBuffer;
        if (mppBuffer) {
            mpp_buffer_put(mppBuffer);
        }
        buffer->block = block;
        buffer->site = MY_BUFFER_SITE_BY_MPI;

        c2_trace("commit this Buffer: fd %d slot %d mBuffer %p outBlock %p",
                 fd, bqSlot, mppBuffer, block.get());
    } else {
        /* register this buffer to mpp group */
        MppBuffer mppBuffer = nullptr;
        MppBufferInfo info;
        memset(&info, 0, sizeof(info));

        info.type = MPP_BUFFER_TYPE_ION;
        info.fd = fd;
        info.ptr = nullptr;
        info.hnd = nullptr;
        info.size = width * height * 2;

        mpp_buffer_import_with_tag(mFrmGrp, &info,
                                   &mppBuffer, "codec2", __FUNCTION__);

        c2_trace("register this Buffer: fd %d slot %d mBuffer %p outBlock %p",
                 fd, bqSlot, mppBuffer, block.get());

        MyC2Buffer *buffer = new MyC2Buffer();
        buffer->index = bqSlot;
        buffer->mBuffer = mppBuffer;
        buffer->block = block;
        buffer->site = MY_BUFFER_SITE_BY_MPI;

        mpp_buffer_put(mppBuffer);
        mC2Buffers.push(buffer);
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::ensureBlockState(
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t ret = C2_OK;

    /* query block info */
    C2StreamMaxReferenceCountTuning::output maxRefCount(0u);
    C2StreamBlockSizeInfo::output blockSize(0u, 0, 0);
    C2StreamBlockCountInfo::output blockCount(0u);
    std::vector<std::unique_ptr<C2Param>> params;
    ret = intf()->query_vb(
            { &maxRefCount, &blockSize, &blockCount },
            {},
            C2_DONT_BLOCK,
            &params);
    if (ret) {
        c2_err_f("failed to query BlockInfo, err %d", ret);
        return ret;
    }

    uint32_t blockW = blockSize.width;
    uint32_t blockH = blockSize.height;

    uint32_t usage = (GRALLOC_USAGE_SW_READ_OFTEN |
                      GRALLOC_USAGE_SW_WRITE_OFTEN |
                      RK_GRALLOC_USAGE_SPECIFY_STRIDE);

    uint32_t format = HAL_PIXEL_FORMAT_YCrCb_NV12;
    if (!C2RKMediaUtils::colorFormatMpiToAndroid(mColorFormat, &format)) {
        c2_err_f("failed to convert color format.");
    }

    if (blockW <= 0 || blockH <= 0) {
        c2_err_f("query get illegal block size, w %d h %d", blockW, blockH);
        return ret;
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

            c2_trace("provided (%dx%d) required (%dx%d)",
                     mOutBlock->width(), mOutBlock->height(), blockW, blockH);
        }
    } else {
        uint32_t count = 0;
        std::shared_ptr<C2GraphicBlock> outblock;

        for (int i = 0; i < maxRefCount.value - blockCount.value; i++) {
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
            count++;
        }

        c2_trace("required (%dx%d), fetch %d/%d", blockW, blockH,
                 count, (maxRefCount.value - blockCount.value));

        blockCount.value += count;
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        ret = mIntf->config({&blockCount}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err_f("failed to config block count, err %d", ret);
            return C2_CORRUPTED;
        }
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
