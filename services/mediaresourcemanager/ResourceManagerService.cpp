/*
**
** Copyright 2015, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

//#define LOG_NDEBUG 0
#define LOG_TAG "ResourceManagerService"
#include <utils/Log.h>

#include <binder/IServiceManager.h>
#include <dirent.h>
#include <media/stagefright/ProcessInfo.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "ResourceManagerService.h"
#include "ServiceLog.h"

namespace android {

template <typename T>
static String8 getString(const Vector<T> &items) {
    String8 itemsStr;
    for (size_t i = 0; i < items.size(); ++i) {
        itemsStr.appendFormat("%s ", items[i].toString().string());
    }
    return itemsStr;
}

static bool hasResourceType(String8 type, Vector<MediaResource> resources) {
    for (size_t i = 0; i < resources.size(); ++i) {
        if (resources[i].mType == type) {
            return true;
        }
    }
    return false;
}

static bool hasResourceType(String8 type, ResourceInfos infos) {
    for (size_t i = 0; i < infos.size(); ++i) {
        if (hasResourceType(type, infos[i].resources)) {
            return true;
        }
    }
    return false;
}

static ResourceInfos& getResourceInfosForEdit(
        int pid,
        PidResourceInfosMap& map) {
    ssize_t index = map.indexOfKey(pid);
    if (index < 0) {
        // new pid
        ResourceInfos infosForPid;
        map.add(pid, infosForPid);
    }

    return map.editValueFor(pid);
}

static ResourceInfo& getResourceInfoForEdit(
        int64_t clientId,
        const sp<IResourceManagerClient> client,
        ResourceInfos& infos) {
    for (size_t i = 0; i < infos.size(); ++i) {
        if (infos[i].clientId == clientId) {
            return infos.editItemAt(i);
        }
    }
    ResourceInfo info;
    info.clientId = clientId;
    info.client = client;
    infos.push_back(info);
    return infos.editItemAt(infos.size() - 1);
}

status_t ResourceManagerService::dump(int fd, const Vector<String16>& /* args */) {
    String8 result;

    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        result.format("Permission Denial: "
                "can't dump ResourceManagerService from pid=%d, uid=%d\n",
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid());
        write(fd, result.string(), result.size());
        return PERMISSION_DENIED;
    }

    PidResourceInfosMap mapCopy;
    bool supportsMultipleSecureCodecs;
    bool supportsSecureWithNonSecureCodec;
    String8 serviceLog;
    {
        Mutex::Autolock lock(mLock);
        mapCopy = mMap;  // Shadow copy, real copy will happen on write.
        supportsMultipleSecureCodecs = mSupportsMultipleSecureCodecs;
        supportsSecureWithNonSecureCodec = mSupportsSecureWithNonSecureCodec;
        serviceLog = mServiceLog->toString("    " /* linePrefix */);
    }

    const size_t SIZE = 256;
    char buffer[SIZE];
    snprintf(buffer, SIZE, "ResourceManagerService: %p\n", this);
    result.append(buffer);
    result.append("  Policies:\n");
    snprintf(buffer, SIZE, "    SupportsMultipleSecureCodecs: %d\n", supportsMultipleSecureCodecs);
    result.append(buffer);
    snprintf(buffer, SIZE, "    SupportsSecureWithNonSecureCodec: %d\n",
            supportsSecureWithNonSecureCodec);
    result.append(buffer);

    result.append("  Processes:\n");
    for (size_t i = 0; i < mapCopy.size(); ++i) {
        snprintf(buffer, SIZE, "    Pid: %d\n", mapCopy.keyAt(i));
        result.append(buffer);

        const ResourceInfos &infos = mapCopy.valueAt(i);
        for (size_t j = 0; j < infos.size(); ++j) {
            result.append("      Client:\n");
            snprintf(buffer, SIZE, "        Id: %lld\n", (long long)infos[j].clientId);
            result.append(buffer);

            snprintf(buffer, SIZE, "        Name: %s\n", infos[j].client->getName().string());
            result.append(buffer);

            Vector<MediaResource> resources = infos[j].resources;
            result.append("        Resources:\n");
            for (size_t k = 0; k < resources.size(); ++k) {
                snprintf(buffer, SIZE, "          %s\n", resources[k].toString().string());
                result.append(buffer);
            }
        }
    }
    result.append("  Events logs (most recent at top):\n");
    result.append(serviceLog);

    write(fd, result.string(), result.size());
    return OK;
}

ResourceManagerService::ResourceManagerService()
    : mProcessInfo(new ProcessInfo()),
      mServiceLog(new ServiceLog()),
      mSupportsMultipleSecureCodecs(true),
      mSupportsSecureWithNonSecureCodec(true) {}

ResourceManagerService::ResourceManagerService(sp<ProcessInfoInterface> processInfo)
    : mProcessInfo(processInfo),
      mServiceLog(new ServiceLog()),
      mSupportsMultipleSecureCodecs(true),
      mSupportsSecureWithNonSecureCodec(true) {}

ResourceManagerService::~ResourceManagerService() {}

void ResourceManagerService::config(const Vector<MediaResourcePolicy> &policies) {
    String8 log = String8::format("config(%s)", getString(policies).string());
    mServiceLog->add(log);

    Mutex::Autolock lock(mLock);
    for (size_t i = 0; i < policies.size(); ++i) {
        String8 type = policies[i].mType;
        String8 value = policies[i].mValue;
        if (type == kPolicySupportsMultipleSecureCodecs) {
            mSupportsMultipleSecureCodecs = (value == "true");
        } else if (type == kPolicySupportsSecureWithNonSecureCodec) {
            mSupportsSecureWithNonSecureCodec = (value == "true");
        }
    }
}

void ResourceManagerService::addResource(
        int pid,
        int64_t clientId,
        const sp<IResourceManagerClient> client,
        const Vector<MediaResource> &resources) {
    String8 log = String8::format("addResource(pid %d, clientId %lld, resources %s)",
            pid, (long long) clientId, getString(resources).string());
    mServiceLog->add(log);

    Mutex::Autolock lock(mLock);
    ResourceInfos& infos = getResourceInfosForEdit(pid, mMap);
    ResourceInfo& info = getResourceInfoForEdit(clientId, client, infos);
    // TODO: do the merge instead of append.
    info.resources.appendVector(resources);
}

void ResourceManagerService::removeResource(int pid, int64_t clientId) {
    String8 log = String8::format(
            "removeResource(pid %d, clientId %lld)",
            pid, (long long) clientId);
    mServiceLog->add(log);

    Mutex::Autolock lock(mLock);
    ssize_t index = mMap.indexOfKey(pid);
    if (index < 0) {
        ALOGV("removeResource: didn't find pid %d for clientId %lld", pid, (long long) clientId);
        return;
    }
    bool found = false;
    ResourceInfos &infos = mMap.editValueAt(index);
    for (size_t j = 0; j < infos.size(); ++j) {
        if (infos[j].clientId == clientId) {
            j = infos.removeAt(j);
            found = true;
            break;
        }
    }
    if (!found) {
        ALOGV("didn't find client");
    }
}

void ResourceManagerService::getClientForResource_l(
        int callingPid, const MediaResource *res, Vector<sp<IResourceManagerClient>> *clients) {
    if (res == NULL) {
        return;
    }
    sp<IResourceManagerClient> client;
    if (getLowestPriorityBiggestClient_l(callingPid, res->mType, &client)) {
        clients->push_back(client);
    }
}

bool ResourceManagerService::reclaimResource(
        int callingPid, const Vector<MediaResource> &resources) {
    String8 log = String8::format("reclaimResource(callingPid %d, resources %s)",
            callingPid, getString(resources).string());
    mServiceLog->add(log);

    Vector<sp<IResourceManagerClient>> clients;
    {
        Mutex::Autolock lock(mLock);
        const MediaResource *secureCodec = NULL;
        const MediaResource *nonSecureCodec = NULL;
        const MediaResource *graphicMemory = NULL;
        for (size_t i = 0; i < resources.size(); ++i) {
            String8 type = resources[i].mType;
            if (resources[i].mType == kResourceSecureCodec) {
                secureCodec = &resources[i];
            } else if (type == kResourceNonSecureCodec) {
                nonSecureCodec = &resources[i];
            } else if (type == kResourceGraphicMemory) {
                graphicMemory = &resources[i];
            }
        }

        // first pass to handle secure/non-secure codec conflict
        if (secureCodec != NULL) {
            if (!mSupportsMultipleSecureCodecs) {
                if (!getAllClients_l(callingPid, String8(kResourceSecureCodec), &clients)) {
                    return false;
                }
            }
            if (!mSupportsSecureWithNonSecureCodec) {
                if (!getAllClients_l(callingPid, String8(kResourceNonSecureCodec), &clients)) {
                    return false;
                }
            }
        }
        if (nonSecureCodec != NULL) {
            if (!mSupportsSecureWithNonSecureCodec) {
                if (!getAllClients_l(callingPid, String8(kResourceSecureCodec), &clients)) {
                    return false;
                }
            }
        }

        if (clients.size() == 0) {
            // if no secure/non-secure codec conflict, run second pass to handle other resources.
            getClientForResource_l(callingPid, graphicMemory, &clients);
        }

        if (clients.size() == 0) {
            // if we are here, run the third pass to free one codec with the same type.
            getClientForResource_l(callingPid, secureCodec, &clients);
            getClientForResource_l(callingPid, nonSecureCodec, &clients);
        }

        if (clients.size() == 0) {
            // if we are here, run the fourth pass to free one codec with the different type.
            if (secureCodec != NULL) {
                MediaResource temp(String8(kResourceNonSecureCodec), 1);
                getClientForResource_l(callingPid, &temp, &clients);
            }
            if (nonSecureCodec != NULL) {
                MediaResource temp(String8(kResourceSecureCodec), 1);
                getClientForResource_l(callingPid, &temp, &clients);
            }
        }
    }

    if (clients.size() == 0) {
        return false;
    }

    sp<IResourceManagerClient> failedClient;
    for (size_t i = 0; i < clients.size(); ++i) {
        log = String8::format("reclaimResource from client %p", clients[i].get());
        mServiceLog->add(log);
        if (!clients[i]->reclaimResource()) {
            failedClient = clients[i];
            break;
        }
    }

    if (failedClient == NULL) {
        return true;
    }

    {
        Mutex::Autolock lock(mLock);
        bool found = false;
        for (size_t i = 0; i < mMap.size(); ++i) {
            ResourceInfos &infos = mMap.editValueAt(i);
            for (size_t j = 0; j < infos.size();) {
                if (infos[j].client == failedClient) {
                    j = infos.removeAt(j);
                    found = true;
                } else {
                    ++j;
                }
            }
            if (found) {
                break;
            }
        }
        if (!found) {
            ALOGV("didn't find failed client");
        }
    }

    return false;
}

bool ResourceManagerService::getAllClients_l(
        int callingPid, const String8 &type, Vector<sp<IResourceManagerClient>> *clients) {
    Vector<sp<IResourceManagerClient>> temp;
    for (size_t i = 0; i < mMap.size(); ++i) {
        ResourceInfos &infos = mMap.editValueAt(i);
        for (size_t j = 0; j < infos.size(); ++j) {
            if (hasResourceType(type, infos[j].resources)) {
                if (!isCallingPriorityHigher_l(callingPid, mMap.keyAt(i))) {
                    // some higher/equal priority process owns the resource,
                    // this request can't be fulfilled.
                    ALOGE("getAllClients_l: can't reclaim resource %s from pid %d",
                            type.string(), mMap.keyAt(i));
                    return false;
                }
                temp.push_back(infos[j].client);
            }
        }
    }
    if (temp.size() == 0) {
        ALOGV("getAllClients_l: didn't find any resource %s", type.string());
        return true;
    }
    clients->appendVector(temp);
    return true;
}

bool ResourceManagerService::getLowestPriorityBiggestClient_l(
        int callingPid, const String8 &type, sp<IResourceManagerClient> *client) {
    int lowestPriorityPid;
    int lowestPriority;
    int callingPriority;
    if (!mProcessInfo->getPriority(callingPid, &callingPriority)) {
        ALOGE("getLowestPriorityBiggestClient_l: can't get process priority for pid %d",
                callingPid);
        return false;
    }
    if (!getLowestPriorityPid_l(type, &lowestPriorityPid, &lowestPriority)) {
        return false;
    }
    if (lowestPriority <= callingPriority) {
        ALOGE("getLowestPriorityBiggestClient_l: lowest priority %d vs caller priority %d",
                lowestPriority, callingPriority);
        return false;
    }

    if (!getBiggestClient_l(lowestPriorityPid, type, client)) {
        return false;
    }
    return true;
}

bool ResourceManagerService::getLowestPriorityPid_l(
        const String8 &type, int *lowestPriorityPid, int *lowestPriority) {
    int pid = -1;
    int priority = -1;
    for (size_t i = 0; i < mMap.size(); ++i) {
        if (mMap.valueAt(i).size() == 0) {
            // no client on this process.
            continue;
        }
        if (!hasResourceType(type, mMap.valueAt(i))) {
            // doesn't have the requested resource type
            continue;
        }
        int tempPid = mMap.keyAt(i);
        int tempPriority;
        if (!mProcessInfo->getPriority(tempPid, &tempPriority)) {
            ALOGV("getLowestPriorityPid_l: can't get priority of pid %d, skipped", tempPid);
            // TODO: remove this pid from mMap?
            continue;
        }
        if (pid == -1 || tempPriority > priority) {
            // initial the value
            pid = tempPid;
            priority = tempPriority;
        }
    }
    if (pid != -1) {
        *lowestPriorityPid = pid;
        *lowestPriority = priority;
    }
    return (pid != -1);
}

bool ResourceManagerService::isCallingPriorityHigher_l(int callingPid, int pid) {
    int callingPidPriority;
    if (!mProcessInfo->getPriority(callingPid, &callingPidPriority)) {
        return false;
    }

    int priority;
    if (!mProcessInfo->getPriority(pid, &priority)) {
        return false;
    }

    return (callingPidPriority < priority);
}

bool ResourceManagerService::getBiggestClient_l(
        int pid, const String8 &type, sp<IResourceManagerClient> *client) {
    ssize_t index = mMap.indexOfKey(pid);
    if (index < 0) {
        ALOGE("getBiggestClient_l: can't find resource info for pid %d", pid);
        return false;
    }

    sp<IResourceManagerClient> clientTemp;
    uint64_t largestValue = 0;
    const ResourceInfos &infos = mMap.valueAt(index);
    for (size_t i = 0; i < infos.size(); ++i) {
        Vector<MediaResource> resources = infos[i].resources;
        for (size_t j = 0; j < resources.size(); ++j) {
            if (resources[j].mType == type) {
                if (resources[j].mValue > largestValue) {
                    largestValue = resources[j].mValue;
                    clientTemp = infos[i].client;
                }
            }
        }
    }

    if (clientTemp == NULL) {
        ALOGE("getBiggestClient_l: can't find resource type %s for pid %d", type.string(), pid);
        return false;
    }

    *client = clientTemp;
    return true;
}

} // namespace android
