LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_ARCH), $(filter $(TARGET_ARCH), arm arm64 x86 x86_64))
include $(CLEAR_VARS)
LOCAL_MODULE := mediaextractor-seccomp.policy
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT)/etc/seccomp_policy

# mediaextractor runs in 32-bit combatibility mode. For 64 bit architectures,
# use the 32 bit policy
ifdef TARGET_2ND_ARCH
    LOCAL_SRC_FILES := $(LOCAL_PATH)/seccomp_policy/mediaextractor-seccomp-$(TARGET_2ND_ARCH).policy
else
    LOCAL_SRC_FILES := $(LOCAL_PATH)/seccomp_policy/mediaextractor-seccomp-$(TARGET_ARCH).policy
endif

# allow device specific additions to the syscall whitelist
ifneq (,$(wildcard $(BOARD_SECCOMP_POLICY)/mediaextractor-seccomp.policy))
    LOCAL_SRC_FILES += $(BOARD_SECCOMP_POLICY)/mediaextractor-seccomp.policy
endif

include $(BUILD_SYSTEM)/base_rules.mk

$(LOCAL_BUILT_MODULE): $(LOCAL_SRC_FILES)
	@mkdir -p $(dir $@)
	$(hide) cat > $@ $^

endif
