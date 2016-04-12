/*
 * Copyright (C) 2015 The Android Open Source Project
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

#pragma once

#include <string>
#include <system/audio_policy.h>

//////////////////////////////////////////////////////////////////////////////////////////////////
//      Definitions for audio policy criteria configuration file (audio_policy_criteria.conf)   //
//                                                                                              //
//      @TODO: scripted from audio.h & audio_policy,h                                           //
//////////////////////////////////////////////////////////////////////////////////////////////////

static const char *const gAudioPolicyCriteriaConfFilePath =
    "/system/etc/audio_policy_criteria.conf";
static const char *const gAudioPolicyCriteriaVendorConfFilePath =
    "/vendor/etc/audio_policy_criteria.conf";

/**
 * PFW instances tags
 */
static const std::string &gPolicyConfTag = "Policy";
static const std::string &gDefaultTag = "Default";
static const std::string &gTypeTag = "Type";

/**
 * PFW elements tags
 */
static const std::string &gInclusiveCriterionTypeTag = "InclusiveCriterionType";
static const std::string &gExclusiveCriterionTypeTag = "ExclusiveCriterionType";
static const std::string &gCriterionTag = "Criterion";

/**
 * PFW known criterion tags
 */
static const std::string &gInputDeviceCriterionTag = "AvailableInputDevices";
static const std::string &gOutputDeviceCriterionTag = "AvailableOutputDevices";
static const std::string &gPhoneStateCriterionTag = "TelephonyMode";

/**
 * Order MUST be align with defintiion of audio_policy_force_use_t within audio_policy.h
 */
static const std::string gForceUseCriterionTag[AUDIO_POLICY_FORCE_USE_CNT] =
{
    [AUDIO_POLICY_FORCE_FOR_COMMUNICATION] =        "ForceUseForCommunication",
    [AUDIO_POLICY_FORCE_FOR_MEDIA] =                "ForceUseForMedia",
    [AUDIO_POLICY_FORCE_FOR_RECORD] =               "ForceUseForRecord",
    [AUDIO_POLICY_FORCE_FOR_DOCK] =                 "ForceUseForDock",
    [AUDIO_POLICY_FORCE_FOR_SYSTEM] =               "ForceUseForSystem",
    [AUDIO_POLICY_FORCE_FOR_HDMI_SYSTEM_AUDIO] =    "ForceUseForHdmiSystemAudio",
    [AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND] =     "ForceUseForEncodedSurround"
};






