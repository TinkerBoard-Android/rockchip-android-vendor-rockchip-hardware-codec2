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

#include "C2RKComponent.h"
#include "C2RKInterface.h"
#include "mpp/rk_mpi.h"

#include <utils/Vector.h>

namespace android {

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
    enum OutBufferSite{
        BUFFER_SITE_BY_MPI = 0,
        BUFFER_SITE_BY_C2,
        BUFFER_SITE_BY_ABANDON,
    };

    typedef struct {
        /* index to find this buffer */
        uint32_t       index;
        /* mpp buffer */
        MppBuffer      mppBuffer;
        /* who own this buffer */
        OutBufferSite  site;
        /* block shared by surface*/
        std::shared_ptr<C2GraphicBlock> block;
    } OutBuffer;

    typedef struct {
        std::shared_ptr<C2GraphicBlock> outblock;
        uint64_t frameIndex;
    } OutWorkEntry;

    std::shared_ptr<IntfImpl> mIntf;

    /* MPI interface parameters */
    MppCtx          mMppCtx;
    MppApi         *mMppMpi;
    MppCodingType   mCodingType;
    MppFrameFormat  mColorFormat;
    MppBufferGroup  mFrmGrp;
    Vector<OutBuffer*> mOutBuffers;

    uint32_t mWidth;
    uint32_t mHeight;
    uint32_t mHorStride;
    uint32_t mVerStride;
    uint32_t mTransfer;
    int64_t  mLastPts;

    bool mStarted;
    bool mFlushed;
    bool mOutputEos;
    bool mSignalledInputEos;
    bool mSignalledError;

    // C2Work info, <key, value> = <frameIndex, pts>
    std::map<uint64_t, uint64_t> mWorkQueue;

    /*
       1. BufferMode:  without surcace
       2. SurfaceMode: with surface
    */
    bool mBufferMode;

    struct FbcConfig {
        uint32_t mode;
        // fbc decode output padding
        uint32_t paddingX;
        uint32_t paddingY;
    } mFbcCfg;

    std::shared_ptr<C2GraphicBlock> mOutBlock;

    // Color aspects. These are ISO values and are meant to detect changes
    // in aspects to avoid converting them to C2 values for each frame.
    struct VuiColorAspects {
        uint8_t primaries;
        uint8_t transfer;
        uint8_t coeffs;
        uint8_t fullRange;

        // default color aspects
        VuiColorAspects()
            : primaries(2), transfer(2), coeffs(2), fullRange(0) { }

        bool operator==(const VuiColorAspects &o) {
            return primaries == o.primaries && transfer == o.transfer &&
                    coeffs == o.coeffs && fullRange == o.fullRange;
        }
    } mBitstreamColorAspects;

    void fillEmptyWork(const std::unique_ptr<C2Work> &work);
    void finishWork(
            uint64_t index,
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2GraphicBlock> block,
            bool delayOutput = false);
    c2_status_t drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work);

    c2_status_t initDecoder();
    void getVuiParams(MppFrame frame);
    c2_status_t sendpacket(
            uint8_t *data, size_t size, uint64_t pts, uint64_t frmIndex, uint32_t flags);
    c2_status_t getoutframe(OutWorkEntry *entry);

    c2_status_t commitBufferToMpp(std::shared_ptr<C2GraphicBlock> block);
    c2_status_t ensureDecoderState(const std::shared_ptr<C2BlockPool> &pool);

    /*
     * OutBuffer vector operations
     */
    OutBuffer* findOutBuffer(uint32_t index) {
        for (int i = 0; i < mOutBuffers.size(); i++) {
            OutBuffer *buffer = mOutBuffers.editItemAt(i);
            if (buffer->index == index) {
                return buffer;
            }
        }
        return nullptr;
    }

    OutBuffer* findOutBuffer(MppBuffer mppBuffer) {
        for (int i = 0; i < mOutBuffers.size(); i++) {
            OutBuffer *buffer = mOutBuffers.editItemAt(i);
            if (buffer->mppBuffer == mppBuffer) {
                return buffer;
            }
        }
        return nullptr;
    }

    void clearOutBuffers() {
        while (!mOutBuffers.isEmpty()) {
            OutBuffer *buffer = mOutBuffers.editItemAt(0);
            if (buffer != NULL) {
                if (buffer->site != BUFFER_SITE_BY_MPI) {
                    mpp_buffer_put(buffer->mppBuffer);
                }
                delete buffer;
            }
            mOutBuffers.removeAt(0);
        }
    }

    int getOutBufferCountOwnByMpi() {
        int count = 0;
        for (int i = 0; i < mOutBuffers.size(); i++) {
            OutBuffer *buffer = mOutBuffers.editItemAt(i);
            if (buffer->site == BUFFER_SITE_BY_MPI) {
                count++;
            }
        }
        return count;
    }

    C2_DO_NOT_COPY(C2RKMpiDec);
};

C2ComponentFactory* CreateRKMpiDecFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_SOFT_MPI_DEC_H_
