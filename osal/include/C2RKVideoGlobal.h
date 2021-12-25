/*
 * Copyright (C) 2021 Rockchip Electronics Co. LTD
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

#ifndef ANDROID_C2_RK_VIDEO_GLOBAL_H__
#define ANDROID_C2_RK_VIDEO_GLOBAL_H__

#include "C2RKTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_DBG_RECORD_MASK                0xff000000
#define VIDEO_DBG_RECORD_IN                  0x01000000
#define VIDEO_DBG_RECORD_OUT                 0x02000000

/** Get time */
#define GETTIME(a, b) gettimeofday(a, b)

/** Compute difference between start and end */
#define TIME_DIFF(start, end, diff)                      \
    diff = (((end).tv_sec - (start).tv_sec) * 1000000) + \
           ((end).tv_usec - (start).tv_usec);

#ifdef __cplusplus
}
#endif

#endif  // ANDROID_C2_RK_VIDEO_GLOBAL_H__

