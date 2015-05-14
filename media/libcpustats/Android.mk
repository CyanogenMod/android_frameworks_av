LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES :=     \
        CentralTendencyStatistics.cpp \
        ThreadCpuUsage.cpp

LOCAL_MODULE := libcpustats

LOCAL_CFLAGS := -std=gnu++11 -Werror

include $(BUILD_STATIC_LIBRARY)
