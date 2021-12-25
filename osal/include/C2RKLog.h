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

#ifndef ANDROID_C2_RK_LOG_H__
#define ANDROID_C2_RK_LOG_H__

#include <stdint.h>
#include "C2RKTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ROCKCHIP_LOG_TAG
#define ROCKCHIP_LOG_TAG    "rk_c2_log"
#endif

typedef enum _LOG_LEVEL
{
    ROCKCHIP_LOG_TRACE,
    ROCKCHIP_LOG_INFO,
    ROCKCHIP_LOG_WARNING,
    ROCKCHIP_LOG_ERROR,
    ROCKCHIP_LOG_DEBUG
} ROCKCHIP_LOG_LEVEL;

/*
 * omx_debug bit definition
 */
#define C2_DBG_UNKNOWN                 0x00000000
#define C2_DBG_FUNCTION                0x80000000
#define C2_DBG_MALLOC                  0x40000000
#define C2_DBG_CAPACITYS               0x00000001

void _Rockchip_C2_Log(ROCKCHIP_LOG_LEVEL logLevel, C2_U32 flag, const char *tag, const char *msg, ...);

#define c2_info(fmt, ...)        _Rockchip_C2_Log(ROCKCHIP_LOG_INFO,     C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define c2_trace(fmt, ...)       _Rockchip_C2_Log(ROCKCHIP_LOG_TRACE,    C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define c2_err(fmt, ...)         _Rockchip_C2_Log(ROCKCHIP_LOG_ERROR,    C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define c2_warn(fmt, ...)        _Rockchip_C2_Log(ROCKCHIP_LOG_WARNING,  C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, fmt "\n", ##__VA_ARGS__)

#define c2_info_f(fmt, ...)      _Rockchip_C2_Log(ROCKCHIP_LOG_INFO,     C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, "%s(%d): " fmt "\n",__FUNCTION__, __LINE__, ##__VA_ARGS__)
#define c2_trace_f(fmt, ...)     _Rockchip_C2_Log(ROCKCHIP_LOG_TRACE,    C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, "%s(%d): " fmt "\n",__FUNCTION__, __LINE__, ##__VA_ARGS__)
#define c2_err_f(fmt, ...)       _Rockchip_C2_Log(ROCKCHIP_LOG_ERROR,    C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, "%s(%d): " fmt "\n",__FUNCTION__, __LINE__, ##__VA_ARGS__)
#define c2_warn_f(fmt, ...)      _Rockchip_C2_Log(ROCKCHIP_LOG_WARNING,  C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, "%s(%d): " fmt "\n",__FUNCTION__, __LINE__, ##__VA_ARGS__)

#define _c2_dbg(fmt, ...)          _Rockchip_C2_Log(ROCKCHIP_LOG_INFO,     C2_DBG_UNKNOWN, ROCKCHIP_LOG_TAG, "%s(%d): " fmt "\n",__FUNCTION__, __LINE__, ##__VA_ARGS__)

#define c2_dbg_f(flags, fmt, ...)  _Rockchip_C2_Log(ROCKCHIP_LOG_DEBUG, flags, ROCKCHIP_LOG_TAG, "%s(%d): " fmt "\n",__FUNCTION__, __LINE__, ##__VA_ARGS__)

#define c2_dbg(debug, flag, fmt, ...) \
            do { \
               if (debug & flag) \
                   _c2_dbg(fmt, ## __VA_ARGS__); \
            } while (0)

#define FunctionIn()  c2_dbg_f(C2_DBG_FUNCTION, "IN")

#define FunctionOut() c2_dbg_f(C2_DBG_FUNCTION, "OUT")

#ifdef __cplusplus
}
#endif

#endif  // ANDROID_C2_RK_LOG_H__

