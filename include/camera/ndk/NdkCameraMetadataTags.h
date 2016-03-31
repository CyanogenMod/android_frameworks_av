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

/*
 * This file defines an NDK API.
 * Do not remove methods.
 * Do not change method signatures.
 * Do not change the value of constants.
 * Do not change the size of any of the classes defined in here.
 * Do not reference types that are not part of the NDK.
 * Do not #include files that aren't part of the NDK.
 */

#ifndef _NDK_CAMERA_METADATA_TAGS_H
#define _NDK_CAMERA_METADATA_TAGS_H

typedef enum acamera_metadata_section {
    ACAMERA_COLOR_CORRECTION,
    ACAMERA_CONTROL,
    ACAMERA_DEMOSAIC,
    ACAMERA_EDGE,
    ACAMERA_FLASH,
    ACAMERA_FLASH_INFO,
    ACAMERA_HOT_PIXEL,
    ACAMERA_JPEG,
    ACAMERA_LENS,
    ACAMERA_LENS_INFO,
    ACAMERA_NOISE_REDUCTION,
    ACAMERA_QUIRKS,
    ACAMERA_REQUEST,
    ACAMERA_SCALER,
    ACAMERA_SENSOR,
    ACAMERA_SENSOR_INFO,
    ACAMERA_SHADING,
    ACAMERA_STATISTICS,
    ACAMERA_STATISTICS_INFO,
    ACAMERA_TONEMAP,
    ACAMERA_LED,
    ACAMERA_INFO,
    ACAMERA_BLACK_LEVEL,
    ACAMERA_SYNC,
    ACAMERA_REPROCESS,
    ACAMERA_DEPTH,
    ACAMERA_SECTION_COUNT,

    ACAMERA_VENDOR = 0x8000
} acamera_metadata_section_t;

/**
 * Hierarchy positions in enum space.
 */
typedef enum acamera_metadata_section_start {
    ACAMERA_COLOR_CORRECTION_START = ACAMERA_COLOR_CORRECTION  << 16,
    ACAMERA_CONTROL_START          = ACAMERA_CONTROL           << 16,
    ACAMERA_DEMOSAIC_START         = ACAMERA_DEMOSAIC          << 16,
    ACAMERA_EDGE_START             = ACAMERA_EDGE              << 16,
    ACAMERA_FLASH_START            = ACAMERA_FLASH             << 16,
    ACAMERA_FLASH_INFO_START       = ACAMERA_FLASH_INFO        << 16,
    ACAMERA_HOT_PIXEL_START        = ACAMERA_HOT_PIXEL         << 16,
    ACAMERA_JPEG_START             = ACAMERA_JPEG              << 16,
    ACAMERA_LENS_START             = ACAMERA_LENS              << 16,
    ACAMERA_LENS_INFO_START        = ACAMERA_LENS_INFO         << 16,
    ACAMERA_NOISE_REDUCTION_START  = ACAMERA_NOISE_REDUCTION   << 16,
    ACAMERA_QUIRKS_START           = ACAMERA_QUIRKS            << 16,
    ACAMERA_REQUEST_START          = ACAMERA_REQUEST           << 16,
    ACAMERA_SCALER_START           = ACAMERA_SCALER            << 16,
    ACAMERA_SENSOR_START           = ACAMERA_SENSOR            << 16,
    ACAMERA_SENSOR_INFO_START      = ACAMERA_SENSOR_INFO       << 16,
    ACAMERA_SHADING_START          = ACAMERA_SHADING           << 16,
    ACAMERA_STATISTICS_START       = ACAMERA_STATISTICS        << 16,
    ACAMERA_STATISTICS_INFO_START  = ACAMERA_STATISTICS_INFO   << 16,
    ACAMERA_TONEMAP_START          = ACAMERA_TONEMAP           << 16,
    ACAMERA_LED_START              = ACAMERA_LED               << 16,
    ACAMERA_INFO_START             = ACAMERA_INFO              << 16,
    ACAMERA_BLACK_LEVEL_START      = ACAMERA_BLACK_LEVEL       << 16,
    ACAMERA_SYNC_START             = ACAMERA_SYNC              << 16,
    ACAMERA_REPROCESS_START        = ACAMERA_REPROCESS         << 16,
    ACAMERA_DEPTH_START            = ACAMERA_DEPTH             << 16,
    ACAMERA_VENDOR_START           = ACAMERA_VENDOR            << 16
} acamera_metadata_section_start_t;

/**
 * Main enum for camera metadata tags.
 */
typedef enum acamera_metadata_tag {
    ACAMERA_COLOR_CORRECTION_MODE =                             // byte (enum)
            ACAMERA_COLOR_CORRECTION_START,
    ACAMERA_COLOR_CORRECTION_TRANSFORM =                        // rational[3*3]
            ACAMERA_COLOR_CORRECTION_START + 1,
    ACAMERA_COLOR_CORRECTION_GAINS =                            // float[4]
            ACAMERA_COLOR_CORRECTION_START + 2,
    ACAMERA_COLOR_CORRECTION_ABERRATION_MODE =                  // byte (enum)
            ACAMERA_COLOR_CORRECTION_START + 3,
    ACAMERA_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES =       // byte[n]
            ACAMERA_COLOR_CORRECTION_START + 4,
    ACAMERA_COLOR_CORRECTION_END,

    ACAMERA_CONTROL_AE_ANTIBANDING_MODE =                       // byte (enum)
            ACAMERA_CONTROL_START,
    ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION =                  // int32
            ACAMERA_CONTROL_START + 1,
    ACAMERA_CONTROL_AE_LOCK =                                   // byte (enum)
            ACAMERA_CONTROL_START + 2,
    ACAMERA_CONTROL_AE_MODE =                                   // byte (enum)
            ACAMERA_CONTROL_START + 3,
    ACAMERA_CONTROL_AE_REGIONS =                                // int32[5*area_count]
            ACAMERA_CONTROL_START + 4,
    ACAMERA_CONTROL_AE_TARGET_FPS_RANGE =                       // int32[2]
            ACAMERA_CONTROL_START + 5,
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER =                     // byte (enum)
            ACAMERA_CONTROL_START + 6,
    ACAMERA_CONTROL_AF_MODE =                                   // byte (enum)
            ACAMERA_CONTROL_START + 7,
    ACAMERA_CONTROL_AF_REGIONS =                                // int32[5*area_count]
            ACAMERA_CONTROL_START + 8,
    ACAMERA_CONTROL_AF_TRIGGER =                                // byte (enum)
            ACAMERA_CONTROL_START + 9,
    ACAMERA_CONTROL_AWB_LOCK =                                  // byte (enum)
            ACAMERA_CONTROL_START + 10,
    ACAMERA_CONTROL_AWB_MODE =                                  // byte (enum)
            ACAMERA_CONTROL_START + 11,
    ACAMERA_CONTROL_AWB_REGIONS =                               // int32[5*area_count]
            ACAMERA_CONTROL_START + 12,
    ACAMERA_CONTROL_CAPTURE_INTENT =                            // byte (enum)
            ACAMERA_CONTROL_START + 13,
    ACAMERA_CONTROL_EFFECT_MODE =                               // byte (enum)
            ACAMERA_CONTROL_START + 14,
    ACAMERA_CONTROL_MODE =                                      // byte (enum)
            ACAMERA_CONTROL_START + 15,
    ACAMERA_CONTROL_SCENE_MODE =                                // byte (enum)
            ACAMERA_CONTROL_START + 16,
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE =                  // byte (enum)
            ACAMERA_CONTROL_START + 17,
    ACAMERA_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES =            // byte[n]
            ACAMERA_CONTROL_START + 18,
    ACAMERA_CONTROL_AE_AVAILABLE_MODES =                        // byte[n]
            ACAMERA_CONTROL_START + 19,
    ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES =            // int32[2*n]
            ACAMERA_CONTROL_START + 20,
    ACAMERA_CONTROL_AE_COMPENSATION_RANGE =                     // int32[2]
            ACAMERA_CONTROL_START + 21,
    ACAMERA_CONTROL_AE_COMPENSATION_STEP =                      // rational
            ACAMERA_CONTROL_START + 22,
    ACAMERA_CONTROL_AF_AVAILABLE_MODES =                        // byte[n]
            ACAMERA_CONTROL_START + 23,
    ACAMERA_CONTROL_AVAILABLE_EFFECTS =                         // byte[n]
            ACAMERA_CONTROL_START + 24,
    ACAMERA_CONTROL_AVAILABLE_SCENE_MODES =                     // byte[n]
            ACAMERA_CONTROL_START + 25,
    ACAMERA_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES =       // byte[n]
            ACAMERA_CONTROL_START + 26,
    ACAMERA_CONTROL_AWB_AVAILABLE_MODES =                       // byte[n]
            ACAMERA_CONTROL_START + 27,
    ACAMERA_CONTROL_MAX_REGIONS =                               // int32[3]
            ACAMERA_CONTROL_START + 28,
    ACAMERA_CONTROL_AE_STATE =                                  // byte (enum)
            ACAMERA_CONTROL_START + 31,
    ACAMERA_CONTROL_AF_STATE =                                  // byte (enum)
            ACAMERA_CONTROL_START + 32,
    ACAMERA_CONTROL_AWB_STATE =                                 // byte (enum)
            ACAMERA_CONTROL_START + 34,
    ACAMERA_CONTROL_AVAILABLE_HIGH_SPEED_VIDEO_CONFIGURATIONS = // int32[5*n]
            ACAMERA_CONTROL_START + 35,
    ACAMERA_CONTROL_AE_LOCK_AVAILABLE =                         // byte (enum)
            ACAMERA_CONTROL_START + 36,
    ACAMERA_CONTROL_AWB_LOCK_AVAILABLE =                        // byte (enum)
            ACAMERA_CONTROL_START + 37,
    ACAMERA_CONTROL_AVAILABLE_MODES =                           // byte[n]
            ACAMERA_CONTROL_START + 38,
    ACAMERA_CONTROL_POST_RAW_SENSITIVITY_BOOST_RANGE =          // int32[2]
            ACAMERA_CONTROL_START + 39,
    ACAMERA_CONTROL_POST_RAW_SENSITIVITY_BOOST =                // int32
            ACAMERA_CONTROL_START + 40,
    ACAMERA_CONTROL_END,

    ACAMERA_EDGE_MODE =                                         // byte (enum)
            ACAMERA_EDGE_START,
    ACAMERA_EDGE_AVAILABLE_EDGE_MODES =                         // byte[n]
            ACAMERA_EDGE_START + 2,
    ACAMERA_EDGE_END,

    ACAMERA_FLASH_MODE =                                        // byte (enum)
            ACAMERA_FLASH_START + 2,
    ACAMERA_FLASH_STATE =                                       // byte (enum)
            ACAMERA_FLASH_START + 5,
    ACAMERA_FLASH_END,

    ACAMERA_FLASH_INFO_AVAILABLE =                              // byte (enum)
            ACAMERA_FLASH_INFO_START,
    ACAMERA_FLASH_INFO_END,

    ACAMERA_HOT_PIXEL_MODE =                                    // byte (enum)
            ACAMERA_HOT_PIXEL_START,
    ACAMERA_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES =               // byte[n]
            ACAMERA_HOT_PIXEL_START + 1,
    ACAMERA_HOT_PIXEL_END,

    ACAMERA_JPEG_GPS_COORDINATES =                              // double[3]
            ACAMERA_JPEG_START,
    ACAMERA_JPEG_GPS_PROCESSING_METHOD =                        // byte
            ACAMERA_JPEG_START + 1,
    ACAMERA_JPEG_GPS_TIMESTAMP =                                // int64
            ACAMERA_JPEG_START + 2,
    ACAMERA_JPEG_ORIENTATION =                                  // int32
            ACAMERA_JPEG_START + 3,
    ACAMERA_JPEG_QUALITY =                                      // byte
            ACAMERA_JPEG_START + 4,
    ACAMERA_JPEG_THUMBNAIL_QUALITY =                            // byte
            ACAMERA_JPEG_START + 5,
    ACAMERA_JPEG_THUMBNAIL_SIZE =                               // int32[2]
            ACAMERA_JPEG_START + 6,
    ACAMERA_JPEG_AVAILABLE_THUMBNAIL_SIZES =                    // int32[2*n]
            ACAMERA_JPEG_START + 7,
    ACAMERA_JPEG_END,

    ACAMERA_LENS_APERTURE =                                     // float
            ACAMERA_LENS_START,
    ACAMERA_LENS_FILTER_DENSITY =                               // float
            ACAMERA_LENS_START + 1,
    ACAMERA_LENS_FOCAL_LENGTH =                                 // float
            ACAMERA_LENS_START + 2,
    ACAMERA_LENS_FOCUS_DISTANCE =                               // float
            ACAMERA_LENS_START + 3,
    ACAMERA_LENS_OPTICAL_STABILIZATION_MODE =                   // byte (enum)
            ACAMERA_LENS_START + 4,
    ACAMERA_LENS_FACING =                                       // byte (enum)
            ACAMERA_LENS_START + 5,
    ACAMERA_LENS_POSE_ROTATION =                                // float[4]
            ACAMERA_LENS_START + 6,
    ACAMERA_LENS_POSE_TRANSLATION =                             // float[3]
            ACAMERA_LENS_START + 7,
    ACAMERA_LENS_FOCUS_RANGE =                                  // float[2]
            ACAMERA_LENS_START + 8,
    ACAMERA_LENS_STATE =                                        // byte (enum)
            ACAMERA_LENS_START + 9,
    ACAMERA_LENS_INTRINSIC_CALIBRATION =                        // float[5]
            ACAMERA_LENS_START + 10,
    ACAMERA_LENS_RADIAL_DISTORTION =                            // float[6]
            ACAMERA_LENS_START + 11,
    ACAMERA_LENS_END,

    ACAMERA_LENS_INFO_AVAILABLE_APERTURES =                     // float[n]
            ACAMERA_LENS_INFO_START,
    ACAMERA_LENS_INFO_AVAILABLE_FILTER_DENSITIES =              // float[n]
            ACAMERA_LENS_INFO_START + 1,
    ACAMERA_LENS_INFO_AVAILABLE_FOCAL_LENGTHS =                 // float[n]
            ACAMERA_LENS_INFO_START + 2,
    ACAMERA_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION =         // byte[n]
            ACAMERA_LENS_INFO_START + 3,
    ACAMERA_LENS_INFO_HYPERFOCAL_DISTANCE =                     // float
            ACAMERA_LENS_INFO_START + 4,
    ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE =                  // float
            ACAMERA_LENS_INFO_START + 5,
    ACAMERA_LENS_INFO_SHADING_MAP_SIZE =                        // int32[2]
            ACAMERA_LENS_INFO_START + 6,
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION =              // byte (enum)
            ACAMERA_LENS_INFO_START + 7,
    ACAMERA_LENS_INFO_END,

    ACAMERA_NOISE_REDUCTION_MODE =                              // byte (enum)
            ACAMERA_NOISE_REDUCTION_START,
    ACAMERA_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES =   // byte[n]
            ACAMERA_NOISE_REDUCTION_START + 2,
    ACAMERA_NOISE_REDUCTION_END,

    ACAMERA_QUIRKS_USE_PARTIAL_RESULT =                         // Deprecated! DO NOT USE
            ACAMERA_QUIRKS_START + 3,
    ACAMERA_QUIRKS_PARTIAL_RESULT =                             // Deprecated! DO NOT USE
            ACAMERA_QUIRKS_START + 4,
    ACAMERA_QUIRKS_END,

    ACAMERA_REQUEST_FRAME_COUNT =                               // Deprecated! DO NOT USE
            ACAMERA_REQUEST_START,
    ACAMERA_REQUEST_ID =                                        // int32
            ACAMERA_REQUEST_START + 1,
    ACAMERA_REQUEST_MAX_NUM_OUTPUT_STREAMS =                    // int32[3]
            ACAMERA_REQUEST_START + 6,
    ACAMERA_REQUEST_MAX_NUM_INPUT_STREAMS =                     // int32
            ACAMERA_REQUEST_START + 8,
    ACAMERA_REQUEST_PIPELINE_DEPTH =                            // byte
            ACAMERA_REQUEST_START + 9,
    ACAMERA_REQUEST_PIPELINE_MAX_DEPTH =                        // byte
            ACAMERA_REQUEST_START + 10,
    ACAMERA_REQUEST_PARTIAL_RESULT_COUNT =                      // int32
            ACAMERA_REQUEST_START + 11,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES =                    // byte[n] (enum)
            ACAMERA_REQUEST_START + 12,
    ACAMERA_REQUEST_AVAILABLE_REQUEST_KEYS =                    // int32[n]
            ACAMERA_REQUEST_START + 13,
    ACAMERA_REQUEST_AVAILABLE_RESULT_KEYS =                     // int32[n]
            ACAMERA_REQUEST_START + 14,
    ACAMERA_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS =            // int32[n]
            ACAMERA_REQUEST_START + 15,
    ACAMERA_REQUEST_END,

    ACAMERA_SCALER_CROP_REGION =                                // int32[4]
            ACAMERA_SCALER_START,
    ACAMERA_SCALER_AVAILABLE_FORMATS =                          // Deprecated! DO NOT USE
            ACAMERA_SCALER_START + 1,
    ACAMERA_SCALER_AVAILABLE_JPEG_MIN_DURATIONS =               // Deprecated! DO NOT USE
            ACAMERA_SCALER_START + 2,
    ACAMERA_SCALER_AVAILABLE_JPEG_SIZES =                       // Deprecated! DO NOT USE
            ACAMERA_SCALER_START + 3,
    ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM =                 // float
            ACAMERA_SCALER_START + 4,
    ACAMERA_SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS =          // Deprecated! DO NOT USE
            ACAMERA_SCALER_START + 5,
    ACAMERA_SCALER_AVAILABLE_PROCESSED_SIZES =                  // Deprecated! DO NOT USE
            ACAMERA_SCALER_START + 6,
    ACAMERA_SCALER_AVAILABLE_INPUT_OUTPUT_FORMATS_MAP =         // int32
            ACAMERA_SCALER_START + 9,
    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS =            // int32[n*4] (enum)
            ACAMERA_SCALER_START + 10,
    ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS =              // int64[4*n]
            ACAMERA_SCALER_START + 11,
    ACAMERA_SCALER_AVAILABLE_STALL_DURATIONS =                  // int64[4*n]
            ACAMERA_SCALER_START + 12,
    ACAMERA_SCALER_CROPPING_TYPE =                              // byte (enum)
            ACAMERA_SCALER_START + 13,
    ACAMERA_SCALER_END,

    ACAMERA_SENSOR_EXPOSURE_TIME =                              // int64
            ACAMERA_SENSOR_START,
    ACAMERA_SENSOR_FRAME_DURATION =                             // int64
            ACAMERA_SENSOR_START + 1,
    ACAMERA_SENSOR_SENSITIVITY =                                // int32
            ACAMERA_SENSOR_START + 2,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1 =                      // byte (enum)
            ACAMERA_SENSOR_START + 3,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT2 =                      // byte
            ACAMERA_SENSOR_START + 4,
    ACAMERA_SENSOR_CALIBRATION_TRANSFORM1 =                     // rational[3*3]
            ACAMERA_SENSOR_START + 5,
    ACAMERA_SENSOR_CALIBRATION_TRANSFORM2 =                     // rational[3*3]
            ACAMERA_SENSOR_START + 6,
    ACAMERA_SENSOR_COLOR_TRANSFORM1 =                           // rational[3*3]
            ACAMERA_SENSOR_START + 7,
    ACAMERA_SENSOR_COLOR_TRANSFORM2 =                           // rational[3*3]
            ACAMERA_SENSOR_START + 8,
    ACAMERA_SENSOR_FORWARD_MATRIX1 =                            // rational[3*3]
            ACAMERA_SENSOR_START + 9,
    ACAMERA_SENSOR_FORWARD_MATRIX2 =                            // rational[3*3]
            ACAMERA_SENSOR_START + 10,
    ACAMERA_SENSOR_BLACK_LEVEL_PATTERN =                        // int32[4]
            ACAMERA_SENSOR_START + 12,
    ACAMERA_SENSOR_MAX_ANALOG_SENSITIVITY =                     // int32
            ACAMERA_SENSOR_START + 13,
    ACAMERA_SENSOR_ORIENTATION =                                // int32
            ACAMERA_SENSOR_START + 14,
    ACAMERA_SENSOR_TIMESTAMP =                                  // int64
            ACAMERA_SENSOR_START + 16,
    ACAMERA_SENSOR_NEUTRAL_COLOR_POINT =                        // rational[3]
            ACAMERA_SENSOR_START + 18,
    ACAMERA_SENSOR_NOISE_PROFILE =                              // double[2*CFA Channels]
            ACAMERA_SENSOR_START + 19,
    ACAMERA_SENSOR_GREEN_SPLIT =                                // float
            ACAMERA_SENSOR_START + 22,
    ACAMERA_SENSOR_TEST_PATTERN_DATA =                          // int32[4]
            ACAMERA_SENSOR_START + 23,
    ACAMERA_SENSOR_TEST_PATTERN_MODE =                          // int32 (enum)
            ACAMERA_SENSOR_START + 24,
    ACAMERA_SENSOR_AVAILABLE_TEST_PATTERN_MODES =               // int32[n]
            ACAMERA_SENSOR_START + 25,
    ACAMERA_SENSOR_ROLLING_SHUTTER_SKEW =                       // int64
            ACAMERA_SENSOR_START + 26,
    ACAMERA_SENSOR_OPTICAL_BLACK_REGIONS =                      // int32[4*num_regions]
            ACAMERA_SENSOR_START + 27,
    ACAMERA_SENSOR_DYNAMIC_BLACK_LEVEL =                        // float[4]
            ACAMERA_SENSOR_START + 28,
    ACAMERA_SENSOR_DYNAMIC_WHITE_LEVEL =                        // int32
            ACAMERA_SENSOR_START + 29,
    ACAMERA_SENSOR_END,

    ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE =                     // int32[4]
            ACAMERA_SENSOR_INFO_START,
    ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE =                     // int32[2]
            ACAMERA_SENSOR_INFO_START + 1,
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT =              // byte (enum)
            ACAMERA_SENSOR_INFO_START + 2,
    ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE =                   // int64[2]
            ACAMERA_SENSOR_INFO_START + 3,
    ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION =                    // int64
            ACAMERA_SENSOR_INFO_START + 4,
    ACAMERA_SENSOR_INFO_PHYSICAL_SIZE =                         // float[2]
            ACAMERA_SENSOR_INFO_START + 5,
    ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE =                      // int32[2]
            ACAMERA_SENSOR_INFO_START + 6,
    ACAMERA_SENSOR_INFO_WHITE_LEVEL =                           // int32
            ACAMERA_SENSOR_INFO_START + 7,
    ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE =                      // byte (enum)
            ACAMERA_SENSOR_INFO_START + 8,
    ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED =                  // byte (enum)
            ACAMERA_SENSOR_INFO_START + 9,
    ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE =      // int32[4]
            ACAMERA_SENSOR_INFO_START + 10,
    ACAMERA_SENSOR_INFO_END,

    ACAMERA_SHADING_MODE =                                      // byte (enum)
            ACAMERA_SHADING_START,
    ACAMERA_SHADING_AVAILABLE_MODES =                           // byte[n]
            ACAMERA_SHADING_START + 2,
    ACAMERA_SHADING_END,

    ACAMERA_STATISTICS_FACE_DETECT_MODE =                       // byte (enum)
            ACAMERA_STATISTICS_START,
    ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE =                     // byte (enum)
            ACAMERA_STATISTICS_START + 3,
    ACAMERA_STATISTICS_FACE_IDS =                               // int32[n]
            ACAMERA_STATISTICS_START + 4,
    ACAMERA_STATISTICS_FACE_LANDMARKS =                         // int32[n*6]
            ACAMERA_STATISTICS_START + 5,
    ACAMERA_STATISTICS_FACE_RECTANGLES =                        // int32[n*4]
            ACAMERA_STATISTICS_START + 6,
    ACAMERA_STATISTICS_FACE_SCORES =                            // byte[n]
            ACAMERA_STATISTICS_START + 7,
    ACAMERA_STATISTICS_LENS_SHADING_CORRECTION_MAP =            // byte
            ACAMERA_STATISTICS_START + 10,
    ACAMERA_STATISTICS_LENS_SHADING_MAP =                       // float[4*n*m]
            ACAMERA_STATISTICS_START + 11,
    ACAMERA_STATISTICS_PREDICTED_COLOR_GAINS =                  // Deprecated! DO NOT USE
            ACAMERA_STATISTICS_START + 12,
    ACAMERA_STATISTICS_PREDICTED_COLOR_TRANSFORM =              // Deprecated! DO NOT USE
            ACAMERA_STATISTICS_START + 13,
    ACAMERA_STATISTICS_SCENE_FLICKER =                          // byte (enum)
            ACAMERA_STATISTICS_START + 14,
    ACAMERA_STATISTICS_HOT_PIXEL_MAP =                          // int32[2*n]
            ACAMERA_STATISTICS_START + 15,
    ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE =                  // byte (enum)
            ACAMERA_STATISTICS_START + 16,
    ACAMERA_STATISTICS_END,

    ACAMERA_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES =       // byte[n]
            ACAMERA_STATISTICS_INFO_START,
    ACAMERA_STATISTICS_INFO_MAX_FACE_COUNT =                    // int32
            ACAMERA_STATISTICS_INFO_START + 2,
    ACAMERA_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES =     // byte[n]
            ACAMERA_STATISTICS_INFO_START + 6,
    ACAMERA_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES =  // byte[n]
            ACAMERA_STATISTICS_INFO_START + 7,
    ACAMERA_STATISTICS_INFO_END,

    ACAMERA_TONEMAP_CURVE_BLUE =                                // float[n*2]
            ACAMERA_TONEMAP_START,
    ACAMERA_TONEMAP_CURVE_GREEN =                               // float[n*2]
            ACAMERA_TONEMAP_START + 1,
    ACAMERA_TONEMAP_CURVE_RED =                                 // float[n*2]
            ACAMERA_TONEMAP_START + 2,
    ACAMERA_TONEMAP_MODE =                                      // byte (enum)
            ACAMERA_TONEMAP_START + 3,
    ACAMERA_TONEMAP_MAX_CURVE_POINTS =                          // int32
            ACAMERA_TONEMAP_START + 4,
    ACAMERA_TONEMAP_AVAILABLE_TONE_MAP_MODES =                  // byte[n]
            ACAMERA_TONEMAP_START + 5,
    ACAMERA_TONEMAP_GAMMA =                                     // float
            ACAMERA_TONEMAP_START + 6,
    ACAMERA_TONEMAP_PRESET_CURVE =                              // byte (enum)
            ACAMERA_TONEMAP_START + 7,
    ACAMERA_TONEMAP_END,

    ACAMERA_LED_TRANSMIT =                                      // byte (enum)
            ACAMERA_LED_START,
    ACAMERA_LED_AVAILABLE_LEDS =                                // byte[n] (enum)
            ACAMERA_LED_START + 1,
    ACAMERA_LED_END,

    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL =                     // byte (enum)
            ACAMERA_INFO_START,
    ACAMERA_INFO_END,

    ACAMERA_BLACK_LEVEL_LOCK =                                  // byte (enum)
            ACAMERA_BLACK_LEVEL_START,
    ACAMERA_BLACK_LEVEL_END,

    ACAMERA_SYNC_FRAME_NUMBER =                                 // int64 (enum)
            ACAMERA_SYNC_START,
    ACAMERA_SYNC_MAX_LATENCY =                                  // int32 (enum)
            ACAMERA_SYNC_START + 1,
    ACAMERA_SYNC_END,

    ACAMERA_REPROCESS_EFFECTIVE_EXPOSURE_FACTOR =               // float
            ACAMERA_REPROCESS_START,
    ACAMERA_REPROCESS_MAX_CAPTURE_STALL =                       // int32
            ACAMERA_REPROCESS_START + 1,
    ACAMERA_REPROCESS_END,

    ACAMERA_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS =       // int32[n*4] (enum)
            ACAMERA_DEPTH_START + 1,
    ACAMERA_DEPTH_AVAILABLE_DEPTH_MIN_FRAME_DURATIONS =         // int64[4*n]
            ACAMERA_DEPTH_START + 2,
    ACAMERA_DEPTH_AVAILABLE_DEPTH_STALL_DURATIONS =             // int64[4*n]
            ACAMERA_DEPTH_START + 3,
    ACAMERA_DEPTH_DEPTH_IS_EXCLUSIVE =                          // byte (enum)
            ACAMERA_DEPTH_START + 4,
    ACAMERA_DEPTH_END,

} acamera_metadata_tag_t;

/**
 * Enumeration definitions for the various entries that need them
 */

// ACAMERA_COLOR_CORRECTION_MODE
typedef enum acamera_metadata_enum_acamera_color_correction_mode {
    ACAMERA_COLOR_CORRECTION_MODE_TRANSFORM_MATRIX                   = 0,
    ACAMERA_COLOR_CORRECTION_MODE_FAST                               = 1,
    ACAMERA_COLOR_CORRECTION_MODE_HIGH_QUALITY                       = 2,
} acamera_metadata_enum_android_color_correction_mode_t;

// ACAMERA_COLOR_CORRECTION_ABERRATION_MODE
typedef enum acamera_metadata_enum_acamera_color_correction_aberration_mode {
    ACAMERA_COLOR_CORRECTION_ABERRATION_MODE_OFF                     = 0,
    ACAMERA_COLOR_CORRECTION_ABERRATION_MODE_FAST                    = 1,
    ACAMERA_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY            = 2,
} acamera_metadata_enum_android_color_correction_aberration_mode_t;


// ACAMERA_CONTROL_AE_ANTIBANDING_MODE
typedef enum acamera_metadata_enum_acamera_control_ae_antibanding_mode {
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_OFF                          = 0,
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ                         = 1,
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ                         = 2,
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO                         = 3,
} acamera_metadata_enum_android_control_ae_antibanding_mode_t;

// ACAMERA_CONTROL_AE_LOCK
typedef enum acamera_metadata_enum_acamera_control_ae_lock {
    ACAMERA_CONTROL_AE_LOCK_OFF                                      = 0,
    ACAMERA_CONTROL_AE_LOCK_ON                                       = 1,
} acamera_metadata_enum_android_control_ae_lock_t;

// ACAMERA_CONTROL_AE_MODE
typedef enum acamera_metadata_enum_acamera_control_ae_mode {
    ACAMERA_CONTROL_AE_MODE_OFF                                      = 0,
    ACAMERA_CONTROL_AE_MODE_ON                                       = 1,
    ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH                            = 2,
    ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH                          = 3,
    ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE                     = 4,
} acamera_metadata_enum_android_control_ae_mode_t;

// ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
typedef enum acamera_metadata_enum_acamera_control_ae_precapture_trigger {
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE                       = 0,
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START                      = 1,
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL                     = 2,
} acamera_metadata_enum_android_control_ae_precapture_trigger_t;

// ACAMERA_CONTROL_AF_MODE
typedef enum acamera_metadata_enum_acamera_control_af_mode {
    ACAMERA_CONTROL_AF_MODE_OFF                                      = 0,
    ACAMERA_CONTROL_AF_MODE_AUTO                                     = 1,
    ACAMERA_CONTROL_AF_MODE_MACRO                                    = 2,
    ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO                         = 3,
    ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE                       = 4,
    ACAMERA_CONTROL_AF_MODE_EDOF                                     = 5,
} acamera_metadata_enum_android_control_af_mode_t;

// ACAMERA_CONTROL_AF_TRIGGER
typedef enum acamera_metadata_enum_acamera_control_af_trigger {
    ACAMERA_CONTROL_AF_TRIGGER_IDLE                                  = 0,
    ACAMERA_CONTROL_AF_TRIGGER_START                                 = 1,
    ACAMERA_CONTROL_AF_TRIGGER_CANCEL                                = 2,
} acamera_metadata_enum_android_control_af_trigger_t;

// ACAMERA_CONTROL_AWB_LOCK
typedef enum acamera_metadata_enum_acamera_control_awb_lock {
    ACAMERA_CONTROL_AWB_LOCK_OFF                                     = 0,
    ACAMERA_CONTROL_AWB_LOCK_ON                                      = 1,
} acamera_metadata_enum_android_control_awb_lock_t;

// ACAMERA_CONTROL_AWB_MODE
typedef enum acamera_metadata_enum_acamera_control_awb_mode {
    ACAMERA_CONTROL_AWB_MODE_OFF                                     = 0,
    ACAMERA_CONTROL_AWB_MODE_AUTO                                    = 1,
    ACAMERA_CONTROL_AWB_MODE_INCANDESCENT                            = 2,
    ACAMERA_CONTROL_AWB_MODE_FLUORESCENT                             = 3,
    ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT                        = 4,
    ACAMERA_CONTROL_AWB_MODE_DAYLIGHT                                = 5,
    ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT                         = 6,
    ACAMERA_CONTROL_AWB_MODE_TWILIGHT                                = 7,
    ACAMERA_CONTROL_AWB_MODE_SHADE                                   = 8,
} acamera_metadata_enum_android_control_awb_mode_t;

// ACAMERA_CONTROL_CAPTURE_INTENT
typedef enum acamera_metadata_enum_acamera_control_capture_intent {
    ACAMERA_CONTROL_CAPTURE_INTENT_CUSTOM                            = 0,
    ACAMERA_CONTROL_CAPTURE_INTENT_PREVIEW                           = 1,
    ACAMERA_CONTROL_CAPTURE_INTENT_STILL_CAPTURE                     = 2,
    ACAMERA_CONTROL_CAPTURE_INTENT_VIDEO_RECORD                      = 3,
    ACAMERA_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT                    = 4,
    ACAMERA_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG                  = 5,
    ACAMERA_CONTROL_CAPTURE_INTENT_MANUAL                            = 6,
} acamera_metadata_enum_android_control_capture_intent_t;

// ACAMERA_CONTROL_EFFECT_MODE
typedef enum acamera_metadata_enum_acamera_control_effect_mode {
    ACAMERA_CONTROL_EFFECT_MODE_OFF                                  = 0,
    ACAMERA_CONTROL_EFFECT_MODE_MONO                                 = 1,
    ACAMERA_CONTROL_EFFECT_MODE_NEGATIVE                             = 2,
    ACAMERA_CONTROL_EFFECT_MODE_SOLARIZE                             = 3,
    ACAMERA_CONTROL_EFFECT_MODE_SEPIA                                = 4,
    ACAMERA_CONTROL_EFFECT_MODE_POSTERIZE                            = 5,
    ACAMERA_CONTROL_EFFECT_MODE_WHITEBOARD                           = 6,
    ACAMERA_CONTROL_EFFECT_MODE_BLACKBOARD                           = 7,
    ACAMERA_CONTROL_EFFECT_MODE_AQUA                                 = 8,
} acamera_metadata_enum_android_control_effect_mode_t;

// ACAMERA_CONTROL_MODE
typedef enum acamera_metadata_enum_acamera_control_mode {
    ACAMERA_CONTROL_MODE_OFF                                         = 0,
    ACAMERA_CONTROL_MODE_AUTO                                        = 1,
    ACAMERA_CONTROL_MODE_USE_SCENE_MODE                              = 2,
    ACAMERA_CONTROL_MODE_OFF_KEEP_STATE                              = 3,
} acamera_metadata_enum_android_control_mode_t;

// ACAMERA_CONTROL_SCENE_MODE
typedef enum acamera_metadata_enum_acamera_control_scene_mode {
    ACAMERA_CONTROL_SCENE_MODE_DISABLED                              = 0,
    ACAMERA_CONTROL_SCENE_MODE_FACE_PRIORITY                         = 1,
    ACAMERA_CONTROL_SCENE_MODE_ACTION                                = 2,
    ACAMERA_CONTROL_SCENE_MODE_PORTRAIT                              = 3,
    ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE                             = 4,
    ACAMERA_CONTROL_SCENE_MODE_NIGHT                                 = 5,
    ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT                        = 6,
    ACAMERA_CONTROL_SCENE_MODE_THEATRE                               = 7,
    ACAMERA_CONTROL_SCENE_MODE_BEACH                                 = 8,
    ACAMERA_CONTROL_SCENE_MODE_SNOW                                  = 9,
    ACAMERA_CONTROL_SCENE_MODE_SUNSET                                = 10,
    ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO                           = 11,
    ACAMERA_CONTROL_SCENE_MODE_FIREWORKS                             = 12,
    ACAMERA_CONTROL_SCENE_MODE_SPORTS                                = 13,
    ACAMERA_CONTROL_SCENE_MODE_PARTY                                 = 14,
    ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT                           = 15,
    ACAMERA_CONTROL_SCENE_MODE_BARCODE                               = 16,
    ACAMERA_CONTROL_SCENE_MODE_HIGH_SPEED_VIDEO                      = 17,
    ACAMERA_CONTROL_SCENE_MODE_HDR                                   = 18,
} acamera_metadata_enum_android_control_scene_mode_t;

// ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE
typedef enum acamera_metadata_enum_acamera_control_video_stabilization_mode {
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF                     = 0,
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON                      = 1,
} acamera_metadata_enum_android_control_video_stabilization_mode_t;

// ACAMERA_CONTROL_AE_STATE
typedef enum acamera_metadata_enum_acamera_control_ae_state {
    ACAMERA_CONTROL_AE_STATE_INACTIVE                                = 0,
    ACAMERA_CONTROL_AE_STATE_SEARCHING                               = 1,
    ACAMERA_CONTROL_AE_STATE_CONVERGED                               = 2,
    ACAMERA_CONTROL_AE_STATE_LOCKED                                  = 3,
    ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED                          = 4,
    ACAMERA_CONTROL_AE_STATE_PRECAPTURE                              = 5,
} acamera_metadata_enum_android_control_ae_state_t;

// ACAMERA_CONTROL_AF_STATE
typedef enum acamera_metadata_enum_acamera_control_af_state {
    ACAMERA_CONTROL_AF_STATE_INACTIVE                                = 0,
    ACAMERA_CONTROL_AF_STATE_PASSIVE_SCAN                            = 1,
    ACAMERA_CONTROL_AF_STATE_PASSIVE_FOCUSED                         = 2,
    ACAMERA_CONTROL_AF_STATE_ACTIVE_SCAN                             = 3,
    ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED                          = 4,
    ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED                      = 5,
    ACAMERA_CONTROL_AF_STATE_PASSIVE_UNFOCUSED                       = 6,
} acamera_metadata_enum_android_control_af_state_t;

// ACAMERA_CONTROL_AWB_STATE
typedef enum acamera_metadata_enum_acamera_control_awb_state {
    ACAMERA_CONTROL_AWB_STATE_INACTIVE                               = 0,
    ACAMERA_CONTROL_AWB_STATE_SEARCHING                              = 1,
    ACAMERA_CONTROL_AWB_STATE_CONVERGED                              = 2,
    ACAMERA_CONTROL_AWB_STATE_LOCKED                                 = 3,
} acamera_metadata_enum_android_control_awb_state_t;

// ACAMERA_CONTROL_AE_LOCK_AVAILABLE
typedef enum acamera_metadata_enum_acamera_control_ae_lock_available {
    ACAMERA_CONTROL_AE_LOCK_AVAILABLE_FALSE                          = 0,
    ACAMERA_CONTROL_AE_LOCK_AVAILABLE_TRUE                           = 1,
} acamera_metadata_enum_android_control_ae_lock_available_t;

// ACAMERA_CONTROL_AWB_LOCK_AVAILABLE
typedef enum acamera_metadata_enum_acamera_control_awb_lock_available {
    ACAMERA_CONTROL_AWB_LOCK_AVAILABLE_FALSE                         = 0,
    ACAMERA_CONTROL_AWB_LOCK_AVAILABLE_TRUE                          = 1,
} acamera_metadata_enum_android_control_awb_lock_available_t;



// ACAMERA_EDGE_MODE
typedef enum acamera_metadata_enum_acamera_edge_mode {
    ACAMERA_EDGE_MODE_OFF                                            = 0,
    ACAMERA_EDGE_MODE_FAST                                           = 1,
    ACAMERA_EDGE_MODE_HIGH_QUALITY                                   = 2,
    ACAMERA_EDGE_MODE_ZERO_SHUTTER_LAG                               = 3,
} acamera_metadata_enum_android_edge_mode_t;


// ACAMERA_FLASH_MODE
typedef enum acamera_metadata_enum_acamera_flash_mode {
    ACAMERA_FLASH_MODE_OFF                                           = 0,
    ACAMERA_FLASH_MODE_SINGLE                                        = 1,
    ACAMERA_FLASH_MODE_TORCH                                         = 2,
} acamera_metadata_enum_android_flash_mode_t;

// ACAMERA_FLASH_STATE
typedef enum acamera_metadata_enum_acamera_flash_state {
    ACAMERA_FLASH_STATE_UNAVAILABLE                                  = 0,
    ACAMERA_FLASH_STATE_CHARGING                                     = 1,
    ACAMERA_FLASH_STATE_READY                                        = 2,
    ACAMERA_FLASH_STATE_FIRED                                        = 3,
    ACAMERA_FLASH_STATE_PARTIAL                                      = 4,
} acamera_metadata_enum_android_flash_state_t;


// ACAMERA_FLASH_INFO_AVAILABLE
typedef enum acamera_metadata_enum_acamera_flash_info_available {
    ACAMERA_FLASH_INFO_AVAILABLE_FALSE                               = 0,
    ACAMERA_FLASH_INFO_AVAILABLE_TRUE                                = 1,
} acamera_metadata_enum_android_flash_info_available_t;


// ACAMERA_HOT_PIXEL_MODE
typedef enum acamera_metadata_enum_acamera_hot_pixel_mode {
    ACAMERA_HOT_PIXEL_MODE_OFF                                       = 0,
    ACAMERA_HOT_PIXEL_MODE_FAST                                      = 1,
    ACAMERA_HOT_PIXEL_MODE_HIGH_QUALITY                              = 2,
} acamera_metadata_enum_android_hot_pixel_mode_t;



// ACAMERA_LENS_OPTICAL_STABILIZATION_MODE
typedef enum acamera_metadata_enum_acamera_lens_optical_stabilization_mode {
    ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF                      = 0,
    ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON                       = 1,
} acamera_metadata_enum_android_lens_optical_stabilization_mode_t;

// ACAMERA_LENS_FACING
typedef enum acamera_metadata_enum_acamera_lens_facing {
    ACAMERA_LENS_FACING_FRONT                                        = 0,
    ACAMERA_LENS_FACING_BACK                                         = 1,
    ACAMERA_LENS_FACING_EXTERNAL                                     = 2,
} acamera_metadata_enum_android_lens_facing_t;

// ACAMERA_LENS_STATE
typedef enum acamera_metadata_enum_acamera_lens_state {
    ACAMERA_LENS_STATE_STATIONARY                                    = 0,
    ACAMERA_LENS_STATE_MOVING                                        = 1,
} acamera_metadata_enum_android_lens_state_t;


// ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION
typedef enum acamera_metadata_enum_acamera_lens_info_focus_distance_calibration {
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED        = 0,
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE         = 1,
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_CALIBRATED          = 2,
} acamera_metadata_enum_android_lens_info_focus_distance_calibration_t;


// ACAMERA_NOISE_REDUCTION_MODE
typedef enum acamera_metadata_enum_acamera_noise_reduction_mode {
    ACAMERA_NOISE_REDUCTION_MODE_OFF                                 = 0,
    ACAMERA_NOISE_REDUCTION_MODE_FAST                                = 1,
    ACAMERA_NOISE_REDUCTION_MODE_HIGH_QUALITY                        = 2,
    ACAMERA_NOISE_REDUCTION_MODE_MINIMAL                             = 3,
    ACAMERA_NOISE_REDUCTION_MODE_ZERO_SHUTTER_LAG                    = 4,
} acamera_metadata_enum_android_noise_reduction_mode_t;


// ACAMERA_QUIRKS_PARTIAL_RESULT
typedef enum acamera_metadata_enum_acamera_quirks_partial_result {
    ACAMERA_QUIRKS_PARTIAL_RESULT_FINAL                              = 0,
    ACAMERA_QUIRKS_PARTIAL_RESULT_PARTIAL                            = 1,
} acamera_metadata_enum_android_quirks_partial_result_t;


// ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
typedef enum acamera_metadata_enum_acamera_request_available_capabilities {
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE       = 0,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR             = 1,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING    = 2,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW                       = 3,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_PRIVATE_REPROCESSING      = 4,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS      = 5,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE             = 6,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_YUV_REPROCESSING          = 7,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT              = 8,
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_CONSTRAINED_HIGH_SPEED_VIDEO
                                                                      = 9,
} acamera_metadata_enum_android_request_available_capabilities_t;


// ACAMERA_SCALER_AVAILABLE_FORMATS
typedef enum acamera_metadata_enum_acamera_scaler_available_formats {
    ACAMERA_SCALER_AVAILABLE_FORMATS_RAW16                           = 0x20,
    ACAMERA_SCALER_AVAILABLE_FORMATS_RAW_OPAQUE                      = 0x24,
    ACAMERA_SCALER_AVAILABLE_FORMATS_YV12                            = 0x32315659,
    ACAMERA_SCALER_AVAILABLE_FORMATS_YCrCb_420_SP                    = 0x11,
    ACAMERA_SCALER_AVAILABLE_FORMATS_IMPLEMENTATION_DEFINED          = 0x22,
    ACAMERA_SCALER_AVAILABLE_FORMATS_YCbCr_420_888                   = 0x23,
    ACAMERA_SCALER_AVAILABLE_FORMATS_BLOB                            = 0x21,
} acamera_metadata_enum_android_scaler_available_formats_t;

// ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS
typedef enum acamera_metadata_enum_acamera_scaler_available_stream_configurations {
    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT            = 0,
    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_INPUT             = 1,
} acamera_metadata_enum_android_scaler_available_stream_configurations_t;

// ACAMERA_SCALER_CROPPING_TYPE
typedef enum acamera_metadata_enum_acamera_scaler_cropping_type {
    ACAMERA_SCALER_CROPPING_TYPE_CENTER_ONLY                         = 0,
    ACAMERA_SCALER_CROPPING_TYPE_FREEFORM                            = 1,
} acamera_metadata_enum_android_scaler_cropping_type_t;


// ACAMERA_SENSOR_REFERENCE_ILLUMINANT1
typedef enum acamera_metadata_enum_acamera_sensor_reference_illuminant1 {
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT                    = 1,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_FLUORESCENT                 = 2,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_TUNGSTEN                    = 3,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_FLASH                       = 4,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_FINE_WEATHER                = 9,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_CLOUDY_WEATHER              = 10,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_SHADE                       = 11,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT_FLUORESCENT        = 12,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_DAY_WHITE_FLUORESCENT       = 13,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_COOL_WHITE_FLUORESCENT      = 14,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_WHITE_FLUORESCENT           = 15,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_STANDARD_A                  = 17,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_STANDARD_B                  = 18,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_STANDARD_C                  = 19,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_D55                         = 20,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_D65                         = 21,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_D75                         = 22,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_D50                         = 23,
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_ISO_STUDIO_TUNGSTEN         = 24,
} acamera_metadata_enum_android_sensor_reference_illuminant1_t;

// ACAMERA_SENSOR_TEST_PATTERN_MODE
typedef enum acamera_metadata_enum_acamera_sensor_test_pattern_mode {
    ACAMERA_SENSOR_TEST_PATTERN_MODE_OFF                             = 0,
    ACAMERA_SENSOR_TEST_PATTERN_MODE_SOLID_COLOR                     = 1,
    ACAMERA_SENSOR_TEST_PATTERN_MODE_COLOR_BARS                      = 2,
    ACAMERA_SENSOR_TEST_PATTERN_MODE_COLOR_BARS_FADE_TO_GRAY         = 3,
    ACAMERA_SENSOR_TEST_PATTERN_MODE_PN9                             = 4,
    ACAMERA_SENSOR_TEST_PATTERN_MODE_CUSTOM1                         = 256,
} acamera_metadata_enum_android_sensor_test_pattern_mode_t;


// ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT
typedef enum acamera_metadata_enum_acamera_sensor_info_color_filter_arrangement {
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB                = 0,
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GRBG                = 1,
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG                = 2,
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR                = 3,
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGB                 = 4,
} acamera_metadata_enum_android_sensor_info_color_filter_arrangement_t;

// ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE
typedef enum acamera_metadata_enum_acamera_sensor_info_timestamp_source {
    ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN                     = 0,
    ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME                    = 1,
} acamera_metadata_enum_android_sensor_info_timestamp_source_t;

// ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED
typedef enum acamera_metadata_enum_acamera_sensor_info_lens_shading_applied {
    ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED_FALSE                   = 0,
    ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED_TRUE                    = 1,
} acamera_metadata_enum_android_sensor_info_lens_shading_applied_t;


// ACAMERA_SHADING_MODE
typedef enum acamera_metadata_enum_acamera_shading_mode {
    ACAMERA_SHADING_MODE_OFF                                         = 0,
    ACAMERA_SHADING_MODE_FAST                                        = 1,
    ACAMERA_SHADING_MODE_HIGH_QUALITY                                = 2,
} acamera_metadata_enum_android_shading_mode_t;


// ACAMERA_STATISTICS_FACE_DETECT_MODE
typedef enum acamera_metadata_enum_acamera_statistics_face_detect_mode {
    ACAMERA_STATISTICS_FACE_DETECT_MODE_OFF                          = 0,
    ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE                       = 1,
    ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL                         = 2,
} acamera_metadata_enum_android_statistics_face_detect_mode_t;

// ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE
typedef enum acamera_metadata_enum_acamera_statistics_hot_pixel_map_mode {
    ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE_OFF                        = 0,
    ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE_ON                         = 1,
} acamera_metadata_enum_android_statistics_hot_pixel_map_mode_t;

// ACAMERA_STATISTICS_SCENE_FLICKER
typedef enum acamera_metadata_enum_acamera_statistics_scene_flicker {
    ACAMERA_STATISTICS_SCENE_FLICKER_NONE                            = 0,
    ACAMERA_STATISTICS_SCENE_FLICKER_50HZ                            = 1,
    ACAMERA_STATISTICS_SCENE_FLICKER_60HZ                            = 2,
} acamera_metadata_enum_android_statistics_scene_flicker_t;

// ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE
typedef enum acamera_metadata_enum_acamera_statistics_lens_shading_map_mode {
    ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_OFF                     = 0,
    ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON                      = 1,
} acamera_metadata_enum_android_statistics_lens_shading_map_mode_t;



// ACAMERA_TONEMAP_MODE
typedef enum acamera_metadata_enum_acamera_tonemap_mode {
    ACAMERA_TONEMAP_MODE_CONTRAST_CURVE                              = 0,
    ACAMERA_TONEMAP_MODE_FAST                                        = 1,
    ACAMERA_TONEMAP_MODE_HIGH_QUALITY                                = 2,
    ACAMERA_TONEMAP_MODE_GAMMA_VALUE                                 = 3,
    ACAMERA_TONEMAP_MODE_PRESET_CURVE                                = 4,
} acamera_metadata_enum_android_tonemap_mode_t;

// ACAMERA_TONEMAP_PRESET_CURVE
typedef enum acamera_metadata_enum_acamera_tonemap_preset_curve {
    ACAMERA_TONEMAP_PRESET_CURVE_SRGB                                = 0,
    ACAMERA_TONEMAP_PRESET_CURVE_REC709                              = 1,
} acamera_metadata_enum_android_tonemap_preset_curve_t;


// ACAMERA_LED_TRANSMIT
typedef enum acamera_metadata_enum_acamera_led_transmit {
    ACAMERA_LED_TRANSMIT_OFF                                         = 0,
    ACAMERA_LED_TRANSMIT_ON                                          = 1,
} acamera_metadata_enum_android_led_transmit_t;

// ACAMERA_LED_AVAILABLE_LEDS
typedef enum acamera_metadata_enum_acamera_led_available_leds {
    ACAMERA_LED_AVAILABLE_LEDS_TRANSMIT                              = 0,
} acamera_metadata_enum_android_led_available_leds_t;


// ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL
typedef enum acamera_metadata_enum_acamera_info_supported_hardware_level {
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED                    = 0,
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_FULL                       = 1,
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY                     = 2,
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_3                          = 3,
} acamera_metadata_enum_android_info_supported_hardware_level_t;


// ACAMERA_BLACK_LEVEL_LOCK
typedef enum acamera_metadata_enum_acamera_black_level_lock {
    ACAMERA_BLACK_LEVEL_LOCK_OFF                                     = 0,
    ACAMERA_BLACK_LEVEL_LOCK_ON                                      = 1,
} acamera_metadata_enum_android_black_level_lock_t;


// ACAMERA_SYNC_FRAME_NUMBER
typedef enum acamera_metadata_enum_acamera_sync_frame_number {
    ACAMERA_SYNC_FRAME_NUMBER_CONVERGING                             = -1,
    ACAMERA_SYNC_FRAME_NUMBER_UNKNOWN                                = -2,
} acamera_metadata_enum_android_sync_frame_number_t;

// ACAMERA_SYNC_MAX_LATENCY
typedef enum acamera_metadata_enum_acamera_sync_max_latency {
    ACAMERA_SYNC_MAX_LATENCY_PER_FRAME_CONTROL                       = 0,
    ACAMERA_SYNC_MAX_LATENCY_UNKNOWN                                 = -1,
} acamera_metadata_enum_android_sync_max_latency_t;



// ACAMERA_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS
typedef enum acamera_metadata_enum_acamera_depth_available_depth_stream_configurations {
    ACAMERA_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS_OUTPUT       = 0,
    ACAMERA_DEPTH_AVAILABLE_DEPTH_STREAM_CONFIGURATIONS_INPUT        = 1,
} acamera_metadata_enum_android_depth_available_depth_stream_configurations_t;

// ACAMERA_DEPTH_DEPTH_IS_EXCLUSIVE
typedef enum acamera_metadata_enum_acamera_depth_depth_is_exclusive {
    ACAMERA_DEPTH_DEPTH_IS_EXCLUSIVE_FALSE                           = 0,
    ACAMERA_DEPTH_DEPTH_IS_EXCLUSIVE_TRUE                            = 1,
} acamera_metadata_enum_android_depth_depth_is_exclusive_t;



#endif //_NDK_CAMERA_METADATA_TAGS_H
