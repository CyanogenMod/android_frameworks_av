/*
 * Copyright (C) 2014 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in
 * the documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>

#include "avcenc_api.h"
#include "avcenc_int.h"

// Constants.
enum {
    kMaxWidth         = 720,
    kMaxHeight        = 480,
    kMaxFrameRate     = 30,
    kMaxBitrate       = 2048, // in kbps.
    kInputBufferSize  = (kMaxWidth * kMaxHeight * 3) / 2, // For YUV 420 format.
    kOutputBufferSize = kInputBufferSize,
    kMaxDpbBuffers    = 17,
    kIDRFrameRefreshIntervalInSec = 1,
};


static void *MallocCb(void * /*userData*/, int32_t size, int32_t /*attrs*/) {
    void *ptr = calloc(size, 1);
    return ptr;
}

static void FreeCb(void * /*userData*/, void *ptr) {
    free(ptr);
}

static int32_t DpbAllocCb(void * /*userData*/,
        unsigned int sizeInMbs, unsigned int numBuffers) {

    size_t frameSize = (sizeInMbs << 7) * 3;
    if(numBuffers < kMaxDpbBuffers && frameSize <= kInputBufferSize) {
        return 1;
    } else {
        return 0;
    }
}

static int32_t BindFrameCb(void *userData, int32_t index, uint8_t **yuv) {
     assert(index < kMaxDpbBuffers);
     uint8_t** dpbBuffer = static_cast<uint8_t**>(userData);
     *yuv = dpbBuffer[index];
     return 1;
}

static void UnbindFrameCb(void * /*userData*/, int32_t /*index*/) {
}

int main(int argc, char *argv[]) {

    if (argc < 7) {
        fprintf(stderr, "Usage %s <input yuv> <output file> <width> <height>"
                        " <frame rate> <bitrate in kbps>\n", argv[0]);
        fprintf(stderr, "Max width %d\n", kMaxWidth);
        fprintf(stderr, "Max height %d\n", kMaxHeight);
        fprintf(stderr, "Max framerate %d\n", kMaxFrameRate);
        fprintf(stderr, "Max bitrate %d kbps\n", kMaxBitrate);
        return EXIT_FAILURE;
    }

    // Read height and width.
    int32_t width;
    int32_t height;
    width = atoi(argv[3]);
    height = atoi(argv[4]);
    if (width > kMaxWidth || height > kMaxHeight || width <= 0 || height <= 0) {
        fprintf(stderr, "Unsupported dimensions %dx%d\n", width, height);
        return EXIT_FAILURE;
    }

    if (width % 16 != 0 || height % 16 != 0) {
        fprintf(stderr, "Video frame size %dx%d must be a multiple of 16\n",
            width, height);
        return EXIT_FAILURE;
    }

    // Read frame rate.
    int32_t frameRate;
    frameRate = atoi(argv[5]);
    if (frameRate > kMaxFrameRate || frameRate <= 0) {
        fprintf(stderr, "Unsupported frame rate %d\n", frameRate);
        return EXIT_FAILURE;
    }

    // Read bit rate.
    int32_t bitrate;
    bitrate = atoi(argv[6]);
    if (bitrate > kMaxBitrate || bitrate <= 0) {
        fprintf(stderr, "Unsupported bitrate %d\n", bitrate);
        return EXIT_FAILURE;
    }
    bitrate *= 1024; // kbps to bps.

    // Open the input file.
    FILE *fpInput = fopen(argv[1], "rb");
    if (!fpInput) {
        fprintf(stderr, "Could not open %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    // Open the output file.
    FILE *fpOutput = fopen(argv[2], "wb");
    if (!fpOutput) {
        fprintf(stderr, "Could not open %s\n", argv[2]);
        fclose(fpInput);
        return EXIT_FAILURE;
    }

    // Allocate input buffer.
    uint8_t *inputBuf = (uint8_t *)malloc(kInputBufferSize);
    assert(inputBuf != NULL);

    // Allocate output buffer.
    uint8_t *outputBuf = (uint8_t *)malloc(kOutputBufferSize);
    assert(outputBuf != NULL);

    // Allocate dpb buffers.
    uint8_t * dpbBuffers[kMaxDpbBuffers];
    for (int i = 0; i < kMaxDpbBuffers; ++i) {
        dpbBuffers[i] = (uint8_t *)malloc(kInputBufferSize);
        assert(dpbBuffers[i] != NULL);
    }

    // Initialize the encoder parameters.
    tagAVCEncParam encParams;
    memset(&encParams, 0, sizeof(tagAVCEncParam));
    encParams.rate_control = AVC_ON;
    encParams.initQP = 0;
    encParams.init_CBP_removal_delay = 1600;

    encParams.intramb_refresh = 0;
    encParams.auto_scd = AVC_ON;
    encParams.out_of_band_param_set = AVC_ON;
    encParams.poc_type = 2;
    encParams.log2_max_poc_lsb_minus_4 = 12;
    encParams.delta_poc_zero_flag = 0;
    encParams.offset_poc_non_ref = 0;
    encParams.offset_top_bottom = 0;
    encParams.num_ref_in_cycle = 0;
    encParams.offset_poc_ref = NULL;

    encParams.num_ref_frame = 1;
    encParams.num_slice_group = 1;
    encParams.fmo_type = 0;

    encParams.db_filter = AVC_ON;
    encParams.disable_db_idc = 0;

    encParams.alpha_offset = 0;
    encParams.beta_offset = 0;
    encParams.constrained_intra_pred = AVC_OFF;

    encParams.data_par = AVC_OFF;
    encParams.fullsearch = AVC_OFF;
    encParams.search_range = 16;
    encParams.sub_pel = AVC_OFF;
    encParams.submb_pred = AVC_OFF;
    encParams.rdopt_mode = AVC_OFF;
    encParams.bidir_pred = AVC_OFF;

    encParams.use_overrun_buffer = AVC_OFF;

    encParams.width = width;
    encParams.height = height;
    encParams.bitrate = bitrate;
    encParams.frame_rate = 1000 * frameRate;  // In frames/ms.
    encParams.CPB_size = (uint32_t) (bitrate >> 1);

    int32_t  IDRFrameRefreshIntervalInSec = kIDRFrameRefreshIntervalInSec;
    if (IDRFrameRefreshIntervalInSec == 0) {
        encParams.idr_period = 1;  // All I frames.
    } else {
        encParams.idr_period = (IDRFrameRefreshIntervalInSec * frameRate);
    }

    int32_t nMacroBlocks = ((((width + 15) >> 4) << 4) *
            (((height + 15) >> 4) << 4)) >> 8;
    uint32_t *sliceGroup = (uint32_t *) malloc(sizeof(uint32_t) * nMacroBlocks);
    assert(sliceGroup != NULL);
    for (int i = 0, idx = 0; i < nMacroBlocks; ++i) {
        sliceGroup[i] = idx++;
        if (idx >= encParams.num_slice_group) {
            idx = 0;
        }
    }
    encParams.slice_group = sliceGroup;
    encParams.profile = AVC_BASELINE;
    encParams.level = AVC_LEVEL2;

    // Initialize the handle.
    tagAVCHandle handle;
    memset(&handle, 0, sizeof(tagAVCHandle));
    handle.AVCObject = NULL;
    handle.userData = dpbBuffers;
    handle.CBAVC_DPBAlloc = DpbAllocCb;
    handle.CBAVC_FrameBind = BindFrameCb;
    handle.CBAVC_FrameUnbind = UnbindFrameCb;
    handle.CBAVC_Malloc = MallocCb;
    handle.CBAVC_Free = FreeCb;

    // Initialize the encoder.
    AVCEnc_Status status;
    status = PVAVCEncInitialize(&handle, &encParams, NULL, NULL);
    if (status != AVCENC_SUCCESS) {
        fprintf(stderr, "Failed to initialize the encoder\n");

        // Release resources.
        fclose(fpInput);
        fclose(fpOutput);
        free(sliceGroup);
        free(inputBuf);
        free(outputBuf);
        for (int i = 0; i < kMaxDpbBuffers; ++i) {
            free(dpbBuffers[i]);
        }
        return EXIT_FAILURE;
    }

    // Encode Sequence Parameter Set.
    uint32_t dataLength = kOutputBufferSize;
    int32_t type;
    status = PVAVCEncodeNAL(&handle, outputBuf, &dataLength, &type);
    assert(type == AVC_NALTYPE_SPS);
    fwrite("\x00\x00\x00\x01", 1, 4, fpOutput); // Start Code.
    fwrite(outputBuf, 1, dataLength, fpOutput); // SPS.

    // Encode Picture Paramater Set.
    dataLength = kOutputBufferSize;
    status = PVAVCEncodeNAL(&handle, outputBuf, &dataLength, &type);
    assert(type == AVC_NALTYPE_PPS);
    fwrite("\x00\x00\x00\x01", 1, 4, fpOutput); // Start Code.
    fwrite(outputBuf, 1, dataLength, fpOutput); // PPS.

    // Core loop.
    int32_t retVal = EXIT_SUCCESS;
    int32_t frameSize = (width * height * 3) / 2;
    int32_t numInputFrames = 0;
    int32_t numNalEncoded = 0;
    bool readyForNextFrame = true;

    while (1) {
        if (readyForNextFrame == true) {
            // Read the input frame.
            int32_t bytesRead;
            bytesRead = fread(inputBuf, 1, frameSize, fpInput);
            if (bytesRead != frameSize) {
                break; // End of file.
            }

            // Set the input frame.
            AVCFrameIO vin;
            memset(&vin, 0, sizeof(vin));
            vin.height = ((height + 15) >> 4) << 4;
            vin.pitch  = ((width  + 15) >> 4) << 4;
            vin.coding_timestamp = (numInputFrames * 1000) / frameRate;  // in ms
            vin.YCbCr[0] = inputBuf;
            vin.YCbCr[1] = vin.YCbCr[0] + vin.height * vin.pitch;
            vin.YCbCr[2] = vin.YCbCr[1] + ((vin.height * vin.pitch) >> 2);
            vin.disp_order = numInputFrames;

            status = PVAVCEncSetInput(&handle, &vin);
            if (status == AVCENC_SUCCESS || status == AVCENC_NEW_IDR) {
                readyForNextFrame = false;
                ++numInputFrames;
            } else if (status < AVCENC_SUCCESS) {
                fprintf(stderr, "Error %d while setting input frame\n", status);
                retVal = EXIT_FAILURE;
                break;
            } else {
                fprintf(stderr, "Frame drop\n");
                readyForNextFrame = true;
                ++numInputFrames;
                continue;
            }
        }

        // Encode the input frame.
        dataLength = kOutputBufferSize;
        status = PVAVCEncodeNAL(&handle, outputBuf, &dataLength, &type);
        if (status == AVCENC_SUCCESS) {
            PVAVCEncGetOverrunBuffer(&handle);
        } else if (status == AVCENC_PICTURE_READY) {
            PVAVCEncGetOverrunBuffer(&handle);
            readyForNextFrame = true;
            AVCFrameIO recon;
            if (PVAVCEncGetRecon(&handle, &recon) == AVCENC_SUCCESS) {
                PVAVCEncReleaseRecon(&handle, &recon);
            }
        } else {
            dataLength = 0;
            readyForNextFrame = true;
        }

        if (status < AVCENC_SUCCESS) {
            fprintf(stderr, "Error %d while encoding frame\n", status);
            retVal = EXIT_FAILURE;
            break;
        }

        numNalEncoded++;

        // Write the output.
        if (dataLength > 0) {
            fwrite("\x00\x00\x00\x01", 1, 4, fpOutput); // Start Code.
            fwrite(outputBuf, 1, dataLength, fpOutput); // NAL.
            printf("NAL %d of size %d written\n", numNalEncoded, dataLength + 4);
        }
    }

    // Close input and output file.
    fclose(fpInput);
    fclose(fpOutput);

    // Free allocated memory.
    free(sliceGroup);
    free(inputBuf);
    free(outputBuf);
    for (int i = 0; i < kMaxDpbBuffers; ++i) {
        free(dpbBuffers[i]);
    }

    // Close encoder instance.
    PVAVCCleanUpEncoder(&handle);

    return retVal;
}
