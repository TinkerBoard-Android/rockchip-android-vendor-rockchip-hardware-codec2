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

#include <media/hardware/VideoAPI.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/foundation/AUtils.h>

#include <C2Debug.h>
#include <Codec2Mapper.h>
#include <C2PlatformSupport.h>
#include <Codec2BufferUtils.h>
#include <C2RKInterface.h>
#include <util/C2InterfaceHelper.h>
#include <C2AllocatorGralloc.h>
#include <ui/GraphicBufferMapper.h>

#include "hardware/hardware_rockchip.h"
#include "C2RKMpiEnc.h"
#include <C2RKMediaDefs.h>
#include "C2RKRgaProcess.h"
#include "mpp/h264_syntax.h"
#include "mpp/h265_syntax.h"
#include "C2RKLog.h"
#include "C2RKEnv.h"
#include "C2RKVideoGlobal.h"
#include "C2RKVersion.h"

namespace android {

namespace {

MppEncCfg enc_cfg = nullptr;

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
                .withDefault(new C2StreamPictureSizeInfo::input(0u, 1280, 720))
                .withFields({
                    C2F(mSize, width).inRange(2, 1920, 2),
                    C2F(mSize, height).inRange(2, 1920, 2),
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
                DefineParam(mActualInputDelay, C2_PARAMKEY_INPUT_DELAY)
                .withDefault(new C2PortActualDelayTuning::input(0))
                .withFields({C2F(mActualInputDelay, value).inRange(0, 2)})
                .calculatedAs(InputDelaySetter, mGop)
                .build());

        addParameter(
                DefineParam(mFrameRate, C2_PARAMKEY_FRAME_RATE)
                .withDefault(new C2StreamFrameRateInfo::output(0u, 30.))
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
        c2_info("%s %d in", __FUNCTION__, __LINE__);
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
                c2_err(" %s unsupport type:%d", __func__, type);
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
            { LEVEL_AVC_1B,         99 },
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
            c2_err("%s unsupport type:%d", __func__, type);
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

    // unsafe getters
    std::shared_ptr<C2StreamPictureSizeInfo::input> getSize_l() const { return mSize; }
    std::shared_ptr<C2StreamIntraRefreshTuning::output> getIntraRefresh_l() const { return mIntraRefresh; }
    std::shared_ptr<C2StreamFrameRateInfo::output> getFrameRate_l() const { return mFrameRate; }
    std::shared_ptr<C2StreamBitrateModeTuning::output> getBitrateMode_l() const { return mBitrateMode; }
    std::shared_ptr<C2StreamBitrateInfo::output> getBitrate_l() const { return mBitrate; }
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> getRequestSync_l() const { return mRequestSync; }
    std::shared_ptr<C2StreamGopTuning::output> getGop_l() const { return mGop; }
    std::shared_ptr<C2StreamColorAspectsInfo::output> getCodedColorAspects_l() const {
        return mCodedColorAspects;
    }

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
    std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mCodedColorAspects;
};

C2RKMpiEnc::C2RKMpiEnc(
        const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mSpsPpsHeaderReceived(false),
      mSignalledEos(false),
      mSignalledError(false),
      mStarted(false),
      mVpumem(nullptr),
      mEncProfile(0),
      mEncLevel(0),
      mEos(0),
      mMppCtx(nullptr),
      mMppMpi(nullptr),
      mFp_enc_out(nullptr),
      mFp_enc_in(nullptr),
      mCodingType(MPP_VIDEO_CodingUnused){
    c2_info("version:%s", C2_GIT_BUILD_VERSION);
    int err = getCodingTypeFromComponentName(name, &mCodingType);
    if (err) {
        c2_err("get coding type from component name failed! now codingtype=%d", mCodingType);
    }

    GETTIME(&mTimeStart, nullptr);
    GETTIME(&mTimeEnd, nullptr);

    if (!Rockchip_C2_GetEnvU32("vendor.c2.venc.debug", &c2_venc_debug, 0) && c2_venc_debug > 0) {
        c2_info("open video encoder debug, value: 0x%x", c2_venc_debug);
    }

    if (c2_venc_debug & VIDEO_DBG_RECORD_IN) {
        char file_name_in[128];
        memset(file_name_in, 0, 128);
        sprintf(file_name_in, "/data/video/enc_in_%ld.bin", mTimeStart.tv_sec);
        c2_info("Start recording stream to %s", file_name_in);
        if (NULL != mFp_enc_in) {
            fclose(mFp_enc_in);
        }
        mFp_enc_in = fopen(file_name_in, "wb");
        if (NULL == mFp_enc_in) {
            c2_err("record in file fopen failed, err: %s", strerror(errno));
        }
    }

    if (c2_venc_debug & VIDEO_DBG_RECORD_OUT) {
        char file_name_out[128];
        memset(file_name_out, 0, 128);
        sprintf(file_name_out, "/data/video/enc_out_%ld.bin", mTimeStart.tv_sec);
        c2_info("Start recording stream to %s", file_name_out);
        if (NULL != mFp_enc_out) {
            fclose(mFp_enc_out);
        }
        mFp_enc_out = fopen(file_name_out, "wb");
        if (NULL == mFp_enc_out) {
            c2_err("record in file fopen failed, err: %s", strerror(errno));
        }
    }

    switch (mCodingType) {
    case MPP_VIDEO_CodingAVC:
        mEncProfile = H264_PROFILE_BASELINE;
        mEncLevel = 31;
        break;
    case MPP_VIDEO_CodingHEVC:
        mEncProfile = MPP_PROFILE_HEVC_MAIN;
        mEncLevel = 123;
        break;
    default:
        break;
    }
}

C2RKMpiEnc::~C2RKMpiEnc() {
    c2_info("%s in", __func__);
    onRelease();
}

c2_status_t C2RKMpiEnc::onInit() {
    c2_info("%s in", __func__);
    return C2_OK;
}

c2_status_t C2RKMpiEnc::onStop() {
    c2_info("%s in", __func__);
    return C2_OK;
}

void C2RKMpiEnc::onReset() {
    c2_info("%s in", __func__);
    releaseEncoder();
}

void C2RKMpiEnc::onRelease() {
    c2_info("%s in", __func__);
    releaseEncoder();
}

c2_status_t C2RKMpiEnc::onFlush_sm() {
    c2_info("%s in", __func__);
    return C2_OK;
}

c2_status_t C2RKMpiEnc::setVuiParams() {
    ColorAspects sfAspects;
    if (!C2Mapper::map(mColorAspects->primaries, &sfAspects.mPrimaries)) {
        sfAspects.mPrimaries = android::ColorAspects::PrimariesUnspecified;
    }
    if (!C2Mapper::map(mColorAspects->range, &sfAspects.mRange)) {
        sfAspects.mRange = android::ColorAspects::RangeUnspecified;
    }
    if (!C2Mapper::map(mColorAspects->matrix, &sfAspects.mMatrixCoeffs)) {
        sfAspects.mMatrixCoeffs = android::ColorAspects::MatrixUnspecified;
    }
    if (!C2Mapper::map(mColorAspects->transfer, &sfAspects.mTransfer)) {
        sfAspects.mTransfer = android::ColorAspects::TransferUnspecified;
    }
    int32_t primaries, transfer, matrixCoeffs;
    bool range;
    ColorUtils::convertCodecColorAspectsToIsoAspects(sfAspects,
            &primaries,
            &transfer,
            &matrixCoeffs,
            &range);

    mpp_enc_cfg_set_s32(enc_cfg, "prep:range", range ? 2 : 0);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:colorprim", primaries);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:colortrc", transfer);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:colorspace", matrixCoeffs);

    return C2_OK;
}

c2_status_t C2RKMpiEnc::initEncParams() {
    c2_info("%s in", __func__);
    c2_status_t ret = C2_OK;
    int err = 0;
    int32_t gop = 30;
    MppEncSeiMode seiMode = MPP_ENC_SEI_MODE_ONE_FRAME;

    err = mpp_enc_cfg_init(&enc_cfg);
    if (err) {
        ret = C2_CORRUPTED;
        c2_err("mpp_enc_cfg_init failed ret %d\n", err);
        goto __RETURN;
    }

    mpp_enc_cfg_set_s32(enc_cfg, "prep:width", mSize->width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:height", mSize->height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride", C2_ALIGN(mSize->width, 16));
    mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride", C2_ALIGN(mSize->height, 8));
    mpp_enc_cfg_set_s32(enc_cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:rotation", MPP_ENC_ROT_0);

    /* setup bitrate for different rc_mode */
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target", mBitrate->value);
    switch (mBitrateMode->value) {
        case C2Config::BITRATE_CONST:
            /* CBR mode has narrow bound */
            mpp_enc_cfg_set_s32(enc_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
            mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", mBitrate->value * 17 / 16);
            mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", mBitrate->value * 15 / 16);
            break;
        case C2Config::BITRATE_IGNORE:[[fallthrough]];
        case C2Config::BITRATE_VARIABLE:
            /* VBR mode has wide bound */
            mpp_enc_cfg_set_s32(enc_cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
            mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", mBitrate->value * 17 / 16);
            mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", mBitrate->value * 1 / 16);
            break;
        default:
            /* default use CBR mode */
            mpp_enc_cfg_set_s32(enc_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
            mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", mBitrate->value * 17 / 16);
            mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", mBitrate->value * 15 / 16);
            break;
    }

    /* fix input / output frame rate */
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num", mFrameRate->value);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_denorm", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num", mFrameRate->value);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_denorm", 1);

    if (mGop && mGop->flexCount() > 0) {
        uint32_t syncInterval = 30;
        uint32_t iInterval = 30;
        uint32_t maxBframes = 0;
        ParseGop(*mGop, &syncInterval, &iInterval, &maxBframes);
        if (syncInterval > 0) {
            c2_info("Updating IDR interval from GOP: old %u new %u", mIDRInterval, syncInterval);
            mIDRInterval = syncInterval;
        }
        if (iInterval > 0) {
            c2_info("Updating I interval from GOP: old %u new %u", mIInterval, iInterval);
            mIInterval = iInterval;
        }
        // if (mBframes != maxBframes) {
        //     c2_info("Updating max B frames from GOP: old %u new %u", mBframes, maxBframes);
        //     mBframes = maxBframes;
        // }
    }
    gop = (mIDRInterval  < 8640000) ? mIDRInterval : mFrameRate->value;
    mpp_enc_cfg_set_s32(enc_cfg, "rc:gop", gop);
    c2_info("(%s): bps %d fps %f gop %d\n",
            intf()->getName().c_str(), mBitrate->value, mFrameRate->value, gop);

    mpp_enc_cfg_set_s32(enc_cfg, "codec:type", mCodingType);
    switch (mCodingType) {
    case MPP_VIDEO_CodingAVC : {
        mpp_enc_cfg_set_s32(enc_cfg, "h264:profile", mEncProfile);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:level", mEncLevel);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:trans8x8", 1);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_init", 26);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_min", 10);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_max", 49);//49 for testEncoderQualityAVCCBR
        mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_min_i", 10);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_max_i", 51);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_step", 4);
        mpp_enc_cfg_set_s32(enc_cfg, "h264:qp_delta_ip", 3);
        /* disable mb_rc for vepu, this cfg does not apply to rkvenc */
        mpp_enc_cfg_set_s32(enc_cfg, "hw:mb_rc_disable", 1);
    } break;
    case MPP_VIDEO_CodingMJPEG : {
        mpp_enc_cfg_set_s32(enc_cfg, "jpeg:quant", 10);
        mpp_enc_cfg_set_s32(enc_cfg, "jpeg:change", MPP_ENC_JPEG_CFG_CHANGE_QP);
    } break;
    case MPP_VIDEO_CodingVP8 : {
        mpp_enc_cfg_set_s32(enc_cfg, "vp8:qp_init", -1);
        mpp_enc_cfg_set_s32(enc_cfg, "vp8:qp_min", 0);
        mpp_enc_cfg_set_s32(enc_cfg, "vp8:qp_max", 127);
        mpp_enc_cfg_set_s32(enc_cfg, "vp8:qp_min_i", 0);
        mpp_enc_cfg_set_s32(enc_cfg, "vp8:qp_max_i", 127);
    } break;
    case MPP_VIDEO_CodingHEVC : {
        mpp_enc_cfg_set_s32(enc_cfg, "h265:profile", mEncProfile);
        mpp_enc_cfg_set_s32(enc_cfg, "h265:level", mEncLevel);
        mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_init", 26);
        mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_min", 10);
        mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_max", 49);
        mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_min_i", 15);
        mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_max_i", 51);
        mpp_enc_cfg_set_s32(enc_cfg, "h265:qp_delta_ip", 4);
    } break;
    default : {
        c2_err("support encoder coding type %d\n", mCodingType);
    } break;
    }

    /* Video control Set VUI params */
    setVuiParams();

    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_CFG, enc_cfg);
    if (err) {
        c2_err("mpi control enc set codec cfg failed ret %d\n", err);
        ret = C2_CORRUPTED;
        goto __RETURN;
    }

    /* optional */
    err = mMppMpi->control(mMppCtx, MPP_ENC_SET_SEI_CFG, &seiMode);
    if (err) {
        c2_err("mpi control enc set sei cfg failed ret %d\n", err);
        ret = C2_CORRUPTED;
        goto __RETURN;
    }
__RETURN:
    return ret;
}

c2_status_t C2RKMpiEnc::initEncoder() {
    c2_info("%s %d in", __FUNCTION__, __LINE__);
    MppCodingType type = mCodingType;
    {
        IntfImpl::Lock lock = mIntf->lock();
        mSize = mIntf->getSize_l();
        mBitrateMode = mIntf->getBitrateMode_l();
        mBitrate = mIntf->getBitrate_l();
        mFrameRate = mIntf->getFrameRate_l();
        mEncProfile = mIntf->getProfile_l(type);
        mEncLevel = mIntf->getLevel_l(type);
        mIDRInterval = mIntf->getSyncFramePeriod_l();
        mIInterval = mIntf->getSyncFramePeriod_l();
        mGop = mIntf->getGop_l();
        mRequestSync = mIntf->getRequestSync_l();
        mColorAspects = mIntf->getCodedColorAspects_l();
    }

    c2_info("%s %d in frame rate = %f", __FUNCTION__, __LINE__, mFrameRate->value);
    c2_info("%s %d in bit rate = %d iInterval = %d", __FUNCTION__, __LINE__, mBitrate->value, mIDRInterval);
    c2_info("%s %d in width=%d, height=%d", __FUNCTION__, __LINE__, mSize->width, mSize->height);
    c2_info("%s %d in bitrate mode = %d", __FUNCTION__, __LINE__, (int)mBitrateMode->value);
    c2_info("%s %d in profile = %d level = %d", __FUNCTION__, __LINE__, mEncProfile, mEncLevel);

    c2_status_t ret = C2_OK;
    int err = 0;
    uint32_t width = mSize->width;
    uint32_t height = mSize->height;
    MppPollType timeout = MPP_POLL_BLOCK;
    //open rga
    if (rga_dev_open(&mRgaCtx)  < 0) {
        c2_err("open rga device fail!");
    }

    //init vpumem for mpp input
    mVpumem = (VPUMemLinear_t*)malloc(sizeof( VPUMemLinear_t));
    err = VPUMallocLinear(mVpumem, ((width + 15) & 0xfff0) * height * 4);
    if (err) {
        c2_err("VPUMallocLinear failed\n");
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    //create mpp and init mpp
    err = mpp_create(&mMppCtx, &mMppMpi);
    if (err) {
        c2_err("mpp_create failed ret %d\n", err);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    err = mMppMpi->control(mMppCtx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    if (MPP_OK != err) {
        c2_err("mpi control set output timeout %d ret %d\n", timeout, err);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    err = mpp_init(mMppCtx, MPP_CTX_ENC, mCodingType);
    if (err) {
        c2_err("mpp_init failed ret %d\n", err);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    //int MppEncCfg
    ret = initEncParams();
    if (ret) {
        c2_err("init encoder params failed, ret=0x%x", ret);
        ret = C2_CORRUPTED;
        goto __FAILED;
    }

    mStarted = true;
    return C2_OK;
__FAILED:
    if (mMppCtx) {
        mpp_destroy(mMppCtx);
        mMppCtx = NULL;
    }
    return ret;
}

c2_status_t C2RKMpiEnc::releaseEncoder() {
    mSpsPpsHeaderReceived = false;
    mSignalledEos = false;
    mSignalledError = false;
    mStarted = false;
    mEos = 0;

    if(enc_cfg){
        mpp_enc_cfg_deinit(enc_cfg);
        enc_cfg = nullptr;
    }

    if (mMppCtx){
        mpp_destroy(mMppCtx);
        mMppCtx = nullptr;
    }

    if (mRgaCtx) {
        rga_dev_close(mRgaCtx);
        mRgaCtx = nullptr;
    }

    if (mVpumem) {
        VPUFreeLinear(mVpumem);
        free(mVpumem);
        mVpumem = nullptr;
    }

    if (mFp_enc_in != nullptr) {
        fclose(mFp_enc_in);
    }

    if (mFp_enc_out != nullptr) {
        fclose(mFp_enc_out);
    }

    return C2_OK;
}

static void fillEmptyWork(const std::unique_ptr<C2Work>& work) {
    c2_trace("%s in", __func__);
    uint32_t flags = 0;
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

    c2_trace("timestamp %d frameindex %d, flags %x",
          (int)work->input.ordinal.timestamp.peeku(),
          (int)work->input.ordinal.frameIndex.peeku(), work->input.flags);

    if (mSignalledError) {
        work->result = C2_BAD_VALUE;
        c2_info("Signalled Error");
        return;
    }

    c2_status_t status = C2_OK;
    // Initialize encoder if not already initialized
    if (!mStarted) {
        status = initEncoder();
        if (C2_OK != status) {
            c2_err("Failed to initialize encoder : 0x%x", status);
            mSignalledError = true;
            work->result = status;
            work->workletsProcessed = 1u;
            return;
        }
    }

    //get input buffer
    std::shared_ptr<const C2GraphicView> view;
    std::shared_ptr<C2Buffer> inputBuffer = nullptr;
    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    if (eos) mSignalledEos = true;
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
            c2_err("unexpected Capacity Aspect %d(%d) x %d(%d)", input->width(),
                mSize->width, input->height(), mSize->height);
            mSignalledError = true;
            work->result = C2_CORRUPTED;
            work->workletsProcessed = 1u;
            return;
        }
    }

    //get sps/pps para
    if (!mSpsPpsHeaderReceived){
        MppPacket enc_hdr_pkt = nullptr;
        void *enc_hdr_buf = nullptr;
        int enc_hdr_buf_size;
        int extradata_size = 0;
        void *extradata = nullptr;
        if (NULL == enc_hdr_pkt) {
            if (NULL == enc_hdr_buf) {
                enc_hdr_buf_size = 1024;
                enc_hdr_buf = malloc(enc_hdr_buf_size * sizeof(uint8_t));
            }

            if (enc_hdr_buf)
                mpp_packet_init(&enc_hdr_pkt, enc_hdr_buf, enc_hdr_buf_size);
        }

        if (enc_hdr_pkt) {
            mMppMpi->control(mMppCtx, MPP_ENC_GET_HDR_SYNC, enc_hdr_pkt);
            extradata_size = mpp_packet_get_length(enc_hdr_pkt);
            extradata      = mpp_packet_get_data(enc_hdr_pkt);
        }
        mSpsPpsHeaderReceived = true;
        std::unique_ptr<C2StreamInitDataInfo::output> csd =
            C2StreamInitDataInfo::output::AllocUnique(extradata_size, 0u);
        if (!csd) {
            c2_err("CSD allocation failed");
            work->result = C2_NO_MEMORY;
            C2_SAFE_FREE(enc_hdr_buf);
            work->workletsProcessed = 1u;
            return;
        }
        memcpy(csd->m.value, extradata, extradata_size);
        if (mFp_enc_out != nullptr) {
            fwrite(extradata, 1, extradata_size , mFp_enc_out);
            fflush(mFp_enc_out);
        }
        work->worklets.front()->output.configUpdate.push_back(std::move(csd));
        //free hdr_pkt
        if(enc_hdr_pkt){
            mpp_packet_deinit(&enc_hdr_pkt);
            enc_hdr_pkt = NULL;
        }
        C2_SAFE_FREE(enc_hdr_buf);
        if (work->input.buffers.empty()) {
            work->workletsProcessed = 1u;
            return;
        }
    }
    //handle dynamic config parameters
    {
        IntfImpl::Lock lock = mIntf->lock();
        std::shared_ptr<C2StreamBitrateInfo::output> bitrate = mIntf->getBitrate_l();
        lock.unlock();
        if (bitrate != mBitrate) {
            mBitrate = bitrate;
        }
    }

    c2_status_t err             = C2_OK;
    //set frame to mpp
    err = encoder_sendframe(work);
    if (err != C2_OK) {
        c2_err("encoder_sendframe failed: %d", err);
        mSignalledError = true;
        work->result = C2_CORRUPTED;
        work->workletsProcessed = 1u;
        return;
    }
    // get pkt from mpp
    err = encoder_getstream(work, pool);
    if (err != C2_OK) {
        // nothing to do
        c2_err("encoder_getstream failed or eos!");
        if (work && work->workletsProcessed != 1u) fillEmptyWork(work);
    }

    if (!eos && work->input.buffers.empty()) {
        fillEmptyWork(work);
    }

    if (eos && (mEos != 1)) {
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
    }

    FunctionOut();
}

c2_status_t C2RKMpiEnc::encoder_sendframe(const std::unique_ptr<C2Work> &work){
    c2_trace("%s in", __func__);
    int err = 0;
    c2_status_t ret = C2_OK;
    MppFrame frame = NULL;
    std::shared_ptr<const C2GraphicView> view;
    std::shared_ptr<C2Buffer> inputBuffer = nullptr;

    MppBufferInfo inputCommit;
    memset(&inputCommit, 0, sizeof(inputCommit));
    inputCommit.type = MPP_BUFFER_TYPE_ION;

    uint32_t width = mSize->width;
    uint32_t height = mSize->height;
    uint32_t hor_stride = C2_ALIGN(width, 16);
    uint32_t ver_stride = C2_ALIGN(height, 8);
    uint64_t workIndex = work->input.ordinal.frameIndex.peekull();

    err = mpp_frame_init(&frame);
    if (err) {
        c2_err("mpp_frame_init failed\n");
        ret = C2_CORRUPTED;
        if (frame) {
            mpp_frame_deinit(&frame);
        }
        return ret;
    }

    mpp_frame_set_width(frame, width);
    mpp_frame_set_height(frame, height);
    mpp_frame_set_hor_stride(frame, C2_ALIGN(width, 16));
    mpp_frame_set_ver_stride(frame, C2_ALIGN(height, 8));
    mpp_frame_set_pts(frame, workIndex);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);

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
                // unset request
                C2StreamRequestSyncFrameTuning::output clearSync(0u, C2_FALSE);
                std::vector<std::unique_ptr<C2SettingResult>> failures;
                mIntf->config({ &clearSync }, C2_MAY_BLOCK, &failures);
                c2_trace("Got sync request");
                //Force this as an IDR frame
                mMppMpi->control(mMppCtx, MPP_ENC_SET_IDR_FRAME, nullptr);
            }
            mRequestSync = requestSync;
        }
    }

    if (!work->input.buffers.empty()){
        //get input buffer
        inputBuffer = work->input.buffers[0];
        view = std::make_shared<const C2GraphicView>(
                inputBuffer->data().graphicBlocks().front().map().get());
        const C2Handle *c2Handle = inputBuffer->data().graphicBlocks().front().handle();
        uint32_t bqSlot;
        uint32_t Width;
        uint32_t Height;
        uint32_t Format;
        uint64_t usage;
        uint32_t Stride;
        uint32_t generation;
        uint64_t bqId;
        android::_UnwrapNativeCodec2GrallocMetadata(
                c2Handle, &Width, &Height, &Format, &usage, &Stride, &generation, &bqId, &bqSlot);
        c2_trace("%s Width:%d Height:%d Format:%d Stride:%d usage:0x%x", __func__, Width, Height, Format, Stride, usage);
        // Fix error for wifidisplay when Stride is 0
        if (Stride == 0) {
            native_handle_t *grallocHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);
            std::vector<ui::PlaneLayout> layouts;
            buffer_handle_t bufferHandle;
            GraphicBufferMapper &gm(GraphicBufferMapper::get());
            gm.importBuffer(const_cast<native_handle_t *>(grallocHandle), Width, Height, 1,
                                Format, usage, Stride, &bufferHandle);
            gm.getPlaneLayouts(const_cast<native_handle_t *>(bufferHandle), &layouts);
            if(layouts[0].sampleIncrementInBits != 0) {
                Stride = layouts[0].strideInBytes * 8 / layouts[0].sampleIncrementInBits;
            } else {
                c2_err("layouts[0].sampleIncrementInBits = 0");
                Stride = hor_stride;
            }
            gm.freeBuffer(bufferHandle);
            native_handle_delete(grallocHandle);
        }
        //convert rgb or yuv to nv12
        {
            RKVideoPlane *vplanes = (RKVideoPlane *)malloc(sizeof(RKVideoPlane));
            {
                vplanes[0].fd = c2Handle->data[0];
                vplanes[0].offset = 0;
                vplanes[0].addr = NULL;
                vplanes[0].type = 1;
                vplanes[0].stride = Stride;
            }
            const C2GraphicView* const input = view.get();
            const C2PlanarLayout& layout = input->layout();
            switch (layout.type) {
                case C2PlanarLayout::TYPE_RGB:
                    [[fallthrough]];
                case C2PlanarLayout::TYPE_RGBA: {
                    c2_trace("%s %d input rgb", __func__,__LINE__);
                    if (mFp_enc_in != nullptr) {
                        fwrite(input->data()[0], 1, mSize->width * mSize->height * 4, mFp_enc_in);
                        fflush(mFp_enc_in);
                    }
                    rga_rgb2nv12(vplanes, mVpumem, width, height, hor_stride, ver_stride, mRgaCtx);
                    C2_SAFE_FREE(vplanes);
                    inputCommit.size = hor_stride * ver_stride * 3/2;
                    inputCommit.fd = mVpumem->phy_addr;
                    break;
                }
                case C2PlanarLayout::TYPE_YUV:
                    c2_trace("%s %d input yuv", __func__,__LINE__);
                    if (!IsYUV420(*input)) {
                        c2_err("input is not YUV420");
                        C2_SAFE_FREE(vplanes);
                        return C2_BAD_VALUE;
                    }
                    if (mFp_enc_in != nullptr) {
                        fwrite(input->data()[0], 1, mSize->width * mSize->height * 3 / 2, mFp_enc_in);
                        fflush(mFp_enc_in);
                    }
                    if (Format == HAL_PIXEL_FORMAT_YCbCr_420_888 || Format == HAL_PIXEL_FORMAT_YCrCb_NV12) {
                        rga_nv12_copy(vplanes, mVpumem, hor_stride, ver_stride, mRgaCtx);
                        inputCommit.fd = mVpumem->phy_addr;
                        inputCommit.size = hor_stride * ver_stride * 3 / 2;
                    } else {
                        //TODO: if(input->width() != width) do more(rga_nv12_copy)?
                        inputCommit.size = width * height * 3/2;
                        inputCommit.fd = c2Handle->data[0];
                    }
                    C2_SAFE_FREE(vplanes);
                    break;
                case C2PlanarLayout::TYPE_YUVA:
                    c2_err("YUVA plane type is not supported");
                    C2_SAFE_FREE(vplanes);
                    return C2_BAD_VALUE;
                default:
                    c2_err("Unrecognized plane type: %d", layout.type);
                    C2_SAFE_FREE(vplanes);
                    return C2_BAD_VALUE;
            }
        }
        MppBuffer buffer = NULL;
        ret = (c2_status_t)mpp_buffer_import(&buffer, &inputCommit);
        if (ret) {
            c2_err("import input picture buffer failed\n");
            ret = C2_NOT_FOUND;
            goto __FAILED;
        }
        mpp_frame_set_buffer(frame, buffer);
        mpp_buffer_put(buffer);
        buffer = NULL;
    } else {
        mpp_frame_set_buffer(frame, NULL);
    }
    err = mMppMpi->encode_put_frame(mMppCtx, frame);
    if (err) {
        c2_err("encode_put_frame ret %d\n", ret);
        ret = C2_NOT_FOUND;
        goto __FAILED;
    }
__FAILED:
    if (frame) {
        mpp_frame_deinit(&frame);
    }
    return ret;
}

c2_status_t C2RKMpiEnc::encoder_getstream(const std::unique_ptr<C2Work> &work,
                                          const std::shared_ptr<C2BlockPool>& pool){
    c2_trace("%s in", __func__);
    int err = 0;
    c2_status_t ret = C2_OK;
    MppPacket packet = NULL;

    err = mMppMpi->encode_get_packet(mMppCtx, &packet);
    if (err) {
        c2_err("mpp encode get packet failed\n");
        ret = C2_CORRUPTED;
        goto __FAILED;
    } else {
        mEos = mpp_packet_get_eos(packet);
        uint64_t workId = (uint64_t)mpp_packet_get_pts(packet);
        uint8_t *src = (uint8_t *)mpp_packet_get_data(packet);
        size_t len  = mpp_packet_get_length(packet);
        if (mEos == 1 && workId == 0){
            c2_err("eos with empty pkt!\n");
            ret = C2_CORRUPTED;
            goto __FAILED;
        }
        if (!src || (len == 0)) {
            c2_err("src empty or len = 0!\n");
            ret = C2_CORRUPTED;
            goto __FAILED;
        }
        finishWork(workId, work, pool, packet);
    }

__FAILED:
    return ret;
}

void C2RKMpiEnc::finishWork(uint64_t workIndex, 
                            const std::unique_ptr<C2Work> &work,
                            const std::shared_ptr<C2BlockPool>& pool, 
                            MppPacket outputPkt) {
    uint8_t *src = (uint8_t *)mpp_packet_get_data(outputPkt);
    size_t len  = mpp_packet_get_length(outputPkt);
    std::shared_ptr<C2LinearBlock> block;
    C2MemoryUsage usage = {C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE};
    c2_status_t status = pool->fetchLinearBlock(len, usage, &block);
    if (C2_OK != status) {
        c2_err("fetchLinearBlock for Output failed with status 0x%x", status);
        mSignalledError = true;
        work->result = status;
        work->workletsProcessed = 1u;
        return;
    }
    C2WriteView wView = block->map().get();
    if (C2_OK != wView.error()) {
        c2_err("write view map failed with status 0x%x", wView.error());
        mSignalledError = true;
        work->result = wView.error();
        work->workletsProcessed = 1u;
        return;
    }
    // copy mpp enc output to c2 output
    memcpy(wView.data(), src, len);
    c2_trace("encoded frame size %zu\n", len);
    if (mFp_enc_out != nullptr) {
        fwrite(src, 1, len , mFp_enc_out);
        fflush(mFp_enc_out);
    }
    std::shared_ptr<C2Buffer> buffer = createLinearBuffer(block, 0, len);
    MppMeta meta = mpp_packet_get_meta(outputPkt);
    RK_S32 is_intra = 0;
    mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_intra);
    if (is_intra) {
        c2_info("IDR frame produced");
        buffer->setInfo(std::make_shared<C2StreamPictureTypeMaskInfo::output>(
                0u /* stream id */, C2Config::SYNC_FRAME));
    }
    mpp_packet_deinit(&outputPkt);
    auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.flags = (C2FrameData::flags_t)0;
        work->worklets.front()->output.buffers.clear();
        work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };
    if (work && c2_cntr64_t(workIndex) == work->input.ordinal.frameIndex) {
        fillWork(work);
        if(mSignalledEos) {
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
    c2_trace("%s %d in", __FUNCTION__, __LINE__);

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
            c2_err("encoder_getstream failed or eos!");
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
        int err = getMimeFromComponentName(componentName, &mMime);
        if (err) {
            c2_err("get mime from component name failed, component name=%s", componentName.c_str());
        }
        err = getDomainFromComponentName(componentName, &mDomain);
        if (err) {
            c2_err("get domain from component name failed, component name=%s", componentName.c_str());
        }
        err = getKindFromComponentName(componentName, &mKind);
        if (err) {
            c2_err("get kind from component name failed, component name=%s", componentName.c_str());
        }
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        c2_trace("%s %d in", __FUNCTION__, __LINE__);
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
    c2_trace("in %s", __func__);
    return new ::android::C2RKMpiEncFactory(componentName);
}

} // namespace android

