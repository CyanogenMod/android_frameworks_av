/*
**
** Copyright (C) 2008, The Android Open Source Project
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

#ifndef ANDROID_SERVERS_CAMERA_CAMERASERVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERASERVICE_H

#include <binder/BinderService.h>
#include <camera/ICameraService.h>
#include <hardware/camera.h>

/* This needs to be increased if we can have more cameras */
#ifdef OMAP_ENHANCEMENT
#define MAX_CAMERAS 3
#else
#define MAX_CAMERAS 2
#endif

namespace android {

extern volatile int32_t gLogLevel;

class MemoryHeapBase;
class MediaPlayer;

class CameraService :
    public BinderService<CameraService>,
    public BnCameraService,
    public IBinder::DeathRecipient
{
    friend class BinderService<CameraService>;
public:
    class Client;
    static char const* getServiceName() { return "media.camera"; }

                        CameraService();
    virtual             ~CameraService();

    virtual int32_t     getNumberOfCameras();
    virtual status_t    getCameraInfo(int cameraId,
                                      struct CameraInfo* cameraInfo);
    virtual sp<ICamera> connect(const sp<ICameraClient>& cameraClient, int cameraId);
    virtual void        removeClient(const sp<ICameraClient>& cameraClient);
    // returns plain pointer of client. Note that mClientLock should be acquired to
    // prevent the client from destruction. The result can be NULL.
    virtual Client*     getClientByIdUnsafe(int cameraId);
    virtual Mutex*      getClientLockById(int cameraId);

    virtual sp<Client>  getClientByRemote(const wp<IBinder>& cameraClient);

    virtual status_t    dump(int fd, const Vector<String16>& args);
    virtual status_t    onTransact(uint32_t code, const Parcel& data,
                                   Parcel* reply, uint32_t flags);
    virtual void onFirstRef();

    enum sound_kind {
        SOUND_SHUTTER = 0,
        SOUND_RECORDING = 1,
        NUM_SOUNDS
    };

    void                loadSound();
    void                playSound(sound_kind kind);
    void                releaseSound();

    class Client : public BnCamera
    {
    public:
        // ICamera interface (see ICamera for details)
        virtual void          disconnect();
        virtual status_t      connect(const sp<ICameraClient>& client) = 0;
        virtual status_t      lock() = 0;
        virtual status_t      unlock() = 0;
        virtual status_t      setPreviewDisplay(const sp<Surface>& surface) = 0;
        virtual status_t      setPreviewTexture(const sp<ISurfaceTexture>& surfaceTexture) = 0;
        virtual void          setPreviewCallbackFlag(int flag) = 0;
        virtual status_t      startPreview() = 0;
        virtual void          stopPreview() = 0;
        virtual bool          previewEnabled() = 0;
        virtual status_t      storeMetaDataInBuffers(bool enabled) = 0;
        virtual status_t      startRecording() = 0;
        virtual void          stopRecording() = 0;
        virtual bool          recordingEnabled() = 0;
        virtual void          releaseRecordingFrame(const sp<IMemory>& mem) = 0;
        virtual status_t      autoFocus() = 0;
        virtual status_t      cancelAutoFocus() = 0;
        virtual status_t      takePicture(int msgType) = 0;
        virtual status_t      setParameters(const String8& params) = 0;
        virtual String8       getParameters() const = 0;
        virtual status_t      sendCommand(int32_t cmd, int32_t arg1, int32_t arg2) = 0;

        // Interface used by CameraService
        Client(const sp<CameraService>& cameraService,
                const sp<ICameraClient>& cameraClient,
                int cameraId,
                int cameraFacing,
                int clientPid,
                int servicePid);
        ~Client();

        // return our camera client
        const sp<ICameraClient>&    getCameraClient() {
            return mCameraClient;
        }

        virtual status_t initialize(camera_module_t *module) = 0;

        virtual status_t dump(int fd, const Vector<String16>& args) = 0;

    protected:
        static Mutex*        getClientLockFromCookie(void* user);
        // convert client from cookie. Client lock should be acquired before getting Client.
        static Client*       getClientFromCookie(void* user);

        // the instance is in the middle of destruction. When this is set,
        // the instance should not be accessed from callback.
        // CameraService's mClientLock should be acquired to access this.
        bool                            mDestructionStarted;

        // these are initialized in the constructor.
        sp<CameraService>               mCameraService;  // immutable after constructor
        sp<ICameraClient>               mCameraClient;
        int                             mCameraId;       // immutable after constructor
        int                             mCameraFacing;   // immutable after constructor
        pid_t                           mClientPid;
        pid_t                           mServicePid;     // immutable after constructor

    };

private:
    Mutex               mServiceLock;
    wp<Client>          mClient[MAX_CAMERAS];  // protected by mServiceLock
    Mutex               mClientLock[MAX_CAMERAS]; // prevent Client destruction inside callbacks
    int                 mNumberOfCameras;

    // needs to be called with mServiceLock held
    sp<Client>          findClientUnsafe(const wp<IBinder>& cameraClient, int& outIndex);

    // atomics to record whether the hardware is allocated to some client.
    volatile int32_t    mBusy[MAX_CAMERAS];
    void                setCameraBusy(int cameraId);
    void                setCameraFree(int cameraId);

    // sounds
    MediaPlayer*        newMediaPlayer(const char *file);

    Mutex               mSoundLock;
    sp<MediaPlayer>     mSoundPlayer[NUM_SOUNDS];
    int                 mSoundRef;  // reference count (release all MediaPlayer when 0)

    camera_module_t *mModule;

    // IBinder::DeathRecipient implementation
    virtual void binderDied(const wp<IBinder> &who);
};

} // namespace android

#endif
