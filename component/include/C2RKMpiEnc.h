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

#include <map>

#include <utils/Vector.h>

#include <C2RKComponent.h>

#include "mpp/rk_mpi.h"
#include "mpp/rk_mpi_cmd.h"
#include "mpp/mpp_frame.h"
#include "mpp/rk_venc_cfg.h"
#include "C2RKMediaDefs.h"

namespace android {

struct C2RKMpiEnc : public C2RKComponent {
    class IntfImpl;

    C2RKMpiEnc(const char *name, c2_node_id_t id, const std::shared_ptr<IntfImpl> &intfImpl);

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

protected:
    virtual ~C2RKMpiEnc();

private:
    c2_status_t setVuiParams();
    c2_status_t initEncParams();
    c2_status_t initEncoder();
    c2_status_t releaseEncoder();
    c2_status_t encoder_sendframe(const std::unique_ptr<C2Work> &work);
    c2_status_t encoder_getstream(const std::unique_ptr<C2Work> &work,
                                  const std::shared_ptr<C2BlockPool>& pool);
    void finishWork(uint64_t workIndex,
                    const std::unique_ptr<C2Work> &work,
                    const std::shared_ptr<C2BlockPool>& pool,
                    MppPacket outputPkt);
    c2_status_t drainInternal(uint32_t drainMode,
            const std::shared_ptr<C2BlockPool> &pool,
            const std::unique_ptr<C2Work> &work);

    C2_DO_NOT_COPY(C2RKMpiEnc);

private:    
    std::shared_ptr<IntfImpl> mIntf;
    bool    mSpsPpsHeaderReceived;
    bool    mSignalledEos;
    bool    mSignalledError;
    bool    mStarted;
    void    *mRgaCtx;
    VPUMemLinear_t *mVpumem;
    int32_t mEncProfile;
    int32_t mEncLevel;
    int32_t mIDRInterval;
    int32_t mIInterval;
    int32_t mEos;
    MppCtx mMppCtx;
    MppApi *mMppMpi;
    FILE *mFp_enc_out;
    FILE *mFp_enc_in;
    struct timeval mTimeStart;
    struct timeval mTimeEnd;
    MppCodingType mCodingType;
    // configurations used by component in process
    // (TODO: keep this in intf but make them internal only)
    std::shared_ptr<C2StreamPictureSizeInfo::input> mSize;
    std::shared_ptr<C2StreamFrameRateInfo::output> mFrameRate;
    std::shared_ptr<C2StreamBitrateInfo::output> mBitrate;
    std::shared_ptr<C2StreamBitrateModeTuning::output> mBitrateMode;
    std::shared_ptr<C2StreamGopTuning::output> mGop;
    std::shared_ptr<C2StreamRequestSyncFrameTuning::output> mRequestSync;
    std::shared_ptr<C2StreamColorAspectsInfo::output> mColorAspects;
};

C2ComponentFactory* CreateRKMpiEncFactory(std::string componentName);

}  // namespace android

#endif  // ANDROID_C2_RK_MPI_ENC_H__

