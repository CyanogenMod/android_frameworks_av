#!/bin/bash

if [ -z "$ANDROID_BUILD_TOP" ]; then
    echo "Android build environment not set"
    exit -1
fi

# ensure we have mm
. $ANDROID_BUILD_TOP/build/envsetup.sh

pushd $ANDROID_BUILD_TOP/frameworks/av/services/audioflinger/
pwd
mm

echo "waiting for device"
adb root && adb wait-for-device remount
adb push $OUT/system/lib/libaudioresampler.so /system/lib
adb push $OUT/system/bin/resampler_tests /system/bin

sh $ANDROID_BUILD_TOP/frameworks/av/services/audioflinger/tests/run_all_unit_tests.sh

popd
