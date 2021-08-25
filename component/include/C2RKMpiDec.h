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

#ifndef ANDROID_C2_RK_MPI_DEC_H_
#define ANDROID_C2_RK_MPI_DEC_H_

#include <sys/time.h>
#include <map>

#include <media/stagefright/foundation/ColorUtils.h>

#include <atomic>
#include <C2RKComponent.h>
#include <C2RKMediaDefs.h>
#include <C2Debug.h>
#include <C2PlatformSupport.h>
#include <Codec2Mapper.h>
#include <C2RKInterface.h>

#include "mpp/rk_mpi.h"
#include "C2RKLog.h"

namespace android {

#define FLAG_NON_DISPLAY_FRAME (1u << 15)

typedef struct _MpiCodecContext {
    MppCtx              mppCtx;
    MppApi             *mppMpi;
    MppBufferGroup      frameGroup;
} MpiCodecContext;

constexpr uint32_t kDefaultOutputDelay = 16;
constexpr uint32_t kMaxOutputDelay = 16;
constexpr uint32_t kMaxReferenceCount = 16;

class C2RKMpiDec : public C2RKComponent {
public:
    class IntfImpl;
    C2RKMpiDec(
            const char *name,
            c2_node_id_t id,
            const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2RKMpiDec();

    // From SimpleC2Component
    c2_status_t onInit() override;
    c2_status_t onStop() override;
    void onReset() override;
    void onRelease() override;
    c2_status_t onFlush_sm() override;
    void process(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool) override;

private:
    void finishWork(
            uint64_t index,
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2GraphicBlock> block);
    void fillEmptyWork(const std::unique_ptr<C2Work> &work);
    bool getVuiParams(MppFrame *frame);
    c2_status_t decode_sendstream(const std::unique_ptr<C2Work> &work);
    c2_status_t decode_getoutframe(const std::unique_ptr<C2Work> &work);
    c2_status_t drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work);
    c2_status_t ensureMppGroupReady(const std::shared_ptr<C2BlockPool> &pool);
    c2_status_t registerBufferToMpp(std::shared_ptr<C2GraphicBlock> block, bool isLocalBuffer);

private:
    std::shared_ptr<IntfImpl> mIntf;

    std::map<uint32_t, void *> mBufferMaps;
    std::map<void *, std::shared_ptr<C2GraphicBlock>> mBlockMaps;
    std::map<uint64_t, uint32_t> mFrameIndexMaps;
    uint32_t mFindIndex; //for local buffer
    bool mOutputEos;
    bool mFlushed;

    uint32_t mWidth;
    uint32_t mHeight;
    std::atomic_uint64_t mOutIndex;

    MppFrameFormat mColorFormat;
    MpiCodecContext *mCtx;

    MppCodingType mCodingType;
    bool mSignalledOutputEos;

    // Color aspects. These are ISO values and are meant to detect changes in aspects to avoid
    // converting them to C2 values for each frame
    struct VuiColorAspects {
        uint8_t primaries;
        uint8_t transfer;
        uint8_t coeffs;
        uint8_t fullRange;

        // default color aspects
        VuiColorAspects()
            : primaries(2), transfer(2), coeffs(2), fullRange(0) { }

        bool operator==(const VuiColorAspects &o) {
            return primaries == o.primaries && transfer == o.transfer && coeffs == o.coeffs
                    && fullRange == o.fullRange;
        }
    } mBitstreamColorAspects;

    C2_DO_NOT_COPY(C2RKMpiDec);
};

class C2RKMpiDec::IntfImpl : public C2RKInterface<void>::BaseParams {
public:
    explicit IntfImpl(
                const std::shared_ptr<C2ReflectorHelper> &helper,
                C2String name,
                C2Component::kind_t kind,
                C2Component::domain_t domain,
                C2String mediaType);

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
        return res;
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

    static C2R MaxPictureSizeSetter(bool mayBlock, C2P<C2StreamMaxPictureSizeTuning::output> &me,
                                    const C2P<C2StreamPictureSizeInfo::output> &size) {
        c2_info("%s %d in", __FUNCTION__, __LINE__);
        (void)mayBlock;
        // TODO: get max width/height from the size's field helpers vs. hardcoding
        me.set().width = c2_min(c2_max(me.v.width, size.v.width), 4080u);
        me.set().height = c2_min(c2_max(me.v.height, size.v.height), 4080u);
        return C2R::Ok();
    }

    static C2R MaxInputSizeSetter(bool mayBlock, C2P<C2StreamMaxBufferSizeInfo::input> &me,
                                  const C2P<C2StreamMaxPictureSizeTuning::output> &maxSize) {
        (void)mayBlock;
        // assume compression ratio of 2
        me.set().value = (((maxSize.v.width + 15) / 16) * ((maxSize.v.height + 15) / 16) * 192);
        return C2R::Ok();
    }

    static C2R ProfileLevelSetter(bool mayBlock, C2P<C2StreamProfileLevelInfo::input> &me,
                                  const C2P<C2StreamPictureSizeInfo::output> &size) {
        (void)mayBlock;
        (void)size;
        (void)me;  // TODO: validate
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

    std::shared_ptr<C2StreamColorAspectsInfo::output> getColorAspects_l() {
        return mColorAspects;
    }
public:
    std::shared_ptr<C2StreamProfileLevelInfo::input> mProfileLevel;
    std::shared_ptr<C2StreamPictureSizeInfo::output> mSize;
    std::shared_ptr<C2StreamBlockSizeInfo::output> mBlockSize;
    std::shared_ptr<C2StreamBlockCountInfo::output> mBlockCount;
    std::shared_ptr<C2StreamMaxPictureSizeTuning::output> mMaxSize;
    std::shared_ptr<C2StreamMaxBufferSizeInfo::input> mMaxInputSize;
    std::shared_ptr<C2StreamColorInfo::output> mColorInfo;
    std::shared_ptr<C2StreamPixelFormatInfo::output> mPixelFormat;
    std::shared_ptr<C2StreamMaxReferenceCountTuning::output> mMaxRefCount;
    std::shared_ptr<C2StreamColorAspectsInfo::input> mCodedColorAspects;
    std::shared_ptr<C2StreamColorAspectsTuning::output> mDefaultColorAspects;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
};

C2ComponentFactory* CreateRKMpiDecFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_SOFT_MPI_DEC_H_
