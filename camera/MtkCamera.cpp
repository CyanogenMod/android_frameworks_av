/* Copyright Statement:
 *
 * This software/firmware and related documentation ("MediaTek Software") are
 * protected under relevant copyright laws. The information contained herein is
 * confidential and proprietary to MediaTek Inc. and/or its licensors. Without
 * the prior written permission of MediaTek inc. and/or its licensors, any
 * reproduction, modification, use or disclosure of MediaTek Software, and
 * information contained herein, in whole or in part, shall be strictly
 * prohibited.
 * 
 * MediaTek Inc. (C) 2010. All rights reserved.
 * 
 * BY OPENING THIS FILE, RECEIVER HEREBY UNEQUIVOCALLY ACKNOWLEDGES AND AGREES
 * THAT THE SOFTWARE/FIRMWARE AND ITS DOCUMENTATIONS ("MEDIATEK SOFTWARE")
 * RECEIVED FROM MEDIATEK AND/OR ITS REPRESENTATIVES ARE PROVIDED TO RECEIVER
 * ON AN "AS-IS" BASIS ONLY. MEDIATEK EXPRESSLY DISCLAIMS ANY AND ALL
 * WARRANTIES, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE OR
 * NONINFRINGEMENT. NEITHER DOES MEDIATEK PROVIDE ANY WARRANTY WHATSOEVER WITH
 * RESPECT TO THE SOFTWARE OF ANY THIRD PARTY WHICH MAY BE USED BY,
 * INCORPORATED IN, OR SUPPLIED WITH THE MEDIATEK SOFTWARE, AND RECEIVER AGREES
 * TO LOOK ONLY TO SUCH THIRD PARTY FOR ANY WARRANTY CLAIM RELATING THERETO.
 * RECEIVER EXPRESSLY ACKNOWLEDGES THAT IT IS RECEIVER'S SOLE RESPONSIBILITY TO
 * OBTAIN FROM ANY THIRD PARTY ALL PROPER LICENSES CONTAINED IN MEDIATEK
 * SOFTWARE. MEDIATEK SHALL ALSO NOT BE RESPONSIBLE FOR ANY MEDIATEK SOFTWARE
 * RELEASES MADE TO RECEIVER'S SPECIFICATION OR TO CONFORM TO A PARTICULAR
 * STANDARD OR OPEN FORUM. RECEIVER'S SOLE AND EXCLUSIVE REMEDY AND MEDIATEK'S
 * ENTIRE AND CUMULATIVE LIABILITY WITH RESPECT TO THE MEDIATEK SOFTWARE
 * RELEASED HEREUNDER WILL BE, AT MEDIATEK'S OPTION, TO REVISE OR REPLACE THE
 * MEDIATEK SOFTWARE AT ISSUE, OR REFUND ANY SOFTWARE LICENSE FEES OR SERVICE
 * CHARGE PAID BY RECEIVER TO MEDIATEK FOR SUCH MEDIATEK SOFTWARE AT ISSUE.
 *
 * The following software/firmware and/or related documentation ("MediaTek
 * Software") have been modified by MediaTek Inc. All revisions are subject to
 * any receiver's applicable license agreements with MediaTek Inc.
 */

/*
**
** Copyright 2008, The Android Open Source Project
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

#define LOG_TAG "MtkCamera"
#include <utils/Log.h>

#include <binder/MemoryBase.h>
#include <binder/MemoryHeapBase.h>
#include <camera/MtkCamera.h>

#define LOGD(fmt, arg...) ALOGD(fmt, ##arg)
#define LOGE(fmt, arg...) ALOGE(fmt, ##arg)
#define LOGW(fmt, arg...) ALOGW(fmt, ##arg)

namespace android {


MtkCamMsgExtDataHelper::
MtkCamMsgExtDataHelper()
    : mIsValid(false)
    , mspData(0)
    , mspHeap(0)
    , mDataOffset(0)
    , mDataSize(0)
{
    ::memset(&mExtDataHdr, 0, sizeof(mExtDataHdr));
}


MtkCamMsgExtDataHelper::
~MtkCamMsgExtDataHelper()
{
    uninit();
}


bool
MtkCamMsgExtDataHelper::
init(const sp<IMemory>& dataPtr)
{
    bool ret = false;
    //
    sp<IMemoryHeap> heap = 0;
    ssize_t         offset = 0;
    size_t          size = 0;
    //
    if  ( NULL == dataPtr.get() ) {
        LOGE("[MtkCamMsgExtDataHelper] dataPtr is NULL");
        goto lbExit;
    }
    //
    heap = dataPtr->getMemory(&offset, &size);
    if  ( NULL == heap.get() || NULL == heap->base() ) {
        LOGE("[MtkCamMsgExtDataHelper] heap or heap->base() is NULL - (heap,offset,size)=(%p,%ld,%d)", heap.get(), offset, size);
        goto lbExit;
    }
    //
    if ( sizeof(DataHeader) > size ) {
        LOGE("[MtkCamMsgExtDataHelper] sizeof(DataHeader)(%d) > size(%d)", sizeof(DataHeader), size);
        goto lbExit;
    }
    //
    ::memcpy(&mExtDataHdr, ((uint8_t*)heap->base()) + offset, sizeof(DataHeader));
    mspData = dataPtr;
    mspHeap = heap;
    mDataOffset = offset;
    mDataSize   = size;
    mIsValid= true;
    ret = true;
lbExit:
    return  ret;
}


bool
MtkCamMsgExtDataHelper::
uninit()
{
    mIsValid= false;
    mspData = NULL;
    mspHeap = NULL;
    mDataOffset = 0;
    mDataSize   = 0;
    ::memset(&mExtDataHdr, 0, sizeof(mExtDataHdr));
    return  true;
}


bool
MtkCamMsgExtDataHelper::
create(size_t const extParamSize, uint32_t const u4ExtMsgType)
{
    bool ret = false;
    //
    size_t const extDataSize = sizeof(DataHeader) + extParamSize;
    sp<IMemoryHeap> heap = 0;
    sp<IMemory> dataPtr = 0;

    //  (1) Check arguments.
    if  ( 0 == extParamSize )
    {
        LOGW("[MtkCamMsgExtDataHelper::create] extParamSize==0");
    }

    //  (2) Allocate memory
    heap = new MemoryHeapBase(extDataSize, 0, NULL);
    dataPtr = new MemoryBase(heap, 0, extDataSize);

    //  (3) Initialize.
    ret = init(dataPtr);
    if  ( ! ret )
    {
        LOGE("[MtkCamMsgExtDataHelper::create] init fail");
        goto lbExit;
    }

    //  (4) Assign the header.
    mExtDataHdr.extMsgType = u4ExtMsgType;
    ::memcpy(((uint8_t*)mspHeap->base()) + mDataOffset, &mExtDataHdr, sizeof(DataHeader));

    ret = true;
lbExit:
    return  ret;
}


bool
MtkCamMsgExtDataHelper::
destroy()
{
    return  uninit();
}


uint8_t*
MtkCamMsgExtDataHelper::
getExtParamBase() const
{
    return  mIsValid
        ?   static_cast<uint8_t*>(mspHeap->base()) + mDataOffset + sizeof(DataHeader)
        :   NULL;
}


size_t
MtkCamMsgExtDataHelper::
getExtParamSize() const
{
    return  mIsValid
        ?   (mDataSize - sizeof(DataHeader))
        :   0;
}


ssize_t
MtkCamMsgExtDataHelper::
getExtParamOffset() const
{
    return  mIsValid
        ?   (mDataOffset + sizeof(DataHeader))
        :   0;
}


}; // namespace android
