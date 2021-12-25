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

#include <string.h>
#include <android/log.h>
#include "C2RKLog.h"
#include "C2RKEnv.h"
#include "C2RKTypes.h"

void _Rockchip_C2_Log(ROCKCHIP_LOG_LEVEL logLevel, C2_U32 flag, const char *tag, const char *msg, ...)
{
    C2_U32 value = 0;

    va_list argptr;

    va_start(argptr, msg);

    switch (logLevel) {
    case ROCKCHIP_LOG_TRACE: {
        Rockchip_C2_GetEnvU32("vendor.dump.c2.log", &value, 0);
        if (value) {
            __android_log_vprint(ANDROID_LOG_DEBUG, tag, msg, argptr);
        }
    }
    break;
    case ROCKCHIP_LOG_DEBUG: {
        Rockchip_C2_GetEnvU32("vendor.c2.log.debug", &value, 0);
        if (value & flag) {
            __android_log_vprint(ANDROID_LOG_DEBUG, tag, msg, argptr);
        }
    } break;
    case ROCKCHIP_LOG_INFO:
        __android_log_vprint(ANDROID_LOG_INFO, tag, msg, argptr);
        break;
    case ROCKCHIP_LOG_WARNING:
        __android_log_vprint(ANDROID_LOG_WARN, tag, msg, argptr);
        break;
    case ROCKCHIP_LOG_ERROR:
        __android_log_vprint(ANDROID_LOG_ERROR, tag, msg, argptr);
        break;
    default:
        __android_log_vprint(ANDROID_LOG_VERBOSE, tag, msg, argptr);
    }

    va_end(argptr);
}


