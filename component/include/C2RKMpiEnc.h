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

#ifndef ANDROID_C2_RK_MPI_ENC_H__
#define ANDROID_C2_RK_MPI_ENC_H__

#include <stdio.h>
#include "C2RKComponent.h"
#include "mpp/rk_mpi.h"

namespace android {

enum ExtendedC2ParamIndexKind : C2Param::type_index_t {
    kParamIndexSceneMode = C2Param::TYPE_INDEX_VENDOR_START,
};

struct C2RKMpiEnc : public C2RKComponent {
public:
    class IntfImpl;

    C2RKMpiEnc(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);
    virtual ~C2RKMpiEnc();

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
    /* DMA buffer memery */
    typedef struct {
        int32_t  fd;
        int32_t  size;
        void    *handler; /* buffer_handle_t */
    } MyDmaBuffer_t;

    typedef struct {
        MppPacket outPacket;
        uint64_t  frameIndex;
    } OutWorkEntry;

    std::shared_ptr<IntfImpl> mIntf;
    MyDmaBuffer_t *mDmaMem;

    /* MPI interface parameters */
    MppCtx         mMppCtx;
    MppApi        *mMppMpi;
    MppEncCfg      mEncCfg;
    MppCodingType  mCodingType;

    bool           mStarted;
    bool           mSpsPpsHeaderReceived;
    bool           mSawInputEOS;
    bool           mOutputEOS;
    bool           mSignalledError;
    int32_t        mHorStride;
    int32_t        mVerStride;

    /* dump file for debug */
    FILE          *mInFile;
    FILE          *mOutFile;

    // configurations used by component in process
    // (TODO: keep this in intf but make them internal only)
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;

    void fillEmptyWork(const std::unique_ptr<C2Work> &work);
    void finishWork(
            const std::unique_ptr<C2Work> &work,
            const std::shared_ptr<C2BlockPool>& pool,
            OutWorkEntry entry);
    c2_status_t drainInternal(uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool,
            const std::unique_ptr<C2Work> &work);

    c2_status_t setupBaseCodec();
    c2_status_t setupSceneMode();
    c2_status_t setupFrameRate();
    c2_status_t setupBitRate();
    c2_status_t setupProfileParams();
    c2_status_t setupQp();
    c2_status_t setupVuiParams();
    c2_status_t setupTemporalLayers();
    c2_status_t setupEncCfg();

    c2_status_t initEncoder();
    c2_status_t releaseEncoder();

    c2_status_t getInBufferFromWork(
            const std::unique_ptr<C2Work> &work, MyDmaBuffer_t *outBuffer);
    c2_status_t sendframe(
            MyDmaBuffer_t dBuffer, uint64_t pts, uint32_t flags);
    c2_status_t getoutpacket(OutWorkEntry *entry);

    C2_DO_NOT_COPY(C2RKMpiEnc);
};

C2ComponentFactory* CreateRKMpiEncFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_RK_MPI_ENC_H__

