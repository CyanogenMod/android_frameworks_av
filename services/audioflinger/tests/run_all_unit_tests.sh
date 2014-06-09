#!/bin/bash

if [ -z "$ANDROID_BUILD_TOP" ]; then
    echo "Android build environment not set"
    exit -1
fi

echo "waiting for device"
adb root && adb wait-for-device remount

adb shell /system/bin/resampler_tests
