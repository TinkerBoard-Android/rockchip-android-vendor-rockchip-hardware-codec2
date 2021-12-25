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
#define ROCKCHIP_LOG_TAG    "C2RKMpiEnc"

#include <utils/misc.h>

#include <C2Debug.h>
#include <Codec2Mapper.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <C2RKInterface.h>
#include <util/C2InterfaceHelper.h>
#include <C2AllocatorGralloc.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/GraphicBufferAllocator.h>
#include <gralloc_priv_omx.h>
#include <sys/syscall.h>

#include "hardware/hardware_rockchip.h"
#include "C2RKMpiEnc.h"
#include "C2RKMediaUtils.h"
#include "C2RKRgaDef.h"
#include "mpp/h264_syntax.h"
#include "mpp/h265_syntax.h"
#include "C2RKLog.h"
#include "C2RKEnv.h"
#include "C2RKVideoGlobal.h"
#include "C2RKVersion.h"

#define RK_GRALLOC_USAGE_WITHIN_4G   (1ULL << 56)

namespace android {

namespace {

void ParseGop(
        const C2StreamGopTuning::output &gop,
        uint32_t *syncInterval, uint32_t *iInterval, uint32_t *maxBframes) {
    uint32_t syncInt = 1;
    uint32_t iInt = 1;

    for (size_t i = 0; i < gop.flexCount(); ++i) {
        const C2GopLayerStruct &layer = gop.m.values[i];
        if (layer.count == UINT32_MAX) {
            syncInt = 0;
        } else if (syncInt <= UINT32_MAX / (layer.count + 1)) {
            syncInt *= (layer.count + 1);
        }
        if ((layer.type_ & I_FRAME) == 0) {
            if (layer.count == UINT32_MAX) {
                iInt = 0;
            } else if (iInt <= UINT32_MAX / (layer.count + 1)) {
                iInt *= (layer.count + 1);
            }
        }
        if (layer.type_ == C2Config::picture_type_t(P_FRAME | B_FRAME) && maxBframes) {
            *maxBframes = layer.count;
        }
    }

    if (syncInterval) {
        *syncInterval = syncInt;
    }
    if (iInterval) {
        *iInterval = iInt;
    }
}

} // namepsace

class C2RKMpiEnc::IntfImpl : public C2RKInterface<void>::BaseParams {
public:
    explicit IntfImpl(
            const std::shared_ptr<C2ReflectorHelper> &helper,
            C2String name,
            C2Component::kind_t kind,
            C2Component::domain_t domain,
            C2String mediaType)
        : C2RKInterface<void>::BaseParams(
                helper,
                name,
                kind,
                domain,
                mediaType) {
        noPrivateBuffers(); // TODO: account for our buffers here
        noInputReferences();
        noOutputReferences();
        noTimeStretch();
        setDerivedInstance(this);

        addParameter(
                DefineParam(mUsage, C2_PARAMKEY_INPUT_STREAM_USAGE)
                .withConstValue(new C2StreamUsageTuning::input(
                        0u, (uint64_t)C2MemoryUsage::CPU_READ))
                .build());

        addParameter(
                DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
                .withConstValue(new C2ComponentAttributesSetting(
                    C2Component::ATTRIB_IS_TEMPORAL))
                .build());

        addParameter(
                DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
                .withDefault(new C2StreamPictureSizeInfo::input(0u, 176, 144))
                .withFields({
                    C2F(mSize, width).inRange(2, 7680, 2),
                    C2F(mSize, height).inRange(2, 7680, 2),
                })
                .withSetter(SizeSetter)
                .build());

        addParameter(
                DefineParam(mGop, C2_PARAMKEY_GOP)
                .withDefault(C2StreamGopTuning::output::AllocShared(
                        0 /* flexCount */, 0u /* stream */))
                .withFields({C2F(mGop, m.values[0].type_).any(),
                             C2F(mGop, m.values[0].count).any()})
                .withSetter(GopSetter)
                .build());

        addParameter(
                DefineParam(mPictureQuantization, C2_PARAMKEY_PICTURE_QUANTIZATION)
                .withDefault(C2StreamPictureQuantizationTuning::output::AllocShared(
                        0 /* flexCount */, 0u /* stream */))
                .withFields({C2F(mPictureQuantization, m.values[0].type_).oneOf(
                                {C2Config::picture_type_t(I_FRAME),
                                  C2Config::picture_type_t(P_FRAME),
                                  C2Config::picture_type_t(B_FRAME)}),
                             C2F(mPictureQuantization, m.values[0].min).any(),
                             C2F(mPictureQuantization, m.values[0].max).any()})
                .withSetter(PictureQuantizationSetter)
                .build());

        addParameter(
                DefineParam(mActualInputDelay, C2_PARAMKEY_INPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::input(0))
                .withFields({C2F(mActualInputDelay, value).inRange(0, 2)})
                .calculatedAs(InputDelaySetter, mGop)
                .build());

        addParameter(
                DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
                .withDefault(new C2StreamFrameRateInfo::output(0u, 1.))
                // TODO: More restriction?
                .withFields({C2F(mFrameRate, value).greaterThan(0.)})
                .withSetter(Setter<decltype(*mFrameRate)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrateMode, C2_PARAMKEY_BITRATE_MODE)
                .withDefault(new C2StreamBitrateModeTuning::output(
                        0u, C2Config::BITRATE_VARIABLE))
                .withFields({
                    C2F(mBitrateMode, value).oneOf({
                        C2Config::BITRATE_CONST,
                        C2Config::BITRATE_VARIABLE,
                        C2Config::BITRATE_IGNORE})
                })
                .withSetter(
                    Setter<decltype(*mBitrateMode)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mBitrate, C2_PARAMKEY_BITRATE)
                .withDefault(new C2StreamBitrateInfo::output(0u, 64000))
                .withFields({C2F(mBitrate, value).inRange(4096, 10000000)})
                .withSetter(BitrateSetter)
                .build());

        addParameter(
                DefineParam(mIntraRefresh, C2_PARAMKEY_INTRA_REFRESH)
                .withDefault(new C2StreamIntraRefreshTuning::output(
                        0u, C2Config::INTRA_REFRESH_DISABLED, 0.))
                .withFields({
                    C2F(mIntraRefresh, mode).oneOf({
                        C2Config::INTRA_REFRESH_DISABLED, C2Config::INTRA_REFRESH_ARBITRARY }),
                    C2F(mIntraRefresh, period).any()
                })
                .withSetter(IntraRefreshSetter)
                .build());

        if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC){
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::output(
                            0u, PROFILE_AVC_BASELINE, LEVEL_AVC_3_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                            PROFILE_AVC_BASELINE,
                            PROFILE_AVC_MAIN,
                            PROFILE_AVC_HIGH,
                        }),
                        C2F(mProfileLevel, level).oneOf({
                            LEVEL_AVC_1,
                            LEVEL_AVC_1B,
                            LEVEL_AVC_1_1,
                            LEVEL_AVC_1_2,
                            LEVEL_AVC_1_3,
                            LEVEL_AVC_2,
                            LEVEL_AVC_2_1,
                            LEVEL_AVC_2_2,
                            LEVEL_AVC_3,
                            LEVEL_AVC_3_1,
                            LEVEL_AVC_3_2,
                            LEVEL_AVC_4,
                            LEVEL_AVC_4_1,
                            LEVEL_AVC_4_2,
                            LEVEL_AVC_5,
                            LEVEL_AVC_5_1,
                        }),
                    })
                    .withSetter(AVCProfileLevelSetter, mSize, mFrameRate, mBitrate)
                    .build());
        } else if (mediaType == MEDIA_MIMETYPE_VIDEO_HEVC) {
            addParameter(
                    DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
                    .withDefault(new C2StreamProfileLevelInfo::output(
                            0u, PROFILE_HEVC_MAIN, LEVEL_HEVC_MAIN_4_1))
                    .withFields({
                        C2F(mProfileLevel, profile).oneOf({
                            PROFILE_HEVC_MAIN,
                            PROFILE_HEVC_MAIN_10,
                        }),
                        C2F(mProfileLevel, level).oneOf({
                            LEVEL_HEVC_MAIN_4_1,
                        }),
                    })
                    .withSetter(HEVCProfileLevelSetter, mSize, mFrameRate, mBitrate)
                    .build());
        }

        addParameter(
                DefineParam(mRequestSync, C2_PARAMKEY_REQUEST_SYNC_FRAME)
                .withDefault(new C2StreamRequestSyncFrameTuning::output(0u, C2_FALSE))
                .withFields({C2F(mRequestSync, value).oneOf({ C2_FALSE, C2_TRUE }) })
                .withSetter(Setter<decltype(*mRequestSync)>::NonStrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mSyncFramePeriod, C2_PARAMKEY_SYNC_FRAME_INTERVAL)
                .withDefault(new C2StreamSyncFrameIntervalTuning::output(0u, 1000000))
                .withFields({C2F(mSyncFramePeriod, value).any()})
                .withSetter(Setter<decltype(*mSyncFramePeriod)>::StrictValueWithNoDeps)
                .build());

        addParameter(
                DefineParam(mColorAspects, C2_PARAMKEY_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsInfo::input(
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
                .withSetter(ColorAspectsSetter)
                .build());

        addParameter(
                DefineParam(mCodedColorAspects, C2_PARAMKEY_VUI_COLOR_ASPECTS)
                .withDefault(new C2StreamColorAspectsInfo::output(
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
                .withSetter(CodedColorAspectsSetter, mColorAspects)
                .build());

        addParameter(
            DefineParam(mLayering, C2_PARAMKEY_TEMPORAL_LAYERING)
                .withDefault(C2StreamTemporalLayeringTuning::output::AllocShared(0u, 0, 0, 0))
                .withFields({
                    C2F(mLayering, m.layerCount).inRange(0, 4),
                    C2F(mLayering, m.bLayerCount).inRange(0, 0),
                    C2F(mLayering, m.bitrateRatios).inRange(0., 1.)
                })
                .withSetter(LayeringSetter)
                .build());
    }

    static C2R InputDelaySetter(
            bool mayBlock,
            C2P<C2PortActualDelayTuning::input> &me,
            const C2P<C2StreamGopTuning::output> &gop) {
        (void)mayBlock;
        uint32_t maxBframes = 0;
        ParseGop(gop.v, nullptr, nullptr, &maxBframes);
        me.set().value = maxBframes;
        c2_info("%s %d in", __FUNCTION__, __LINE__);
        return C2R::Ok();
    }

    static C2R BitrateSetter(bool mayBlock, C2P<C2StreamBitrateInfo::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (me.v.value <= 4096) {
            me.set().value = 4096;
        }
        return res;
    }

    static C2R SizeSetter(bool mayBlock, const C2P<C2StreamPictureSizeInfo::input> &oldMe,
                          C2P<C2StreamPictureSizeInfo::input> &me) {
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

    static C2R IntraRefreshSetter(bool mayBlock, C2P<C2StreamIntraRefreshTuning::output> &me) {
        (void)mayBlock;
        C2R res = C2R::Ok();
        if (me.v.period < 1) {
            me.set().mode = C2Config::INTRA_REFRESH_DISABLED;
            me.set().period = 0;
        } else {
            // only support arbitrary mode (cyclic in our case)
            me.set().mode = C2Config::INTRA_REFRESH_ARBITRARY;
        }
        return res;
    }

    static C2R GopSetter(bool mayBlock, C2P<C2StreamGopTuning::output> &me) {
        (void)mayBlock;
        (void)me;
        c2_info_f("in");
        return C2R::Ok();
    }

    static C2R PictureQuantizationSetter(bool mayBlock,
                                         C2P<C2StreamPictureQuantizationTuning::output> &me) {
        (void)mayBlock;
        (void)me;
        c2_info_f("in");
        return C2R::Ok();
    }


    uint32_t getSyncFramePeriod_l() const {
        if (mSyncFramePeriod->value < 0 || mSyncFramePeriod->value == INT64_MAX) {
            return 0;
        }
        double period = mSyncFramePeriod->value / 1e6 * mFrameRate->value;
        return (uint32_t)c2_max(c2_min(period + 0.5, double(UINT32_MAX)), 1.);
    }

    static C2R AVCProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            me.set().profile = PROFILE_AVC_MAIN;
        }

        struct LevelLimits {
            C2Config::level_t level;
            float mbsPerSec;
            uint64_t mbs;
            uint32_t bitrate;
        };
        constexpr LevelLimits kLimits[] = {
            { LEVEL_AVC_1,     1485,    99,     64000 },
            // Decoder does not properly handle level 1b.
            // { LEVEL_AVC_1B,    1485,   99,   128000 },
            { LEVEL_AVC_1_1,   3000,   396,    192000 },
            { LEVEL_AVC_1_2,   6000,   396,    384000 },
            { LEVEL_AVC_1_3,  11880,   396,    768000 },
            { LEVEL_AVC_2,    11880,   396,   2000000 },
            { LEVEL_AVC_2_1,  19800,   792,   4000000 },
            { LEVEL_AVC_2_2,  20250,  1620,   4000000 },
            { LEVEL_AVC_3,    40500,  1620,  10000000 },
            { LEVEL_AVC_3_1, 108000,  3600,  14000000 },
            { LEVEL_AVC_3_2, 216000,  5120,  20000000 },
            { LEVEL_AVC_4,   245760,  8192,  20000000 },
            { LEVEL_AVC_4_1, 245760,  8192,  50000000 },
            { LEVEL_AVC_4_2, 522240,  8704,  50000000 },
            { LEVEL_AVC_5,   589824, 22080, 135000000 },
        };

        uint64_t mbs = uint64_t((size.v.width + 15) / 16) * ((size.v.height + 15) / 16);
        float mbsPerSec = float(mbs) * frameRate.v.value;

        // Check if the supplied level meets the MB / bitrate requirements. If
        // not, update the level with the lowest level meeting the requirements.

        bool found = false;
        // By default needsUpdate = false in case the supplied level does meet
        // the requirements. For Level 1b, we want to update the level anyway,
        // so we set it to true in that case.
        bool needsUpdate = (me.v.level == LEVEL_AVC_1B);
        for (const LevelLimits &limit : kLimits) {
            if (mbs <= limit.mbs && mbsPerSec <= limit.mbsPerSec &&
                    bitrate.v.value <= limit.bitrate) {
                // This is the lowest level that meets the requirements, and if
                // we haven't seen the supplied level yet, that means we don't
                // need the update.
                if (needsUpdate) {
                    c2_info("Given level %x does not cover current configuration: "
                        "adjusting to %x", me.v.level, limit.level);
                    me.set().level = limit.level;
                }
                found = true;
                break;
            }
            if (me.v.level == limit.level) {
                // We break out of the loop when the lowest feasible level is
                // found. The fact that we're here means that our level doesn't
                // meet the requirement and needs to be updated.
                needsUpdate = true;
            }
        }
        if (!found) {
            // We set to the highest supported level.
            me.set().level = LEVEL_AVC_5;
        }

        return C2R::Ok();
    }

    static C2R HEVCProfileLevelSetter(
            bool mayBlock,
            C2P<C2StreamProfileLevelInfo::output> &me,
            const C2P<C2StreamPictureSizeInfo::input> &size,
            const C2P<C2StreamFrameRateInfo::output> &frameRate,
            const C2P<C2StreamBitrateInfo::output> &bitrate) {
        (void)mayBlock;
        if (!me.F(me.v.profile).supportsAtAll(me.v.profile)) {
            me.set().profile = PROFILE_HEVC_MAIN;
        }

        struct LevelLimits {
            C2Config::level_t level;
            uint64_t samplesPerSec;
            uint64_t samples;
            uint32_t bitrate;
        };

        constexpr LevelLimits kLimits[] = {
            { LEVEL_HEVC_MAIN_1,       552960,    36864,    128000 },
            { LEVEL_HEVC_MAIN_2,      3686400,   122880,   1500000 },
            { LEVEL_HEVC_MAIN_2_1,    7372800,   245760,   3000000 },
            { LEVEL_HEVC_MAIN_3,     16588800,   552960,   6000000 },
            { LEVEL_HEVC_MAIN_3_1,   33177600,   983040,  10000000 },
            { LEVEL_HEVC_MAIN_4,     66846720,  2228224,  12000000 },
            { LEVEL_HEVC_MAIN_4_1,  133693440,  2228224,  20000000 },
            { LEVEL_HEVC_MAIN_5,    267386880,  8912896,  25000000 },
            { LEVEL_HEVC_MAIN_5_1,  534773760,  8912896,  40000000 },
            { LEVEL_HEVC_MAIN_5_2, 1069547520,  8912896,  60000000 },
            { LEVEL_HEVC_MAIN_6,   1069547520, 35651584,  60000000 },
            { LEVEL_HEVC_MAIN_6_1, 2139095040, 35651584, 120000000 },
            { LEVEL_HEVC_MAIN_6_2, 4278190080, 35651584, 240000000 },
        };

        uint64_t samples = size.v.width * size.v.height;
        uint64_t samplesPerSec = samples * frameRate.v.value;

        // Check if the supplied level meets the MB / bitrate requirements. If
        // not, update the level with the lowest level meeting the requirements.

        bool found = false;
        // By default needsUpdate = false in case the supplied level does meet
        // the requirements.
        bool needsUpdate = false;
        for (const LevelLimits &limit : kLimits) {
            if (samples <= limit.samples && samplesPerSec <= limit.samplesPerSec &&
                    bitrate.v.value <= limit.bitrate) {
                // This is the lowest level that meets the requirements, and if
                // we haven't seen the supplied level yet, that means we don't
                // need the update.
                if (needsUpdate) {
                    c2_info("Given level %x does not cover current configuration: "
                        "adjusting to %x", me.v.level, limit.level);
                    me.set().level = limit.level;
                }
                found = true;
                break;
            }
            if (me.v.level == limit.level) {
                // We break out of the loop when the lowest feasible level is
                // found. The fact that we're here means that our level doesn't
                // meet the requirement and needs to be updated.
                needsUpdate = true;
            }
        }
        if (!found) {
            // We set to the highest supported level.
            me.set().level = LEVEL_HEVC_MAIN_4_1;
        }
        return C2R::Ok();
    }

    int32_t getProfile_l(MppCodingType type) const {
        switch (mProfileLevel->profile) {
        case PROFILE_AVC_BASELINE: return H264_PROFILE_BASELINE;
        case PROFILE_AVC_MAIN:     return H264_PROFILE_MAIN;
        case PROFILE_AVC_HIGH:     return H264_PROFILE_HIGH;
        case PROFILE_HEVC_MAIN:    return MPP_PROFILE_HEVC_MAIN;
        case PROFILE_HEVC_MAIN_10: return MPP_PROFILE_HEVC_MAIN_10;
        default:
            c2_info("Unrecognized profile: %x", mProfileLevel->profile);
            if (type == MPP_VIDEO_CodingAVC) {
                return H264_PROFILE_MAIN;
            } else if (type == MPP_VIDEO_CodingHEVC) {
                return MPP_PROFILE_HEVC_MAIN;
            } else {
                c2_err_f("unsupport type:%d", type);
                return 0;
            }
        }
    }

    int32_t getLevel_l(MppCodingType type) const {
        struct Level {
            C2Config::level_t c2Level;
            int32_t Level;
        };
        constexpr Level levels[] = {
            //avc level
            { LEVEL_AVC_1,          10 },
            { LEVEL_AVC_1B,          9 },
            { LEVEL_AVC_1_1,        11 },
            { LEVEL_AVC_1_2,        12 },
            { LEVEL_AVC_1_3,        13 },
            { LEVEL_AVC_2,          20 },
            { LEVEL_AVC_2_1,        21 },
            { LEVEL_AVC_2_2,        22 },
            { LEVEL_AVC_3,          30 },
            { LEVEL_AVC_3_1,        31 },
            { LEVEL_AVC_3_2,        32 },
            { LEVEL_AVC_4,          40 },
            { LEVEL_AVC_4_1,        41 },
            { LEVEL_AVC_4_2,        42 },
            { LEVEL_AVC_5,          50 },
            { LEVEL_AVC_5_1,        51 },
            //hevc level
            { LEVEL_HEVC_MAIN_4_1,  123},
        };
        for (const Level &level : levels) {
            if (mProfileLevel->level == level.c2Level) {
                return level.Level;
            }
        }
        c2_info("Unrecognized level: %x", mProfileLevel->level);
        if (type == MPP_VIDEO_CodingAVC) {
            return 41;
        } else if (type == MPP_VIDEO_CodingHEVC) {
            return 123;
        } else {
            c2_err_f("unsupport type:%d", type);
            return 0;
        }
    }

    static C2R ColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::input> &me) {
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

    static C2R CodedColorAspectsSetter(bool mayBlock, C2P<C2StreamColorAspectsInfo::output> &me,
                                       const C2P<C2StreamColorAspectsInfo::input> &coded) {
        (void)mayBlock;
        me.set().range = coded.v.range;
        me.set().primaries = coded.v.primaries;
        me.set().transfer = coded.v.transfer;
        me.set().matrix = coded.v.matrix;
        return C2R::Ok();
    }

    static C2R LayeringSetter(bool mayBlock,
                              C2P<C2StreamTemporalLayeringTuning::output>& me) {
       (void)mayBlock;
       (void)me;
       c2_info_f("in");
       return C2R::Ok();
    }

    // unsafe getters
    std::shared_ptr<C2StreamPictureSizeInfo::input> getSize_l() const
    { return mSize; }
    std::shared_ptr<C2StreamIntraRefreshTuning::output> getIntraRefresh_l() const
    { return mIntraRefresh; }
    std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate_l() const
    { return mFrameRate; }
    std::shared_ptr<C2StreamBitrateModeTuning::output> getBitrateMode_l() const
    { return mBitrateMode; }
    std::shared_ptr<C2StreamBitrateInfo::output> getBitrate_l() const
    { return mBitrate; }
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> getRequestSync_l() const
    { return mRequestSync; }
    std::shared_ptr<C2StreamGopTuning::output> getGop_l() const
    { return mGop; }
    std::shared_ptr<C2StreamPictureQuantizationTuning::output> getPictureQuantization_l() const
    { return mPictureQuantization; }
    std::shared_ptr<C2StreamColorAspectsInfo::output> getCodedColorAspects_l() const
    { return mCodedColorAspects; }
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> getTemporalLayers_l() const
    { return mLayering; }

private:
    std::shared_ptr<C2StreamUsageTuning::input> mUsage;
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamIntraRefreshTuning::output> mIntraRefresh;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamProfileLevelInfo::output> mProfileLevel;
    std::shared_ptr<C2StreamSyncFrameIntervalTuning::output> mSyncFramePeriod;
    std::shared_ptr<C2StreamGopTuning::output> mGop;
    std::shared_ptr<C2StreamPictureQuantizationTuning::output> mPictureQuantization;
    std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
    std::shared_ptr<C2StreamTemporalLayeringTuning::output> mLayering;
};

C2RKMpiEnc::C2RKMpiEnc(
        const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mDmaMem(nullptr),
      mMppCtx(nullptr),
      mMppMpi(nullptr),
      mEncCfg(nullptr),
      mCodingType(MPP_VIDEO_CodingUnused),
      mStarted(false),
      mSpsPpsHeaderReceived(false),
      mSawInputEOS(false),
      mSawOutputEOS(false),
      mSignalledError(false),
      mHorStride(0),
      mVerStride(0),
      mInFile(nullptr),
      mOutFile(nullptr) {
    c2_info("version: %s", C2_GIT_BUILD_VERSION);

    if (!C2RKMediaUtils::getCodingTypeFromComponentName(name, &mCodingType)) {
        c2_err("failed to get MppCodingType from component %s", name);
    }

    Rockchip_C2_GetEnvU32("vendor.c2.venc.debug", &c2_venc_debug, 0);
    c2_info("venc_debug: 0x%x", c2_venc_debug);

    if (c2_venc_debug & VIDEO_DBG_RECORD_IN) {
        char fileName[128];
        memset(fileName, 0, 128);

        sprintf(fileName, "/data/video/enc_in_%ld.bin", syscall(SYS_gettid));
        mInFile = fopen(fileName, "wb");
        if (mInFile == nullptr) {
            c2_err("failed to open input file, err %s", strerror(errno));
        } else {
            c2_info("recording input to %s", fileName);
        }
    }

    if (c2_venc_debug & VIDEO_DBG_RECORD_OUT) {
        char fileName[128];
        memset(fileName, 0, 128);

        sprintf(fileName, "/data/video/enc_out_%ld.bin", syscall(SYS_gettid));
        mOutFile = fopen(fileName, "wb");
        if (mOutFile == nullptr) {
            c2_err("failed to open output file, err %s", strerror(errno));
        } else {
            c2_info("recording output to %s", fileName);
        }
    }
}

C2RKMpiEnc::~C2RKMpiEnc() {
    c2_info_f("in");
    onRelease();
}

c2_status_t C2RKMpiEnc::onInit() {
    c2_info_f("in");
    return C2_OK;
}

c2_status_t C2RKMpiEnc::onStop() {
    c2_info_f("in");
    return C2_OK;
}

void C2RKMpiEnc::onReset() {
    c2_info_f("in");
    releaseEncoder();
}

void C2RKMpiEnc::onRelease() {
    c2_info_f("in");
    releaseEncoder();
}

c2_status_t C2RKMpiEnc::onFlush_sm() {
    c2_info_f("in");
    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupBaseCodec() {
    /* default stride */
    mHorStride = C2_ALIGN(mSize->width, 16);
    mVerStride = C2_ALIGN(mSize->height, 8);

    c2_info("setupBaseCodec: coding %d w %d h %d hor %d ver %d",
            mCodingType, mSize->width, mSize->height, mHorStride, mVerStride);

    mpp_enc_cfg_set_s32(mEncCfg, "codec:type", mCodingType);

    mpp_enc_cfg_set_s32(mEncCfg, "prep:width", mSize->width);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:height", mSize->height);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", mHorStride);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:ver_stride", mVerStride);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(mEncCfg, "prep:rotation", MPP_ENC_ROT_0);

    if (mCodingType == MPP_VIDEO_CodingAVC) {
        mpp_enc_cfg_set_s32(mEncCfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(mEncCfg, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(mEncCfg, "h264:trans8x8", 1);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupFrameRate() {
    float frameRate = 0.0;
    uint32_t idrInterval = 0, gop = 0;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamGopTuning::output> c2Gop = mIntf->getGop_l();
    std::shared_ptr<C2StreamFrameRateInfo::output> c2FrameRate
            = mIntf->getFrameRate_l();

    idrInterval = mIntf->getSyncFramePeriod_l();
    frameRate = c2FrameRate->value;

    if (frameRate == 1) {
        // set default frameRate 30
        frameRate = 30;
    }

    if (c2Gop && c2Gop->flexCount() > 0) {
        uint32_t syncInterval = 30;
        uint32_t iInterval = 0;
        uint32_t maxBframes = 0;

        ParseGop(*c2Gop, &syncInterval, &iInterval, &maxBframes);
        if (syncInterval > 0) {
            c2_info("updating IDR interval: %d -> %d", idrInterval, syncInterval);
            idrInterval = syncInterval;
        }
    }

    c2_info("setupFrameRate: framerate %.2f gop %d", frameRate, idrInterval);

    gop = (idrInterval  < 8640000 &&  idrInterval > 1) ? idrInterval : 30;
    mpp_enc_cfg_set_s32(mEncCfg, "rc:gop", gop);

    /* fix input / output frame rate */
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_num", frameRate);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_num", frameRate);
    mpp_enc_cfg_set_s32(mEncCfg, "rc:fps_out_denorm", 1);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupBitRate() {
    uint32_t bitrate = 0;
    uint32_t bitrateMode = 0;

    IntfImpl::Lock lock = mIntf->lock();

    mBitrate = mIntf->getBitrate_l();
    mBitrateMode = mIntf->getBitrateMode_l();

    bitrate = mBitrate->value;
    bitrateMode = mBitrateMode->value;

    c2_info("setupBitRate: mode %d bitrate %d", bitrateMode, bitrate);

    mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_target", bitrate);
    switch (bitrateMode) {
    case C2Config::BITRATE_CONST_SKIP_ALLOWED:
    case C2Config::BITRATE_CONST: {
        /* CBR mode has narrow bound */
        mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bitrate * 17 / 16);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bitrate * 15 / 16);
    } break;
    case C2Config::BITRATE_IGNORE:[[fallthrough]];
    case C2Config::BITRATE_VARIABLE_SKIP_ALLOWED:
    case C2Config::BITRATE_VARIABLE: {
        /* VBR mode has wide bound */
        mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bitrate * 17 / 16);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bitrate * 1 / 16);
    } break;
    default: {
        /* default use CBR mode */
        mpp_enc_cfg_set_s32(mEncCfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_max", bitrate * 17 / 16);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:bps_min", bitrate * 15 / 16);
    } break;
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupProfileParams() {
    int32_t profile, level;

    IntfImpl::Lock lock = mIntf->lock();

    profile = mIntf->getProfile_l(mCodingType);
    level = mIntf->getLevel_l(mCodingType);

    c2_info("setupProfileParams: profile %d level %d", profile, level);

    switch (mCodingType) {
    case MPP_VIDEO_CodingAVC : {
        mpp_enc_cfg_set_s32(mEncCfg, "h264:profile", profile);
        mpp_enc_cfg_set_s32(mEncCfg, "h264:level", level);
    } break;
    case MPP_VIDEO_CodingHEVC : {
        mpp_enc_cfg_set_s32(mEncCfg, "h265:profile", profile);
        mpp_enc_cfg_set_s32(mEncCfg, "h265:level", level);
    } break;
    default : {
        c2_err("setupProfileParams: unsupport coding type %d", mCodingType);
    } break;
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupQp() {
    int32_t defaultIMin = 0, defaultIMax = 0;
    int32_t defaultPMin = 0, defaultPMax = 0;
    int32_t qpInit = 0;

    if (mCodingType == MPP_VIDEO_CodingVP8) {
        defaultIMin = defaultPMin = 0;
        defaultIMax = defaultPMax = 127;
        qpInit = 40;
    } else {
        /* the quality of h264/265 range from 10~51 */
        defaultIMin = defaultPMin = 10;
        defaultIMax = 51;
        // TODO: CTS testEncoderQualityAVCCBR 49
        defaultPMax = 49;
        qpInit = 26;
    }

    int32_t iMin = defaultIMin, iMax = defaultIMax;
    int32_t pMin = defaultPMin, pMax = defaultPMax;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamPictureQuantizationTuning::output> qp =
            mIntf->getPictureQuantization_l();

    for (size_t i = 0; i < qp->flexCount(); ++i) {
        const C2PictureQuantizationStruct &layer = qp->m.values[i];

        if (layer.type_ == C2Config::picture_type_t(I_FRAME)) {
            iMax = layer.max;
            iMin = layer.min;
            c2_info("PictureQuanlitySetter: iMin %d iMax %d", iMin, iMax);
        } else if (layer.type_ == C2Config::picture_type_t(P_FRAME)) {
            pMax = layer.max;
            pMin = layer.min;
            c2_info("PictureQuanlitySetter: pMin %d pMax %d", pMin, pMax);
        }
    }

    iMax = std::clamp(iMax, defaultIMin, defaultIMax);
    iMin = std::clamp(iMin, defaultIMin, defaultIMax);
    pMax = std::clamp(pMax, defaultPMin, defaultPMax);
    pMin = std::clamp(pMin, defaultPMin, defaultPMax);

    if (qpInit > iMax || qpInit < iMin) {
        qpInit = iMin;
    }

    c2_info("setupQp: qpInit %d i %d-%d p %d-%d", qpInit, iMin, iMax, pMin, pMax);

    switch (mCodingType) {
    case MPP_VIDEO_CodingAVC:
    case MPP_VIDEO_CodingHEVC: {
        /*
         * disable mb_rc for vepu, this cfg does not apply to rkvenc.
         * since the vepu has pool performance, mb_rc will cause mosaic.
         */
        mpp_enc_cfg_set_s32(mEncCfg, "hw:mb_rc_disable", 1);

        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min", pMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max", pMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min_i", iMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max_i", iMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_init", qpInit);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_ip", 2);
    } break;
    case MPP_VIDEO_CodingVP8: {
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min", pMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max", pMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_min_i", iMin);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_max_i", iMax);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_init", qpInit);
        mpp_enc_cfg_set_s32(mEncCfg, "rc:qp_ip", 6);
    } break;
    default: {
        c2_err("setupQp: unsupport coding type %d", mCodingType);
        break;
    }
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupVuiParams() {
    ColorAspects sfAspects;
    int32_t primaries, transfer, matrixCoeffs;
    bool range;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamColorAspectsInfo::output> colorAspects
            = mIntf->getCodedColorAspects_l();

    if (!C2Mapper::map(colorAspects->primaries, &sfAspects.mPrimaries)) {
        sfAspects.mPrimaries = android::ColorAspects::PrimariesUnspecified;
    }
    if (!C2Mapper::map(colorAspects->range, &sfAspects.mRange)) {
        sfAspects.mRange = android::ColorAspects::RangeUnspecified;
    }
    if (!C2Mapper::map(colorAspects->matrix, &sfAspects.mMatrixCoeffs)) {
        sfAspects.mMatrixCoeffs = android::ColorAspects::MatrixUnspecified;
    }
    if (!C2Mapper::map(colorAspects->transfer, &sfAspects.mTransfer)) {
        sfAspects.mTransfer = android::ColorAspects::TransferUnspecified;
    }

    ColorUtils::convertCodecColorAspectsToIsoAspects(
            sfAspects, &primaries, &transfer,
            &matrixCoeffs, &range);

    if (mEncCfg != NULL) {
        mpp_enc_cfg_set_s32(mEncCfg, "prep:range", range ? 2 : 0);
        mpp_enc_cfg_set_s32(mEncCfg, "prep:colorprim", primaries);
        mpp_enc_cfg_set_s32(mEncCfg, "prep:colortrc", transfer);
        mpp_enc_cfg_set_s32(mEncCfg, "prep:colorspace", matrixCoeffs);
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::setupTemporalLayers() {
    size_t temporalLayers = 0;

    IntfImpl::Lock lock = mIntf->lock();

    std::shared_ptr<C2StreamTemporalLayeringTuning::output> layering =
            mIntf->getTemporalLayers_l();

    temporalLayers = layering->m.layerCount;
    if (temporalLayers == 0) {
        return C2_OK;
    }

    if (temporalLayers < 2 || temporalLayers > 4) {
        c2_warn("only support tsvc layer 2 ~ 4(%zu); ignored.", temporalLayers);
        return C2_OK;
    }

    /*
     * NOTE:
     * 1. not support set bLayerCount and bitrateRatios yet.
     *    - layering->m.bLayerCount
     *    - layering->m.bitrateRatios
     * 2. only support tsvc layer 2 ~ 4.
     */

    int ret = 0;
    MppEncRefCfg ref;
    MppEncRefLtFrmCfg ltRef[4];
    MppEncRefStFrmCfg stRef[16];
    RK_S32 ltCnt = 0;
    RK_S32 stCnt = 0;

    memset(&ltRef, 0, sizeof(ltRef));
    memset(&stRef, 0, sizeof(stRef));

    mpp_enc_ref_cfg_init(&ref);

    c2_info("setupTemporalLayers: layers %zu", temporalLayers);

    switch (temporalLayers) {
    case 4: {
        // tsvc4
        //      /-> P1      /-> P3        /-> P5      /-> P7
        //     /           /             /           /
        //    //--------> P2            //--------> P6
        //   //                        //
        //  ///---------------------> P4
        // ///
        // P0 ------------------------------------------------> P8
        ltCnt = 1;

        /* set 8 frame lt-ref gap */
        ltRef[0].lt_idx        = 0;
        ltRef[0].temporal_id   = 0;
        ltRef[0].ref_mode      = REF_TO_PREV_LT_REF;
        ltRef[0].lt_gap        = 8;
        ltRef[0].lt_delay      = 0;

        stCnt = 9;
        /* set tsvc4 st-ref struct */
        /* st 0 layer 0 - ref */
        stRef[0].is_non_ref    = 0;
        stRef[0].temporal_id   = 0;
        stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[0].ref_arg       = 0;
        stRef[0].repeat        = 0;
        /* st 1 layer 3 - non-ref */
        stRef[1].is_non_ref    = 1;
        stRef[1].temporal_id   = 3;
        stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[1].ref_arg       = 0;
        stRef[1].repeat        = 0;
        /* st 2 layer 2 - ref */
        stRef[2].is_non_ref    = 0;
        stRef[2].temporal_id   = 2;
        stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[2].ref_arg       = 0;
        stRef[2].repeat        = 0;
        /* st 3 layer 3 - non-ref */
        stRef[3].is_non_ref    = 1;
        stRef[3].temporal_id   = 3;
        stRef[3].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[3].ref_arg       = 0;
        stRef[3].repeat        = 0;
        /* st 4 layer 1 - ref */
        stRef[4].is_non_ref    = 0;
        stRef[4].temporal_id   = 1;
        stRef[4].ref_mode      = REF_TO_PREV_LT_REF;
        stRef[4].ref_arg       = 0;
        stRef[4].repeat        = 0;
        /* st 5 layer 3 - non-ref */
        stRef[5].is_non_ref    = 1;
        stRef[5].temporal_id   = 3;
        stRef[5].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[5].ref_arg       = 0;
        stRef[5].repeat        = 0;
        /* st 6 layer 2 - ref */
        stRef[6].is_non_ref    = 0;
        stRef[6].temporal_id   = 2;
        stRef[6].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[6].ref_arg       = 0;
        stRef[6].repeat        = 0;
        /* st 7 layer 3 - non-ref */
        stRef[7].is_non_ref    = 1;
        stRef[7].temporal_id   = 3;
        stRef[7].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[7].ref_arg       = 0;
        stRef[7].repeat        = 0;
        /* st 8 layer 0 - ref */
        stRef[8].is_non_ref    = 0;
        stRef[8].temporal_id   = 0;
        stRef[8].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[8].ref_arg       = 0;
        stRef[8].repeat        = 0;
    } break;
    case 3: {
        // tsvc3
        //     /-> P1      /-> P3
        //    /           /
        //   //--------> P2
        //  //
        // P0/---------------------> P4
        ltCnt = 0;

        stCnt = 5;
        /* set tsvc4 st-ref struct */
        /* st 0 layer 0 - ref */
        stRef[0].is_non_ref    = 0;
        stRef[0].temporal_id   = 0;
        stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[0].ref_arg       = 0;
        stRef[0].repeat        = 0;
        /* st 1 layer 2 - non-ref */
        stRef[1].is_non_ref    = 1;
        stRef[1].temporal_id   = 2;
        stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[1].ref_arg       = 0;
        stRef[1].repeat        = 0;
        /* st 2 layer 1 - ref */
        stRef[2].is_non_ref    = 0;
        stRef[2].temporal_id   = 1;
        stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[2].ref_arg       = 0;
        stRef[2].repeat        = 0;
        /* st 3 layer 2 - non-ref */
        stRef[3].is_non_ref    = 1;
        stRef[3].temporal_id   = 2;
        stRef[3].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[3].ref_arg       = 0;
        stRef[3].repeat        = 0;
        /* st 4 layer 0 - ref */
        stRef[4].is_non_ref    = 0;
        stRef[4].temporal_id   = 0;
        stRef[4].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[4].ref_arg       = 0;
        stRef[4].repeat        = 0;
    } break;
    case 2: {
        // tsvc2
        //   /-> P1
        //  /
        // P0--------> P2
        ltCnt = 0;

        stCnt = 3;
        /* set tsvc4 st-ref struct */
        /* st 0 layer 0 - ref */
        stRef[0].is_non_ref    = 0;
        stRef[0].temporal_id   = 0;
        stRef[0].ref_mode      = REF_TO_TEMPORAL_LAYER;
        stRef[0].ref_arg       = 0;
        stRef[0].repeat        = 0;
        /* st 1 layer 2 - non-ref */
        stRef[1].is_non_ref    = 1;
        stRef[1].temporal_id   = 1;
        stRef[1].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[1].ref_arg       = 0;
        stRef[1].repeat        = 0;
        /* st 2 layer 1 - ref */
        stRef[2].is_non_ref    = 0;
        stRef[2].temporal_id   = 0;
        stRef[2].ref_mode      = REF_TO_PREV_REF_FRM;
        stRef[2].ref_arg       = 0;
        stRef[2].repeat        = 0;
    } break;
    default : {
    } break;
    }

    if (ltCnt || stCnt) {
        mpp_enc_ref_cfg_set_cfg_cnt(ref, ltCnt, stCnt);

        if (ltCnt)
            mpp_enc_ref_cfg_add_lt_cfg(ref, ltCnt, ltRef);

        if (stCnt)
            mpp_enc_ref_cfg_add_st_cfg(ref, stCnt, stRef);

        /* check and get dpb size */
        mpp_enc_ref_cfg_check(ref);
    }

    ret = mMppMpi->control(mMppCtx, MPP_ENC_SET_REF_CFG, ref);
    if (ret) {
        c2_err("setupTemporalLayers: failed to set ref cfg ret %d", ret);
        return C2_CORRUPTED;
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::initEncParams() {
    c2_status_t ret = C2_OK;
    int err = 0;

    err = mpp_enc_cfg_init(&mEncCfg);
    if (err) {
        ret = C2_CORRUPTED;
        c2_err("failed to get enc_cfg, ret %d", err);
        goto __RETURN;
    }

    /* Video control Set Base Codec */
    setupBaseCodec();

    /* Video control Set FrameRates and gop */
    setupFrameRate();

    /* Video control Set Bitrate */
    setupBitRate();

    /* Video control Set Profile params */
    setupProfileParams();

    /* Video control Set QP */
    setupQp();

    /* Video control Set VUI params */
    setupVuiParams();

    /* Video control Set Temporal Layers */
    setupTemporalLayers();

    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
    if (err) {
        c2_err("failed to setup codec cfg, ret %d", err);
        ret = C2_CORRUPTED;
        goto __RETURN;
    } else {
        /* optional */
        MppEncSeiMode seiMode = MPP_ENC_SEI_MODE_ONE_FRAME;
        err = mMppMpi->control(mMppCtx, MPP_ENC_SET_SEI_CFG, &seiMode);
        if (err) {
            c2_err("failed to setup sei cfg, ret %d", err);
            ret = C2_CORRUPTED;
            goto __RETURN;
        }
    }

__RETURN:
    return ret;
}

c2_status_t C2RKMpiEnc::initEncoder() {
    c2_status_t ret = C2_OK;
    int err = 0;
    MppPollType timeout = MPP_POLL_BLOCK;

    c2_info_f("in");

    {
        IntfImpl::Lock lock = mIntf->lock();
        mSize = mIntf->getSize_l();
        mBitrateMode = mIntf->getBitrateMode_l();
        mBitrate = mIntf->getBitrate_l();
        mRequestSync = mIntf->getRequestSync_l();
    }

    /*
     * create vpumem for mpp input
     *
     * NOTE: We need temporary buffer to store rga nv12 output for some rgba input,
     * since mpp can't process rgba input properly. in addition to this, alloc buffer
     * within 4G in view of rga efficiency.
     */
    buffer_handle_t bufferHandle;
    gralloc_private_handle_t privHandle;
    uint32_t stride = 0;

    uint64_t usage = (GRALLOC_USAGE_SW_READ_OFTEN
                      | GRALLOC_USAGE_SW_WRITE_OFTEN
                      | RK_GRALLOC_USAGE_WITHIN_4G);

    status_t status = GraphicBufferAllocator::get().allocate(
            C2_ALIGN(mSize->width, 16), C2_ALIGN(mSize->height, 16),
            0x15 /* NV12 */, 1u /* layer count */,
            usage, &bufferHandle, &stride, "C2RKMpiEnc");
    if (status) {
        c2_err("failed transaction: allocate");
        goto __FAILED;
    }

    Rockchip_get_gralloc_private((uint32_t *)bufferHandle, &privHandle);

    mDmaMem = (MyDmaBuffer_t *)malloc(sizeof(MyDmaBuffer_t));
    mDmaMem->fd = privHandle.share_fd;
    mDmaMem->size = privHandle.size;
    mDmaMem->handler = (void *)bufferHandle;

    c2_info("alloc temporary DmaMem fd %d size %d", mDmaMem->fd, mDmaMem->size);

    // create mpp and init mpp
    err = mpp_create(&mMppCtx, &mMppMpi);
    if (err) {
        c2_err("failed to mpp_create, ret %d", err);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    err = mMppMpi->control(mMppCtx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (MPP_OK != err) {
        c2_err("failed to set output timeout %d, ret %d", timeout, err);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    err = mpp_init(mMppCtx, MPP_CTX_ENC, mCodingType);
    if (err) {
        c2_err("failed to mpp_init, ret %d", err);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    ret = initEncParams();
    if (ret) {
        c2_err("failed to init encoder params, ret=0x%x", ret);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    mStarted = true;

    return C2_OK;

__FAILED:
    releaseEncoder();

    return ret;
}

c2_status_t C2RKMpiEnc::releaseEncoder() {
    mStarted = false;
    mSpsPpsHeaderReceived = false;
    mSawInputEOS = false;
    mSawOutputEOS = false;
    mSignalledError = false;

    if (mEncCfg) {
        mpp_enc_cfg_deinit(mEncCfg);
        mEncCfg = nullptr;
    }

    if (mMppCtx){
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    if (mDmaMem != nullptr) {
        GraphicBufferAllocator::get().free((buffer_handle_t)mDmaMem->handler);
        free(mDmaMem);
        mDmaMem = nullptr;
    }

    if (mInFile != nullptr) {
        fclose(mInFile);
        mInFile = nullptr;
    }

    if (mOutFile != nullptr) {
        fclose(mOutFile);
        mOutFile = nullptr;
    }

    return C2_OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work>& work) {
    uint32_t flags = 0;

    c2_trace_f("in");

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        flags |= C2FrameData::FLAG_END_OF_STREAM;
        c2_info("Signalling EOS");
    }
    work->worklets.front()->output.flags = (C2FrameData::flags_t)flags;
    work->worklets.front()->output.buffers.clear();
    work->worklets.front()->output.ordinal = work->input.ordinal;
    work->workletsProcessed = 1u;
}

void C2RKMpiEnc::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    FunctionIn();

    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    c2_trace("process one work timestamp %llu frameindex %llu, flags %x",
             work->input.ordinal.timestamp.peeku(),
             work->input.ordinal.frameIndex.peeku(),
             work->input.flags);

    if (mSignalledError) {
        work->result = C2_BAD_VALUE;
        c2_info("Signalled Error");
        return;
    }

    // Initialize encoder if not already initialized
    if (!mStarted) {
        c2_status_t status = initEncoder();
        if (C2_OK != status) {
            c2_err("failed to initialize encoder: 0x%x", status);
            mSignalledError = true;
            work->result = status;
            work->workletsProcessed = 1u;
            return;
        }
    }

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        mSawInputEOS = true;
    } else {
        mSawInputEOS = false;
    }

    std::shared_ptr<const C2GraphicView> view;
    std::shared_ptr<C2Buffer> inputBuffer = nullptr;

    if (!work->input.buffers.empty()) {
        inputBuffer = work->input.buffers[0];
        view = std::make_shared<const C2GraphicView>(
                inputBuffer->data().graphicBlocks().front().map().get());
        if (view->error() != C2_OK) {
            c2_err("graphic view map err = %d", view->error());
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            work->workletsProcessed = 1u;
            return;
        }
        const C2GraphicView *const input = view.get();
        if ((input != nullptr) && (input->width() < mSize->width ||
            input->height() < mSize->height)) {
            /* Expect width height to be configured */
            c2_err("unexpected Capacity Aspect %d(%d) x %d(%d)",
                   input->width(), mSize->width, input->height(), mSize->height);
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            work->workletsProcessed = 1u;
            return;
        }
    }

    if (!mSpsPpsHeaderReceived) {
        MppPacket hdrPkt = nullptr;
        void *hdrBuf = nullptr;
        void *extradata = nullptr;
        int hdrBufSize = 0;
        int extradataSize = 0;

        hdrBufSize = 1024;
        hdrBuf = malloc(hdrBufSize * sizeof(uint8_t));
        if (hdrBuf)
            mpp_packet_init(&hdrPkt, hdrBuf, hdrBufSize);

        if (hdrPkt) {
            mMppMpi->control(mMppCtx, MPP_ENC_GET_HDR_SYNC, hdrPkt);
            extradataSize = mpp_packet_get_length(hdrPkt);
            extradata = mpp_packet_get_data(hdrPkt);
        }

        std::unique_ptr<C2StreamInitDataInfo::output> csd =
            C2StreamInitDataInfo::output::AllocUnique(extradataSize, 0u);
        if (!csd) {
            c2_err("CSD allocation failed");
            work->result = C2_NO_MEMORY;
            work->workletsProcessed = 1u;
            C2_SAFE_FREE(hdrBuf);
            return;
        }

        memcpy(csd->m.value, extradata, extradataSize);
        work->worklets.front()->output.configUpdate.push_back(std::move(csd));

        if (mOutFile != nullptr) {
            fwrite(extradata, 1, extradataSize , mOutFile);
            fflush(mOutFile);
        }

        mSpsPpsHeaderReceived = true;

        if (hdrPkt){
            mpp_packet_deinit(&hdrPkt);
            hdrPkt = NULL;
        }
        C2_SAFE_FREE(hdrBuf);

        if (work->input.buffers.empty()) {
            work->workletsProcessed = 1u;
            return;
        }
    }

    {
        // handle dynamic config parameters
        // TODO set bitrate dynamically
        IntfImpl::Lock lock = mIntf->lock();
        std::shared_ptr<C2StreamBitrateInfo::output> bitrate = mIntf->getBitrate_l();
        lock.unlock();
        if (bitrate != mBitrate) {
            c2_info("new bitrate requeset, value %d", bitrate->value);
            mBitrate = bitrate;
        }
    }

    c2_status_t err = C2_OK;

    /* send frame to mpp */
    err = encoder_sendframe(work);
    if (C2_OK != err) {
        c2_err("failed to sendframe, err %d", err);
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        mSignalledError = true;
        return;
    }

    /* get packet from mpp */
    err = encoder_getstream(work, pool);
    if (C2_OK != err) {
        c2_err("getstream failed or eos!");
        if (work && work->workletsProcessed != 1u) {
            fillEmptyWork(work);
        }
    }

    if (!mSawInputEOS && work->input.buffers.empty()) {
        fillEmptyWork(work);
    }

    if (mSawInputEOS && mSawOutputEOS != 1) {
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
    }

    FunctionOut();
}

c2_status_t C2RKMpiEnc::encoder_sendframe(const std::unique_ptr<C2Work> &work){
    int err = 0;
    c2_status_t ret = C2_OK;
    MppFrame frame = nullptr;

    c2_trace("%s in", __func__);

    uint64_t workIndex = work->input.ordinal.frameIndex.peekull();

    MppBufferInfo inputCommit;
    memset(&inputCommit, 0, sizeof(inputCommit));
    inputCommit.type = MPP_BUFFER_TYPE_ION;

    mpp_frame_init(&frame);

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        mpp_frame_set_eos(frame, 1);
    }

    // handle request key frame
    {
        IntfImpl::Lock lock = mIntf->lock();
        std::shared_ptr<C2StreamRequestSyncFrameTuning::output> requestSync;
        requestSync = mIntf->getRequestSync_l();
        lock.unlock();

        if (requestSync != mRequestSync) {
            // we can handle IDR immediately
            if (requestSync->value) {
                c2_trace("got sync request");
                // unset request
                C2StreamRequestSyncFrameTuning::output clearSync(0u, C2_FALSE);
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                mIntf->config({ &clearSync }, C2_MAY_BLOCK, &failures);
                // force set IDR frame
                mMppMpi->control(mMppCtx, MPP_ENC_SET_IDR_FRAME, nullptr);
            }
            mRequestSync = requestSync;
        }
    }

    std::shared_ptr<const C2GraphicView> view;
    std::shared_ptr<C2Buffer> inputBuffer = nullptr;

    if (!work->input.buffers.empty()) {
        inputBuffer = work->input.buffers[0];
        view = std::make_shared<const C2GraphicView>(
                inputBuffer->data().graphicBlocks().front().map().get());
        const C2Handle *c2Handle = inputBuffer->data().graphicBlocks().front().handle();

        uint32_t bqSlot, width, height, format, stride, generation;
        uint64_t usage, bqId;

        android::_UnwrapNativeCodec2GrallocMetadata(
                c2Handle, &width, &height, &format, &usage,
                &stride, &generation, &bqId, &bqSlot);

        // Fix error for wifidisplay when stride is 0
        if (stride == 0) {
            std::vector<ui::PlaneLayout> layouts;
            buffer_handle_t bufferHandle;
            native_handle_t *grallocHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);

            GraphicBufferMapper &gm(GraphicBufferMapper::get());
            gm.importBuffer(const_cast<native_handle_t *>(grallocHandle),
                            width, height, 1, format, usage,
                            stride, &bufferHandle);
            gm.getPlaneLayouts(const_cast<native_handle_t *>(bufferHandle), &layouts);
            if (layouts[0].sampleIncrementInBits != 0) {
                stride = layouts[0].strideInBytes * 8 / layouts[0].sampleIncrementInBits;
            } else {
                c2_err("layouts[0].sampleIncrementInBits = 0");
                stride = mHorStride;
            }
            gm.freeBuffer(bufferHandle);
            native_handle_delete(grallocHandle);
        }

        uint32_t inFd = c2Handle->data[0];
        /* src buffer dimensions */
        uint32_t inWidth = mSize->width;
        uint32_t inHeight = mSize->height;
        uint32_t inWstride = stride;
        uint32_t inHstride = height;
        /* dst buffer stride */
        uint32_t outWstride = mHorStride;
        uint32_t outHstride = mVerStride;

        const C2GraphicView* const input = view.get();
        const C2PlanarLayout& layout = input->layout();

        c2_trace("send input frame w %d h %d hor %d ver %d type %d",
                 inWidth, inHeight, inWstride, inHstride, layout.type);

        switch (layout.type) {
        case C2PlanarLayout::TYPE_RGB:
            [[fallthrough]];
        case C2PlanarLayout::TYPE_RGBA: {
            RgaParam srcParam, dstParam;
            c2_trace("%s %d input rgb", __func__, __LINE__);

            if (mInFile != nullptr) {
                fwrite(input->data()[0], 1, inWidth * inHeight * 4, mInFile);
                fflush(mInFile);
            }

            C2RKRgaDef::paramInit(&srcParam, inFd,
                                  inWidth, inHeight, inWstride, inHstride);
            C2RKRgaDef::paramInit(&dstParam, mDmaMem->fd,
                                  inWidth, inHeight, outWstride, outHstride);

            if (!C2RKRgaDef::rgbToNv12(srcParam, dstParam)) {
                c2_err("failed to convert rgbToNv12");
            }

            inputCommit.size = outWstride * outHstride * 3 / 2;
            inputCommit.fd = mDmaMem->fd;
            break;
        }
        case C2PlanarLayout::TYPE_YUV:
            c2_trace("%s %d input yuv", __func__, __LINE__);

            if (!IsYUV420(*input)) {
                c2_err("input is not YUV420");
                return C2_BAD_VALUE;
            }

            if (mInFile != nullptr) {
                fwrite(input->data()[0], 1, inWstride * inHstride * 3 / 2, mInFile);
                fflush(mInFile);
            }

            /*
             * mpp-driver fetch buffer 16 bits at one time, so the stride of
             * input buffer shoule be aligned to 16.
             * For this reason if the stride of buffer not aligned to 16, we
             * copy input buffer to anothor larger dmaBuffer, and than import
             * this dmaBuffer to encoder.
             */
            if ((inWstride & 0xf) || (inHstride & 0xf)) {
                RgaParam srcParam, dstParam;

                C2RKRgaDef::paramInit(&srcParam, inFd, inWidth, inHeight,
                                      inWstride, inHstride);
                C2RKRgaDef::paramInit(&dstParam, mDmaMem->fd,
                                      inWidth, inHeight, outWstride, outHstride);

                if (!C2RKRgaDef::nv12Copy(srcParam, dstParam)) {
                    c2_err("faild to convert nv12");
                }
                inputCommit.fd = mDmaMem->fd;
                inputCommit.size = outWstride * outHstride * 3 / 2;
            } else {
                if (mHorStride != inWstride || mVerStride != inHstride) {
                    // setup encoder using new stride config
                    mpp_enc_cfg_set_s32(mEncCfg, "prep:hor_stride", inWstride);
                    mpp_enc_cfg_set_s32(mEncCfg, "prep:ver_stride", inHstride);

                    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, mEncCfg);
                    if (!err) {
                        c2_info("cfg stride change from [%d:%d] -> [%d %d]",
                                mHorStride, mVerStride, stride, height);
                        outWstride = mHorStride = inWstride;
                        outHstride = mVerStride = inHstride;
                    } else {
                        c2_err("failed to setup new mpp config.");
                    }
                }
                inputCommit.fd = inFd;
                inputCommit.size = outWstride * outHstride * 3 / 2;
            }
            break;
        case C2PlanarLayout::TYPE_YUVA:
            c2_err("YUVA plane type is not supported");
            ret = C2_BAD_VALUE;
            goto __FAILED;
        default:
            c2_err("Unrecognized plane type: %d", layout.type);
            ret = C2_BAD_VALUE;
            goto __FAILED;
        }

        MppBuffer buffer = NULL;
        ret = (c2_status_t)mpp_buffer_import(&buffer, &inputCommit);
        if (ret) {
            c2_err("failed to import input buffer");
            ret = C2_NOT_FOUND;
            goto __FAILED;
        }
        mpp_frame_set_buffer(frame, buffer);
        mpp_buffer_put(buffer);
        buffer = NULL;
    } else {
        mpp_frame_set_buffer(frame, NULL);
    }

    mpp_frame_set_width(frame, mSize->width);
    mpp_frame_set_height(frame, mSize->height);
    mpp_frame_set_hor_stride(frame, mHorStride);
    mpp_frame_set_ver_stride(frame, mVerStride);
    mpp_frame_set_pts(frame, workIndex);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);

    err = mMppMpi->encode_put_frame(mMppCtx, frame);
    if (err) {
        c2_err("failed to put_frame, ret %d", ret);
        ret = C2_NOT_FOUND;
        goto __FAILED;
    }

    ret = C2_OK;

__FAILED:
    if (frame) {
        mpp_frame_deinit(&frame);
    }

    return ret;
}

c2_status_t C2RKMpiEnc::encoder_getstream(const std::unique_ptr<C2Work> &work,
                                          const std::shared_ptr<C2BlockPool>& pool) {
    int err = 0;
    c2_status_t ret = C2_OK;
    MppPacket packet = NULL;

    c2_trace("%s in", __func__);

    err = mMppMpi->encode_get_packet(mMppCtx, &packet);
    if (err) {
        c2_err("failed to get packet");
        ret = C2_CORRUPTED;
    } else {
        mSawOutputEOS = mpp_packet_get_eos(packet);
        uint64_t workId = (uint64_t)mpp_packet_get_pts(packet);
        uint8_t *src = (uint8_t *)mpp_packet_get_data(packet);
        size_t len  = mpp_packet_get_length(packet);

        if (mSawOutputEOS == 1 && workId == 0) {
            c2_err("eos with empty pkt");
            ret = C2_CORRUPTED;
        }
        if (!src || (len == 0)) {
            c2_err("empty output or len = 0");
            ret = C2_CORRUPTED;
        }

        if (C2_OK == ret) {
            finishWork(workId, work, pool, packet);
        }
    }

    return ret;
}

void C2RKMpiEnc::finishWork(uint64_t workIndex,
                            const std::unique_ptr<C2Work> &work,
                            const std::shared_ptr<C2BlockPool>& pool,
                            MppPacket packet) {
    c2_status_t status;
    std::shared_ptr<C2LinearBlock> block;
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };

    uint8_t *src = (uint8_t *)mpp_packet_get_data(packet);
    size_t length = mpp_packet_get_length(packet);

    status = pool->fetchLinearBlock(length, usage, &block);
    if (C2_OK != status) {
        c2_err("failed to fetchLinearBlock for output, status 0x%x", status);
        work->result = status;
        work->workletsProcessed = 1u;
        mSignalledError = true;
        return;
    }

    C2WriteView wView = block->map().get();
    if (C2_OK != wView.error()) {
        c2_err("write view map failed with status 0x%x", wView.error());
        work->result = wView.error();
        work->workletsProcessed = 1u;
        mSignalledError = true;
        return;
    }

    // copy mpp output to c2 output
    memcpy(wView.data(), src, length);
    c2_trace("encoded frame size %zu", length);

    if (mOutFile != nullptr) {
        fwrite(src, 1, length, mOutFile);
        fflush(mOutFile);
    }

    RK_S32 isIntra = 0;
    std::shared_ptr<C2Buffer> buffer = createLinearBuffer(block, 0, length);
    MppMeta meta = mpp_packet_get_meta(packet);
    mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &isIntra);
    if (isIntra) {
        c2_info("IDR frame produced");
        buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
                0u /* stream id */, C2Config::SYNC_FRAME));
    }
    mpp_packet_deinit(&packet);

    auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };

    if (work && c2_cntr64_t(workIndex) == work->input.ordinal.frameIndex) {
        fillWork(work);
        if (mSawInputEOS) {
            work->worklets.front()->output.flags = C2FrameData::FLAG_END_OF_STREAM;
        }
    } else {
        finish(workIndex, fillWork);
    }
}

c2_status_t C2RKMpiEnc::drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work) {
    c2_trace_f("in");

    if (drainMode == NO_DRAIN) {
        c2_warn("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        c2_warn("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    c2_status_t ret = C2_OK;
    while (true) {
        ret = encoder_getstream(work, pool);
        if (ret != C2_OK) {
            c2_err("failed to getstream or eos!");
            if (work && work->workletsProcessed != 1u) fillEmptyWork(work);
            break;
        }
    }

    return C2_OK;
}

c2_status_t C2RKMpiEnc::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    return drainInternal(drainMode, pool, nullptr);
}

class C2RKMpiEncFactory : public C2ComponentFactory {
public:
    C2RKMpiEncFactory(std::string componentName)
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
                new C2RKMpiEnc(
                        mComponentName.c_str(),
                        id,
                        std::make_shared<C2RKMpiEnc::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual c2_status_t createInterface(
            c2_node_id_t id,
            std::shared_ptr<C2ComponentInterface>* const interface,
            std::function<void(C2ComponentInterface*)> deleter) override {
        c2_trace("%s %d in", __FUNCTION__, __LINE__);
        *interface = std::shared_ptr<C2ComponentInterface>(
                new C2RKInterface<C2RKMpiEnc::IntfImpl>(
                        mComponentName.c_str(),
                        id,
                        std::make_shared<C2RKMpiEnc::IntfImpl>
                            (mHelper, mComponentName, mKind, mDomain, mMime)),
                        deleter);
        return C2_OK;
    }

    virtual ~C2RKMpiEncFactory() override = default;

private:
    std::shared_ptr<C2ReflectorHelper> mHelper;
    std::string mComponentName;
    std::string mMime;
    C2Component::kind_t mKind;
    C2Component::domain_t mDomain;
};

C2ComponentFactory* CreateRKMpiEncFactory(std::string componentName) {
    c2_trace_f("in");
    return new ::android::C2RKMpiEncFactory(componentName);
}

} // namespace android

