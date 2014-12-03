/*Copyright (C) 2014 The Android Open Source Project
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
* This file was modified by DTS, Inc. The portions of the
* code modified by DTS, Inc are copyrighted and
* licensed separately, as follows:
*
*  (C) 2014 DTS, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*    http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#define LOG_TAG "AudioUtil"
//#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <stdlib.h>

#include <cutils/properties.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sound/devdep_params.h>
#include <sound/asound.h>
#include "AudioUtil.h"

#define ROUTE_PATH    "/data/data/dts/route"
#define DEVICE_NODE   "/dev/snd/hwC0D3"

static int32_t mDevices = 0;
static int32_t mCurrDevice = 0;

void create_route_node(void)
{
    char prop[PROPERTY_VALUE_MAX] = "true";
    int fd;
    property_get("use.dts_eagle", prop, "0");
    if (!strncmp("true", prop, sizeof("true")) || atoi(prop)) {
        ALOGV("create_route_node");
        if ((fd=open(ROUTE_PATH, O_RDONLY)) < 0) {
            ALOGV("No File exisit");
        } else {
            ALOGV("A file with the same name exist. Remove it before creating it");
            close(fd);
            remove(ROUTE_PATH);
        }
        if ((fd=creat(ROUTE_PATH, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)) < 0) {
            ALOGE("opening route node failed returned");
            return;
        }
        chmod(ROUTE_PATH, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH);
        ALOGV("opening route  node successful");
        close(fd);
    }
}

void notify_route_node(int active_device, int devices)
{
    char prop[PROPERTY_VALUE_MAX] = "true";
    char buf[1024];
    int fd;
    if ((mCurrDevice == active_device) &&
        (mDevices == devices)) {
        ALOGV("nothing to update to route node");
        return;
    }
    mDevices = devices;
    mCurrDevice = active_device;
    property_get("use.dts_eagle", prop, "0");
    if (!strncmp("true", prop, sizeof("true")) || atoi(prop)) {
        ALOGV("notify active device : %d all_devices : %d", active_device, devices);
        if ((fd=open(ROUTE_PATH, O_TRUNC|O_WRONLY)) < 0) {
            ALOGV("Write device to route node failed");
        } else {
            ALOGV("Write device to route node successful");
            snprintf(buf, sizeof(buf), "device=%d;all_devices=%d", active_device, devices);
            int n = write(fd, buf, strlen(buf));
            ALOGV("number of bytes written: %d", n);
            close(fd);
        }
        int eaglefd = open(DEVICE_NODE, O_RDWR);
        int32_t params[2] = {active_device, 1 /*is primary device*/};
        if (eaglefd > 0) {
            if(ioctl(eaglefd, DTS_EAGLE_IOCTL_SET_ACTIVE_DEVICE, &params) < 0) {
                ALOGE("DTS_EAGLE (%s): error sending primary device\n", __func__);
            }
            ALOGD("DTS_EAGLE (%s): sent primary device\n", __func__);
            close(eaglefd);
        } else {
            ALOGE("DTS_EAGLE (%s): error opening eagle\n", __func__);
        }
    }
}

void remove_route_node(void)
{
    char prop[PROPERTY_VALUE_MAX] = "true";
    int fd;
    property_get("use.dts_eagle", prop, "0");
    if (!strncmp("true", prop, sizeof("true")) || atoi(prop)) {
        ALOGV("remove_route_node");
        if ((fd=open(ROUTE_PATH, O_RDONLY)) < 0) {
            ALOGV("open route  node failed");
        } else {
            ALOGV("open route node successful");
            ALOGV("Remove the file");
            close(fd);
            remove(ROUTE_PATH);
        }
    }
}
