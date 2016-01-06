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
ifeq ($(TARGET_ARCH), $(filter $(TARGET_ARCH), arm arm64))
LOCAL_ADDITIONAL_DEPENDENCIES += mediaextractor-seccomp.policy
endif
LOCAL_SRC_FILES := main_extractorservice.cpp minijail/minijail.cpp
LOCAL_SHARED_LIBRARIES := libmedia libmediaextractorservice libbinder libutils liblog libicuuc libminijail
LOCAL_STATIC_LIBRARIES := libicuandroid_utils
LOCAL_MODULE:= mediaextractor
LOCAL_32_BIT_ONLY := true
LOCAL_INIT_RC := mediaextractor.rc
include $(BUILD_EXECUTABLE)

include $(call all-makefiles-under, $(LOCAL_PATH))
