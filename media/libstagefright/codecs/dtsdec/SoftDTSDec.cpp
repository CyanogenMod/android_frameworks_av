/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "SoftDTSDec"
#define LOG_NDEBUG 0
#include <utils/Log.h>
#include <dlfcn.h>

#include "SoftDTSDec.h"

#include <cutils/properties.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaErrors.h>

#include "OMX_Core.h"
#include "OMX_Audio_DTS.h"

#define DTS_ALOGV
//#define DTS_ALOGV ALOGV

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}


SoftDTSDec::SoftDTSDec( const char *name,
                        const OMX_CALLBACKTYPE *callbacks,
                        OMX_PTR appData,
                        OMX_COMPONENTTYPE **component )
  : SimpleSoftOMXComponent(name, callbacks, appData, component),
    mComponentHandle(NULL)
{
    DTS_ALOGV("+SoftDTSDec() ctor : name = '%s'  this = 0x%x", name, this);

    OMX_Init();

    OMX_ERRORTYPE retVal = OMX_GetHandle( &mComponentHandle,
                                          const_cast<char *>(name),
                                          appData,
                                          const_cast<OMX_CALLBACKTYPE *>(callbacks) );
}


SoftDTSDec::~SoftDTSDec()
{
    DTS_ALOGV("+ ~SoftDTSDec() (dtor)");

    OMX_FreeHandle(mComponentHandle);
    OMX_Deinit();

    DTS_ALOGV("- ~SoftDTSDec() (dtor)");
}


OMX_ERRORTYPE SoftDTSDec::sendCommand(OMX_COMMANDTYPE cmd, OMX_U32 param, OMX_PTR data)
{
    OMX_ERRORTYPE retVal = OMX_SendCommand(mComponentHandle, cmd, param, data);
    DTS_ALOGV("sendCommand() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::getParameter(OMX_INDEXTYPE index, OMX_PTR params)
{
    OMX_ERRORTYPE retVal = OMX_GetParameter(mComponentHandle, index, params);
    DTS_ALOGV("getParameter() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::setParameter(OMX_INDEXTYPE index, const OMX_PTR params)
{
    OMX_ERRORTYPE retVal = OMX_SetParameter(mComponentHandle, index, params);
    DTS_ALOGV("setParameter() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::getConfig(OMX_INDEXTYPE index, OMX_PTR params)
{
    OMX_ERRORTYPE retVal = OMX_GetConfig(mComponentHandle, index, params);
    DTS_ALOGV("getConfig() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::setConfig(OMX_INDEXTYPE index, const OMX_PTR params)
{
    OMX_ERRORTYPE retVal = OMX_SetConfig(mComponentHandle, index, params);
    DTS_ALOGV("setConfig() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::getExtensionIndex(const char *name, OMX_INDEXTYPE *index)
{
    OMX_ERRORTYPE retVal = OMX_GetExtensionIndex(mComponentHandle, (OMX_STRING)name, index);
    DTS_ALOGV("getExtensionIndex() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::useBuffer(OMX_BUFFERHEADERTYPE **buffer,
                                    OMX_U32 portIndex,
                                    OMX_PTR appPrivate,
                                    OMX_U32 size,
                                    OMX_U8 *ptr)
{
    OMX_ERRORTYPE retVal = OMX_UseBuffer(mComponentHandle, buffer, portIndex, appPrivate, size, ptr);
    DTS_ALOGV("useBuffer() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::allocateBuffer(OMX_BUFFERHEADERTYPE **header,
                                         OMX_U32 portIndex,
                                         OMX_PTR appPrivate,
                                         OMX_U32 size)
{
    OMX_ERRORTYPE retVal = OMX_AllocateBuffer(mComponentHandle, header, portIndex, appPrivate, size);
    DTS_ALOGV("allocateBuffer() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::freeBuffer(OMX_U32 portIndex, OMX_BUFFERHEADERTYPE *header)
{
    OMX_ERRORTYPE retVal = OMX_FreeBuffer(mComponentHandle, portIndex, header);
    DTS_ALOGV("freeBuffer() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::emptyThisBuffer(OMX_BUFFERHEADERTYPE *buffer)
{
    OMX_ERRORTYPE retVal = OMX_EmptyThisBuffer(mComponentHandle, buffer);
    DTS_ALOGV("emptyThisBuffer() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::fillThisBuffer(OMX_BUFFERHEADERTYPE *buffer)
{
    OMX_ERRORTYPE retVal = OMX_FillThisBuffer(mComponentHandle, buffer);
    DTS_ALOGV("fillThisBuffer() returns 0x%x", retVal);
    return retVal;
}

OMX_ERRORTYPE SoftDTSDec::getState(OMX_STATETYPE *state)
{
    OMX_ERRORTYPE retVal = OMX_GetState(mComponentHandle, state);
    DTS_ALOGV("getState() returns 0x%x", retVal);
    return retVal;
}

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    ALOGV("createSoftOMXComponent called for SoftDTSDec");

    android::SoftDTSDec * dtsDec = new android::SoftDTSDec(name, callbacks, appData, component);

    return dtsDec;
}
