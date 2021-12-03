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

#include <media/stagefright/foundation/MediaDefs.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <Codec2Mapper.h>
#include <C2RKInterface.h>
#include <C2AllocatorGralloc.h>
#include "hardware/hardware_rockchip.h"
#include "C2RKMpiDec.h"
#include "C2RKMediaDefs.h"
#include "C2RKRgaDef.h"
#include "C2RKVersion.h"
#include "C2RKEnv.h"

#define GRALLOC_USAGE_HW_TEXTURE            1ULL << 8
#define GRALLOC_USAGE_HW_COMPOSER           1ULL << 11
#define RK_GRALLOC_USAGE_SPECIFY_STRIDE     1ULL << 30

namespace android {

C2RKMpiDec::IntfImpl::IntfImpl(
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
    addParameter(
            DefineParam(mActualOutputDelay, C2_PARAMKEY_OUTPUT_DELAY)
            .withDefault(new C2PortActualDelayTuning::output(kDefaultOutputDelay))
            .withFields({C2F(mActualOutputDelay, value).inRange(0, kMaxOutputDelay)})
            .withSetter(Setter<decltype(*mActualOutputDelay)>::StrictValueWithNoDeps)
            .build());

    // TODO: output latency and reordering
    addParameter(
            DefineParam(mAttrib, C2_PARAMKEY_COMPONENT_ATTRIBUTES)
            .withConstValue(new C2ComponentAttributesSetting(C2Component::ATTRIB_IS_TEMPORAL))
            .build());

    // coded and output picture size is the same for this codec
    addParameter(
            DefineParam(mSize, C2_PARAMKEY_PICTURE_SIZE)
            .withDefault(new C2StreamPictureSizeInfo::output(0u, 320, 240))
            .withFields({
                C2F(mSize, width).inRange(2, 7680, 2),
                C2F(mSize, height).inRange(2, 7680, 2),
            })
            .withSetter(SizeSetter)
            .build());

    addParameter(
            DefineParam(mBlockSize, C2_PARAMKEY_BLOCK_SIZE)
            .withDefault(new C2StreamBlockSizeInfo::output(0u, 320, 240))
            .withFields({
                C2F(mBlockSize, width).inRange(2, 7680, 2),
                C2F(mBlockSize, height).inRange(2, 7680, 2),
            })
            .withSetter(BlockSizeSetter)
            .build());

    addParameter(
            DefineParam(mBlockCount, C2_PARAMKEY_BLOCK_COUNT)
            .withDefault(new C2StreamBlockCountInfo::output(0u, 0u))
            .withFields({C2F(mBlockCount, value).inRange(0, kMaxReferenceCount)})
            .withSetter(Setter<decltype(*mBlockCount)>::StrictValueWithNoDeps)
            .build());

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
    if (mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2){
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(0u,
                    C2Config::PROFILE_MP2V_SIMPLE, C2Config::LEVEL_MP2V_HIGH))
            .withFields({
                C2F(mProfileLevel, profile).oneOf({
                        C2Config::PROFILE_MP2V_SIMPLE,
                        C2Config::PROFILE_MP2V_MAIN
                        }),
                C2F(mProfileLevel, level).oneOf({
                        C2Config::LEVEL_MP2V_LOW,
                        C2Config::LEVEL_MP2V_MAIN,
                        C2Config::LEVEL_MP2V_HIGH_1440,
                        C2Config::LEVEL_MP2V_HIGH
                })
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
                        C2Config::PROFILE_MP4V_SIMPLE
                        }),
                C2F(mProfileLevel, level).oneOf({
                            C2Config::LEVEL_MP4V_0,
                            C2Config::LEVEL_MP4V_0B,
                            C2Config::LEVEL_MP4V_1,
                            C2Config::LEVEL_MP4V_2,
                            C2Config::LEVEL_MP4V_3
                })
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
                        C2Config::PROFILE_H263_ISWV2
                        }),
                C2F(mProfileLevel, level).oneOf({
                        C2Config::LEVEL_H263_10,
                        C2Config::LEVEL_H263_20,
                        C2Config::LEVEL_H263_30,
                        C2Config::LEVEL_H263_40,
                        C2Config::LEVEL_H263_45
                })
            })
            .withSetter(ProfileLevelSetter, mSize)
            .build());
    } else if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC) {
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
                        C2Config::PROFILE_AVC_HIGH
                        }),
                C2F(mProfileLevel, level).oneOf({
                        C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
                        C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
                        C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                        C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2,
                        C2Config::LEVEL_AVC_4, C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                        C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1, C2Config::LEVEL_AVC_5_2
                })
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
                        C2Config::PROFILE_HEVC_MAIN_10
                        }),
                C2F(mProfileLevel, level).oneOf({
                        C2Config::LEVEL_HEVC_MAIN_1,
                        C2Config::LEVEL_HEVC_MAIN_2, C2Config::LEVEL_HEVC_MAIN_2_1,
                        C2Config::LEVEL_HEVC_MAIN_3, C2Config::LEVEL_HEVC_MAIN_3_1,
                        C2Config::LEVEL_HEVC_MAIN_4, C2Config::LEVEL_HEVC_MAIN_4_1,
                        C2Config::LEVEL_HEVC_MAIN_5, C2Config::LEVEL_HEVC_MAIN_5_1,
                        C2Config::LEVEL_HEVC_MAIN_5_2, C2Config::LEVEL_HEVC_HIGH_4,
                        C2Config::LEVEL_HEVC_HIGH_4_1, C2Config::LEVEL_HEVC_HIGH_5,
                        C2Config::LEVEL_HEVC_HIGH_5_1
                })
            })
            .withSetter(ProfileLevelSetter, mSize)
            .build());
    } else if (mediaType == MEDIA_MIMETYPE_VIDEO_VP8) {
        //TODO
    } else if (mediaType == MEDIA_MIMETYPE_VIDEO_VP9) {
        addParameter(
            DefineParam(mProfileLevel, C2_PARAMKEY_PROFILE_LEVEL)
            .withDefault(new C2StreamProfileLevelInfo::input(0u,
                    C2Config::PROFILE_VP9_0, C2Config::LEVEL_VP9_5))
            .withFields({
                C2F(mProfileLevel, profile).oneOf({
                        C2Config::PROFILE_VP9_0,
                        C2Config::PROFILE_VP9_2
                        }),
                C2F(mProfileLevel, level).oneOf({
                        C2Config::LEVEL_VP9_1,
                        C2Config::LEVEL_VP9_1_1,
                        C2Config::LEVEL_VP9_2,
                        C2Config::LEVEL_VP9_2_1,
                        C2Config::LEVEL_VP9_3,
                        C2Config::LEVEL_VP9_3_1,
                        C2Config::LEVEL_VP9_4,
                        C2Config::LEVEL_VP9_4_1,
                        C2Config::LEVEL_VP9_5,
                })
            })
            .withSetter(ProfileLevelSetter, mSize)
            .build());
    }

    addParameter(
            DefineParam(mMaxSize, C2_PARAMKEY_MAX_PICTURE_SIZE)
            .withDefault(new C2StreamMaxPictureSizeTuning::output(0u, 320, 240))
            .withFields({
                C2F(mSize, width).inRange(2, 4096, 2),
                C2F(mSize, height).inRange(2, 4096, 2),
            })
            .withSetter(MaxPictureSizeSetter, mSize)
            .build());

    addParameter(
            DefineParam(mMaxInputSize, C2_PARAMKEY_INPUT_MAX_BUFFER_SIZE)
            .withDefault(new C2StreamMaxBufferSizeInfo::input(0u, kMinInputBufferSize))
            .withFields({
                C2F(mMaxInputSize, value).any(),
            })
            .calculatedAs(MaxInputSizeSetter, mMaxSize)
            .build());

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

    //colorAspects
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
    if (mediaType == MEDIA_MIMETYPE_VIDEO_AVC || mediaType == MEDIA_MIMETYPE_VIDEO_HEVC
        || mediaType == MEDIA_MIMETYPE_VIDEO_MPEG2) {
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

C2RKMpiDec::C2RKMpiDec(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mLastPts(-1),
      mOutputEos(false),
      mFlushed(false),
      mAllocWithoutSurface(false),
      mWidth(320),
      mHeight(240),
      mOutIndex(0u),
      mColorFormat(MPP_FMT_YUV420SP),
      mCtx(NULL),
      mCodingType(MPP_VIDEO_CodingUnused),
      mSignalledOutputEos(false) {
    c2_info("version:%s", C2_GIT_BUILD_VERSION);
    int err = getCodingTypeFromComponentName(name, &mCodingType);
    if (err) {
        c2_err("getCodingTypeFromComponentName failed! now codingtype=%d err=%d", mCodingType, err);
    }
}

C2RKMpiDec::~C2RKMpiDec() {
    c2_info_f("in");
    onRelease();
}

c2_status_t C2RKMpiDec::onInit() {
    c2_info_f("in");
    MPP_RET             err = MPP_OK;
    MppCtx           mppCtx = NULL;
    MppApi          *mppMpi = NULL;
    c2_status_t         ret = C2_OK;

    MpiCodecContext* ctx = (MpiCodecContext*)malloc(sizeof(MpiCodecContext));
    if (ctx == NULL) {
        c2_err("MpiCodecContext malloc failed!");
        goto CleanUp;
    }
    memset(ctx, 0, sizeof(*ctx));

    mWidth = mIntf->mSize->width;
    mHeight = mIntf->mSize->height;
    c2_info("width:%d, height: %d", mWidth, mHeight);

    err = mpp_create(&mppCtx, &mppMpi);
    if (err != MPP_OK) {
        c2_err("mpp_create failed!");
        goto CleanUp;
    }

    err = mpp_init(mppCtx, MPP_CTX_DEC, mCodingType);
    if (err != MPP_OK) {
        c2_err("mpp_init failed!");
        goto CleanUp;
    }
    {
        MppFrame          frame = NULL;
        uint32_t      mppFormat = mColorFormat;

        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, mWidth);
        mpp_frame_set_height(frame, mHeight);
        mpp_frame_set_fmt(frame, (MppFrameFormat)mppFormat);
        mppMpi->control(mppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);
        /*
         * in old mpp version, MPP_DEC_SET_FRAME_INFO can't provide
         * stride information, so it can only use unaligned width
         * and height. Infochange is used to solve this problem.
         */
        if (mpp_frame_get_hor_stride(frame) <= 0
                || mpp_frame_get_ver_stride(frame) <= 0) {
            mpp_frame_set_hor_stride(frame, mWidth);
            mpp_frame_set_ver_stride(frame, mHeight);
            mppMpi->control(mppCtx, MPP_DEC_SET_FRAME_INFO, (MppParam)frame);
        }

        c2_info("init block horStride=%d, verStride=%d", mpp_frame_get_hor_stride(frame),
                mpp_frame_get_ver_stride(frame));

        std::vector<std::unique_ptr<C2SettingResult>> failures;
        C2StreamBlockSizeInfo::output blockSize(0u, mpp_frame_get_hor_stride(frame), mpp_frame_get_ver_stride(frame));
        ret = mIntf->config({&blockSize}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err_f("blockSize config failed! ret=0x%x", ret);
            goto CleanUp;
        }

        C2StreamMaxReferenceCountTuning::output maxRefCount(0, kMaxReferenceCount);
        ret = mIntf->config({&maxRefCount}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err_f("maxRefCount config failed! ret=0x%x", ret);
            goto CleanUp;
        }

        mpp_frame_deinit(&frame);
    }

    ctx->mppCtx = mppCtx;
    ctx->mppMpi = mppMpi;
    mCtx = ctx;

    return C2_OK;

CleanUp:
    if (mppCtx) {
        mpp_destroy(mppCtx);
        mppCtx =NULL;
    }
    if (ctx) {
        free(ctx);
        ctx = NULL;
    }

    return C2_CORRUPTED;
}

c2_status_t C2RKMpiDec::onStop() {
    c2_info_f("in");
    c2_status_t ret = C2_OK;

    if (!mFlushed)
        ret = flush();

    return ret;
}

void C2RKMpiDec::onReset() {
    c2_info_f("in");
    (void) onStop();
}

void C2RKMpiDec::onRelease() {
    c2_info_f("in");

    if (mOutBlock) {
        mOutBlock.reset();
    }

    if (!mFlushed)
        flush();

    if (mCtx != NULL) {
        if (mCtx->mCommitList != NULL) {
            delete mCtx->mCommitList;
            mCtx->mCommitList = NULL;
        }

        if (mCtx->mppCtx) {
            mpp_destroy(mCtx->mppCtx);
            mCtx->mppCtx =NULL;
        }

        if (mCtx->frameGroup != NULL) {
            mpp_buffer_group_put(mCtx->frameGroup);
            mCtx->frameGroup = NULL;
        }

        free(mCtx);
        mCtx = NULL;
    }
}

c2_status_t C2RKMpiDec::onFlush_sm() {
    c2_status_t ret = C2_OK;
    ret = flush();
    return ret;
}

c2_status_t C2RKMpiDec::flush() {
    c2_info_f("in");
    c2_status_t ret = C2_OK;
    MPP_RET     err = MPP_OK;

    cleanMppBufferCtx();
    mPtsMaps.clear();

    mSignalledOutputEos = false;
    mOutputEos = false;

    C2StreamBlockCountInfo::output blockCount(0u);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ret = mIntf->config({&blockCount}, C2_MAY_BLOCK, &failures);
    if (ret != C2_OK) {
        c2_err_f("blockCount config failed! ret=0x%x", ret);
        return C2_BAD_INDEX;
    }

    if (mCtx == NULL) {
        return C2_CORRUPTED;
    }

    mpp_buffer_group_clear(mCtx->frameGroup);
    err = mCtx->mppMpi->reset(mCtx->mppCtx);
    if (MPP_OK != err) {
        c2_err("mpi reset failed!");
        return C2_CORRUPTED;
    }
    mFlushed = true;
    return ret;
}

void C2RKMpiDec::fillEmptyWork(const std::unique_ptr<C2Work> &work) {
    c2_info_f("in");
    uint32_t flags = 0;
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
    std::shared_ptr<C2Buffer> buffer = NULL;

    if (block) {
        buffer = createGraphicBuffer(std::move(block), C2Rect(mWidth, mHeight));
    } else {
        c2_err("empty block index:%d",index);
    }
    mOutBlock = nullptr;
    {
        if (buffer && (mCodingType == MPP_VIDEO_CodingAVC || mCodingType == MPP_VIDEO_CodingHEVC
            || mCodingType == MPP_VIDEO_CodingMPEG2)) {
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
        if(buffer)
            work->worklets.front()->output.buffers.push_back(buffer);
        work->worklets.front()->output.ordinal = work->input.ordinal;
        work->workletsProcessed = 1u;
    };
    if (!mAllocWithoutSurface) {
        C2StreamBlockCountInfo::output blockCount(0u);
        std::vector<std::unique_ptr<C2Param>> params;
        c2_status_t err = intf()->query_vb(
                                { &blockCount },
                                {},
                                C2_DONT_BLOCK,
                                &params);

        blockCount.value--;
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        err = mIntf->config({&blockCount}, C2_MAY_BLOCK, &failures);
        if (err != C2_OK) {
            c2_err_f("blockCount config failed! ret=0x%x", err);
        }
        c2_trace_f("block count=%d", blockCount.value);
    }

    if (work && c2_cntr64_t(index) == work->input.ordinal.frameIndex) {
        bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
        if (eos) {
            if (buffer) {
                mOutIndex = index;
                C2WorkOrdinalStruct outOrdinal = work->input.ordinal;
                cloneAndSend(
                    mOutIndex, work,
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

void C2RKMpiDec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err             = C2_OK;
    uint32_t pkt_done           = 0;
    uint32_t retry              = 0;
    mFlushed = false;
    // Initialize output work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    mAllocWithoutSurface = (pool->getLocalId() > C2BlockPool::PLATFORM_START) ? false : true;

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
            c2_err("read view map failed, error=%d", rView.error());
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
        fillEmptyWork(work);
        c2_info("eos and input work is empty!");
        mSignalledOutputEos = true;
    }

    do {
        err = mAllocWithoutSurface ? ensureBlockWithoutSurface(pool) : ensureMppGroupReady(pool);
        if (err != C2_OK) {
            //TODO when err happen,do something
            c2_warn_f("ensure buffer failed! err=%d", err);
            continue;
        }
        err = decode_sendstream(work);
        if (err != C2_OK) {
            // the work should be try again
            c2_trace("decode_sendstream failed, retry！");
            retry++;
            usleep(5 * 1000);
            if(retry > kMaxRetryNum) pkt_done = 1;
        } else {
            pkt_done = 1;
        }
        err = decode_getoutframe(work);
        if (err != C2_OK) {
            // nothing to do
            c2_trace_f("decode_getoutframe failed!");
        }
    } while (!pkt_done);

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

bool C2RKMpiDec::getVuiParams(MppFrame *frame) {
    if (frame == nullptr) {
        c2_err_f("frame is null!");
        return false;
    }

    VuiColorAspects vuiColorAspects;
    vuiColorAspects.primaries = mpp_frame_get_color_primaries(*frame);
    vuiColorAspects.transfer = mpp_frame_get_color_trc(*frame);
    vuiColorAspects.coeffs = mpp_frame_get_colorspace(*frame);
    vuiColorAspects.fullRange = (mCodingType == MPP_VIDEO_CodingMPEG2) ? false :
        (mpp_frame_get_color_range(*frame) == MPP_FRAME_RANGE_JPEG);

    c2_trace("coloraspects: pri:%d tra:%d coeff:%d range:%d",vuiColorAspects.primaries,
        vuiColorAspects.transfer, vuiColorAspects.coeffs, vuiColorAspects.fullRange);

    // convert vui aspects to C2 values if changed
    if (!(vuiColorAspects == mBitstreamColorAspects)) {
        mBitstreamColorAspects = vuiColorAspects;
        ColorAspects sfAspects;
        C2StreamColorAspectsInfo::input codedAspects = { 0u };
        ColorUtils::convertIsoColorAspectsToCodecAspects(
                vuiColorAspects.primaries, vuiColorAspects.transfer, vuiColorAspects.coeffs,
                vuiColorAspects.fullRange, sfAspects);
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
        (void)mIntf->config({&codedAspects}, C2_MAY_BLOCK, &failures);
    }
    return true;
}

c2_status_t C2RKMpiDec::decode_sendstream(const std::unique_ptr<C2Work> &work) {
    uint64_t     timestamps  = -1ll;
    uint64_t     frameindex = -1ll;
    uint8_t     *inData     = NULL;
    size_t      inSize      = 0u;
    MppPacket   mppPkt      = NULL;
    MPP_RET     err         = MPP_OK;
    c2_status_t ret         = C2_OK;

    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
    }

    frameindex = work->input.ordinal.frameIndex.peekull();
    timestamps = work->input.ordinal.timestamp.peekll();

    inData = const_cast<uint8_t *>(rView.data());
    mpp_packet_init(&mppPkt, inData, inSize);

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        mpp_packet_set_eos(mppPkt);
    }

    mpp_packet_set_pts(mppPkt, timestamps);

    if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mpp_packet_set_extra_data(mppPkt);
    }

    mpp_packet_set_pos(mppPkt, inData);
    mpp_packet_set_length(mppPkt, inSize);

    err = mCtx->mppMpi->decode_put_packet(mCtx->mppCtx, mppPkt);
    if (MPP_OK != err) {
        c2_trace("decode_put_packet fail,retry!");
        ret = C2_NOT_FOUND;
        goto CleanUp;
    }else{
        if (!(work->input.flags & (C2FrameData::FLAG_CODEC_CONFIG | FLAG_NON_DISPLAY_FRAME))){
            mPtsMaps[frameindex] = timestamps;
            if(mLastPts != timestamps) {
                mLastPts = timestamps;
            }else{
                //return this work when timestamps repeat
                if (mCodingType == MPP_VIDEO_CodingMPEG2)
                    fillEmptyWork(work);
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

CleanUp:
    if (mppPkt) {
        mpp_packet_deinit(&mppPkt);
    }
    return ret;
}

c2_status_t C2RKMpiDec::decode_getoutframe(
        const std::unique_ptr<C2Work> &work) {
    MPP_RET     err         = MPP_OK;
    c2_status_t ret         = C2_OK;
    MppFrame    mppFrame    = NULL;
    uint32_t    infochange  = 0;

    err = mCtx->mppMpi->decode_get_frame(mCtx->mppCtx, &mppFrame);
    if (MPP_OK != err || NULL == mppFrame) {
        c2_trace("decode_get_frame,retry! ret: %d", err);
        ret = C2_NOT_FOUND;
        goto CleanUp;
    }

    infochange = mpp_frame_get_info_change(mppFrame);
    if (infochange && !mpp_frame_get_eos(mppFrame)) {
        int32_t width = mpp_frame_get_width(mppFrame);
        int32_t height = mpp_frame_get_height(mppFrame);
        int32_t hor_stride = mpp_frame_get_hor_stride(mppFrame);
        int32_t ver_stride = mpp_frame_get_ver_stride(mppFrame);
        MppFrameFormat format = mpp_frame_get_fmt(mppFrame);

        c2_info("infochange happen, decoder require buffer w:h [%d:%d] stride [%d:%d]",
                    width, height, hor_stride, ver_stride);

        cleanMppBufferCtx();
        err = mpp_buffer_group_clear(mCtx->frameGroup);
        if (err) {
            c2_err("mpp_buffer_group_clear failed! ret=%d\n", ret);
            ret = C2_CORRUPTED;
            goto CleanUp;
        }
        mWidth = width;
        mHeight = height;
        mColorFormat = format;
        if (!mAllocWithoutSurface) {
            C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
            C2StreamBlockSizeInfo::output   blockSize(0u, hor_stride, ver_stride);
            C2StreamBlockCountInfo::output  blockCount(0u);
            std::vector<std::unique_ptr<C2SettingResult>> failures;
            ret = mIntf->config({ &size, &blockSize, &blockCount },
                                C2_MAY_BLOCK,
                                &failures);
            if (ret != C2_OK) {
                c2_err_f("size and blockSize config failed!");
                goto CleanUp;
            }
        }
        /*
         * All buffer group config done. Set info change ready to let
         * decoder continue decoding
         */
        err = mCtx->mppMpi->control(mCtx->mppCtx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        if (err) {
            c2_err_f("control MPP_DEC_SET_INFO_CHANGE_READY failed! ret=%d\n", ret);
            ret = C2_CORRUPTED;
            goto CleanUp;
        }

        ret = C2_NO_MEMORY;
    } else {
        c2_trace("frame info(mpp_frame=%p horStride=%d verStride=%d width=%d height=%d"
                    " pts=%lld dts=%lld err=%d eos=%d)",mppFrame,
                mpp_frame_get_hor_stride(mppFrame), mpp_frame_get_ver_stride(mppFrame),
                mpp_frame_get_width(mppFrame), mpp_frame_get_height(mppFrame),
                mpp_frame_get_pts(mppFrame), mpp_frame_get_dts(mppFrame),
                mpp_frame_get_errinfo(mppFrame), mpp_frame_get_eos(mppFrame));

        uint64_t timestamp = (uint64_t)mpp_frame_get_pts(mppFrame);
        uint64_t workIndex = -1;
        MppBufferCtx* bufferCtx = NULL;
        std::shared_ptr<C2GraphicBlock> block = NULL;
        MppBuffer mppBuffer = mpp_frame_get_buffer(mppFrame);

        if (mppBuffer == NULL) {
            if (mpp_frame_get_eos(mppFrame)) {
                mOutputEos = true;
                c2_info_f("mpp_frame_get_eos true!");
            }
            c2_err("empty mppBuffer!");
            goto CleanUp;
        }

        for (auto it_ts = mPtsMaps.begin(); it_ts != mPtsMaps.end(); it_ts++) {
            if (timestamp == it_ts->second) {
                workIndex = it_ts->first;
                mPtsMaps.erase(it_ts);
                break;
            }
        }
        if (workIndex == -1){
            c2_err("can't find timestamp:%llu !", timestamp);
            for (auto it_ts = mPtsMaps.begin(); it_ts != mPtsMaps.end(); it_ts++) {
                if (0 == it_ts->second) {
                    workIndex = it_ts->first;
                    mPtsMaps.erase(it_ts);
                    break;
                }
            }
        }
        if (mAllocWithoutSurface) {
            int32_t width = mpp_frame_get_width(mppFrame);
            int32_t height = mpp_frame_get_height(mppFrame);
            int32_t hor_stride = mpp_frame_get_hor_stride(mppFrame);
            int32_t ver_stride = mpp_frame_get_ver_stride(mppFrame);
            int32_t fd_src = mpp_buffer_get_fd(mppBuffer);
            auto c2Handle = mOutBlock->handle();
            int32_t fd_dst = c2Handle->data[0];
            RgaParam srcParam, dstParam;
            if (fd_dst == -1) {
                c2_err("empty frame!");
                goto CleanUp;
            }
            C2RKRgaDef::paramInit(&srcParam, fd_src,
                                    width, height, hor_stride, ver_stride);
            C2RKRgaDef::paramInit(&dstParam, fd_dst,
                                    width, height, hor_stride, ver_stride);
            if (!C2RKRgaDef::nv12Copy(srcParam, dstParam)) {
                c2_err("nv12Copy failed!");
                // return C2_BAD_VALUE;
            }
            block = mOutBlock;
        } else {
            bufferCtx = (MppBufferCtx*)findMppBufferCtx(mppBuffer);
            if (bufferCtx == NULL || bufferCtx->mBlock == NULL){
                c2_err("can't find block!");
                goto CleanUp;
            }
            bufferCtx->mSite = BUFFER_SITE_BY_C2;
            if (mppBuffer)
                mpp_buffer_inc_ref(mppBuffer);
            block = bufferCtx->mBlock;
        }

        if (mCodingType == MPP_VIDEO_CodingAVC || mCodingType == MPP_VIDEO_CodingHEVC
            || mCodingType == MPP_VIDEO_CodingMPEG2)
            (void)getVuiParams(&mppFrame);

        if (mpp_frame_get_eos(mppFrame)) {
            mOutputEos = true;
            c2_info_f("mpp_frame_get_eos true!");
        }
        c2_trace("frame index: %llu", workIndex);
        finishWork(workIndex, work, block);
    }

CleanUp:
    /*
    * IMPORTANT: mppFrame is malloced from mpi->decode_get_frame
    * So we need to deinit mppFrame here. But the buffer in the frame should not be free with mppFrame.
    * Because buffer need to be set to vframe->vpumem.offset and send to display.
    * We have to clear the buffer pointer in mppFrame then release mppFrame.
    */
    if (mppFrame)
        mpp_frame_deinit(&mppFrame);
    return ret;
}

c2_status_t C2RKMpiDec::drainInternal(
    uint32_t drainMode,
    const std::shared_ptr<C2BlockPool> &pool,
    const std::unique_ptr<C2Work> &work) {
    c2_info_f("in");
    c2_status_t ret = C2_OK;
    uint32_t retry = 0;

    if (drainMode == NO_DRAIN) {
        c2_warn("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        c2_warn("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    while (!mOutputEos){
        ret = mAllocWithoutSurface ? ensureBlockWithoutSurface(pool) : ensureMppGroupReady(pool);
        if (ret != C2_OK) {
            c2_warn_f("can't ensure buffer! ret=%d", ret);
        }
        ret = decode_getoutframe(work);
        retry++;
        usleep(5 * 1000);

        //avoid infinite loop
        if (retry > kMaxRetryNum)
            mOutputEos = true;
        if(mOutputEos)
            fillEmptyWork(work);
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    return drainInternal(drainMode, pool, nullptr);
}

c2_status_t C2RKMpiDec::ensureBlockWithoutSurface(
        const std::shared_ptr<C2BlockPool> &pool) {
    std::shared_ptr<C2GraphicBlock> outblock;
    c2_status_t                     ret     = C2_OK;
    uint32_t                        frameH  = 0;
    uint32_t                        frameW  = 0;
    // TODO() format can't define here
    uint32_t                        format  = HAL_PIXEL_FORMAT_YCrCb_NV12;

    format = colorFormatMpiToAndroid(mColorFormat);

    C2StreamBlockSizeInfo::output blockSize(0u, 0, 0);
    std::vector<std::unique_ptr<C2Param>> params;
    ret = intf()->query_vb(
            { &blockSize },
            {},
            C2_DONT_BLOCK,
            &params);
    if (ret) {
        c2_err_f("query params failed!");
        return ret;
    }

    frameW = blockSize.width;
    frameH = blockSize.height;

    if (frameW <= 0 || frameH <= 0) {
        c2_err("query block width or height failed, width=%d, height=%d", frameW, frameH);
        return ret;
    }

    uint32_t usage = (GRALLOC_USAGE_SW_READ_OFTEN
                      | GRALLOC_USAGE_SW_WRITE_OFTEN
                      | RK_GRALLOC_USAGE_SPECIFY_STRIDE);

    if (mOutBlock &&
            (mOutBlock->width() != frameW || mOutBlock->height() != frameH)) {
        mOutBlock.reset();
    }
    if (!mOutBlock) {
        ret = pool->fetchGraphicBlock(frameW,
                                      frameH,
                                      format,
                                      C2AndroidMemoryUsage::FromGrallocUsage(usage),
                                      &mOutBlock);
        if (ret != C2_OK) {
            c2_err("fetchGraphicBlock failed with status %d", ret);
            return ret;
        }
        c2_trace("provided (%dx%d) required (%dx%d)",
              mOutBlock->width(), mOutBlock->height(), frameW, frameH);
    }

    return ret;
}

c2_status_t C2RKMpiDec::ensureMppGroupReady(
        const std::shared_ptr<C2BlockPool> &pool) {
    std::shared_ptr<C2GraphicBlock> outblock;
    c2_status_t                     ret     = C2_OK;
    MPP_RET                         err     = MPP_OK;
    uint32_t                        frameH  = 0;
    uint32_t                        frameW  = 0;
    // TODO() format can't define here
    uint32_t                        format  = HAL_PIXEL_FORMAT_YCrCb_NV12;
    MppBufferGroup                  frmGrp  = NULL;
    if(mCtx->frameGroup == NULL) {
        c2_info("alloc with surface,get external frame buffer group");
        // init frame grp
        err = mpp_buffer_group_get_external(&frmGrp, MPP_BUFFER_TYPE_ION);
        if (err != MPP_OK) {
            c2_err_f("mpp_buffer_group_get_external failed!");
            return C2_BAD_INDEX;
        }
        mCtx->mppMpi->control(mCtx->mppCtx, MPP_DEC_SET_EXT_BUF_GROUP, frmGrp);
        mCtx->frameGroup = frmGrp;
        mpp_buffer_group_clear(mCtx->frameGroup);
        mCtx->mCommitList = new Vector<MppBufferCtx*>;
    }

    format = colorFormatMpiToAndroid(mColorFormat);

    C2StreamMaxReferenceCountTuning::output maxRefsCount(0u);
    C2StreamBlockSizeInfo::output blockSize(0u, 0, 0);
    C2StreamBlockCountInfo::output blockCount(0u);
    std::vector<std::unique_ptr<C2Param>> params;
    ret = intf()->query_vb(
            { &maxRefsCount, &blockSize, &blockCount },
            {},
            C2_DONT_BLOCK,
            &params);
    if (ret) {
        c2_err("ensureMppGroupReady query params failed!");
        return ret;
    }

    frameW = blockSize.width;
    frameH = blockSize.height;

    if (frameW <= 0 || frameH <= 0) {
        c2_err("query frameW or frameH failed, frameW=%d, frameH=%d", frameW, frameH);
        return ret;
    }
    c2_trace("query param, frameW=%d, frameH=%d, maxRefsCount=%d, blockCount=%d",
                blockSize.width, blockSize.height, maxRefsCount.value, blockCount.value);
    if (blockCount.value == maxRefsCount.value) {
        // nothing todo
        return ret;
    }

    uint32_t i = 0;
    uint32_t count = 0;
    uint32_t usage = (GRALLOC_USAGE_SW_READ_OFTEN
                      | GRALLOC_USAGE_SW_WRITE_OFTEN
                      | GRALLOC_USAGE_HW_TEXTURE
                      | GRALLOC_USAGE_HW_COMPOSER
                      | RK_GRALLOC_USAGE_SPECIFY_STRIDE);

    // TODO: it should use max reference count
    for (i = 0; i < maxRefsCount.value - blockCount.value; i++) {
        ret = pool->fetchGraphicBlock(frameW,
                                      frameH,
                                      format,
                                      C2AndroidMemoryUsage::FromGrallocUsage(usage),
                                      &outblock);
        if (ret != C2_OK) {
            c2_warn("fetchGraphicBlock for Output failed with status %d", ret);
            break;
        }
        c2_trace("provided (%dx%d) required (%dx%d) index(%d)",
              outblock->width(), outblock->height(), frameW, frameH, i);
        ret = registerBufferToMpp(outblock);
        if (ret != C2_OK) {
            c2_err("register buffer to mpp failed with status %d", ret);
            break;
        }
        count++;
    }

    blockCount.value += count;
    c2_trace("%s %d block count=%d", __FUNCTION__, __LINE__, blockCount.value);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ret = mIntf->config({&blockCount}, C2_MAY_BLOCK, &failures);
    if (ret != C2_OK) {
        c2_err_f("blockCount config failed! ret=0x%x", ret);
        return C2_BAD_INDEX;
    }

    return ret;
}

c2_status_t C2RKMpiDec::registerBufferToMpp(std::shared_ptr<C2GraphicBlock> block) {
    uint32_t                      shareFd = 0;
    MppBufferCtx*               bufferCtx = NULL;
    uint32_t bqSlot, width, height, format, stride, generation;
    uint64_t usage, bqId;

    if (!block.get()) {
        c2_err("can't get block!");
        return C2_CORRUPTED;
    }

    auto c2Handle = block->handle();
    shareFd = c2Handle->data[0];

    android::_UnwrapNativeCodec2GrallocMetadata(
            c2Handle, &width, &height, &format, &usage, &stride, &generation, &bqId, &bqSlot);

    c2_trace_f("generation:%d bqId:%lld bqslot:%d shareFd:%d",
            generation, bqId, bqSlot, shareFd);

    std::map<uint32_t, void *>::iterator it;
    bufferCtx = (MppBufferCtx*)findMppBufferCtx((int32_t)bqSlot);
    if (bufferCtx != NULL) {
        //return buffer back to mpp
        MppBuffer mppBuffer = bufferCtx->mMppBuffer;
        bufferCtx->mSite = BUFFER_SITE_BY_MPI;
        if (mppBuffer != NULL)
            mpp_buffer_put(mppBuffer);
        bufferCtx->mBlock = block;

        c2_trace_f("mppBuffer:%p outBlock:%p block fd=%d slot=%d",
                mppBuffer, block.get(), shareFd, bqSlot);
    } else {
        // register this buffer to mpp grp.
        MppBufferInfo info;
        MppBuffer mppBuffer = NULL;
        memset(&info, 0, sizeof(info));
        info.type = MPP_BUFFER_TYPE_ION;
        info.fd = shareFd;
        info.ptr = NULL;
        info.hnd = NULL;
        info.size = width * height * 2;

        mpp_buffer_import_with_tag(mCtx->frameGroup,
                                &info,
                                &mppBuffer,
                                "codec2-Mpp-Group",
                                __FUNCTION__);


        c2_trace_f("width:%d height:%d mppBuffer:%p outBlock:%p info.fd=%d block fd=%d slot=%d",
                width, height, mppBuffer, block.get(), info.fd, shareFd, bqSlot);

        bufferCtx = new MppBufferCtx();
        bufferCtx->mIndex = (int32_t)bqSlot;
        bufferCtx->mMppBuffer = mppBuffer;
        bufferCtx->mBlock = block;
        bufferCtx->mSite = BUFFER_SITE_BY_MPI;
        mCtx->mCommitList->push(bufferCtx);
        mpp_buffer_put(mppBuffer);
    }

    return C2_OK;
}

void* C2RKMpiDec::findMppBufferCtx(int32_t index) {
    MppBufferCtx* bufferCtx = NULL;
    if (mCtx != NULL && mCtx->mCommitList != NULL) {
        for (int i = 0; i < mCtx->mCommitList->size(); i++) {
            bufferCtx = mCtx->mCommitList->editItemAt(i);
            if (bufferCtx && bufferCtx->mIndex == index) {
                return (void*)bufferCtx;
            }
        }
    }

    return NULL;
}

void* C2RKMpiDec::findMppBufferCtx(MppBuffer buffer) {
    MppBufferCtx* bufferCtx = NULL;
    if (mCtx != NULL && mCtx->mCommitList != NULL) {
        for (int i = 0; i < mCtx->mCommitList->size(); i++) {
            bufferCtx = mCtx->mCommitList->editItemAt(i);
            if (bufferCtx && bufferCtx->mMppBuffer == buffer) {
                return (void*)bufferCtx;
            }
        }
    }

    return NULL;
}

void C2RKMpiDec::cleanMppBufferCtx() {
    while (mCtx != NULL && mCtx->mCommitList != NULL && !mCtx->mCommitList->isEmpty()) {
        MppBufferCtx* buffer = mCtx->mCommitList->editItemAt(0);
        if (buffer != NULL) {
            if (buffer->mSite == BUFFER_SITE_BY_C2)
                mpp_buffer_put(buffer->mMppBuffer);
            delete buffer;
        }

        mCtx->mCommitList->removeAt(0);
    }
}

void C2RKMpiDec::cleanMppBufferCtx(int32_t site) {
    if (mCtx != NULL && mCtx->mCommitList != NULL) {
        for (int i = mCtx->mCommitList->size()-1; i >= 0; i--) {
            MppBufferCtx* buffer = mCtx->mCommitList->editItemAt(i);
            if (buffer != NULL && buffer->mSite == site) {
                delete buffer;
                mCtx->mCommitList->removeAt(i);
            }
        }
    }
}

class C2RKMpiDecFactory : public C2ComponentFactory {
public:
    C2RKMpiDecFactory(std::string componentName)
            : mHelper(std::static_pointer_cast<C2ReflectorHelper>(
                  GetCodec2PlatformComponentStore()->getParamReflector())),
              mComponentName(componentName) {
        int err = getMimeFromComponentName(componentName, &mMime);
        if (err) {
            c2_err("getMimeFromComponentName failed, component name=%s", componentName.c_str());
        }
        err = getDomainFromComponentName(componentName, &mDomain);
        if (err) {
            c2_err("getDomainFromComponentName failed, component name=%s", componentName.c_str());
        }
        err = getKindFromComponentName(componentName, &mKind);
        if (err) {
            c2_err("getKindFromComponentName failed, component name=%s", componentName.c_str());
        }
    }

    virtual c2_status_t createComponent(
            c2_node_id_t id,
            std::shared_ptr<C2Component>* const component,
            std::function<void(C2Component*)> deleter) override {
        c2_trace("%s %d in", __FUNCTION__, __LINE__);
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
