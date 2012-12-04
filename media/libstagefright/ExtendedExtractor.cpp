/*Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "ExtendedExtractor"
#include <utils/Log.h>
#include <dlfcn.h>  // for dlopen/dlclose

#include "include/ExtendedExtractor.h"

static const char* EXTENDED_PARSER_LIB = "libExtendedExtractor.so";

namespace android {

void* ExtendedParserLib() {
    static void* extendedParserLib = NULL;
    static bool alreadyTriedToOpenParsers = false;

    if(!alreadyTriedToOpenParsers) {
        alreadyTriedToOpenParsers = true;

        extendedParserLib = ::dlopen(EXTENDED_PARSER_LIB, RTLD_LAZY);

        if(extendedParserLib == NULL) {
            ALOGV("Failed to open EXTENDED_PARSER_LIB, dlerror = %s \n", dlerror());
        }
    }

    return extendedParserLib;
}

MediaExtractor* ExtendedExtractor::CreateExtractor(const sp<DataSource> &source, const char *mime) {
    static MediaExtractorFactory mediaFactoryFunction = NULL;
    static bool alreadyTriedToFindFactoryFunction = false;

    MediaExtractor* extractor = NULL;

    if(!alreadyTriedToFindFactoryFunction) {

        void *extendedParserLib = ExtendedParserLib();
        if (extendedParserLib != NULL) {

            mediaFactoryFunction = (MediaExtractorFactory) dlsym(extendedParserLib, MEDIA_CREATE_EXTRACTOR);
            alreadyTriedToFindFactoryFunction = true;
        }
    }

    if(mediaFactoryFunction==NULL) {
        ALOGE(" dlsym for ExtendedExtractor factory function failed, dlerror = %s \n", dlerror());
        return NULL;
    }

    extractor = mediaFactoryFunction(source, mime);
    if(extractor==NULL) {
        ALOGE(" ExtendedExtractor failed to instantiate extractor \n");
    }

    return extractor;
}

bool SniffExtendedExtractor(const sp<DataSource> &source, String8 *mimeType,
                            float *confidence,sp<AMessage> *meta) {
    void *extendedParserLib = ExtendedParserLib();
    bool retVal = false;
    if (extendedParserLib != NULL) {
       ExtendedExtractorSniffers extendedExtractorSniffers=
           (ExtendedExtractorSniffers) dlsym(extendedParserLib, EXTENDED_EXTRACTOR_SNIFFERS);

       if(extendedExtractorSniffers == NULL) {
           ALOGE(" dlsym for extendedExtractorSniffers function failed, dlerror = %s \n", dlerror());
           return retVal;
       }

       retVal = extendedExtractorSniffers(source, mimeType, confidence, meta);

       if(!retVal) {
           ALOGV("ExtendedExtractor:: ExtendedExtractorSniffers  Failed");
       }
    }
    return retVal;
}

}  // namespace android


