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
    ACAMERA_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM =                 // float
            ACAMERA_SCALER_START + 4,
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
    /*
     * <p>Use the ACAMERA_COLOR_CORRECTION_TRANSFORM matrix
     * and ACAMERA_COLOR_CORRECTION_GAINS to do color conversion.</p>
     * <p>All advanced white balance adjustments (not specified
     * by our white balance pipeline) must be disabled.</p>
     * <p>If AWB is enabled with <code>ACAMERA_CONTROL_AWB_MODE != OFF</code>, then
     * TRANSFORM_MATRIX is ignored. The camera device will override
     * this value to either FAST or HIGH_QUALITY.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     * @see ACAMERA_CONTROL_AWB_MODE
     */
    ACAMERA_COLOR_CORRECTION_MODE_TRANSFORM_MATRIX                   = 0,

    /*
     * <p>Color correction processing must not slow down
     * capture rate relative to sensor raw output.</p>
     * <p>Advanced white balance adjustments above and beyond
     * the specified white balance pipeline may be applied.</p>
     * <p>If AWB is enabled with <code>ACAMERA_CONTROL_AWB_MODE != OFF</code>, then
     * the camera device uses the last frame's AWB values
     * (or defaults if AWB has never been run).</p>
     *
     * @see ACAMERA_CONTROL_AWB_MODE
     */
    ACAMERA_COLOR_CORRECTION_MODE_FAST                               = 1,

    /*
     * <p>Color correction processing operates at improved
     * quality but the capture rate might be reduced (relative to sensor
     * raw output rate)</p>
     * <p>Advanced white balance adjustments above and beyond
     * the specified white balance pipeline may be applied.</p>
     * <p>If AWB is enabled with <code>ACAMERA_CONTROL_AWB_MODE != OFF</code>, then
     * the camera device uses the last frame's AWB values
     * (or defaults if AWB has never been run).</p>
     *
     * @see ACAMERA_CONTROL_AWB_MODE
     */
    ACAMERA_COLOR_CORRECTION_MODE_HIGH_QUALITY                       = 2,

} acamera_metadata_enum_android_color_correction_mode_t;

// ACAMERA_COLOR_CORRECTION_ABERRATION_MODE
typedef enum acamera_metadata_enum_acamera_color_correction_aberration_mode {
    /*
     * <p>No aberration correction is applied.</p>
     */
    ACAMERA_COLOR_CORRECTION_ABERRATION_MODE_OFF                     = 0,

    /*
     * <p>Aberration correction will not slow down capture rate
     * relative to sensor raw output.</p>
     */
    ACAMERA_COLOR_CORRECTION_ABERRATION_MODE_FAST                    = 1,

    /*
     * <p>Aberration correction operates at improved quality but the capture rate might be
     * reduced (relative to sensor raw output rate)</p>
     */
    ACAMERA_COLOR_CORRECTION_ABERRATION_MODE_HIGH_QUALITY            = 2,

} acamera_metadata_enum_android_color_correction_aberration_mode_t;


// ACAMERA_CONTROL_AE_ANTIBANDING_MODE
typedef enum acamera_metadata_enum_acamera_control_ae_antibanding_mode {
    /*
     * <p>The camera device will not adjust exposure duration to
     * avoid banding problems.</p>
     */
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_OFF                          = 0,

    /*
     * <p>The camera device will adjust exposure duration to
     * avoid banding problems with 50Hz illumination sources.</p>
     */
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_50HZ                         = 1,

    /*
     * <p>The camera device will adjust exposure duration to
     * avoid banding problems with 60Hz illumination
     * sources.</p>
     */
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_60HZ                         = 2,

    /*
     * <p>The camera device will automatically adapt its
     * antibanding routine to the current illumination
     * condition. This is the default mode if AUTO is
     * available on given camera device.</p>
     */
    ACAMERA_CONTROL_AE_ANTIBANDING_MODE_AUTO                         = 3,

} acamera_metadata_enum_android_control_ae_antibanding_mode_t;

// ACAMERA_CONTROL_AE_LOCK
typedef enum acamera_metadata_enum_acamera_control_ae_lock {
    /*
     * <p>Auto-exposure lock is disabled; the AE algorithm
     * is free to update its parameters.</p>
     */
    ACAMERA_CONTROL_AE_LOCK_OFF                                      = 0,

    /*
     * <p>Auto-exposure lock is enabled; the AE algorithm
     * must not update the exposure and sensitivity parameters
     * while the lock is active.</p>
     * <p>ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION setting changes
     * will still take effect while auto-exposure is locked.</p>
     * <p>Some rare LEGACY devices may not support
     * this, in which case the value will always be overridden to OFF.</p>
     *
     * @see ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION
     */
    ACAMERA_CONTROL_AE_LOCK_ON                                       = 1,

} acamera_metadata_enum_android_control_ae_lock_t;

// ACAMERA_CONTROL_AE_MODE
typedef enum acamera_metadata_enum_acamera_control_ae_mode {
    /*
     * <p>The camera device's autoexposure routine is disabled.</p>
     * <p>The application-selected ACAMERA_SENSOR_EXPOSURE_TIME,
     * ACAMERA_SENSOR_SENSITIVITY and
     * ACAMERA_SENSOR_FRAME_DURATION are used by the camera
     * device, along with ACAMERA_FLASH_* fields, if there's
     * a flash unit for this camera device.</p>
     * <p>Note that auto-white balance (AWB) and auto-focus (AF)
     * behavior is device dependent when AE is in OFF mode.
     * To have consistent behavior across different devices,
     * it is recommended to either set AWB and AF to OFF mode
     * or lock AWB and AF before setting AE to OFF.
     * See ACAMERA_CONTROL_AWB_MODE, ACAMERA_CONTROL_AF_MODE,
     * ACAMERA_CONTROL_AWB_LOCK, and ACAMERA_CONTROL_AF_TRIGGER
     * for more details.</p>
     * <p>LEGACY devices do not support the OFF mode and will
     * override attempts to use this value to ON.</p>
     *
     * @see ACAMERA_CONTROL_AF_MODE
     * @see ACAMERA_CONTROL_AF_TRIGGER
     * @see ACAMERA_CONTROL_AWB_LOCK
     * @see ACAMERA_CONTROL_AWB_MODE
     * @see ACAMERA_SENSOR_EXPOSURE_TIME
     * @see ACAMERA_SENSOR_FRAME_DURATION
     * @see ACAMERA_SENSOR_SENSITIVITY
     */
    ACAMERA_CONTROL_AE_MODE_OFF                                      = 0,

    /*
     * <p>The camera device's autoexposure routine is active,
     * with no flash control.</p>
     * <p>The application's values for
     * ACAMERA_SENSOR_EXPOSURE_TIME,
     * ACAMERA_SENSOR_SENSITIVITY, and
     * ACAMERA_SENSOR_FRAME_DURATION are ignored. The
     * application has control over the various
     * ACAMERA_FLASH_* fields.</p>
     *
     * @see ACAMERA_SENSOR_EXPOSURE_TIME
     * @see ACAMERA_SENSOR_FRAME_DURATION
     * @see ACAMERA_SENSOR_SENSITIVITY
     */
    ACAMERA_CONTROL_AE_MODE_ON                                       = 1,

    /*
     * <p>Like ON, except that the camera device also controls
     * the camera's flash unit, firing it in low-light
     * conditions.</p>
     * <p>The flash may be fired during a precapture sequence
     * (triggered by ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER) and
     * may be fired for captures for which the
     * ACAMERA_CONTROL_CAPTURE_INTENT field is set to
     * STILL_CAPTURE</p>
     *
     * @see ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
     * @see ACAMERA_CONTROL_CAPTURE_INTENT
     */
    ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH                            = 2,

    /*
     * <p>Like ON, except that the camera device also controls
     * the camera's flash unit, always firing it for still
     * captures.</p>
     * <p>The flash may be fired during a precapture sequence
     * (triggered by ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER) and
     * will always be fired for captures for which the
     * ACAMERA_CONTROL_CAPTURE_INTENT field is set to
     * STILL_CAPTURE</p>
     *
     * @see ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
     * @see ACAMERA_CONTROL_CAPTURE_INTENT
     */
    ACAMERA_CONTROL_AE_MODE_ON_ALWAYS_FLASH                          = 3,

    /*
     * <p>Like ON_AUTO_FLASH, but with automatic red eye
     * reduction.</p>
     * <p>If deemed necessary by the camera device, a red eye
     * reduction flash will fire during the precapture
     * sequence.</p>
     */
    ACAMERA_CONTROL_AE_MODE_ON_AUTO_FLASH_REDEYE                     = 4,

} acamera_metadata_enum_android_control_ae_mode_t;

// ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
typedef enum acamera_metadata_enum_acamera_control_ae_precapture_trigger {
    /*
     * <p>The trigger is idle.</p>
     */
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE                       = 0,

    /*
     * <p>The precapture metering sequence will be started
     * by the camera device.</p>
     * <p>The exact effect of the precapture trigger depends on
     * the current AE mode and state.</p>
     */
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_START                      = 1,

    /*
     * <p>The camera device will cancel any currently active or completed
     * precapture metering sequence, the auto-exposure routine will return to its
     * initial state.</p>
     */
    ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL                     = 2,

} acamera_metadata_enum_android_control_ae_precapture_trigger_t;

// ACAMERA_CONTROL_AF_MODE
typedef enum acamera_metadata_enum_acamera_control_af_mode {
    /*
     * <p>The auto-focus routine does not control the lens;
     * ACAMERA_LENS_FOCUS_DISTANCE is controlled by the
     * application.</p>
     *
     * @see ACAMERA_LENS_FOCUS_DISTANCE
     */
    ACAMERA_CONTROL_AF_MODE_OFF                                      = 0,

    /*
     * <p>Basic automatic focus mode.</p>
     * <p>In this mode, the lens does not move unless
     * the autofocus trigger action is called. When that trigger
     * is activated, AF will transition to ACTIVE_SCAN, then to
     * the outcome of the scan (FOCUSED or NOT_FOCUSED).</p>
     * <p>Always supported if lens is not fixed focus.</p>
     * <p>Use ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE to determine if lens
     * is fixed-focus.</p>
     * <p>Triggering AF_CANCEL resets the lens position to default,
     * and sets the AF state to INACTIVE.</p>
     *
     * @see ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE
     */
    ACAMERA_CONTROL_AF_MODE_AUTO                                     = 1,

    /*
     * <p>Close-up focusing mode.</p>
     * <p>In this mode, the lens does not move unless the
     * autofocus trigger action is called. When that trigger is
     * activated, AF will transition to ACTIVE_SCAN, then to
     * the outcome of the scan (FOCUSED or NOT_FOCUSED). This
     * mode is optimized for focusing on objects very close to
     * the camera.</p>
     * <p>When that trigger is activated, AF will transition to
     * ACTIVE_SCAN, then to the outcome of the scan (FOCUSED or
     * NOT_FOCUSED). Triggering cancel AF resets the lens
     * position to default, and sets the AF state to
     * INACTIVE.</p>
     */
    ACAMERA_CONTROL_AF_MODE_MACRO                                    = 2,

    /*
     * <p>In this mode, the AF algorithm modifies the lens
     * position continually to attempt to provide a
     * constantly-in-focus image stream.</p>
     * <p>The focusing behavior should be suitable for good quality
     * video recording; typically this means slower focus
     * movement and no overshoots. When the AF trigger is not
     * involved, the AF algorithm should start in INACTIVE state,
     * and then transition into PASSIVE_SCAN and PASSIVE_FOCUSED
     * states as appropriate. When the AF trigger is activated,
     * the algorithm should immediately transition into
     * AF_FOCUSED or AF_NOT_FOCUSED as appropriate, and lock the
     * lens position until a cancel AF trigger is received.</p>
     * <p>Once cancel is received, the algorithm should transition
     * back to INACTIVE and resume passive scan. Note that this
     * behavior is not identical to CONTINUOUS_PICTURE, since an
     * ongoing PASSIVE_SCAN must immediately be
     * canceled.</p>
     */
    ACAMERA_CONTROL_AF_MODE_CONTINUOUS_VIDEO                         = 3,

    /*
     * <p>In this mode, the AF algorithm modifies the lens
     * position continually to attempt to provide a
     * constantly-in-focus image stream.</p>
     * <p>The focusing behavior should be suitable for still image
     * capture; typically this means focusing as fast as
     * possible. When the AF trigger is not involved, the AF
     * algorithm should start in INACTIVE state, and then
     * transition into PASSIVE_SCAN and PASSIVE_FOCUSED states as
     * appropriate as it attempts to maintain focus. When the AF
     * trigger is activated, the algorithm should finish its
     * PASSIVE_SCAN if active, and then transition into
     * AF_FOCUSED or AF_NOT_FOCUSED as appropriate, and lock the
     * lens position until a cancel AF trigger is received.</p>
     * <p>When the AF cancel trigger is activated, the algorithm
     * should transition back to INACTIVE and then act as if it
     * has just been started.</p>
     */
    ACAMERA_CONTROL_AF_MODE_CONTINUOUS_PICTURE                       = 4,

    /*
     * <p>Extended depth of field (digital focus) mode.</p>
     * <p>The camera device will produce images with an extended
     * depth of field automatically; no special focusing
     * operations need to be done before taking a picture.</p>
     * <p>AF triggers are ignored, and the AF state will always be
     * INACTIVE.</p>
     */
    ACAMERA_CONTROL_AF_MODE_EDOF                                     = 5,

} acamera_metadata_enum_android_control_af_mode_t;

// ACAMERA_CONTROL_AF_TRIGGER
typedef enum acamera_metadata_enum_acamera_control_af_trigger {
    /*
     * <p>The trigger is idle.</p>
     */
    ACAMERA_CONTROL_AF_TRIGGER_IDLE                                  = 0,

    /*
     * <p>Autofocus will trigger now.</p>
     */
    ACAMERA_CONTROL_AF_TRIGGER_START                                 = 1,

    /*
     * <p>Autofocus will return to its initial
     * state, and cancel any currently active trigger.</p>
     */
    ACAMERA_CONTROL_AF_TRIGGER_CANCEL                                = 2,

} acamera_metadata_enum_android_control_af_trigger_t;

// ACAMERA_CONTROL_AWB_LOCK
typedef enum acamera_metadata_enum_acamera_control_awb_lock {
    /*
     * <p>Auto-white balance lock is disabled; the AWB
     * algorithm is free to update its parameters if in AUTO
     * mode.</p>
     */
    ACAMERA_CONTROL_AWB_LOCK_OFF                                     = 0,

    /*
     * <p>Auto-white balance lock is enabled; the AWB
     * algorithm will not update its parameters while the lock
     * is active.</p>
     */
    ACAMERA_CONTROL_AWB_LOCK_ON                                      = 1,

} acamera_metadata_enum_android_control_awb_lock_t;

// ACAMERA_CONTROL_AWB_MODE
typedef enum acamera_metadata_enum_acamera_control_awb_mode {
    /*
     * <p>The camera device's auto-white balance routine is disabled.</p>
     * <p>The application-selected color transform matrix
     * (ACAMERA_COLOR_CORRECTION_TRANSFORM) and gains
     * (ACAMERA_COLOR_CORRECTION_GAINS) are used by the camera
     * device for manual white balance control.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_OFF                                     = 0,

    /*
     * <p>The camera device's auto-white balance routine is active.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_AUTO                                    = 1,

    /*
     * <p>The camera device's auto-white balance routine is disabled;
     * the camera device uses incandescent light as the assumed scene
     * illumination for white balance.</p>
     * <p>While the exact white balance transforms are up to the
     * camera device, they will approximately match the CIE
     * standard illuminant A.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_INCANDESCENT                            = 2,

    /*
     * <p>The camera device's auto-white balance routine is disabled;
     * the camera device uses fluorescent light as the assumed scene
     * illumination for white balance.</p>
     * <p>While the exact white balance transforms are up to the
     * camera device, they will approximately match the CIE
     * standard illuminant F2.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_FLUORESCENT                             = 3,

    /*
     * <p>The camera device's auto-white balance routine is disabled;
     * the camera device uses warm fluorescent light as the assumed scene
     * illumination for white balance.</p>
     * <p>While the exact white balance transforms are up to the
     * camera device, they will approximately match the CIE
     * standard illuminant F4.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_WARM_FLUORESCENT                        = 4,

    /*
     * <p>The camera device's auto-white balance routine is disabled;
     * the camera device uses daylight light as the assumed scene
     * illumination for white balance.</p>
     * <p>While the exact white balance transforms are up to the
     * camera device, they will approximately match the CIE
     * standard illuminant D65.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_DAYLIGHT                                = 5,

    /*
     * <p>The camera device's auto-white balance routine is disabled;
     * the camera device uses cloudy daylight light as the assumed scene
     * illumination for white balance.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT                         = 6,

    /*
     * <p>The camera device's auto-white balance routine is disabled;
     * the camera device uses twilight light as the assumed scene
     * illumination for white balance.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_TWILIGHT                                = 7,

    /*
     * <p>The camera device's auto-white balance routine is disabled;
     * the camera device uses shade light as the assumed scene
     * illumination for white balance.</p>
     * <p>The application's values for ACAMERA_COLOR_CORRECTION_TRANSFORM
     * and ACAMERA_COLOR_CORRECTION_GAINS are ignored.
     * For devices that support the MANUAL_POST_PROCESSING capability, the
     * values used by the camera device for the transform and gains
     * will be available in the capture result for this request.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     */
    ACAMERA_CONTROL_AWB_MODE_SHADE                                   = 8,

} acamera_metadata_enum_android_control_awb_mode_t;

// ACAMERA_CONTROL_CAPTURE_INTENT
typedef enum acamera_metadata_enum_acamera_control_capture_intent {
    /*
     * <p>The goal of this request doesn't fall into the other
     * categories. The camera device will default to preview-like
     * behavior.</p>
     */
    ACAMERA_CONTROL_CAPTURE_INTENT_CUSTOM                            = 0,

    /*
     * <p>This request is for a preview-like use case.</p>
     * <p>The precapture trigger may be used to start off a metering
     * w/flash sequence.</p>
     */
    ACAMERA_CONTROL_CAPTURE_INTENT_PREVIEW                           = 1,

    /*
     * <p>This request is for a still capture-type
     * use case.</p>
     * <p>If the flash unit is under automatic control, it may fire as needed.</p>
     */
    ACAMERA_CONTROL_CAPTURE_INTENT_STILL_CAPTURE                     = 2,

    /*
     * <p>This request is for a video recording
     * use case.</p>
     */
    ACAMERA_CONTROL_CAPTURE_INTENT_VIDEO_RECORD                      = 3,

    /*
     * <p>This request is for a video snapshot (still
     * image while recording video) use case.</p>
     * <p>The camera device should take the highest-quality image
     * possible (given the other settings) without disrupting the
     * frame rate of video recording.  </p>
     */
    ACAMERA_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT                    = 4,

    /*
     * <p>This request is for a ZSL usecase; the
     * application will stream full-resolution images and
     * reprocess one or several later for a final
     * capture.</p>
     */
    ACAMERA_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG                  = 5,

    /*
     * <p>This request is for manual capture use case where
     * the applications want to directly control the capture parameters.</p>
     * <p>For example, the application may wish to manually control
     * ACAMERA_SENSOR_EXPOSURE_TIME, ACAMERA_SENSOR_SENSITIVITY, etc.</p>
     *
     * @see ACAMERA_SENSOR_EXPOSURE_TIME
     * @see ACAMERA_SENSOR_SENSITIVITY
     */
    ACAMERA_CONTROL_CAPTURE_INTENT_MANUAL                            = 6,

} acamera_metadata_enum_android_control_capture_intent_t;

// ACAMERA_CONTROL_EFFECT_MODE
typedef enum acamera_metadata_enum_acamera_control_effect_mode {
    /*
     * <p>No color effect will be applied.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_OFF                                  = 0,

    /*
     * <p>A "monocolor" effect where the image is mapped into
     * a single color.</p>
     * <p>This will typically be grayscale.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_MONO                                 = 1,

    /*
     * <p>A "photo-negative" effect where the image's colors
     * are inverted.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_NEGATIVE                             = 2,

    /*
     * <p>A "solarisation" effect (Sabattier effect) where the
     * image is wholly or partially reversed in
     * tone.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_SOLARIZE                             = 3,

    /*
     * <p>A "sepia" effect where the image is mapped into warm
     * gray, red, and brown tones.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_SEPIA                                = 4,

    /*
     * <p>A "posterization" effect where the image uses
     * discrete regions of tone rather than a continuous
     * gradient of tones.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_POSTERIZE                            = 5,

    /*
     * <p>A "whiteboard" effect where the image is typically displayed
     * as regions of white, with black or grey details.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_WHITEBOARD                           = 6,

    /*
     * <p>A "blackboard" effect where the image is typically displayed
     * as regions of black, with white or grey details.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_BLACKBOARD                           = 7,

    /*
     * <p>An "aqua" effect where a blue hue is added to the image.</p>
     */
    ACAMERA_CONTROL_EFFECT_MODE_AQUA                                 = 8,

} acamera_metadata_enum_android_control_effect_mode_t;

// ACAMERA_CONTROL_MODE
typedef enum acamera_metadata_enum_acamera_control_mode {
    /*
     * <p>Full application control of pipeline.</p>
     * <p>All control by the device's metering and focusing (3A)
     * routines is disabled, and no other settings in
     * ACAMERA_CONTROL_* have any effect, except that
     * ACAMERA_CONTROL_CAPTURE_INTENT may be used by the camera
     * device to select post-processing values for processing
     * blocks that do not allow for manual control, or are not
     * exposed by the camera API.</p>
     * <p>However, the camera device's 3A routines may continue to
     * collect statistics and update their internal state so that
     * when control is switched to AUTO mode, good control values
     * can be immediately applied.</p>
     *
     * @see ACAMERA_CONTROL_CAPTURE_INTENT
     */
    ACAMERA_CONTROL_MODE_OFF                                         = 0,

    /*
     * <p>Use settings for each individual 3A routine.</p>
     * <p>Manual control of capture parameters is disabled. All
     * controls in ACAMERA_CONTROL_* besides sceneMode take
     * effect.</p>
     */
    ACAMERA_CONTROL_MODE_AUTO                                        = 1,

    /*
     * <p>Use a specific scene mode.</p>
     * <p>Enabling this disables control.aeMode, control.awbMode and
     * control.afMode controls; the camera device will ignore
     * those settings while USE_SCENE_MODE is active (except for
     * FACE_PRIORITY scene mode). Other control entries are still active.
     * This setting can only be used if scene mode is supported (i.e.
     * ACAMERA_CONTROL_AVAILABLE_SCENE_MODES
     * contain some modes other than DISABLED).</p>
     *
     * @see ACAMERA_CONTROL_AVAILABLE_SCENE_MODES
     */
    ACAMERA_CONTROL_MODE_USE_SCENE_MODE                              = 2,

    /*
     * <p>Same as OFF mode, except that this capture will not be
     * used by camera device background auto-exposure, auto-white balance and
     * auto-focus algorithms (3A) to update their statistics.</p>
     * <p>Specifically, the 3A routines are locked to the last
     * values set from a request with AUTO, OFF, or
     * USE_SCENE_MODE, and any statistics or state updates
     * collected from manual captures with OFF_KEEP_STATE will be
     * discarded by the camera device.</p>
     */
    ACAMERA_CONTROL_MODE_OFF_KEEP_STATE                              = 3,

} acamera_metadata_enum_android_control_mode_t;

// ACAMERA_CONTROL_SCENE_MODE
typedef enum acamera_metadata_enum_acamera_control_scene_mode {
    /*
     * <p>Indicates that no scene modes are set for a given capture request.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_DISABLED                              = 0,

    /*
     * <p>If face detection support exists, use face
     * detection data for auto-focus, auto-white balance, and
     * auto-exposure routines.</p>
     * <p>If face detection statistics are disabled
     * (i.e. ACAMERA_STATISTICS_FACE_DETECT_MODE is set to OFF),
     * this should still operate correctly (but will not return
     * face detection statistics to the framework).</p>
     * <p>Unlike the other scene modes, ACAMERA_CONTROL_AE_MODE,
     * ACAMERA_CONTROL_AWB_MODE, and ACAMERA_CONTROL_AF_MODE
     * remain active when FACE_PRIORITY is set.</p>
     *
     * @see ACAMERA_CONTROL_AE_MODE
     * @see ACAMERA_CONTROL_AF_MODE
     * @see ACAMERA_CONTROL_AWB_MODE
     * @see ACAMERA_STATISTICS_FACE_DETECT_MODE
     */
    ACAMERA_CONTROL_SCENE_MODE_FACE_PRIORITY                         = 1,

    /*
     * <p>Optimized for photos of quickly moving objects.</p>
     * <p>Similar to SPORTS.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_ACTION                                = 2,

    /*
     * <p>Optimized for still photos of people.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_PORTRAIT                              = 3,

    /*
     * <p>Optimized for photos of distant macroscopic objects.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_LANDSCAPE                             = 4,

    /*
     * <p>Optimized for low-light settings.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_NIGHT                                 = 5,

    /*
     * <p>Optimized for still photos of people in low-light
     * settings.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_NIGHT_PORTRAIT                        = 6,

    /*
     * <p>Optimized for dim, indoor settings where flash must
     * remain off.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_THEATRE                               = 7,

    /*
     * <p>Optimized for bright, outdoor beach settings.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_BEACH                                 = 8,

    /*
     * <p>Optimized for bright, outdoor settings containing snow.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_SNOW                                  = 9,

    /*
     * <p>Optimized for scenes of the setting sun.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_SUNSET                                = 10,

    /*
     * <p>Optimized to avoid blurry photos due to small amounts of
     * device motion (for example: due to hand shake).</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_STEADYPHOTO                           = 11,

    /*
     * <p>Optimized for nighttime photos of fireworks.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_FIREWORKS                             = 12,

    /*
     * <p>Optimized for photos of quickly moving people.</p>
     * <p>Similar to ACTION.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_SPORTS                                = 13,

    /*
     * <p>Optimized for dim, indoor settings with multiple moving
     * people.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_PARTY                                 = 14,

    /*
     * <p>Optimized for dim settings where the main light source
     * is a flame.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_CANDLELIGHT                           = 15,

    /*
     * <p>Optimized for accurately capturing a photo of barcode
     * for use by camera applications that wish to read the
     * barcode value.</p>
     */
    ACAMERA_CONTROL_SCENE_MODE_BARCODE                               = 16,

    /*
     * <p>This is deprecated, please use {@link
     * android.hardware.camera2.CameraDevice#createConstrainedHighSpeedCaptureSession}
     * and {@link
     * android.hardware.camera2.CameraConstrainedHighSpeedCaptureSession#createHighSpeedRequestList}
     * for high speed video recording.</p>
     * <p>Optimized for high speed video recording (frame rate &gt;=60fps) use case.</p>
     * <p>The supported high speed video sizes and fps ranges are specified in
     * android.control.availableHighSpeedVideoConfigurations. To get desired
     * output frame rates, the application is only allowed to select video size
     * and fps range combinations listed in this static metadata. The fps range
     * can be control via ACAMERA_CONTROL_AE_TARGET_FPS_RANGE.</p>
     * <p>In this mode, the camera device will override aeMode, awbMode, and afMode to
     * ON, ON, and CONTINUOUS_VIDEO, respectively. All post-processing block mode
     * controls will be overridden to be FAST. Therefore, no manual control of capture
     * and post-processing parameters is possible. All other controls operate the
     * same as when ACAMERA_CONTROL_MODE == AUTO. This means that all other
     * ACAMERA_CONTROL_* fields continue to work, such as</p>
     * <ul>
     * <li>ACAMERA_CONTROL_AE_TARGET_FPS_RANGE</li>
     * <li>ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION</li>
     * <li>ACAMERA_CONTROL_AE_LOCK</li>
     * <li>ACAMERA_CONTROL_AWB_LOCK</li>
     * <li>ACAMERA_CONTROL_EFFECT_MODE</li>
     * <li>ACAMERA_CONTROL_AE_REGIONS</li>
     * <li>ACAMERA_CONTROL_AF_REGIONS</li>
     * <li>ACAMERA_CONTROL_AWB_REGIONS</li>
     * <li>ACAMERA_CONTROL_AF_TRIGGER</li>
     * <li>ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER</li>
     * </ul>
     * <p>Outside of ACAMERA_CONTROL_*, the following controls will work:</p>
     * <ul>
     * <li>ACAMERA_FLASH_MODE (automatic flash for still capture will not work since aeMode is ON)</li>
     * <li>ACAMERA_LENS_OPTICAL_STABILIZATION_MODE (if it is supported)</li>
     * <li>ACAMERA_SCALER_CROP_REGION</li>
     * <li>ACAMERA_STATISTICS_FACE_DETECT_MODE</li>
     * </ul>
     * <p>For high speed recording use case, the actual maximum supported frame rate may
     * be lower than what camera can output, depending on the destination Surfaces for
     * the image data. For example, if the destination surface is from video encoder,
     * the application need check if the video encoder is capable of supporting the
     * high frame rate for a given video size, or it will end up with lower recording
     * frame rate. If the destination surface is from preview window, the preview frame
     * rate will be bounded by the screen refresh rate.</p>
     * <p>The camera device will only support up to 2 output high speed streams
     * (processed non-stalling format defined in ACAMERA_REQUEST_MAX_NUM_OUTPUT_STREAMS)
     * in this mode. This control will be effective only if all of below conditions are true:</p>
     * <ul>
     * <li>The application created no more than maxNumHighSpeedStreams processed non-stalling
     * format output streams, where maxNumHighSpeedStreams is calculated as
     * min(2, ACAMERA_REQUEST_MAX_NUM_OUTPUT_STREAMS[Processed (but not-stalling)]).</li>
     * <li>The stream sizes are selected from the sizes reported by
     * android.control.availableHighSpeedVideoConfigurations.</li>
     * <li>No processed non-stalling or raw streams are configured.</li>
     * </ul>
     * <p>When above conditions are NOT satistied, the controls of this mode and
     * ACAMERA_CONTROL_AE_TARGET_FPS_RANGE will be ignored by the camera device,
     * the camera device will fall back to ACAMERA_CONTROL_MODE <code>==</code> AUTO,
     * and the returned capture result metadata will give the fps range choosen
     * by the camera device.</p>
     * <p>Switching into or out of this mode may trigger some camera ISP/sensor
     * reconfigurations, which may introduce extra latency. It is recommended that
     * the application avoids unnecessary scene mode switch as much as possible.</p>
     *
     * @see ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION
     * @see ACAMERA_CONTROL_AE_LOCK
     * @see ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
     * @see ACAMERA_CONTROL_AE_REGIONS
     * @see ACAMERA_CONTROL_AE_TARGET_FPS_RANGE
     * @see ACAMERA_CONTROL_AF_REGIONS
     * @see ACAMERA_CONTROL_AF_TRIGGER
     * @see ACAMERA_CONTROL_AWB_LOCK
     * @see ACAMERA_CONTROL_AWB_REGIONS
     * @see ACAMERA_CONTROL_EFFECT_MODE
     * @see ACAMERA_CONTROL_MODE
     * @see ACAMERA_FLASH_MODE
     * @see ACAMERA_LENS_OPTICAL_STABILIZATION_MODE
     * @see ACAMERA_REQUEST_MAX_NUM_OUTPUT_STREAMS
     * @see ACAMERA_SCALER_CROP_REGION
     * @see ACAMERA_STATISTICS_FACE_DETECT_MODE
     *
     * <b>Deprecated</b>: please refer to this API documentation to find the alternatives
     */
    ACAMERA_CONTROL_SCENE_MODE_HIGH_SPEED_VIDEO                      = 17,

    /*
     * <p>Turn on a device-specific high dynamic range (HDR) mode.</p>
     * <p>In this scene mode, the camera device captures images
     * that keep a larger range of scene illumination levels
     * visible in the final image. For example, when taking a
     * picture of a object in front of a bright window, both
     * the object and the scene through the window may be
     * visible when using HDR mode, while in normal AUTO mode,
     * one or the other may be poorly exposed. As a tradeoff,
     * HDR mode generally takes much longer to capture a single
     * image, has no user control, and may have other artifacts
     * depending on the HDR method used.</p>
     * <p>Therefore, HDR captures operate at a much slower rate
     * than regular captures.</p>
     * <p>In this mode, on LIMITED or FULL devices, when a request
     * is made with a ACAMERA_CONTROL_CAPTURE_INTENT of
     * STILL_CAPTURE, the camera device will capture an image
     * using a high dynamic range capture technique.  On LEGACY
     * devices, captures that target a JPEG-format output will
     * be captured with HDR, and the capture intent is not
     * relevant.</p>
     * <p>The HDR capture may involve the device capturing a burst
     * of images internally and combining them into one, or it
     * may involve the device using specialized high dynamic
     * range capture hardware. In all cases, a single image is
     * produced in response to a capture request submitted
     * while in HDR mode.</p>
     * <p>Since substantial post-processing is generally needed to
     * produce an HDR image, only YUV, PRIVATE, and JPEG
     * outputs are supported for LIMITED/FULL device HDR
     * captures, and only JPEG outputs are supported for LEGACY
     * HDR captures. Using a RAW output for HDR capture is not
     * supported.</p>
     * <p>Some devices may also support always-on HDR, which
     * applies HDR processing at full frame rate.  For these
     * devices, intents other than STILL_CAPTURE will also
     * produce an HDR output with no frame rate impact compared
     * to normal operation, though the quality may be lower
     * than for STILL_CAPTURE intents.</p>
     * <p>If SCENE_MODE_HDR is used with unsupported output types
     * or capture intents, the images captured will be as if
     * the SCENE_MODE was not enabled at all.</p>
     *
     * @see ACAMERA_CONTROL_CAPTURE_INTENT
     */
    ACAMERA_CONTROL_SCENE_MODE_HDR                                   = 18,

} acamera_metadata_enum_android_control_scene_mode_t;

// ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE
typedef enum acamera_metadata_enum_acamera_control_video_stabilization_mode {
    /*
     * <p>Video stabilization is disabled.</p>
     */
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_OFF                     = 0,

    /*
     * <p>Video stabilization is enabled.</p>
     */
    ACAMERA_CONTROL_VIDEO_STABILIZATION_MODE_ON                      = 1,

} acamera_metadata_enum_android_control_video_stabilization_mode_t;

// ACAMERA_CONTROL_AE_STATE
typedef enum acamera_metadata_enum_acamera_control_ae_state {
    /*
     * <p>AE is off or recently reset.</p>
     * <p>When a camera device is opened, it starts in
     * this state. This is a transient state, the camera device may skip reporting
     * this state in capture result.</p>
     */
    ACAMERA_CONTROL_AE_STATE_INACTIVE                                = 0,

    /*
     * <p>AE doesn't yet have a good set of control values
     * for the current scene.</p>
     * <p>This is a transient state, the camera device may skip
     * reporting this state in capture result.</p>
     */
    ACAMERA_CONTROL_AE_STATE_SEARCHING                               = 1,

    /*
     * <p>AE has a good set of control values for the
     * current scene.</p>
     */
    ACAMERA_CONTROL_AE_STATE_CONVERGED                               = 2,

    /*
     * <p>AE has been locked.</p>
     */
    ACAMERA_CONTROL_AE_STATE_LOCKED                                  = 3,

    /*
     * <p>AE has a good set of control values, but flash
     * needs to be fired for good quality still
     * capture.</p>
     */
    ACAMERA_CONTROL_AE_STATE_FLASH_REQUIRED                          = 4,

    /*
     * <p>AE has been asked to do a precapture sequence
     * and is currently executing it.</p>
     * <p>Precapture can be triggered through setting
     * ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER to START. Currently
     * active and completed (if it causes camera device internal AE lock) precapture
     * metering sequence can be canceled through setting
     * ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER to CANCEL.</p>
     * <p>Once PRECAPTURE completes, AE will transition to CONVERGED
     * or FLASH_REQUIRED as appropriate. This is a transient
     * state, the camera device may skip reporting this state in
     * capture result.</p>
     *
     * @see ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
     */
    ACAMERA_CONTROL_AE_STATE_PRECAPTURE                              = 5,

} acamera_metadata_enum_android_control_ae_state_t;

// ACAMERA_CONTROL_AF_STATE
typedef enum acamera_metadata_enum_acamera_control_af_state {
    /*
     * <p>AF is off or has not yet tried to scan/been asked
     * to scan.</p>
     * <p>When a camera device is opened, it starts in this
     * state. This is a transient state, the camera device may
     * skip reporting this state in capture
     * result.</p>
     */
    ACAMERA_CONTROL_AF_STATE_INACTIVE                                = 0,

    /*
     * <p>AF is currently performing an AF scan initiated the
     * camera device in a continuous autofocus mode.</p>
     * <p>Only used by CONTINUOUS_* AF modes. This is a transient
     * state, the camera device may skip reporting this state in
     * capture result.</p>
     */
    ACAMERA_CONTROL_AF_STATE_PASSIVE_SCAN                            = 1,

    /*
     * <p>AF currently believes it is in focus, but may
     * restart scanning at any time.</p>
     * <p>Only used by CONTINUOUS_* AF modes. This is a transient
     * state, the camera device may skip reporting this state in
     * capture result.</p>
     */
    ACAMERA_CONTROL_AF_STATE_PASSIVE_FOCUSED                         = 2,

    /*
     * <p>AF is performing an AF scan because it was
     * triggered by AF trigger.</p>
     * <p>Only used by AUTO or MACRO AF modes. This is a transient
     * state, the camera device may skip reporting this state in
     * capture result.</p>
     */
    ACAMERA_CONTROL_AF_STATE_ACTIVE_SCAN                             = 3,

    /*
     * <p>AF believes it is focused correctly and has locked
     * focus.</p>
     * <p>This state is reached only after an explicit START AF trigger has been
     * sent (ACAMERA_CONTROL_AF_TRIGGER), when good focus has been obtained.</p>
     * <p>The lens will remain stationary until the AF mode (ACAMERA_CONTROL_AF_MODE) is changed or
     * a new AF trigger is sent to the camera device (ACAMERA_CONTROL_AF_TRIGGER).</p>
     *
     * @see ACAMERA_CONTROL_AF_MODE
     * @see ACAMERA_CONTROL_AF_TRIGGER
     */
    ACAMERA_CONTROL_AF_STATE_FOCUSED_LOCKED                          = 4,

    /*
     * <p>AF has failed to focus successfully and has locked
     * focus.</p>
     * <p>This state is reached only after an explicit START AF trigger has been
     * sent (ACAMERA_CONTROL_AF_TRIGGER), when good focus cannot be obtained.</p>
     * <p>The lens will remain stationary until the AF mode (ACAMERA_CONTROL_AF_MODE) is changed or
     * a new AF trigger is sent to the camera device (ACAMERA_CONTROL_AF_TRIGGER).</p>
     *
     * @see ACAMERA_CONTROL_AF_MODE
     * @see ACAMERA_CONTROL_AF_TRIGGER
     */
    ACAMERA_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED                      = 5,

    /*
     * <p>AF finished a passive scan without finding focus,
     * and may restart scanning at any time.</p>
     * <p>Only used by CONTINUOUS_* AF modes. This is a transient state, the camera
     * device may skip reporting this state in capture result.</p>
     * <p>LEGACY camera devices do not support this state. When a passive
     * scan has finished, it will always go to PASSIVE_FOCUSED.</p>
     */
    ACAMERA_CONTROL_AF_STATE_PASSIVE_UNFOCUSED                       = 6,

} acamera_metadata_enum_android_control_af_state_t;

// ACAMERA_CONTROL_AWB_STATE
typedef enum acamera_metadata_enum_acamera_control_awb_state {
    /*
     * <p>AWB is not in auto mode, or has not yet started metering.</p>
     * <p>When a camera device is opened, it starts in this
     * state. This is a transient state, the camera device may
     * skip reporting this state in capture
     * result.</p>
     */
    ACAMERA_CONTROL_AWB_STATE_INACTIVE                               = 0,

    /*
     * <p>AWB doesn't yet have a good set of control
     * values for the current scene.</p>
     * <p>This is a transient state, the camera device
     * may skip reporting this state in capture result.</p>
     */
    ACAMERA_CONTROL_AWB_STATE_SEARCHING                              = 1,

    /*
     * <p>AWB has a good set of control values for the
     * current scene.</p>
     */
    ACAMERA_CONTROL_AWB_STATE_CONVERGED                              = 2,

    /*
     * <p>AWB has been locked.</p>
     */
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
    /*
     * <p>No edge enhancement is applied.</p>
     */
    ACAMERA_EDGE_MODE_OFF                                            = 0,

    /*
     * <p>Apply edge enhancement at a quality level that does not slow down frame rate
     * relative to sensor output. It may be the same as OFF if edge enhancement will
     * slow down frame rate relative to sensor.</p>
     */
    ACAMERA_EDGE_MODE_FAST                                           = 1,

    /*
     * <p>Apply high-quality edge enhancement, at a cost of possibly reduced output frame rate.</p>
     */
    ACAMERA_EDGE_MODE_HIGH_QUALITY                                   = 2,

    /*
     * <p>Edge enhancement is applied at different levels for different output streams,
     * based on resolution. Streams at maximum recording resolution (see {@link
     * android.hardware.camera2.CameraDevice#createCaptureSession}) or below have
     * edge enhancement applied, while higher-resolution streams have no edge enhancement
     * applied. The level of edge enhancement for low-resolution streams is tuned so that
     * frame rate is not impacted, and the quality is equal to or better than FAST (since it
     * is only applied to lower-resolution outputs, quality may improve from FAST).</p>
     * <p>This mode is intended to be used by applications operating in a zero-shutter-lag mode
     * with YUV or PRIVATE reprocessing, where the application continuously captures
     * high-resolution intermediate buffers into a circular buffer, from which a final image is
     * produced via reprocessing when a user takes a picture.  For such a use case, the
     * high-resolution buffers must not have edge enhancement applied to maximize efficiency of
     * preview and to avoid double-applying enhancement when reprocessed, while low-resolution
     * buffers (used for recording or preview, generally) need edge enhancement applied for
     * reasonable preview quality.</p>
     * <p>This mode is guaranteed to be supported by devices that support either the
     * YUV_REPROCESSING or PRIVATE_REPROCESSING capabilities
     * (ACAMERA_REQUEST_AVAILABLE_CAPABILITIES lists either of those capabilities) and it will
     * be the default mode for CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG template.</p>
     *
     * @see ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
     */
    ACAMERA_EDGE_MODE_ZERO_SHUTTER_LAG                               = 3,

} acamera_metadata_enum_android_edge_mode_t;


// ACAMERA_FLASH_MODE
typedef enum acamera_metadata_enum_acamera_flash_mode {
    /*
     * <p>Do not fire the flash for this capture.</p>
     */
    ACAMERA_FLASH_MODE_OFF                                           = 0,

    /*
     * <p>If the flash is available and charged, fire flash
     * for this capture.</p>
     */
    ACAMERA_FLASH_MODE_SINGLE                                        = 1,

    /*
     * <p>Transition flash to continuously on.</p>
     */
    ACAMERA_FLASH_MODE_TORCH                                         = 2,

} acamera_metadata_enum_android_flash_mode_t;

// ACAMERA_FLASH_STATE
typedef enum acamera_metadata_enum_acamera_flash_state {
    /*
     * <p>No flash on camera.</p>
     */
    ACAMERA_FLASH_STATE_UNAVAILABLE                                  = 0,

    /*
     * <p>Flash is charging and cannot be fired.</p>
     */
    ACAMERA_FLASH_STATE_CHARGING                                     = 1,

    /*
     * <p>Flash is ready to fire.</p>
     */
    ACAMERA_FLASH_STATE_READY                                        = 2,

    /*
     * <p>Flash fired for this capture.</p>
     */
    ACAMERA_FLASH_STATE_FIRED                                        = 3,

    /*
     * <p>Flash partially illuminated this frame.</p>
     * <p>This is usually due to the next or previous frame having
     * the flash fire, and the flash spilling into this capture
     * due to hardware limitations.</p>
     */
    ACAMERA_FLASH_STATE_PARTIAL                                      = 4,

} acamera_metadata_enum_android_flash_state_t;


// ACAMERA_FLASH_INFO_AVAILABLE
typedef enum acamera_metadata_enum_acamera_flash_info_available {
    ACAMERA_FLASH_INFO_AVAILABLE_FALSE                               = 0,

    ACAMERA_FLASH_INFO_AVAILABLE_TRUE                                = 1,

} acamera_metadata_enum_android_flash_info_available_t;


// ACAMERA_HOT_PIXEL_MODE
typedef enum acamera_metadata_enum_acamera_hot_pixel_mode {
    /*
     * <p>No hot pixel correction is applied.</p>
     * <p>The frame rate must not be reduced relative to sensor raw output
     * for this option.</p>
     * <p>The hotpixel map may be returned in ACAMERA_STATISTICS_HOT_PIXEL_MAP.</p>
     *
     * @see ACAMERA_STATISTICS_HOT_PIXEL_MAP
     */
    ACAMERA_HOT_PIXEL_MODE_OFF                                       = 0,

    /*
     * <p>Hot pixel correction is applied, without reducing frame
     * rate relative to sensor raw output.</p>
     * <p>The hotpixel map may be returned in ACAMERA_STATISTICS_HOT_PIXEL_MAP.</p>
     *
     * @see ACAMERA_STATISTICS_HOT_PIXEL_MAP
     */
    ACAMERA_HOT_PIXEL_MODE_FAST                                      = 1,

    /*
     * <p>High-quality hot pixel correction is applied, at a cost
     * of possibly reduced frame rate relative to sensor raw output.</p>
     * <p>The hotpixel map may be returned in ACAMERA_STATISTICS_HOT_PIXEL_MAP.</p>
     *
     * @see ACAMERA_STATISTICS_HOT_PIXEL_MAP
     */
    ACAMERA_HOT_PIXEL_MODE_HIGH_QUALITY                              = 2,

} acamera_metadata_enum_android_hot_pixel_mode_t;



// ACAMERA_LENS_OPTICAL_STABILIZATION_MODE
typedef enum acamera_metadata_enum_acamera_lens_optical_stabilization_mode {
    /*
     * <p>Optical stabilization is unavailable.</p>
     */
    ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_OFF                      = 0,

    /*
     * <p>Optical stabilization is enabled.</p>
     */
    ACAMERA_LENS_OPTICAL_STABILIZATION_MODE_ON                       = 1,

} acamera_metadata_enum_android_lens_optical_stabilization_mode_t;

// ACAMERA_LENS_FACING
typedef enum acamera_metadata_enum_acamera_lens_facing {
    /*
     * <p>The camera device faces the same direction as the device's screen.</p>
     */
    ACAMERA_LENS_FACING_FRONT                                        = 0,

    /*
     * <p>The camera device faces the opposite direction as the device's screen.</p>
     */
    ACAMERA_LENS_FACING_BACK                                         = 1,

    /*
     * <p>The camera device is an external camera, and has no fixed facing relative to the
     * device's screen.</p>
     */
    ACAMERA_LENS_FACING_EXTERNAL                                     = 2,

} acamera_metadata_enum_android_lens_facing_t;

// ACAMERA_LENS_STATE
typedef enum acamera_metadata_enum_acamera_lens_state {
    /*
     * <p>The lens parameters (ACAMERA_LENS_FOCAL_LENGTH, ACAMERA_LENS_FOCUS_DISTANCE,
     * ACAMERA_LENS_FILTER_DENSITY and ACAMERA_LENS_APERTURE) are not changing.</p>
     *
     * @see ACAMERA_LENS_APERTURE
     * @see ACAMERA_LENS_FILTER_DENSITY
     * @see ACAMERA_LENS_FOCAL_LENGTH
     * @see ACAMERA_LENS_FOCUS_DISTANCE
     */
    ACAMERA_LENS_STATE_STATIONARY                                    = 0,

    /*
     * <p>One or several of the lens parameters
     * (ACAMERA_LENS_FOCAL_LENGTH, ACAMERA_LENS_FOCUS_DISTANCE,
     * ACAMERA_LENS_FILTER_DENSITY or ACAMERA_LENS_APERTURE) is
     * currently changing.</p>
     *
     * @see ACAMERA_LENS_APERTURE
     * @see ACAMERA_LENS_FILTER_DENSITY
     * @see ACAMERA_LENS_FOCAL_LENGTH
     * @see ACAMERA_LENS_FOCUS_DISTANCE
     */
    ACAMERA_LENS_STATE_MOVING                                        = 1,

} acamera_metadata_enum_android_lens_state_t;


// ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION
typedef enum acamera_metadata_enum_acamera_lens_info_focus_distance_calibration {
    /*
     * <p>The lens focus distance is not accurate, and the units used for
     * ACAMERA_LENS_FOCUS_DISTANCE do not correspond to any physical units.</p>
     * <p>Setting the lens to the same focus distance on separate occasions may
     * result in a different real focus distance, depending on factors such
     * as the orientation of the device, the age of the focusing mechanism,
     * and the device temperature. The focus distance value will still be
     * in the range of <code>[0, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE]</code>, where 0
     * represents the farthest focus.</p>
     *
     * @see ACAMERA_LENS_FOCUS_DISTANCE
     * @see ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE
     */
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED        = 0,

    /*
     * <p>The lens focus distance is measured in diopters.</p>
     * <p>However, setting the lens to the same focus distance
     * on separate occasions may result in a different real
     * focus distance, depending on factors such as the
     * orientation of the device, the age of the focusing
     * mechanism, and the device temperature.</p>
     */
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_APPROXIMATE         = 1,

    /*
     * <p>The lens focus distance is measured in diopters, and
     * is calibrated.</p>
     * <p>The lens mechanism is calibrated so that setting the
     * same focus distance is repeatable on multiple
     * occasions with good accuracy, and the focus distance
     * corresponds to the real physical distance to the plane
     * of best focus.</p>
     */
    ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_CALIBRATED          = 2,

} acamera_metadata_enum_android_lens_info_focus_distance_calibration_t;


// ACAMERA_NOISE_REDUCTION_MODE
typedef enum acamera_metadata_enum_acamera_noise_reduction_mode {
    /*
     * <p>No noise reduction is applied.</p>
     */
    ACAMERA_NOISE_REDUCTION_MODE_OFF                                 = 0,

    /*
     * <p>Noise reduction is applied without reducing frame rate relative to sensor
     * output. It may be the same as OFF if noise reduction will reduce frame rate
     * relative to sensor.</p>
     */
    ACAMERA_NOISE_REDUCTION_MODE_FAST                                = 1,

    /*
     * <p>High-quality noise reduction is applied, at the cost of possibly reduced frame
     * rate relative to sensor output.</p>
     */
    ACAMERA_NOISE_REDUCTION_MODE_HIGH_QUALITY                        = 2,

    /*
     * <p>MINIMAL noise reduction is applied without reducing frame rate relative to
     * sensor output. </p>
     */
    ACAMERA_NOISE_REDUCTION_MODE_MINIMAL                             = 3,

    /*
     * <p>Noise reduction is applied at different levels for different output streams,
     * based on resolution. Streams at maximum recording resolution (see {@link
     * android.hardware.camera2.CameraDevice#createCaptureSession}) or below have noise
     * reduction applied, while higher-resolution streams have MINIMAL (if supported) or no
     * noise reduction applied (if MINIMAL is not supported.) The degree of noise reduction
     * for low-resolution streams is tuned so that frame rate is not impacted, and the quality
     * is equal to or better than FAST (since it is only applied to lower-resolution outputs,
     * quality may improve from FAST).</p>
     * <p>This mode is intended to be used by applications operating in a zero-shutter-lag mode
     * with YUV or PRIVATE reprocessing, where the application continuously captures
     * high-resolution intermediate buffers into a circular buffer, from which a final image is
     * produced via reprocessing when a user takes a picture.  For such a use case, the
     * high-resolution buffers must not have noise reduction applied to maximize efficiency of
     * preview and to avoid over-applying noise filtering when reprocessing, while
     * low-resolution buffers (used for recording or preview, generally) need noise reduction
     * applied for reasonable preview quality.</p>
     * <p>This mode is guaranteed to be supported by devices that support either the
     * YUV_REPROCESSING or PRIVATE_REPROCESSING capabilities
     * (ACAMERA_REQUEST_AVAILABLE_CAPABILITIES lists either of those capabilities) and it will
     * be the default mode for CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG template.</p>
     *
     * @see ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
     */
    ACAMERA_NOISE_REDUCTION_MODE_ZERO_SHUTTER_LAG                    = 4,

} acamera_metadata_enum_android_noise_reduction_mode_t;



// ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
typedef enum acamera_metadata_enum_acamera_request_available_capabilities {
    /*
     * <p>The minimal set of capabilities that every camera
     * device (regardless of ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL)
     * supports.</p>
     * <p>This capability is listed by all normal devices, and
     * indicates that the camera device has a feature set
     * that's comparable to the baseline requirements for the
     * older android.hardware.Camera API.</p>
     * <p>Devices with the DEPTH_OUTPUT capability might not list this
     * capability, indicating that they support only depth measurement,
     * not standard color output.</p>
     *
     * @see ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL
     */
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE       = 0,

    /*
     * <p>The camera device can be manually controlled (3A algorithms such
     * as auto-exposure, and auto-focus can be bypassed).
     * The camera device supports basic manual control of the sensor image
     * acquisition related stages. This means the following controls are
     * guaranteed to be supported:</p>
     * <ul>
     * <li>Manual frame duration control<ul>
     * <li>ACAMERA_SENSOR_FRAME_DURATION</li>
     * <li>ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION</li>
     * </ul>
     * </li>
     * <li>Manual exposure control<ul>
     * <li>ACAMERA_SENSOR_EXPOSURE_TIME</li>
     * <li>ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE</li>
     * </ul>
     * </li>
     * <li>Manual sensitivity control<ul>
     * <li>ACAMERA_SENSOR_SENSITIVITY</li>
     * <li>ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE</li>
     * </ul>
     * </li>
     * <li>Manual lens control (if the lens is adjustable)<ul>
     * <li>ACAMERA_LENS_*</li>
     * </ul>
     * </li>
     * <li>Manual flash control (if a flash unit is present)<ul>
     * <li>ACAMERA_FLASH_*</li>
     * </ul>
     * </li>
     * <li>Manual black level locking<ul>
     * <li>ACAMERA_BLACK_LEVEL_LOCK</li>
     * </ul>
     * </li>
     * <li>Auto exposure lock<ul>
     * <li>ACAMERA_CONTROL_AE_LOCK</li>
     * </ul>
     * </li>
     * </ul>
     * <p>If any of the above 3A algorithms are enabled, then the camera
     * device will accurately report the values applied by 3A in the
     * result.</p>
     * <p>A given camera device may also support additional manual sensor controls,
     * but this capability only covers the above list of controls.</p>
     * <p>If this is supported, android.scaler.streamConfigurationMap will
     * additionally return a min frame duration that is greater than
     * zero for each supported size-format combination.</p>
     *
     * @see ACAMERA_BLACK_LEVEL_LOCK
     * @see ACAMERA_CONTROL_AE_LOCK
     * @see ACAMERA_SENSOR_EXPOSURE_TIME
     * @see ACAMERA_SENSOR_FRAME_DURATION
     * @see ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE
     * @see ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION
     * @see ACAMERA_SENSOR_INFO_SENSITIVITY_RANGE
     * @see ACAMERA_SENSOR_SENSITIVITY
     */
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR             = 1,

    /*
     * <p>The camera device post-processing stages can be manually controlled.
     * The camera device supports basic manual control of the image post-processing
     * stages. This means the following controls are guaranteed to be supported:</p>
     * <ul>
     * <li>
     * <p>Manual tonemap control</p>
     * <ul>
     * <li>android.tonemap.curve</li>
     * <li>ACAMERA_TONEMAP_MODE</li>
     * <li>ACAMERA_TONEMAP_MAX_CURVE_POINTS</li>
     * <li>ACAMERA_TONEMAP_GAMMA</li>
     * <li>ACAMERA_TONEMAP_PRESET_CURVE</li>
     * </ul>
     * </li>
     * <li>
     * <p>Manual white balance control</p>
     * <ul>
     * <li>ACAMERA_COLOR_CORRECTION_TRANSFORM</li>
     * <li>ACAMERA_COLOR_CORRECTION_GAINS</li>
     * </ul>
     * </li>
     * <li>Manual lens shading map control<ul>
     * <li>ACAMERA_SHADING_MODE</li>
     * <li>ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE</li>
     * <li>ACAMERA_STATISTICS_LENS_SHADING_MAP</li>
     * <li>ACAMERA_LENS_INFO_SHADING_MAP_SIZE</li>
     * </ul>
     * </li>
     * <li>Manual aberration correction control (if aberration correction is supported)<ul>
     * <li>ACAMERA_COLOR_CORRECTION_ABERRATION_MODE</li>
     * <li>ACAMERA_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES</li>
     * </ul>
     * </li>
     * <li>Auto white balance lock<ul>
     * <li>ACAMERA_CONTROL_AWB_LOCK</li>
     * </ul>
     * </li>
     * </ul>
     * <p>If auto white balance is enabled, then the camera device
     * will accurately report the values applied by AWB in the result.</p>
     * <p>A given camera device may also support additional post-processing
     * controls, but this capability only covers the above list of controls.</p>
     *
     * @see ACAMERA_COLOR_CORRECTION_ABERRATION_MODE
     * @see ACAMERA_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES
     * @see ACAMERA_COLOR_CORRECTION_GAINS
     * @see ACAMERA_COLOR_CORRECTION_TRANSFORM
     * @see ACAMERA_CONTROL_AWB_LOCK
     * @see ACAMERA_LENS_INFO_SHADING_MAP_SIZE
     * @see ACAMERA_SHADING_MODE
     * @see ACAMERA_STATISTICS_LENS_SHADING_MAP
     * @see ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE
     * @see ACAMERA_TONEMAP_GAMMA
     * @see ACAMERA_TONEMAP_MAX_CURVE_POINTS
     * @see ACAMERA_TONEMAP_MODE
     * @see ACAMERA_TONEMAP_PRESET_CURVE
     */
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_POST_PROCESSING    = 2,

    /*
     * <p>The camera device supports outputting RAW buffers and
     * metadata for interpreting them.</p>
     * <p>Devices supporting the RAW capability allow both for
     * saving DNG files, and for direct application processing of
     * raw sensor images.</p>
     * <ul>
     * <li>RAW_SENSOR is supported as an output format.</li>
     * <li>The maximum available resolution for RAW_SENSOR streams
     *   will match either the value in
     *   ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE or
     *   ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE.</li>
     * <li>All DNG-related optional metadata entries are provided
     *   by the camera device.</li>
     * </ul>
     *
     * @see ACAMERA_SENSOR_INFO_PIXEL_ARRAY_SIZE
     * @see ACAMERA_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE
     */
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_RAW                       = 3,

    /*
     * <p>The camera device supports accurately reporting the sensor settings for many of
     * the sensor controls while the built-in 3A algorithm is running.  This allows
     * reporting of sensor settings even when these settings cannot be manually changed.</p>
     * <p>The values reported for the following controls are guaranteed to be available
     * in the CaptureResult, including when 3A is enabled:</p>
     * <ul>
     * <li>Exposure control<ul>
     * <li>ACAMERA_SENSOR_EXPOSURE_TIME</li>
     * </ul>
     * </li>
     * <li>Sensitivity control<ul>
     * <li>ACAMERA_SENSOR_SENSITIVITY</li>
     * </ul>
     * </li>
     * <li>Lens controls (if the lens is adjustable)<ul>
     * <li>ACAMERA_LENS_FOCUS_DISTANCE</li>
     * <li>ACAMERA_LENS_APERTURE</li>
     * </ul>
     * </li>
     * </ul>
     * <p>This capability is a subset of the MANUAL_SENSOR control capability, and will
     * always be included if the MANUAL_SENSOR capability is available.</p>
     *
     * @see ACAMERA_LENS_APERTURE
     * @see ACAMERA_LENS_FOCUS_DISTANCE
     * @see ACAMERA_SENSOR_EXPOSURE_TIME
     * @see ACAMERA_SENSOR_SENSITIVITY
     */
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS      = 5,

    /*
     * <p>The camera device supports capturing high-resolution images at &gt;= 20 frames per
     * second, in at least the uncompressed YUV format, when post-processing settings are set
     * to FAST. Additionally, maximum-resolution images can be captured at &gt;= 10 frames
     * per second.  Here, 'high resolution' means at least 8 megapixels, or the maximum
     * resolution of the device, whichever is smaller.</p>
     * <p>More specifically, this means that a size matching the camera device's active array
     * size is listed as a supported size for the {@link
     * android.graphics.ImageFormat#YUV_420_888} format in either {@link
     * android.hardware.camera2.params.StreamConfigurationMap#getOutputSizes} or {@link
     * android.hardware.camera2.params.StreamConfigurationMap#getHighResolutionOutputSizes},
     * with a minimum frame duration for that format and size of either &lt;= 1/20 s, or
     * &lt;= 1/10 s, respectively; and the ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES entry
     * lists at least one FPS range where the minimum FPS is &gt;= 1 / minimumFrameDuration
     * for the maximum-size YUV_420_888 format.  If that maximum size is listed in {@link
     * android.hardware.camera2.params.StreamConfigurationMap#getHighResolutionOutputSizes},
     * then the list of resolutions for YUV_420_888 from {@link
     * android.hardware.camera2.params.StreamConfigurationMap#getOutputSizes} contains at
     * least one resolution &gt;= 8 megapixels, with a minimum frame duration of &lt;= 1/20
     * s.</p>
     * <p>If the device supports the {@link android.graphics.ImageFormat#RAW10}, {@link
     * android.graphics.ImageFormat#RAW12}, then those can also be captured at the same rate
     * as the maximum-size YUV_420_888 resolution is.</p>
     * <p>If the device supports the PRIVATE_REPROCESSING capability, then the same guarantees
     * as for the YUV_420_888 format also apply to the {@link
     * android.graphics.ImageFormat#PRIVATE} format.</p>
     * <p>In addition, the ACAMERA_SYNC_MAX_LATENCY field is guaranted to have a value between 0
     * and 4, inclusive. ACAMERA_CONTROL_AE_LOCK_AVAILABLE and ACAMERA_CONTROL_AWB_LOCK_AVAILABLE
     * are also guaranteed to be <code>true</code> so burst capture with these two locks ON yields
     * consistent image output.</p>
     *
     * @see ACAMERA_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES
     * @see ACAMERA_CONTROL_AE_LOCK_AVAILABLE
     * @see ACAMERA_CONTROL_AWB_LOCK_AVAILABLE
     * @see ACAMERA_SYNC_MAX_LATENCY
     */
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_BURST_CAPTURE             = 6,

    /*
     * <p>The camera device can produce depth measurements from its field of view.</p>
     * <p>This capability requires the camera device to support the following:</p>
     * <ul>
     * <li>{@link android.graphics.ImageFormat#DEPTH16} is supported as an output format.</li>
     * <li>{@link android.graphics.ImageFormat#DEPTH_POINT_CLOUD} is optionally supported as an
     *   output format.</li>
     * <li>This camera device, and all camera devices with the same ACAMERA_LENS_FACING,
     *   will list the following calibration entries in both
     *   {@link android.hardware.camera2.CameraCharacteristics} and
     *   {@link android.hardware.camera2.CaptureResult}:<ul>
     * <li>ACAMERA_LENS_POSE_TRANSLATION</li>
     * <li>ACAMERA_LENS_POSE_ROTATION</li>
     * <li>ACAMERA_LENS_INTRINSIC_CALIBRATION</li>
     * <li>ACAMERA_LENS_RADIAL_DISTORTION</li>
     * </ul>
     * </li>
     * <li>The ACAMERA_DEPTH_DEPTH_IS_EXCLUSIVE entry is listed by this device.</li>
     * <li>A LIMITED camera with only the DEPTH_OUTPUT capability does not have to support
     *   normal YUV_420_888, JPEG, and PRIV-format outputs. It only has to support the DEPTH16
     *   format.</li>
     * </ul>
     * <p>Generally, depth output operates at a slower frame rate than standard color capture,
     * so the DEPTH16 and DEPTH_POINT_CLOUD formats will commonly have a stall duration that
     * should be accounted for (see
     * {@link android.hardware.camera2.params.StreamConfigurationMap#getOutputStallDuration}).
     * On a device that supports both depth and color-based output, to enable smooth preview,
     * using a repeating burst is recommended, where a depth-output target is only included
     * once every N frames, where N is the ratio between preview output rate and depth output
     * rate, including depth stall time.</p>
     *
     * @see ACAMERA_DEPTH_DEPTH_IS_EXCLUSIVE
     * @see ACAMERA_LENS_FACING
     * @see ACAMERA_LENS_INTRINSIC_CALIBRATION
     * @see ACAMERA_LENS_POSE_ROTATION
     * @see ACAMERA_LENS_POSE_TRANSLATION
     * @see ACAMERA_LENS_RADIAL_DISTORTION
     */
    ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT              = 8,

} acamera_metadata_enum_android_request_available_capabilities_t;


// ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS
typedef enum acamera_metadata_enum_acamera_scaler_available_stream_configurations {
    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT            = 0,

    ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_INPUT             = 1,

} acamera_metadata_enum_android_scaler_available_stream_configurations_t;

// ACAMERA_SCALER_CROPPING_TYPE
typedef enum acamera_metadata_enum_acamera_scaler_cropping_type {
    /*
     * <p>The camera device only supports centered crop regions.</p>
     */
    ACAMERA_SCALER_CROPPING_TYPE_CENTER_ONLY                         = 0,

    /*
     * <p>The camera device supports arbitrarily chosen crop regions.</p>
     */
    ACAMERA_SCALER_CROPPING_TYPE_FREEFORM                            = 1,

} acamera_metadata_enum_android_scaler_cropping_type_t;


// ACAMERA_SENSOR_REFERENCE_ILLUMINANT1
typedef enum acamera_metadata_enum_acamera_sensor_reference_illuminant1 {
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT                    = 1,

    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_FLUORESCENT                 = 2,

    /*
     * <p>Incandescent light</p>
     */
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_TUNGSTEN                    = 3,

    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_FLASH                       = 4,

    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_FINE_WEATHER                = 9,

    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_CLOUDY_WEATHER              = 10,

    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_SHADE                       = 11,

    /*
     * <p>D 5700 - 7100K</p>
     */
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_DAYLIGHT_FLUORESCENT        = 12,

    /*
     * <p>N 4600 - 5400K</p>
     */
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_DAY_WHITE_FLUORESCENT       = 13,

    /*
     * <p>W 3900 - 4500K</p>
     */
    ACAMERA_SENSOR_REFERENCE_ILLUMINANT1_COOL_WHITE_FLUORESCENT      = 14,

    /*
     * <p>WW 3200 - 3700K</p>
     */
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
    /*
     * <p>No test pattern mode is used, and the camera
     * device returns captures from the image sensor.</p>
     * <p>This is the default if the key is not set.</p>
     */
    ACAMERA_SENSOR_TEST_PATTERN_MODE_OFF                             = 0,

    /*
     * <p>Each pixel in <code>[R, G_even, G_odd, B]</code> is replaced by its
     * respective color channel provided in
     * ACAMERA_SENSOR_TEST_PATTERN_DATA.</p>
     * <p>For example:</p>
     * <pre><code>android.testPatternData = [0, 0xFFFFFFFF, 0xFFFFFFFF, 0]
     * </code></pre>
     * <p>All green pixels are 100% green. All red/blue pixels are black.</p>
     * <pre><code>android.testPatternData = [0xFFFFFFFF, 0, 0xFFFFFFFF, 0]
     * </code></pre>
     * <p>All red pixels are 100% red. Only the odd green pixels
     * are 100% green. All blue pixels are 100% black.</p>
     *
     * @see ACAMERA_SENSOR_TEST_PATTERN_DATA
     */
    ACAMERA_SENSOR_TEST_PATTERN_MODE_SOLID_COLOR                     = 1,

    /*
     * <p>All pixel data is replaced with an 8-bar color pattern.</p>
     * <p>The vertical bars (left-to-right) are as follows:</p>
     * <ul>
     * <li>100% white</li>
     * <li>yellow</li>
     * <li>cyan</li>
     * <li>green</li>
     * <li>magenta</li>
     * <li>red</li>
     * <li>blue</li>
     * <li>black</li>
     * </ul>
     * <p>In general the image would look like the following:</p>
     * <pre><code>W Y C G M R B K
     * W Y C G M R B K
     * W Y C G M R B K
     * W Y C G M R B K
     * W Y C G M R B K
     * . . . . . . . .
     * . . . . . . . .
     * . . . . . . . .
     *
     * (B = Blue, K = Black)
     * </code></pre>
     * <p>Each bar should take up 1/8 of the sensor pixel array width.
     * When this is not possible, the bar size should be rounded
     * down to the nearest integer and the pattern can repeat
     * on the right side.</p>
     * <p>Each bar's height must always take up the full sensor
     * pixel array height.</p>
     * <p>Each pixel in this test pattern must be set to either
     * 0% intensity or 100% intensity.</p>
     */
    ACAMERA_SENSOR_TEST_PATTERN_MODE_COLOR_BARS                      = 2,

    /*
     * <p>The test pattern is similar to COLOR_BARS, except that
     * each bar should start at its specified color at the top,
     * and fade to gray at the bottom.</p>
     * <p>Furthermore each bar is further subdivided into a left and
     * right half. The left half should have a smooth gradient,
     * and the right half should have a quantized gradient.</p>
     * <p>In particular, the right half's should consist of blocks of the
     * same color for 1/16th active sensor pixel array width.</p>
     * <p>The least significant bits in the quantized gradient should
     * be copied from the most significant bits of the smooth gradient.</p>
     * <p>The height of each bar should always be a multiple of 128.
     * When this is not the case, the pattern should repeat at the bottom
     * of the image.</p>
     */
    ACAMERA_SENSOR_TEST_PATTERN_MODE_COLOR_BARS_FADE_TO_GRAY         = 3,

    /*
     * <p>All pixel data is replaced by a pseudo-random sequence
     * generated from a PN9 512-bit sequence (typically implemented
     * in hardware with a linear feedback shift register).</p>
     * <p>The generator should be reset at the beginning of each frame,
     * and thus each subsequent raw frame with this test pattern should
     * be exactly the same as the last.</p>
     */
    ACAMERA_SENSOR_TEST_PATTERN_MODE_PN9                             = 4,

    /*
     * <p>The first custom test pattern. All custom patterns that are
     * available only on this camera device are at least this numeric
     * value.</p>
     * <p>All of the custom test patterns will be static
     * (that is the raw image must not vary from frame to frame).</p>
     */
    ACAMERA_SENSOR_TEST_PATTERN_MODE_CUSTOM1                         = 256,

} acamera_metadata_enum_android_sensor_test_pattern_mode_t;


// ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT
typedef enum acamera_metadata_enum_acamera_sensor_info_color_filter_arrangement {
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGGB                = 0,

    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GRBG                = 1,

    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_GBRG                = 2,

    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_BGGR                = 3,

    /*
     * <p>Sensor is not Bayer; output has 3 16-bit
     * values for each pixel, instead of just 1 16-bit value
     * per pixel.</p>
     */
    ACAMERA_SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_RGB                 = 4,

} acamera_metadata_enum_android_sensor_info_color_filter_arrangement_t;

// ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE
typedef enum acamera_metadata_enum_acamera_sensor_info_timestamp_source {
    /*
     * <p>Timestamps from ACAMERA_SENSOR_TIMESTAMP are in nanoseconds and monotonic,
     * but can not be compared to timestamps from other subsystems
     * (e.g. accelerometer, gyro etc.), or other instances of the same or different
     * camera devices in the same system. Timestamps between streams and results for
     * a single camera instance are comparable, and the timestamps for all buffers
     * and the result metadata generated by a single capture are identical.</p>
     *
     * @see ACAMERA_SENSOR_TIMESTAMP
     */
    ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN                     = 0,

    /*
     * <p>Timestamps from ACAMERA_SENSOR_TIMESTAMP are in the same timebase as
     * {@link android.os.SystemClock#elapsedRealtimeNanos},
     * and they can be compared to other timestamps using that base.</p>
     *
     * @see ACAMERA_SENSOR_TIMESTAMP
     */
    ACAMERA_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME                    = 1,

} acamera_metadata_enum_android_sensor_info_timestamp_source_t;

// ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED
typedef enum acamera_metadata_enum_acamera_sensor_info_lens_shading_applied {
    ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED_FALSE                   = 0,

    ACAMERA_SENSOR_INFO_LENS_SHADING_APPLIED_TRUE                    = 1,

} acamera_metadata_enum_android_sensor_info_lens_shading_applied_t;


// ACAMERA_SHADING_MODE
typedef enum acamera_metadata_enum_acamera_shading_mode {
    /*
     * <p>No lens shading correction is applied.</p>
     */
    ACAMERA_SHADING_MODE_OFF                                         = 0,

    /*
     * <p>Apply lens shading corrections, without slowing
     * frame rate relative to sensor raw output</p>
     */
    ACAMERA_SHADING_MODE_FAST                                        = 1,

    /*
     * <p>Apply high-quality lens shading correction, at the
     * cost of possibly reduced frame rate.</p>
     */
    ACAMERA_SHADING_MODE_HIGH_QUALITY                                = 2,

} acamera_metadata_enum_android_shading_mode_t;


// ACAMERA_STATISTICS_FACE_DETECT_MODE
typedef enum acamera_metadata_enum_acamera_statistics_face_detect_mode {
    /*
     * <p>Do not include face detection statistics in capture
     * results.</p>
     */
    ACAMERA_STATISTICS_FACE_DETECT_MODE_OFF                          = 0,

    /*
     * <p>Return face rectangle and confidence values only.</p>
     */
    ACAMERA_STATISTICS_FACE_DETECT_MODE_SIMPLE                       = 1,

    /*
     * <p>Return all face
     * metadata.</p>
     * <p>In this mode, face rectangles, scores, landmarks, and face IDs are all valid.</p>
     */
    ACAMERA_STATISTICS_FACE_DETECT_MODE_FULL                         = 2,

} acamera_metadata_enum_android_statistics_face_detect_mode_t;

// ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE
typedef enum acamera_metadata_enum_acamera_statistics_hot_pixel_map_mode {
    /*
     * <p>Hot pixel map production is disabled.</p>
     */
    ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE_OFF                        = 0,

    /*
     * <p>Hot pixel map production is enabled.</p>
     */
    ACAMERA_STATISTICS_HOT_PIXEL_MAP_MODE_ON                         = 1,

} acamera_metadata_enum_android_statistics_hot_pixel_map_mode_t;

// ACAMERA_STATISTICS_SCENE_FLICKER
typedef enum acamera_metadata_enum_acamera_statistics_scene_flicker {
    /*
     * <p>The camera device does not detect any flickering illumination
     * in the current scene.</p>
     */
    ACAMERA_STATISTICS_SCENE_FLICKER_NONE                            = 0,

    /*
     * <p>The camera device detects illumination flickering at 50Hz
     * in the current scene.</p>
     */
    ACAMERA_STATISTICS_SCENE_FLICKER_50HZ                            = 1,

    /*
     * <p>The camera device detects illumination flickering at 60Hz
     * in the current scene.</p>
     */
    ACAMERA_STATISTICS_SCENE_FLICKER_60HZ                            = 2,

} acamera_metadata_enum_android_statistics_scene_flicker_t;

// ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE
typedef enum acamera_metadata_enum_acamera_statistics_lens_shading_map_mode {
    /*
     * <p>Do not include a lens shading map in the capture result.</p>
     */
    ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_OFF                     = 0,

    /*
     * <p>Include a lens shading map in the capture result.</p>
     */
    ACAMERA_STATISTICS_LENS_SHADING_MAP_MODE_ON                      = 1,

} acamera_metadata_enum_android_statistics_lens_shading_map_mode_t;



// ACAMERA_TONEMAP_MODE
typedef enum acamera_metadata_enum_acamera_tonemap_mode {
    /*
     * <p>Use the tone mapping curve specified in
     * the ACAMERA_TONEMAPCURVE_* entries.</p>
     * <p>All color enhancement and tonemapping must be disabled, except
     * for applying the tonemapping curve specified by
     * android.tonemap.curve.</p>
     * <p>Must not slow down frame rate relative to raw
     * sensor output.</p>
     */
    ACAMERA_TONEMAP_MODE_CONTRAST_CURVE                              = 0,

    /*
     * <p>Advanced gamma mapping and color enhancement may be applied, without
     * reducing frame rate compared to raw sensor output.</p>
     */
    ACAMERA_TONEMAP_MODE_FAST                                        = 1,

    /*
     * <p>High-quality gamma mapping and color enhancement will be applied, at
     * the cost of possibly reduced frame rate compared to raw sensor output.</p>
     */
    ACAMERA_TONEMAP_MODE_HIGH_QUALITY                                = 2,

    /*
     * <p>Use the gamma value specified in ACAMERA_TONEMAP_GAMMA to peform
     * tonemapping.</p>
     * <p>All color enhancement and tonemapping must be disabled, except
     * for applying the tonemapping curve specified by ACAMERA_TONEMAP_GAMMA.</p>
     * <p>Must not slow down frame rate relative to raw sensor output.</p>
     *
     * @see ACAMERA_TONEMAP_GAMMA
     */
    ACAMERA_TONEMAP_MODE_GAMMA_VALUE                                 = 3,

    /*
     * <p>Use the preset tonemapping curve specified in
     * ACAMERA_TONEMAP_PRESET_CURVE to peform tonemapping.</p>
     * <p>All color enhancement and tonemapping must be disabled, except
     * for applying the tonemapping curve specified by
     * ACAMERA_TONEMAP_PRESET_CURVE.</p>
     * <p>Must not slow down frame rate relative to raw sensor output.</p>
     *
     * @see ACAMERA_TONEMAP_PRESET_CURVE
     */
    ACAMERA_TONEMAP_MODE_PRESET_CURVE                                = 4,

} acamera_metadata_enum_android_tonemap_mode_t;

// ACAMERA_TONEMAP_PRESET_CURVE
typedef enum acamera_metadata_enum_acamera_tonemap_preset_curve {
    /*
     * <p>Tonemapping curve is defined by sRGB</p>
     */
    ACAMERA_TONEMAP_PRESET_CURVE_SRGB                                = 0,

    /*
     * <p>Tonemapping curve is defined by ITU-R BT.709</p>
     */
    ACAMERA_TONEMAP_PRESET_CURVE_REC709                              = 1,

} acamera_metadata_enum_android_tonemap_preset_curve_t;



// ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL
typedef enum acamera_metadata_enum_acamera_info_supported_hardware_level {
    /*
     * <p>This camera device does not have enough capabilities to qualify as a <code>FULL</code> device or
     * better.</p>
     * <p>Only the stream configurations listed in the <code>LEGACY</code> and <code>LIMITED</code> tables in the
     * {@link android.hardware.camera2.CameraDevice#createCaptureSession
     * createCaptureSession} documentation are guaranteed to be supported.</p>
     * <p>All <code>LIMITED</code> devices support the <code>BACKWARDS_COMPATIBLE</code> capability, indicating basic
     * support for color image capture. The only exception is that the device may
     * alternatively support only the <code>DEPTH_OUTPUT</code> capability, if it can only output depth
     * measurements and not color images.</p>
     * <p><code>LIMITED</code> devices and above require the use of ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
     * to lock exposure metering (and calculate flash power, for cameras with flash) before
     * capturing a high-quality still image.</p>
     * <p>A <code>LIMITED</code> device that only lists the <code>BACKWARDS_COMPATIBLE</code> capability is only
     * required to support full-automatic operation and post-processing (<code>OFF</code> is not
     * supported for ACAMERA_CONTROL_AE_MODE, ACAMERA_CONTROL_AF_MODE, or
     * ACAMERA_CONTROL_AWB_MODE)</p>
     * <p>Additional capabilities may optionally be supported by a <code>LIMITED</code>-level device, and
     * can be checked for in ACAMERA_REQUEST_AVAILABLE_CAPABILITIES.</p>
     *
     * @see ACAMERA_CONTROL_AE_MODE
     * @see ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
     * @see ACAMERA_CONTROL_AF_MODE
     * @see ACAMERA_CONTROL_AWB_MODE
     * @see ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
     */
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED                    = 0,

    /*
     * <p>This camera device is capable of supporting advanced imaging applications.</p>
     * <p>The stream configurations listed in the <code>FULL</code>, <code>LEGACY</code> and <code>LIMITED</code> tables in the
     * {@link android.hardware.camera2.CameraDevice#createCaptureSession
     * createCaptureSession} documentation are guaranteed to be supported.</p>
     * <p>A <code>FULL</code> device will support below capabilities:</p>
     * <ul>
     * <li><code>BURST_CAPTURE</code> capability (ACAMERA_REQUEST_AVAILABLE_CAPABILITIES contains
     *   <code>BURST_CAPTURE</code>)</li>
     * <li>Per frame control (ACAMERA_SYNC_MAX_LATENCY <code>==</code> PER_FRAME_CONTROL)</li>
     * <li>Manual sensor control (ACAMERA_REQUEST_AVAILABLE_CAPABILITIES contains <code>MANUAL_SENSOR</code>)</li>
     * <li>Manual post-processing control (ACAMERA_REQUEST_AVAILABLE_CAPABILITIES contains
     *   <code>MANUAL_POST_PROCESSING</code>)</li>
     * <li>The required exposure time range defined in ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE</li>
     * <li>The required maxFrameDuration defined in ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION</li>
     * </ul>
     * <p>Note:
     * Pre-API level 23, FULL devices also supported arbitrary cropping region
     * (ACAMERA_SCALER_CROPPING_TYPE <code>== FREEFORM</code>); this requirement was relaxed in API level
     * 23, and <code>FULL</code> devices may only support <code>CENTERED</code> cropping.</p>
     *
     * @see ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
     * @see ACAMERA_SCALER_CROPPING_TYPE
     * @see ACAMERA_SENSOR_INFO_EXPOSURE_TIME_RANGE
     * @see ACAMERA_SENSOR_INFO_MAX_FRAME_DURATION
     * @see ACAMERA_SYNC_MAX_LATENCY
     */
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_FULL                       = 1,

    /*
     * <p>This camera device is running in backward compatibility mode.</p>
     * <p>Only the stream configurations listed in the <code>LEGACY</code> table in the {@link
     * android.hardware.camera2.CameraDevice#createCaptureSession createCaptureSession}
     * documentation are supported.</p>
     * <p>A <code>LEGACY</code> device does not support per-frame control, manual sensor control, manual
     * post-processing, arbitrary cropping regions, and has relaxed performance constraints.
     * No additional capabilities beyond <code>BACKWARD_COMPATIBLE</code> will ever be listed by a
     * <code>LEGACY</code> device in ACAMERA_REQUEST_AVAILABLE_CAPABILITIES.</p>
     * <p>In addition, the ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER is not functional on <code>LEGACY</code>
     * devices. Instead, every request that includes a JPEG-format output target is treated
     * as triggering a still capture, internally executing a precapture trigger.  This may
     * fire the flash for flash power metering during precapture, and then fire the flash
     * for the final capture, if a flash is available on the device and the AE mode is set to
     * enable the flash.</p>
     *
     * @see ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER
     * @see ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
     */
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY                     = 2,

    /*
     * <p>This camera device is capable of YUV reprocessing and RAW data capture, in addition to
     * FULL-level capabilities.</p>
     * <p>The stream configurations listed in the <code>LEVEL_3</code>, <code>RAW</code>, <code>FULL</code>, <code>LEGACY</code> and
     * <code>LIMITED</code> tables in the {@link
     * android.hardware.camera2.CameraDevice#createCaptureSession createCaptureSession}
     * documentation are guaranteed to be supported.</p>
     * <p>The following additional capabilities are guaranteed to be supported:</p>
     * <ul>
     * <li><code>YUV_REPROCESSING</code> capability (ACAMERA_REQUEST_AVAILABLE_CAPABILITIES contains
     *   <code>YUV_REPROCESSING</code>)</li>
     * <li><code>RAW</code> capability (ACAMERA_REQUEST_AVAILABLE_CAPABILITIES contains
     *   <code>RAW</code>)</li>
     * </ul>
     *
     * @see ACAMERA_REQUEST_AVAILABLE_CAPABILITIES
     */
    ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL_3                          = 3,

} acamera_metadata_enum_android_info_supported_hardware_level_t;


// ACAMERA_BLACK_LEVEL_LOCK
typedef enum acamera_metadata_enum_acamera_black_level_lock {
    ACAMERA_BLACK_LEVEL_LOCK_OFF                                     = 0,

    ACAMERA_BLACK_LEVEL_LOCK_ON                                      = 1,

} acamera_metadata_enum_android_black_level_lock_t;


// ACAMERA_SYNC_FRAME_NUMBER
typedef enum acamera_metadata_enum_acamera_sync_frame_number {
    /*
     * <p>The current result is not yet fully synchronized to any request.</p>
     * <p>Synchronization is in progress, and reading metadata from this
     * result may include a mix of data that have taken effect since the
     * last synchronization time.</p>
     * <p>In some future result, within ACAMERA_SYNC_MAX_LATENCY frames,
     * this value will update to the actual frame number frame number
     * the result is guaranteed to be synchronized to (as long as the
     * request settings remain constant).</p>
     *
     * @see ACAMERA_SYNC_MAX_LATENCY
     */
    ACAMERA_SYNC_FRAME_NUMBER_CONVERGING                             = -1,

    /*
     * <p>The current result's synchronization status is unknown.</p>
     * <p>The result may have already converged, or it may be in
     * progress.  Reading from this result may include some mix
     * of settings from past requests.</p>
     * <p>After a settings change, the new settings will eventually all
     * take effect for the output buffers and results. However, this
     * value will not change when that happens. Altering settings
     * rapidly may provide outcomes using mixes of settings from recent
     * requests.</p>
     * <p>This value is intended primarily for backwards compatibility with
     * the older camera implementations (for android.hardware.Camera).</p>
     */
    ACAMERA_SYNC_FRAME_NUMBER_UNKNOWN                                = -2,

} acamera_metadata_enum_android_sync_frame_number_t;

// ACAMERA_SYNC_MAX_LATENCY
typedef enum acamera_metadata_enum_acamera_sync_max_latency {
    /*
     * <p>Every frame has the requests immediately applied.</p>
     * <p>Changing controls over multiple requests one after another will
     * produce results that have those controls applied atomically
     * each frame.</p>
     * <p>All FULL capability devices will have this as their maxLatency.</p>
     */
    ACAMERA_SYNC_MAX_LATENCY_PER_FRAME_CONTROL                       = 0,

    /*
     * <p>Each new frame has some subset (potentially the entire set)
     * of the past requests applied to the camera settings.</p>
     * <p>By submitting a series of identical requests, the camera device
     * will eventually have the camera settings applied, but it is
     * unknown when that exact point will be.</p>
     * <p>All LEGACY capability devices will have this as their maxLatency.</p>
     */
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
