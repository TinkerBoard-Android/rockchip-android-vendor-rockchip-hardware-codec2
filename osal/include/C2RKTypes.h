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

#ifndef ANDROID_C2_RK_TYPES_H__
#define ANDROID_C2_RK_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

/** C2_U8 is an 8 bit unsigned quantity that is byte aligned */
typedef unsigned char C2_U8;

/** C2_S8 is an 8 bit signed quantity that is byte aligned */
typedef signed char C2_S8;

/** C2_U16 is a 16 bit unsigned quantity that is 16 bit word aligned */
typedef unsigned short C2_U16;

/** C2_S16 is a 16 bit signed quantity that is 16 bit word aligned */
typedef signed short C2_S16;

/** C2_U32 is a 32 bit unsigned quantity that is 32 bit word aligned */
typedef unsigned long C2_U32;

/** C2_S32 is a 32 bit signed quantity that is 32 bit word aligned */
typedef signed long C2_S32;

#ifdef __cplusplus
}
#endif

#endif  // ANDROID_C2_RK_TYPES_H__

