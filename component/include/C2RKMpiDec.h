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
    c2_status_t flush();
    void process(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool> &pool) override;
    c2_status_t drain(
            uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool) override;

private:
    enum MyBufferSite{
        MY_BUFFER_SITE_BY_MPI = 0,
        MY_BUFFER_SITE_BY_C2,
        MY_BUFFER_SITE_BY_ABANDON,
    };

    typedef struct {
        /* index to find MppBuffer */
        uint32_t     index;
        /* mpp buffer */
        MppBuffer    mBuffer;
        /* who own this buffer */
        MyBufferSite site;
        /* block shared by surface*/
        std::shared_ptr<C2GraphicBlock> block;
    } MyC2Buffer;

    std::shared_ptr<IntfImpl> mIntf;

    /* MPI interface parameters */
    MppCtx          mMppCtx;
    MppApi         *mMppMpi;
    MppCodingType   mCodingType;
    MppFrameFormat  mColorFormat;
    MppBufferGroup  mFrmGrp;
    Vector<MyC2Buffer*> mC2Buffers;

    uint32_t mWidth;
    uint32_t mHeight;
    uint32_t mHorStride;
    uint32_t mVerStride;
    uint64_t mLastPts;

    bool mStarted;
    bool mFlushed;
    bool mOutputEos;
    bool mSignalledOutputEos;

    /*
       1. BufferMode:  without surcace
       2. SurfaceMode: with surface
    */
    bool mBufferMode;

    std::shared_ptr<C2GraphicBlock> mOutBlock;
    std::map<uint64_t, uint64_t> mPtsMaps;

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
            const std::shared_ptr<C2GraphicBlock> block);
    c2_status_t drainInternal(
        uint32_t drainMode,
        const std::shared_ptr<C2BlockPool> &pool,
        const std::unique_ptr<C2Work> &work);

    c2_status_t initDecoder();
    void getVuiParams(MppFrame frame);
    c2_status_t decode_sendstream(const std::unique_ptr<C2Work> &work);
    c2_status_t decode_getoutframe(const std::unique_ptr<C2Work> &work);

    c2_status_t commitBufferToMpp(std::shared_ptr<C2GraphicBlock> block);
    c2_status_t ensureBlockState(const std::shared_ptr<C2BlockPool> &pool);

    /*
     * MyC2Buffer vector operations
     */
    MyC2Buffer* findMyBuffer(uint32_t index) {
        for (int i = 0; i < mC2Buffers.size(); i++) {
            MyC2Buffer *buffer = mC2Buffers.editItemAt(i);
            if (buffer->index == index) {
                return buffer;
            }
        }
        return nullptr;
    }

    MyC2Buffer* findMyBuffer(MppBuffer mBuffer) {
        for (int i = 0; i < mC2Buffers.size(); i++) {
            MyC2Buffer *buffer = mC2Buffers.editItemAt(i);
            if (buffer->mBuffer == mBuffer) {
                return buffer;
            }
        }
        return nullptr;
    }

    void clearMyBuffers() {
        while (!mC2Buffers.isEmpty()) {
            MyC2Buffer *buffer = mC2Buffers.editItemAt(0);
            if (buffer != NULL) {
                if (buffer->site != MY_BUFFER_SITE_BY_MPI) {
                    mpp_buffer_put(buffer->mBuffer);
                }
                delete buffer;
            }
            mC2Buffers.removeAt(0);
        }
    }

    C2_DO_NOT_COPY(C2RKMpiDec);
};

C2ComponentFactory* CreateRKMpiDecFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_SOFT_MPI_DEC_H_
