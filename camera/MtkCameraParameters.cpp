/* Copyright Statement:
 *
 * This software/firmware and related documentation ("MediaTek Software") are
 * protected under relevant copyright laws. The information contained herein is
 * confidential and proprietary to MediaTek Inc. and/or its licensors. Without
 * the prior written permission of MediaTek inc. and/or its licensors, any
 * reproduction, modification, use or disclosure of MediaTek Software, and
 * information contained herein, in whole or in part, shall be strictly
 * prohibited.
 *
 * MediaTek Inc. (C) 2010. All rights reserved.
 *
 * BY OPENING THIS FILE, RECEIVER HEREBY UNEQUIVOCALLY ACKNOWLEDGES AND AGREES
 * THAT THE SOFTWARE/FIRMWARE AND ITS DOCUMENTATIONS ("MEDIATEK SOFTWARE")
 * RECEIVED FROM MEDIATEK AND/OR ITS REPRESENTATIVES ARE PROVIDED TO RECEIVER
 * ON AN "AS-IS" BASIS ONLY. MEDIATEK EXPRESSLY DISCLAIMS ANY AND ALL
 * WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE OR
 * NONINFRINGEMENT. NEITHER DOES MEDIATEK PROVIDE ANY WARRANTY WHATSOEVER WITH
 * RESPECT TO THE SOFTWARE OF ANY THIRD PARTY WHICH MAY BE USED BY,
 * INCORPORATED IN, OR SUPPLIED WITH THE MEDIATEK SOFTWARE, AND RECEIVER AGREES
 * TO LOOK ONLY TO SUCH THIRD PARTY FOR ANY WARRANTY CLAIM RELATING THERETO.
 * RECEIVER EXPRESSLY ACKNOWLEDGES THAT IT IS RECEIVER'S SOLE RESPONSIBILITY TO
 * OBTAIN FROM ANY THIRD PARTY ALL PROPER LICENSES CONTAINED IN MEDIATEK
 * SOFTWARE. MEDIATEK SHALL ALSO NOT BE RESPONSIBLE FOR ANY MEDIATEK SOFTWARE
 * RELEASES MADE TO RECEIVER'S SPECIFICATION OR TO CONFORM TO A PARTICULAR
 * STANDARD OR OPEN FORUM. RECEIVER'S SOLE AND EXCLUSIVE REMEDY AND MEDIATEK'S
 * ENTIRE AND CUMULATIVE LIABILITY WITH RESPECT TO THE MEDIATEK SOFTWARE
 * RELEASED HEREUNDER WILL BE, AT MEDIATEK'S OPTION, TO REVISE OR REPLACE THE
 * MEDIATEK SOFTWARE AT ISSUE, OR REFUND ANY SOFTWARE LICENSE FEES OR SERVICE
 * CHARGE PAID BY RECEIVER TO MEDIATEK FOR SUCH MEDIATEK SOFTWARE AT ISSUE.
 *
 * The following software/firmware and/or related documentation ("MediaTek
 * Software") have been modified by MediaTek Inc. All revisions are subject to
 * any receiver's applicable license agreements with MediaTek Inc.
 */

/*
**
** Copyright 2008, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "MTKCameraParams"
#include <utils/Log.h>

#include <string.h>
#include <stdlib.h>
#include <camera/MtkCameraParameters.h>

namespace android {


//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//  App Mode.
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
const char MtkCameraParameters::PROPERTY_KEY_CLIENT_APPMODE[]   = "client.appmode";
//
const char MtkCameraParameters::APP_MODE_NAME_DEFAULT[]         = "Default";
const char MtkCameraParameters::APP_MODE_NAME_MTK_ENG[]         = "MtkEng";
const char MtkCameraParameters::APP_MODE_NAME_MTK_ATV[]         = "MtkAtv";
const char MtkCameraParameters::APP_MODE_NAME_MTK_S3D[]         = "MtkS3d";
const char MtkCameraParameters::APP_MODE_NAME_MTK_VT[]          = "MtkVt";
const char MtkCameraParameters::APP_MODE_NAME_MTK_PHOTO[]       = "MtkPhoto";
const char MtkCameraParameters::APP_MODE_NAME_MTK_VIDEO[]       = "MtkVideo";
const char MtkCameraParameters::APP_MODE_NAME_MTK_ZSD[]         = "MtkZsd";

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//  Scene Mode
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
const char MtkCameraParameters::SCENE_MODE_NORMAL[] = "normal";

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//  Face Beauty
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
const char MtkCameraParameters::KEY_FB_SMOOTH_LEVEL[]       = "fb-smooth-level";
const char MtkCameraParameters::KEY_FB_SMOOTH_LEVEL_MIN[]   = "fb-smooth-level-min";
const char MtkCameraParameters::KEY_FB_SMOOTH_LEVEL_MAX[]   = "fb-smooth-level-max";
//
const char MtkCameraParameters::KEY_FB_SKIN_COLOR[]         = "fb-skin-color";
const char MtkCameraParameters::KEY_FB_SKIN_COLOR_MIN[]     = "fb-skin-color-min";
const char MtkCameraParameters::KEY_FB_SKIN_COLOR_MAX[]     = "fb-skin-color-max";
//
const char MtkCameraParameters::KEY_FB_SHARP[]              = "fb-sharp";
const char MtkCameraParameters::KEY_FB_SHARP_MIN[]          = "fb-sharp-min";
const char MtkCameraParameters::KEY_FB_SHARP_MAX[]          = "fb-sharp-max";


//
const char MtkCameraParameters::KEY_EXPOSURE[] = "exposure";
const char MtkCameraParameters::KEY_EXPOSURE_METER[] = "exposure-meter";
const char MtkCameraParameters::KEY_ISO_SPEED[] = "iso-speed";
const char MtkCameraParameters::KEY_AE_MODE[] = "ae-mode";
const char MtkCameraParameters::KEY_FOCUS_METER[] = "focus-meter";
const char MtkCameraParameters::KEY_EDGE[] = "edge";
const char MtkCameraParameters::KEY_HUE[] = "hue";
const char MtkCameraParameters::KEY_SATURATION[] = "saturation";
const char MtkCameraParameters::KEY_BRIGHTNESS[] = "brightness";
const char MtkCameraParameters::KEY_CONTRAST[] = "contrast";
const char MtkCameraParameters::KEY_AF_LAMP_MODE [] = "aflamp-mode";
const char MtkCameraParameters::KEY_STEREO_3D_PREVIEW_SIZE[] = "stereo3d-preview-size";
const char MtkCameraParameters::KEY_STEREO_3D_PICTURE_SIZE[] = "stereo3d-picture-size";
const char MtkCameraParameters::KEY_STEREO_3D_TYPE [] = "stereo3d-type";
const char MtkCameraParameters::KEY_STEREO_3D_MODE [] = "stereo3d-mode";
const char MtkCameraParameters::KEY_STEREO_3D_IMAGE_FORMAT [] = "stereo3d-image-format";

// ZSD
const char MtkCameraParameters::KEY_ZSD_MODE[] = "zsd-mode"; 
const char MtkCameraParameters::KEY_SUPPORTED_ZSD_MODE[] = "zsd-supported";
//
const char MtkCameraParameters::KEY_FPS_MODE[] = "fps-mode";
//
const char MtkCameraParameters::KEY_FOCUS_DRAW[] = "af-draw";
//
const char MtkCameraParameters::KEY_CAPTURE_MODE[] = "cap-mode";
const char MtkCameraParameters::KEY_SUPPORTED_CAPTURE_MODES[] = "cap-mode-values";
const char MtkCameraParameters::KEY_CAPTURE_PATH[] = "capfname";
const char MtkCameraParameters::KEY_BURST_SHOT_NUM[] = "burst-num";
//
const char MtkCameraParameters::KEY_MATV_PREVIEW_DELAY[] = "tv-delay";
const char MtkCameraParameters::KEY_PANORAMA_IDX[] = "pano-idx";
const char MtkCameraParameters::KEY_PANORAMA_DIR[] = "pano-dir";

// Values for KEY_EXPOSURE
const char MtkCameraParameters::EXPOSURE_METER_SPOT[] = "spot";
const char MtkCameraParameters::EXPOSURE_METER_CENTER[] = "center";
const char MtkCameraParameters::EXPOSURE_METER_AVERAGE[] = "average";

// Valeus for KEY_ISO_SPEED
const char MtkCameraParameters::ISO_SPEED_AUTO[] = "auto";
const char MtkCameraParameters::ISO_SPEED_100[] = "100";
const char MtkCameraParameters::ISO_SPEED_200[] = "200";
const char MtkCameraParameters::ISO_SPEED_400[] = "400";
const char MtkCameraParameters::ISO_SPEED_800[] = "800";
const char MtkCameraParameters::ISO_SPEED_1600[] = "1600";

// Values for KEY_AE_MODE = "ae-mode"

// Values for KEY_FOCUS_METER
const char MtkCameraParameters::FOCUS_METER_SPOT[] = "spot";
const char MtkCameraParameters::FOCUS_METER_MULTI[] = "multi";

// AWB2PASS
const char MtkCameraParameters::KEY_AWB2PASS[] = "awb-2pass"; 


//
//  Camera Mode
const char MtkCameraParameters::KEY_CAMERA_MODE[] = "cam-mode";
// Values for KEY_CAMERA_MODE
const int MtkCameraParameters::CAMERA_MODE_NORMAL  = 0;
const int MtkCameraParameters::CAMERA_MODE_MTK_PRV = 1;
const int MtkCameraParameters::CAMERA_MODE_MTK_VDO = 2;
const int MtkCameraParameters::CAMERA_MODE_MTK_VT  = 3;

// Values for KEY_FPS_MODE
const int MtkCameraParameters::FPS_MODE_NORMAL = 0;
const int MtkCameraParameters::FPS_MODE_FIX = 1;

// Values for raw save mode

// Values for KEY_FOCUS_DRAW

// Values for capture mode
const char MtkCameraParameters::CAPTURE_MODE_PANORAMA_SHOT[] = "panoramashot";
const char MtkCameraParameters::CAPTURE_MODE_BURST_SHOT[] = "burstshot";
const char MtkCameraParameters::CAPTURE_MODE_NORMAL[] = "normal";
const char MtkCameraParameters::CAPTURE_MODE_BEST_SHOT[] = "bestshot";
const char MtkCameraParameters::CAPTURE_MODE_EV_BRACKET_SHOT[] = "evbracketshot";
const char MtkCameraParameters::CAPTURE_MODE_SMILE_SHOT[] = "smileshot";
const char MtkCameraParameters::CAPTURE_MODE_MAV_SHOT[] = "mav"; 
const char MtkCameraParameters::CAPTURE_MODE_AUTO_PANORAMA_SHOT[] = "autorama"; 
const char MtkCameraParameters::CAPTURE_MODE_HDR_SHOT[] = "hdr"; 
const char MtkCameraParameters::CAPTURE_MODE_ASD_SHOT[] = "asd"; 
const char MtkCameraParameters::CAPTURE_MODE_ZSD_SHOT[] = "zsd";
const char MtkCameraParameters::CAPTURE_MODE_PANO_3D[] = "pano_3d"; 
const char MtkCameraParameters::CAPTURE_MODE_SINGLE_3D[] = "single_3d"; 
const char MtkCameraParameters::CAPTURE_MODE_FACE_BEAUTY[] = "face_beauty"; 
const char MtkCameraParameters::CAPTURE_MODE_CONTINUOUS_SHOT[] = "continuousshot";
const char MtkCameraParameters::CAPTURE_MODE_MULTI_MOTION[] = "multi_motion";

// Values for panorama direction settings
const char MtkCameraParameters::PANORAMA_DIR_RIGHT[] = "right";
const char MtkCameraParameters::PANORAMA_DIR_LEFT[] = "left";
const char MtkCameraParameters::PANORAMA_DIR_TOP[] = "top";
const char MtkCameraParameters::PANORAMA_DIR_DOWN[] = "down";

//
const int MtkCameraParameters::ENABLE = 1;
const int MtkCameraParameters::DISABLE = 0;

// Values for KEY_EDGE, KEY_HUE, KEY_SATURATION, KEY_BRIGHTNESS, KEY_CONTRAST
const char MtkCameraParameters::HIGH[] = "high";
const char MtkCameraParameters::MIDDLE[] = "middle";
const char MtkCameraParameters::LOW[] = "low";

// Preview Internal Format.
const char MtkCameraParameters::KEY_PREVIEW_INT_FORMAT[] = "prv-int-fmt";

// Pixel color formats for KEY_PREVIEW_FORMAT, KEY_PICTURE_FORMAT,
// and KEY_VIDEO_FRAME_FORMAT
const char MtkCameraParameters::PIXEL_FORMAT_YUV420I[] = "yuv420i-yyuvyy-3plane";
const char MtkCameraParameters::PIXEL_FORMAT_YV12_GPU[] = "yv12-gpu";
const char MtkCameraParameters::PIXEL_FORMAT_YUV422I_UYVY[] = "yuv422i-uyvy";
const char MtkCameraParameters::PIXEL_FORMAT_YUV422I_VYUY[] = "yuv422i-vyuy";
const char MtkCameraParameters::PIXEL_FORMAT_YUV422I_YVYU[] = "yuv422i-yvyu";

const char MtkCameraParameters::PIXEL_FORMAT_BAYER8[] = "bayer8"; 
const char MtkCameraParameters::PIXEL_FORMAT_BAYER10[] = "bayer10";  

const char MtkCameraParameters::KEY_BRIGHTNESS_VALUE[] = "brightness_value";

// ISP Operation mode for meta mode use 
const char MtkCameraParameters::KEY_ISP_MODE[] = "isp-mode"; 
// AF 
const char MtkCameraParameters::KEY_AF_X[] = "af-x"; 
const char MtkCameraParameters::KEY_AF_Y[] = "af-y"; 
// Effect 
const char MtkCameraParameters::EFFECT_SEPIA_BLUE[] = "sepiablue";
const char MtkCameraParameters::EFFECT_SEPIA_GREEN[] = "sepiagreen";

//
//  on/off => FIXME: should be replaced with TRUE[]
const char MtkCameraParameters::ON[] = "on";
const char MtkCameraParameters::OFF[] = "off";
// 
const char MtkCameraParameters::WHITE_BALANCE_TUNGSTEN[] = "tungsten";
//
const char MtkCameraParameters::ISO_SPEED_ENG[] = "iso-speed-eng";
const char MtkCameraParameters::KEY_RAW_SAVE_MODE[] = "rawsave-mode";
const char MtkCameraParameters::KEY_RAW_PATH[] = "rawfname";

const char MtkCameraParameters::KEY_FAST_CONTINUOUS_SHOT[] = "fast-continuous-shot";

// AF EM MODE
const char MtkCameraParameters::KEY_FOCUS_ENG_MODE[]		= "afeng-mode";
const char MtkCameraParameters::KEY_FOCUS_ENG_STEP[] 		= "afeng-pos";
const char MtkCameraParameters::KEY_FOCUS_ENG_MAX_STEP[] 	= "afeng-max-focus-step";
const char MtkCameraParameters::KEY_FOCUS_ENG_MIN_STEP[] 	= "afeng-min-focus-step";
const char MtkCameraParameters::KEY_FOCUS_ENG_BEST_STEP[]   = "afeng-best-focus-step";
const char MtkCameraParameters::KEY_RAW_DUMP_FLAG[]         = "afeng_raw_dump_flag";
const char MtkCameraParameters::KEY_PREVIEW_DUMP_RESOLUTION[] = "preview-dump-resolution";
// Values for KEY_PREVIEW_DUMP_RESOLUTION
const int MtkCameraParameters::PREVIEW_DUMP_RESOLUTION_NORMAL  = 0;
const int MtkCameraParameters::PREVIEW_DUMP_RESOLUTION_CROP  = 1;

const char MtkCameraParameters::KEY_VIDEO_HDR[] = "video-hdr"; 

}; // namespace android
