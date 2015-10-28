LOCAL_PATH := $(call my-dir)

# service library
include $(CLEAR_VARS)
LOCAL_SRC_FILES := MediaExtractorService.cpp
LOCAL_SHARED_LIBRARIES := libmedia libstagefright libbinder libutils liblog
LOCAL_MODULE:= libmediaextractorservice
LOCAL_32_BIT_ONLY := true
include $(BUILD_SHARED_LIBRARY)


# service executable
include $(CLEAR_VARS)
LOCAL_SRC_FILES := main_extractorservice.cpp
LOCAL_SHARED_LIBRARIES := libmedia libmediaextractorservice libbinder libutils liblog libicuuc
LOCAL_STATIC_LIBRARIES := libicuandroid_utils
LOCAL_MODULE:= mediaextractor
LOCAL_32_BIT_ONLY := true
LOCAL_INIT_RC := mediaextractor.rc
include $(BUILD_EXECUTABLE)


