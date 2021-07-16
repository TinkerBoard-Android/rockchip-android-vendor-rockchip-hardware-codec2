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
#include "C2RKVersion.h"
#define GRALLOC_USAGE_PRIVATE_2 1ULL << 30
#define GRALLOC_USAGE_HW_TEXTURE 1ULL << 8
#define GRALLOC_USAGE_EXTERNAL_DISP 1ULL << 13

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
                C2F(mSize, width).inRange(2, 4096, 2),
                C2F(mSize, height).inRange(2, 4096, 2),
            })
            .withSetter(SizeSetter)
            .build());

    addParameter(
            DefineParam(mBlockSize, C2_PARAMKEY_BLOCK_SIZE)
            .withDefault(new C2StreamBlockSizeInfo::output(0u, 320, 240))
            .withFields({
                C2F(mBlockSize, width).inRange(2, 8192, 2),
                C2F(mBlockSize, height).inRange(2, 4096, 2),
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
                        C2Config::PROFILE_AVC_BASELINE,
                        C2Config::PROFILE_AVC_HIGH
                        }),
                C2F(mProfileLevel, level).oneOf({
                        C2Config::LEVEL_AVC_1, C2Config::LEVEL_AVC_1B, C2Config::LEVEL_AVC_1_1,
                        C2Config::LEVEL_AVC_1_2, C2Config::LEVEL_AVC_1_3,
                        C2Config::LEVEL_AVC_2, C2Config::LEVEL_AVC_2_1, C2Config::LEVEL_AVC_2_2,
                        C2Config::LEVEL_AVC_3, C2Config::LEVEL_AVC_3_1, C2Config::LEVEL_AVC_3_2,
                        C2Config::LEVEL_AVC_4, C2Config::LEVEL_AVC_4_1, C2Config::LEVEL_AVC_4_2,
                        C2Config::LEVEL_AVC_5, C2Config::LEVEL_AVC_5_1
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

C2RKMpiDec::C2RKMpiDec(
        const char *name,
        c2_node_id_t id,
        const std::shared_ptr<IntfImpl> &intfImpl)
    : C2RKComponent(std::make_shared<C2RKInterface<IntfImpl>>(name, id, intfImpl)),
      mIntf(intfImpl),
      mFindIndex(0l),
      mOutputEos(false),
      mLastTimestamp(-1ll),
      mLastFrameIndex(-1l),
      mIndex(0l),
      mWidth(320),
      mHeight(240),
      mOutIndex(0u),
      mColorFormat(MPP_FMT_YUV420SP),
      mCtx(NULL),
      mCodingType(MPP_VIDEO_CodingUnused),
      mSignalledOutputEos(false) {
    FunctionIn();
    c2_info("version:%s", C2_GIT_BUILD_VERSION);
    int err = getCodingTypeFromComponentName(name, &mCodingType);
    if (err) {
        c2_err("get coding type from component name failed! now codingtype=%d", mCodingType);
    }

    FunctionOut();
}

C2RKMpiDec::~C2RKMpiDec() {
    c2_info("%s in", __func__);
    onRelease();
}

c2_status_t C2RKMpiDec::onInit() {
    c2_info("%s in", __func__);

    MPP_RET         err     = MPP_OK;
    MppCtx          mppCtx  = NULL;
    MppApi         *mppMpi  = NULL;
    MppBufferGroup  frmGrp  = NULL;
    c2_status_t     ret     = C2_OK;

    MpiCodecContext* ctx = (MpiCodecContext*)malloc(sizeof(MpiCodecContext));
    if (ctx == NULL) {
        c2_err("%s:%d malloc fail", __FUNCTION__, __LINE__);
        goto __FAILED;
    }

    memset(ctx, 0, sizeof(*ctx));

    mWidth = mIntf->mSize->width;
    mHeight = mIntf->mSize->height;
    c2_info("width:%d, height: %d", mWidth, mHeight);
    err = mpp_create(&mppCtx, &mppMpi);
    if (err != MPP_OK) {
        c2_err("%s:%d create fail", __FUNCTION__, __LINE__);
        goto __FAILED;
    }

    err = mpp_init(mppCtx, MPP_CTX_DEC, mCodingType);
    if (err != MPP_OK) {
        c2_err("%s unsupport rockit codec id: 0x%x", __FUNCTION__, mCodingType);
        goto __FAILED;
    }

    {
        MppFrame frame = NULL;

        uint32_t mppFormat = mColorFormat;
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

        c2_info("init block width=%d, height=%d", mpp_frame_get_hor_stride(frame), mpp_frame_get_ver_stride(frame));
        C2StreamBlockSizeInfo::output blockSize(0u,
                                                mpp_frame_get_hor_stride(frame),
                                                mpp_frame_get_ver_stride(frame));
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        ret = mIntf->config({&blockSize}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err("block size config failed!");
            goto __FAILED;
        }

        C2StreamMaxReferenceCountTuning::output maxRefCount(0, kMaxReferenceCount);
        ret = mIntf->config({&maxRefCount}, C2_MAY_BLOCK, &failures);
        if (ret != C2_OK) {
            c2_err("max refs count config failed! ret=0x%x", ret);
            goto __FAILED;
        }

        mpp_frame_deinit(&frame);
    }

    // init frame grp
    err = mpp_buffer_group_get_external(&frmGrp, MPP_BUFFER_TYPE_ION);
    if (err != MPP_OK) {
        c2_err("%s unsupport rockit codec id: 0x%x", __FUNCTION__, MPP_VIDEO_CodingAVC);
        goto __FAILED;
    }

    // TODO(control cmds)
    mppMpi->control(mppCtx, MPP_DEC_SET_EXT_BUF_GROUP, frmGrp);

    ctx->mppCtx = mppCtx;
    ctx->mppMpi = mppMpi;
    ctx->frameGroup = frmGrp;

    mpp_buffer_group_clear(ctx->frameGroup);

    mCtx = ctx;

    return C2_OK;
__FAILED:

    if (mppCtx) {
        mpp_destroy(mppCtx);
        mppCtx =NULL;
    }
    if (ctx) {
        free(ctx);
        ctx = NULL;
    }

    mCtx = NULL;
    return C2_CORRUPTED;
}

c2_status_t C2RKMpiDec::onStop() {
    c2_info("%s in", __func__);
    mSignalledOutputEos = false;
    mOutputEos = false;
    if (mCtx == NULL) {
        return C2_OK;
    }

    MpiCodecContext* ctx = (MpiCodecContext*)mCtx;
    int ret = ctx->mppMpi->reset(ctx->mppCtx);
    if (MPP_OK != ret) {
        c2_err("mpi->reset failed\n");
        return C2_CORRUPTED;
    }
    return C2_OK;
}

void C2RKMpiDec::onReset() {
    c2_info("%s in", __func__);
    (void) onStop();
}

void C2RKMpiDec::onRelease() {
    c2_info("%s in", __func__);
    onFlush_sm();

    if (mCtx == NULL) {
        return;
    }

    if (mCtx->frameGroup != NULL) {
        mpp_buffer_group_put(mCtx->frameGroup);
        mCtx->frameGroup = NULL;
    }

    if (mCtx->mppCtx) {
        mpp_destroy(mCtx->mppCtx);
        mCtx->mppCtx =NULL;
    }
    if (mCtx) {
        free(mCtx);
        mCtx = NULL;
    }
    c2_trace("%s %d in mCtx=%p", __FUNCTION__, __LINE__, mCtx);
}

c2_status_t C2RKMpiDec::onFlush_sm() {
    c2_status_t ret = C2_OK;
    MPP_RET err = MPP_OK;

    mSignalledOutputEos = false;
    mOutputEos = false;

    // all buffer should dec ref for frame group
    for (const std::pair<uint32_t, void *> &it : mBufferMaps) {
        mpp_buffer_put(it.second);
    }

    mBufferMaps.clear();
    mBlockMaps.clear();
    mFrameIndexMaps.clear();
    mIndex = 0l;

    C2StreamBlockCountInfo::output blockCount(0u);
    std::vector<std::unique_ptr<C2SettingResult>> failures;
    ret = mIntf->config({&blockCount}, C2_MAY_BLOCK, &failures);
    if (ret != C2_OK) {
        c2_err("block count config failed!");
        return C2_BAD_INDEX;
    }

    if (mCtx == NULL) {
        return C2_CORRUPTED;
    }

    mpp_buffer_group_clear(mCtx->frameGroup);
    err = mCtx->mppMpi->reset(mCtx->mppCtx);
    if (MPP_OK != err) {
        c2_err("mpi->reset failed\n");
        return C2_CORRUPTED;
    }
    return ret;
}

void C2RKMpiDec::fillEmptyWork(const std::unique_ptr<C2Work> &work) {
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
    (void)work;
    std::shared_ptr<C2Buffer> buffer = NULL;
    if (block) {
        buffer = createGraphicBuffer(std::move(block), C2Rect(mWidth, mHeight));
    }
    {
        if (mCodingType == MPP_VIDEO_CodingAVC || mCodingType == MPP_VIDEO_CodingHEVC
            || mCodingType ==MPP_VIDEO_CodingMPEG2) {
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
        c2_err("block count config failed!");
    }
    c2_trace("%s %d block count=%d", __FUNCTION__, __LINE__, blockCount.value);

    finish(index, fillWork);
}

void C2RKMpiDec::finishNewWork(
        std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2GraphicBlock> block) {
    std::shared_ptr<C2Buffer> buffer = NULL;
    if (block) {
        buffer = createGraphicBuffer(std::move(block), C2Rect(mWidth, mHeight));
    }
    {
        if (mCodingType == MPP_VIDEO_CodingAVC || mCodingType == MPP_VIDEO_CodingHEVC
            || mCodingType ==MPP_VIDEO_CodingMPEG2) {
            IntfImpl::Lock lock = mIntf->lock();
            buffer->setInfo(mIntf->getColorAspects_l());
        }
    }

    auto fillWork = [buffer](const std::unique_ptr<C2Work> &work) {
        work->worklets.front()->output.buffers.clear();
        if(buffer)
            work->worklets.front()->output.buffers.push_back(buffer);
        work->workletsProcessed = 1u;
    };
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
        c2_err("block count config failed!");
    }
    c2_trace("%s %d block count=%d", __FUNCTION__, __LINE__, blockCount.value);

    finish(work, fillWork);
}

void C2RKMpiDec::process(
        const std::unique_ptr<C2Work> &work,
        const std::shared_ptr<C2BlockPool> &pool) {
    c2_status_t err             = C2_OK;
    uint32_t    inputComplete   = 0;

    if (mSignalledOutputEos) {
        work->result = C2_BAD_VALUE;
        return;
    }

    bool eos = ((work->input.flags & C2FrameData::FLAG_END_OF_STREAM) != 0);
    while (!inputComplete) {
        err = ensureMppGroupReady(pool);
        if (err != C2_OK) {
            ALOGW("can't ensure mpp group buffer enough! err=%d", err);
        }
        err = decode_sendstream(work);
        if (err != C2_OK) {
            // the work should be try again
            c2_info("stream list is full,retry");
        } else {
            inputComplete = 1;
        }

        err = decode_getoutframe(work);
        if (err != C2_OK) {
            // nothing to do
            c2_err("handle output work failed");
        }
        usleep(5 * 1000);
    }
    if(eos){
        drainInternal(DRAIN_COMPONENT_WITH_EOS, pool, work);
        mSignalledOutputEos = true;
    }

    c2_trace("%s %d in", __FUNCTION__, __LINE__);
}

bool C2RKMpiDec::getVuiParams(MppFrame *frame) {
    if (frame == nullptr) {
        c2_err("%s %d frame is null", __func__, __LINE__);
        return false;
    }

    VuiColorAspects vuiColorAspects;
    vuiColorAspects.primaries = mpp_frame_get_color_primaries(*frame);
    vuiColorAspects.transfer = mpp_frame_get_color_trc(*frame);
    vuiColorAspects.coeffs = mpp_frame_get_colorspace(*frame);
    vuiColorAspects.fullRange = (mpp_frame_get_color_range(*frame) == MPP_FRAME_RANGE_JPEG);

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
    uint32_t     frameindex = -1ll;
    uint8_t     *inData     = NULL;
    size_t      inSize      = 0u;
    MppPacket   mppPkt      = NULL;
    MPP_RET     err         = MPP_OK;
    c2_status_t ret         = C2_OK;

    // Initialize work
    work->result = C2_OK;
    work->workletsProcessed = 0u;
    work->worklets.front()->output.flags = work->input.flags;

    C2ReadView rView = mDummyReadView;
    if (!work->input.buffers.empty()) {
        rView = work->input.buffers[0]->data().linearBlocks().front().map().get();
        inSize = rView.capacity();
        if (inSize && rView.error()) {
            c2_err("read view map failed %d", rView.error());
            work->result = rView.error();
            ret = C2_CORRUPTED;
            goto __FAILED;
        }
    }
    if(work->input.flags == 0){
        frameindex = work->input.ordinal.frameIndex.peeku() & 0xFFFFFFFF;
        timestamps = work->input.ordinal.timestamp.peekll();
        if(mLastFrameIndex != frameindex){
            mLastFrameIndex = frameindex;
            if(mLastTimestamp != timestamps){
                mLastTimestamp = timestamps;
                mFrameIndexMaps[timestamps] = frameindex;
            }else if(mLastTimestamp == timestamps) {
                mIndex++;
                mFrameIndexMaps[mLastTimestamp + mIndex] = frameindex;
                timestamps = mLastTimestamp + mIndex;
            }
        }
    }else{
        timestamps = work->input.ordinal.timestamp.peekll();
    }
    inData = const_cast<uint8_t *>(rView.data());
    c2_trace("in buffer attr. size %zu timestamp %d frameindex %d, flags %x",
          inSize, (int)work->input.ordinal.timestamp.peeku(),
          (int)work->input.ordinal.frameIndex.peeku(), work->input.flags);

    mpp_packet_init(&mppPkt, inData, inSize);

    if (work->input.flags & C2FrameData::FLAG_END_OF_STREAM) {
        mpp_packet_set_eos(mppPkt);
    }

    mpp_packet_set_pts(mppPkt, timestamps);
    mpp_packet_set_dts(mppPkt, timestamps);

    if (work->input.flags & C2FrameData::FLAG_CODEC_CONFIG) {
        mpp_packet_set_extra_data(mppPkt);
    }

    mpp_packet_set_pos(mppPkt, inData);
    mpp_packet_set_length(mppPkt, inSize);

    err = mCtx->mppMpi->decode_put_packet(mCtx->mppCtx, mppPkt);
    if (MPP_OK != err) {
        c2_warn("%s %d in put packet failed,retry!", __FUNCTION__, __LINE__);
        ret = C2_NOT_FOUND;
        goto __FAILED;
    }else{
        if (work->input.flags != 0) {
            fillEmptyWork(work);
        }
    }

__FAILED:
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
        c2_info("mpp decode get frame failed,retry! ret: %d", err);
        ret = C2_NOT_FOUND;
        goto __FAILED;
    }

    infochange = (mWidth != mpp_frame_get_width(mppFrame) || mHeight != mpp_frame_get_height(mppFrame)) ? 1 : 0;
    if (infochange && !mpp_frame_get_eos(mppFrame)) {
        RK_U32 width = mpp_frame_get_width(mppFrame);
        RK_U32 height = mpp_frame_get_height(mppFrame);
        RK_U32 hor_stride = mpp_frame_get_hor_stride(mppFrame);
        RK_U32 ver_stride = mpp_frame_get_ver_stride(mppFrame);
        MppFrameFormat format = mpp_frame_get_fmt(mppFrame);

        c2_info("decode_get_frame get info changed found\n");
        c2_info("decoder require buffer w:h [%d:%d] stride [%d:%d]",
                    width, height, hor_stride, ver_stride);

        mBufferMaps.clear();
        mBlockMaps.clear();
        err = mpp_buffer_group_clear(mCtx->frameGroup);
        if (err) {
            c2_err("clear buffer group failed ret %d\n", ret);
            ret = C2_CORRUPTED;
            goto __FAILED;
        }
        mWidth = width;
        mHeight = height;
        mColorFormat = format;
        C2StreamPictureSizeInfo::output size(0u, mWidth, mHeight);
        C2StreamBlockSizeInfo::output   blockSize(0u, hor_stride, ver_stride);
        C2StreamBlockCountInfo::output  blockCount(0u);
        std::vector<std::unique_ptr<C2SettingResult>> failures;
        ret = mIntf->config({ &size, &blockSize, &blockCount },
                            C2_MAY_BLOCK,
                            &failures);
        if (ret != C2_OK) {
            c2_err("Cannot set width and height");
            goto __FAILED;
        }

        /*
         * All buffer group config done. Set info change ready to let
         * decoder continue decoding
         */
        err = mCtx->mppMpi->control(mCtx->mppCtx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
        if (err) {
            c2_err("info change ready failed ret %d\n", ret);
            ret = C2_CORRUPTED;
            goto __FAILED;
        }

        ret = C2_NO_MEMORY;
    } else {
        (void)getVuiParams(&mppFrame);
        c2_trace("%s:, frame info(mpp_frame=%p frameW=%d frameH=%d W=%d H=%d"
                           " pts=%lld dts=%lld Err=%d eos=%d)",
                           __FUNCTION__, mppFrame,
                           mpp_frame_get_hor_stride(mppFrame),
                           mpp_frame_get_ver_stride(mppFrame),
                           mpp_frame_get_width(mppFrame),
                           mpp_frame_get_height(mppFrame),
                           mpp_frame_get_pts(mppFrame),
                           mpp_frame_get_dts(mppFrame),
                           mpp_frame_get_errinfo(mppFrame),
                           mpp_frame_get_eos(mppFrame));

        auto it_frameindex = mFrameIndexMaps.find((uint64_t)mpp_frame_get_pts(mppFrame));
        auto it = mBlockMaps.find(mpp_frame_get_buffer(mppFrame));
        if(!mpp_frame_get_eos(mppFrame)){
            if(it_frameindex != mFrameIndexMaps.end()){
                c2_trace("frame index: %llu", (unsigned long long)it_frameindex->second);
                finishWork((uint64_t)it_frameindex->second, work, it->second);
            }else{
                c2_trace("can not find frame index, create new work and send!");
                std::unique_ptr<C2Work> outputWork(new C2Work);
                outputWork->worklets.clear();
                outputWork->worklets.emplace_back(new C2Worklet);
                outputWork->worklets.front()->output.flags = (C2FrameData::flags_t)0;
                outputWork->worklets.front()->output.ordinal.timestamp = mpp_frame_get_pts(mppFrame);
                finishNewWork(outputWork, it->second);
            }
        }else{
            mOutputEos = true;
            ALOGD("mpp_frame_get_eos true !");
        }
        mBlockMaps[mpp_frame_get_buffer(mppFrame)] = NULL;

    }

__FAILED:
    return ret;
}

c2_status_t C2RKMpiDec::drainInternal(
    uint32_t drainMode,
    const std::shared_ptr<C2BlockPool> &pool,
    const std::unique_ptr<C2Work> &work) {
    c2_trace("%s %d in", __FUNCTION__, __LINE__);
    c2_status_t ret = C2_OK;
    int retry = 50;

    if (drainMode == NO_DRAIN) {
        c2_warn("drain with NO_DRAIN: no-op");
        return C2_OK;
    }
    if (drainMode == DRAIN_CHAIN) {
        c2_warn("DRAIN_CHAIN not supported");
        return C2_OMITTED;
    }

    while (!mOutputEos && retry != 0){
        ret = ensureMppGroupReady(pool);
        if (ret != C2_OK) {
            c2_warn("can't ensure mpp group buffer enough! err=%d", ret);
        }
        ret = decode_getoutframe(work);
        retry --;
        usleep(5 * 1000);
    }

    return C2_OK;
}

c2_status_t C2RKMpiDec::drain(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool) {
    return drainInternal(drainMode, pool, nullptr);
}

c2_status_t C2RKMpiDec::ensureMppGroupReady(
        const std::shared_ptr<C2BlockPool> &pool) {
    std::shared_ptr<C2GraphicBlock> outblock;
    c2_status_t                     ret     = C2_OK;
    uint32_t                        frameH  = 0;
    uint32_t                        frameW  = 0;
    // TODO() format can't define here
    uint32_t                        format  = HAL_PIXEL_FORMAT_YCrCb_NV12;

    format = colorFormatMpiToAndroid(mColorFormat);

    C2StreamMaxReferenceCountTuning::output maxRefsCount(0u);
    C2StreamBlockSizeInfo::output blockSize(0u, 0, 0);
    C2StreamBlockCountInfo::output blockCount(0u);
    std::vector<std::unique_ptr<C2Param>> params;
    ret = intf()->query_vb(
            {
                &maxRefsCount,
                &blockSize,
                &blockCount
            },
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
    C2MemoryUsage usage = { C2MemoryUsage::CPU_READ, C2MemoryUsage::CPU_WRITE };
    usage.expected |= (GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_EXTERNAL_DISP | GRALLOC_USAGE_PRIVATE_2);
    bool isLocalBuffer = (pool->getLocalId() > C2BlockPool::PLATFORM_START) ? false : true;

    uint32_t i = 0;
    uint32_t count = 0;
    // TODO: it should use max reference count
    for (i = 0; i < maxRefsCount.value - blockCount.value; i++) {
        ret = pool->fetchGraphicBlock(frameW,
                                      frameH,
                                      format,
                                      usage,
                                      &outblock);
        if (ret != C2_OK) {
            ALOGW("fetchGraphicBlock for Output failed with status %d", ret);
            break;
        }
        c2_trace("provided (%dx%d) required (%dx%d) index(%d)",
              outblock->width(), outblock->height(), frameW, frameH, i);
        ret = registerBufferToMpp(outblock, isLocalBuffer);
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
        c2_err("block count config failed!");
        return C2_BAD_INDEX;
    }

    return ret;
}

c2_status_t C2RKMpiDec::registerBufferToMpp(std::shared_ptr<C2GraphicBlock> block, bool isLocalBuffer) {
    uint32_t                    findIndex = 0;
    uint32_t                      shareFd = 0;

    if (!block.get()) {
        c2_err("register buffer failed, block is null");
        return C2_CORRUPTED;
    }

    auto c2Handle = block->handle();
    native_handle_t *grallocHandle = UnwrapNativeCodec2GrallocHandle(c2Handle);
    shareFd = grallocHandle->data[0];


    uint32_t bqSlot;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t usage;
    uint32_t stride;
    uint32_t generation;
    uint64_t bqId;
    android::_UnwrapNativeCodec2GrallocMetadata(
            block->handle(), &width, &height, &format, &usage, &stride, &generation, &bqId, &bqSlot);

    c2_trace("%s %d isLocalBuffer:%d bqId:%lld bqSLOT:%ld shareFd：%d", __func__, __LINE__, isLocalBuffer,
            (unsigned long long)bqId, (unsigned long)bqSlot, shareFd);
    if (isLocalBuffer) {
        mFindIndex ++;
        // register this buffer.
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


        c2_trace("%s %d width: %d height: %d mppBuffer: %p outBlock: %p info.fd=%d block fd=%d slot=%d",
                __FUNCTION__, __LINE__, width, height, mppBuffer, block.get(), info.fd, shareFd, bqSlot);
        mBufferMaps[mFindIndex] = mppBuffer;
        mBlockMaps[mppBuffer] = block;

        mpp_buffer_put(mppBuffer);
    } else {
        std::map<uint32_t, void *>::iterator it;
        findIndex = bqSlot;
        it = mBufferMaps.find(findIndex);
        if (it != mBufferMaps.end()) {
            // list have this item.
            MppBuffer mppBuffer = it->second;
            if (mppBuffer) {
                mpp_buffer_put(mppBuffer);
            }
            mBlockMaps[mppBuffer] = block;
            c2_trace("%s %d mppBuffer: %p outBlock: %p block fd=%d slot=%d",
                    __FUNCTION__, __LINE__, mppBuffer, block.get(), shareFd, bqSlot);
        } else {
            // register this buffer.
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


            c2_trace("%s %d width: %d height: %d mppBuffer: %p outBlock: %p info.fd=%d block fd=%d slot=%d",
                    __FUNCTION__, __LINE__, width, height, mppBuffer, block.get(), info.fd, shareFd, bqSlot);
            mBufferMaps[findIndex] = mppBuffer;
            mBlockMaps[mppBuffer] = block;

            mpp_buffer_put(mppBuffer);
        }
    }

    native_handle_delete(grallocHandle);
    return C2_OK;
}

class C2RKMpiDecFactory : public C2ComponentFactory {
public:
    C2RKMpiDecFactory(std::string componentName)
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
        c2_trace("%s %d in", __FUNCTION__, __LINE__);
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
    c2_trace("in %s", __func__);
    return new ::android::C2RKMpiDecFactory(componentName);
}

}  // namespace android
