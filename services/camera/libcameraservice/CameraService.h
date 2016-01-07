/*
 * Copyright (C) 2008 The Android Open Source Project
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

#ifndef ANDROID_SERVERS_CAMERA_CAMERASERVICE_H
#define ANDROID_SERVERS_CAMERA_CAMERASERVICE_H

#include <cutils/multiuser.h>
#include <utils/Vector.h>
#include <utils/KeyedVector.h>
#include <binder/AppOpsManager.h>
#include <binder/BinderService.h>
#include <binder/IAppOpsCallback.h>
#include <camera/ICameraService.h>
#include <camera/ICameraServiceProxy.h>
#include <hardware/camera.h>

#include <camera/ICamera.h>
#include <camera/ICameraClient.h>
#include <camera/camera2/ICameraDeviceUser.h>
#include <camera/camera2/ICameraDeviceCallbacks.h>
#include <camera/VendorTagDescriptor.h>
#include <camera/CaptureResult.h>
#include <camera/CameraParameters.h>

#include <camera/ICameraServiceListener.h>
#include "CameraFlashlight.h"

#include "common/CameraModule.h"
#include "media/RingBuffer.h"
#include "utils/AutoConditionLock.h"
#include "utils/ClientManager.h"

#include <set>
#include <string>
#include <map>
#include <memory>
#include <utility>

#ifndef MAX_CAMERAS
#define MAX_CAMERAS 2
#endif

namespace android {

extern volatile int32_t gLogLevel;

class MemoryHeapBase;
class MediaPlayer;

class CameraService :
    public BinderService<CameraService>,
    public BnCameraService,
    public IBinder::DeathRecipient,
    public camera_module_callbacks_t
{
    friend class BinderService<CameraService>;
public:
    class Client;
    class BasicClient;

    // The effective API level.  The Camera2 API running in LEGACY mode counts as API_1.
    enum apiLevel {
        API_1 = 1,
        API_2 = 2
    };

    // Process state (mirrors frameworks/base/core/java/android/app/ActivityManager.java)
    static const int PROCESS_STATE_NONEXISTENT = -1;
    static const int PROCESS_STATE_TOP = 2;
    static const int PROCESS_STATE_TOP_SLEEPING = 5;

    // 3 second busy timeout when other clients are connecting
    static const nsecs_t DEFAULT_CONNECT_TIMEOUT_NS = 3000000000;

    // 1 second busy timeout when other clients are disconnecting
    static const nsecs_t DEFAULT_DISCONNECT_TIMEOUT_NS = 1000000000;

    // Default number of messages to store in eviction log
    static const size_t DEFAULT_EVENT_LOG_LENGTH = 100;

    // Implementation of BinderService<T>
    static char const* getServiceName() { return "media.camera"; }

                        CameraService();
    virtual             ~CameraService();

    /////////////////////////////////////////////////////////////////////
    // HAL Callbacks
    virtual void        onDeviceStatusChanged(camera_device_status_t cameraId,
                                              camera_device_status_t newStatus);
    virtual void        onTorchStatusChanged(const String8& cameraId,
                                             ICameraServiceListener::TorchStatus
                                                   newStatus);

    /////////////////////////////////////////////////////////////////////
    // ICameraService
    virtual int32_t     getNumberOfCameras(int type);
    virtual int32_t     getNumberOfCameras();

    virtual status_t    getCameraInfo(int cameraId,
                                      struct CameraInfo* cameraInfo);
    virtual status_t    getCameraCharacteristics(int cameraId,
                                                 CameraMetadata* cameraInfo);
    virtual status_t    getCameraVendorTagDescriptor(/*out*/ sp<VendorTagDescriptor>& desc);

    virtual status_t connect(const sp<ICameraClient>& cameraClient, int cameraId,
            const String16& clientPackageName, int clientUid,
            /*out*/
            sp<ICamera>& device);

    virtual status_t connectLegacy(const sp<ICameraClient>& cameraClient, int cameraId,
            int halVersion, const String16& clientPackageName, int clientUid,
            /*out*/
            sp<ICamera>& device);

    virtual status_t connectDevice(
            const sp<ICameraDeviceCallbacks>& cameraCb,
            int cameraId,
            const String16& clientPackageName,
            int clientUid,
            /*out*/
            sp<ICameraDeviceUser>& device);

    virtual status_t    addListener(const sp<ICameraServiceListener>& listener);
    virtual status_t    removeListener(
                                    const sp<ICameraServiceListener>& listener);

    virtual status_t    getLegacyParameters(
            int cameraId,
            /*out*/
            String16* parameters);

    virtual status_t    setTorchMode(const String16& cameraId, bool enabled,
            const sp<IBinder>& clientBinder);

    virtual void notifySystemEvent(int32_t eventId, const int32_t* args, size_t length);

    // OK = supports api of that version, -EOPNOTSUPP = does not support
    virtual status_t    supportsCameraApi(
            int cameraId, int apiVersion);

    // Extra permissions checks
    virtual status_t    onTransact(uint32_t code, const Parcel& data,
                                   Parcel* reply, uint32_t flags);

    virtual status_t    dump(int fd, const Vector<String16>& args);

    /////////////////////////////////////////////////////////////////////
    // Client functionality

    enum sound_kind {
        SOUND_SHUTTER = 0,
        SOUND_RECORDING_START = 1,
        SOUND_RECORDING_STOP = 2,
        NUM_SOUNDS
    };

    void                loadSound();
    void                playSound(sound_kind kind);
    void                releaseSound();

    /**
     * Update the state of a given camera device (open/close/active/idle) with
     * the camera proxy service in the system service
     */
    static void         updateProxyDeviceState(
            ICameraServiceProxy::CameraState newState,
            const String8& cameraId);

    /////////////////////////////////////////////////////////////////////
    // CameraDeviceFactory functionality
    int                 getDeviceVersion(int cameraId, int* facing = NULL);

    /////////////////////////////////////////////////////////////////////
    // Shared utilities
    static status_t     filterGetInfoErrorCode(status_t err);

    /////////////////////////////////////////////////////////////////////
    // CameraClient functionality

    class BasicClient : public virtual RefBase {
    public:
        virtual status_t    initialize(CameraModule *module) = 0;
        virtual void        disconnect();

        // because we can't virtually inherit IInterface, which breaks
        // virtual inheritance
        virtual sp<IBinder> asBinderWrapper() = 0;

        // Return the remote callback binder object (e.g. ICameraDeviceCallbacks)
        sp<IBinder>         getRemote() {
            return mRemoteBinder;
        }

        virtual status_t    dump(int fd, const Vector<String16>& args) = 0;

        // Return the package name for this client
        virtual String16 getPackageName() const;

        // Notify client about a fatal error
        virtual void notifyError(ICameraDeviceCallbacks::CameraErrorCode errorCode,
                const CaptureResultExtras& resultExtras) = 0;

        // Get the UID of the application client using this
        virtual uid_t getClientUid() const;

        // Get the PID of the application client using this
        virtual int getClientPid() const;

        // Check what API level is used for this client. This is used to determine which
        // superclass this can be cast to.
        virtual bool canCastToApiClient(apiLevel level) const;
    protected:
        BasicClient(const sp<CameraService>& cameraService,
                const sp<IBinder>& remoteCallback,
                const String16& clientPackageName,
                int cameraId,
                int cameraFacing,
                int clientPid,
                uid_t clientUid,
                int servicePid);

        virtual ~BasicClient();

        // the instance is in the middle of destruction. When this is set,
        // the instance should not be accessed from callback.
        // CameraService's mClientLock should be acquired to access this.
        // - subclasses should set this to true in their destructors.
        bool                            mDestructionStarted;

        // these are initialized in the constructor.
        sp<CameraService>               mCameraService;  // immutable after constructor
        int                             mCameraId;       // immutable after constructor
        int                             mCameraFacing;   // immutable after constructor
        const String16                  mClientPackageName;
        pid_t                           mClientPid;
        uid_t                           mClientUid;      // immutable after constructor
        pid_t                           mServicePid;     // immutable after constructor
        bool                            mDisconnected;

        // - The app-side Binder interface to receive callbacks from us
        sp<IBinder>                     mRemoteBinder;   // immutable after constructor

        // permissions management
        status_t                        startCameraOps();
        status_t                        finishCameraOps();

    private:
        AppOpsManager                   mAppOpsManager;

        class OpsCallback : public BnAppOpsCallback {
        public:
            OpsCallback(wp<BasicClient> client);
            virtual void opChanged(int32_t op, const String16& packageName);

        private:
            wp<BasicClient> mClient;

        }; // class OpsCallback

        sp<OpsCallback> mOpsCallback;
        // Track whether startCameraOps was called successfully, to avoid
        // finishing what we didn't start.
        bool            mOpsActive;

        // IAppOpsCallback interface, indirected through opListener
        virtual void opChanged(int32_t op, const String16& packageName);
    }; // class BasicClient

    class Client : public BnCamera, public BasicClient
    {
    public:
        typedef ICameraClient TCamCallbacks;

        // ICamera interface (see ICamera for details)
        virtual void          disconnect();
        virtual status_t      connect(const sp<ICameraClient>& client) = 0;
        virtual status_t      lock() = 0;
        virtual status_t      unlock() = 0;
        virtual status_t      setPreviewTarget(const sp<IGraphicBufferProducer>& bufferProducer)=0;
        virtual void          setPreviewCallbackFlag(int flag) = 0;
        virtual status_t      setPreviewCallbackTarget(
                const sp<IGraphicBufferProducer>& callbackProducer) = 0;
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
                const String16& clientPackageName,
                int cameraId,
                int cameraFacing,
                int clientPid,
                uid_t clientUid,
                int servicePid);
        ~Client();

        // return our camera client
        const sp<ICameraClient>&    getRemoteCallback() {
            return mRemoteCallback;
        }

        virtual sp<IBinder> asBinderWrapper() {
            return asBinder(this);
        }

        virtual void         notifyError(ICameraDeviceCallbacks::CameraErrorCode errorCode,
                                         const CaptureResultExtras& resultExtras);

        // Check what API level is used for this client. This is used to determine which
        // superclass this can be cast to.
        virtual bool canCastToApiClient(apiLevel level) const;
    protected:
        // Convert client from cookie.
        static sp<CameraService::Client> getClientFromCookie(void* user);

        // Initialized in constructor

        // - The app-side Binder interface to receive callbacks from us
        sp<ICameraClient>               mRemoteCallback;

    }; // class Client

    /**
     * A listener class that implements the LISTENER interface for use with a ClientManager, and
     * implements the following methods:
     *    void onClientRemoved(const ClientDescriptor<KEY, VALUE>& descriptor);
     *    void onClientAdded(const ClientDescriptor<KEY, VALUE>& descriptor);
     */
    class ClientEventListener {
    public:
        void onClientAdded(const resource_policy::ClientDescriptor<String8,
                sp<CameraService::BasicClient>>& descriptor);
        void onClientRemoved(const resource_policy::ClientDescriptor<String8,
                sp<CameraService::BasicClient>>& descriptor);
    }; // class ClientEventListener

    typedef std::shared_ptr<resource_policy::ClientDescriptor<String8,
            sp<CameraService::BasicClient>>> DescriptorPtr;

    /**
     * A container class for managing active camera clients that are using HAL devices.  Active
     * clients are represented by ClientDescriptor objects that contain strong pointers to the
     * actual BasicClient subclass binder interface implementation.
     *
     * This class manages the eviction behavior for the camera clients.  See the parent class
     * implementation in utils/ClientManager for the specifics of this behavior.
     */
    class CameraClientManager : public resource_policy::ClientManager<String8,
            sp<CameraService::BasicClient>, ClientEventListener> {
    public:
        CameraClientManager();
        virtual ~CameraClientManager();

        /**
         * Return a strong pointer to the active BasicClient for this camera ID, or an empty
         * if none exists.
         */
        sp<CameraService::BasicClient> getCameraClient(const String8& id) const;

        /**
         * Return a string describing the current state.
         */
        String8 toString() const;

        /**
         * Make a ClientDescriptor object wrapping the given BasicClient strong pointer.
         */
        static DescriptorPtr makeClientDescriptor(const String8& key, const sp<BasicClient>& value,
                int32_t cost, const std::set<String8>& conflictingKeys, int32_t priority,
                int32_t ownerId);

        /**
         * Make a ClientDescriptor object wrapping the given BasicClient strong pointer with
         * values intialized from a prior ClientDescriptor.
         */
        static DescriptorPtr makeClientDescriptor(const sp<BasicClient>& value,
                const CameraService::DescriptorPtr& partial);

    }; // class CameraClientManager

private:

    /**
     * Container class for the state of each logical camera device, including: ID, status, and
     * dependencies on other devices.  The mapping of camera ID -> state saved in mCameraStates
     * represents the camera devices advertised by the HAL (and any USB devices, when we add
     * those).
     *
     * This container does NOT represent an active camera client.  These are represented using
     * the ClientDescriptors stored in mActiveClientManager.
     */
    class CameraState {
    public:
        /**
         * Make a new CameraState and set the ID, cost, and conflicting devices using the values
         * returned in the HAL's camera_info struct for each device.
         */
        CameraState(const String8& id, int cost, const std::set<String8>& conflicting);
        virtual ~CameraState();

        /**
         * Return the status for this device.
         *
         * This method acquires mStatusLock.
         */
        ICameraServiceListener::Status getStatus() const;

        /**
         * This function updates the status for this camera device, unless the given status
         * is in the given list of rejected status states, and execute the function passed in
         * with a signature onStatusUpdateLocked(const String8&, ICameraServiceListener::Status)
         * if the status has changed.
         *
         * This method is idempotent, and will not result in the function passed to
         * onStatusUpdateLocked being called more than once for the same arguments.
         * This method aquires mStatusLock.
         */
        template<class Func>
        void updateStatus(ICameraServiceListener::Status status, const String8& cameraId,
                std::initializer_list<ICameraServiceListener::Status> rejectSourceStates,
                Func onStatusUpdatedLocked);

        /**
         * Return the last set CameraParameters object generated from the information returned by
         * the HAL for this device (or an empty CameraParameters object if none has been set).
         */
        CameraParameters getShimParams() const;

        /**
         * Set the CameraParameters for this device.
         */
        void setShimParams(const CameraParameters& params);

        /**
         * Return the resource_cost advertised by the HAL for this device.
         */
        int getCost() const;

        /**
         * Return a set of the IDs of conflicting devices advertised by the HAL for this device.
         */
        std::set<String8> getConflicting() const;

        /**
         * Return the ID of this camera device.
         */
        String8 getId() const;

    private:
        const String8 mId;
        ICameraServiceListener::Status mStatus; // protected by mStatusLock
        const int mCost;
        std::set<String8> mConflicting;
        mutable Mutex mStatusLock;
        CameraParameters mShimParams;
    }; // class CameraState

    // Delay-load the Camera HAL module
    virtual void onFirstRef();

    // Check if we can connect, before we acquire the service lock.
    status_t validateConnectLocked(const String8& cameraId, /*inout*/int& clientUid) const;

    // Handle active client evictions, and update service state.
    // Only call with with mServiceLock held.
    status_t handleEvictionsLocked(const String8& cameraId, int clientPid,
        apiLevel effectiveApiLevel, const sp<IBinder>& remoteCallback, const String8& packageName,
        /*out*/
        sp<BasicClient>* client,
        std::shared_ptr<resource_policy::ClientDescriptor<String8, sp<BasicClient>>>* partial);

    // Single implementation shared between the various connect calls
    template<class CALLBACK, class CLIENT>
    status_t connectHelper(const sp<CALLBACK>& cameraCb, const String8& cameraId, int halVersion,
            const String16& clientPackageName, int clientUid, apiLevel effectiveApiLevel,
            bool legacyMode, bool shimUpdateOnly, /*out*/sp<CLIENT>& device);

    // Lock guarding camera service state
    Mutex               mServiceLock;

    // Condition to use with mServiceLock, used to handle simultaneous connect calls from clients
    std::shared_ptr<WaitableMutexWrapper> mServiceLockWrapper;

    // Return NO_ERROR if the device with a give ID can be connected to
    status_t checkIfDeviceIsUsable(const String8& cameraId) const;

    // Container for managing currently active application-layer clients
    CameraClientManager mActiveClientManager;

    // Mapping from camera ID -> state for each device, map is protected by mCameraStatesLock
    std::map<String8, std::shared_ptr<CameraState>> mCameraStates;

    // Mutex guarding mCameraStates map
    mutable Mutex mCameraStatesLock;

    // Circular buffer for storing event logging for dumps
    RingBuffer<String8> mEventLog;
    Mutex mLogLock;

    // Currently allowed user IDs
    std::set<userid_t> mAllowedUsers;

    /**
     * Check camera capabilities, such as support for basic color operation
     */
    int checkCameraCapabilities(int id, camera_info info, int *latestStrangeCameraId);

    /**
     * Get the camera state for a given camera id.
     *
     * This acquires mCameraStatesLock.
     */
    std::shared_ptr<CameraService::CameraState> getCameraState(const String8& cameraId) const;

    /**
     * Evict client who's remote binder has died.  Returns true if this client was in the active
     * list and was disconnected.
     *
     * This method acquires mServiceLock.
     */
    bool evictClientIdByRemote(const wp<IBinder>& cameraClient);

    /**
     * Remove the given client from the active clients list; does not disconnect the client.
     *
     * This method acquires mServiceLock.
     */
    void removeByClient(const BasicClient* client);

    /**
     * Add new client to active clients list after conflicting clients have disconnected using the
     * values set in the partial descriptor passed in to construct the actual client descriptor.
     * This is typically called at the end of a connect call.
     *
     * This method must be called with mServiceLock held.
     */
    void finishConnectLocked(const sp<BasicClient>& client, const DescriptorPtr& desc);

    /**
     * Returns the integer corresponding to the given camera ID string, or -1 on failure.
     */
    static int cameraIdToInt(const String8& cameraId);

    /**
     * Remove a single client corresponding to the given camera id from the list of active clients.
     * If none exists, return an empty strongpointer.
     *
     * This method must be called with mServiceLock held.
     */
    sp<CameraService::BasicClient> removeClientLocked(const String8& cameraId);

    /**
     * Handle a notification that the current device user has changed.
     */
    void doUserSwitch(const int32_t* newUserId, size_t length);

    /**
     * Add an event log message.
     */
    void logEvent(const char* event);

    /**
     * Add an event log message that a client has been disconnected.
     */
    void logDisconnected(const char* cameraId, int clientPid, const char* clientPackage);

    /**
     * Add an event log message that a client has been connected.
     */
    void logConnected(const char* cameraId, int clientPid, const char* clientPackage);

    /**
     * Add an event log message that a client's connect attempt has been rejected.
     */
    void logRejected(const char* cameraId, int clientPid, const char* clientPackage,
            const char* reason);

    /**
     * Add an event log message that the current device user has been switched.
     */
    void logUserSwitch(const std::set<userid_t>& oldUserIds,
        const std::set<userid_t>& newUserIds);

    /**
     * Add an event log message that a device has been removed by the HAL
     */
    void logDeviceRemoved(const char* cameraId, const char* reason);

    /**
     * Add an event log message that a device has been added by the HAL
     */
    void logDeviceAdded(const char* cameraId, const char* reason);

    /**
     * Add an event log message that a client has unexpectedly died.
     */
    void logClientDied(int clientPid, const char* reason);

    /**
     * Add a event log message that a serious service-level error has occured
     */
    void logServiceError(const char* msg, int errorCode);

    /**
     * Dump the event log to an FD
     */
    void dumpEventLog(int fd);

    int                 mNumberOfCameras;
    int                 mNumberOfNormalCameras;

    // sounds
    MediaPlayer*        newMediaPlayer(const char *file);

    Mutex               mSoundLock;
    sp<MediaPlayer>     mSoundPlayer[NUM_SOUNDS];
    int                 mSoundRef;  // reference count (release all MediaPlayer when 0)

    CameraModule*     mModule;

    // Guarded by mStatusListenerMutex
    std::vector<sp<ICameraServiceListener>> mListenerList;
    Mutex       mStatusListenerLock;

    /**
     * Update the status for the given camera id (if that device exists), and broadcast the
     * status update to all current ICameraServiceListeners if the status has changed.  Any
     * statuses in rejectedSourceStates will be ignored.
     *
     * This method must be idempotent.
     * This method acquires mStatusLock and mStatusListenerLock.
     */
    void updateStatus(ICameraServiceListener::Status status, const String8& cameraId,
            std::initializer_list<ICameraServiceListener::Status> rejectedSourceStates);
    void updateStatus(ICameraServiceListener::Status status, const String8& cameraId);

    // flashlight control
    sp<CameraFlashlight> mFlashlight;
    // guard mTorchStatusMap
    Mutex                mTorchStatusMutex;
    // guard mTorchClientMap
    Mutex                mTorchClientMapMutex;
    // guard mTorchUidMap
    Mutex                mTorchUidMapMutex;
    // camera id -> torch status
    KeyedVector<String8, ICameraServiceListener::TorchStatus> mTorchStatusMap;
    // camera id -> torch client binder
    // only store the last client that turns on each camera's torch mode
    KeyedVector<String8, sp<IBinder>> mTorchClientMap;
    // camera id -> [incoming uid, current uid] pair
    std::map<String8, std::pair<int, int>> mTorchUidMap;

    // check and handle if torch client's process has died
    void handleTorchClientBinderDied(const wp<IBinder> &who);

    // handle torch mode status change and invoke callbacks. mTorchStatusMutex
    // should be locked.
    void onTorchStatusChangedLocked(const String8& cameraId,
            ICameraServiceListener::TorchStatus newStatus);

    // get a camera's torch status. mTorchStatusMutex should be locked.
    status_t getTorchStatusLocked(const String8 &cameraId,
            ICameraServiceListener::TorchStatus *status) const;

    // set a camera's torch status. mTorchStatusMutex should be locked.
    status_t setTorchStatusLocked(const String8 &cameraId,
            ICameraServiceListener::TorchStatus status);

    // IBinder::DeathRecipient implementation
    virtual void        binderDied(const wp<IBinder> &who);

    // Helpers

    bool                setUpVendorTags();

    /**
     * Initialize and cache the metadata used by the HAL1 shim for a given cameraId.
     *
     * Returns OK on success, or a negative error code.
     */
    status_t            initializeShimMetadata(int cameraId);

    /**
     * Get the cached CameraParameters for the camera. If they haven't been
     * cached yet, then initialize them for the first time.
     *
     * Returns OK on success, or a negative error code.
     */
    status_t            getLegacyParametersLazy(int cameraId, /*out*/CameraParameters* parameters);

    /**
     * Generate the CameraCharacteristics metadata required by the Camera2 API
     * from the available HAL1 CameraParameters and CameraInfo.
     *
     * Returns OK on success, or a negative error code.
     */
    status_t            generateShimMetadata(int cameraId, /*out*/CameraMetadata* cameraInfo);

    static int getCallingPid();

    static int getCallingUid();

    /**
     * Get the current system time as a formatted string.
     */
    static String8 getFormattedCurrentTime();

    /**
     * Get the camera eviction priority from the current process state given by ActivityManager.
     */
    static int getCameraPriorityFromProcState(int procState);

    static status_t makeClient(const sp<CameraService>& cameraService,
            const sp<IInterface>& cameraCb, const String16& packageName, const String8& cameraId,
            int facing, int clientPid, uid_t clientUid, int servicePid, bool legacyMode,
            int halVersion, int deviceVersion, apiLevel effectiveApiLevel,
            /*out*/sp<BasicClient>* client);

    status_t checkCameraAccess(const String16& opPackageName);

    static String8 toString(std::set<userid_t> intSet);

    static sp<ICameraServiceProxy> getCameraServiceProxy();
    static void pingCameraServiceProxy();

};

template<class Func>
void CameraService::CameraState::updateStatus(ICameraServiceListener::Status status,
        const String8& cameraId,
        std::initializer_list<ICameraServiceListener::Status> rejectSourceStates,
        Func onStatusUpdatedLocked) {
    Mutex::Autolock lock(mStatusLock);
    ICameraServiceListener::Status oldStatus = mStatus;
    mStatus = status;

    if (oldStatus == status) {
        return;
    }

    ALOGV("%s: Status has changed for camera ID %s from %#x to %#x", __FUNCTION__,
            cameraId.string(), oldStatus, status);

    if (oldStatus == ICameraServiceListener::STATUS_NOT_PRESENT &&
        (status != ICameraServiceListener::STATUS_PRESENT &&
         status != ICameraServiceListener::STATUS_ENUMERATING)) {

        ALOGW("%s: From NOT_PRESENT can only transition into PRESENT or ENUMERATING",
                __FUNCTION__);
        mStatus = oldStatus;
        return;
    }

    /**
     * Sometimes we want to conditionally do a transition.
     * For example if a client disconnects, we want to go to PRESENT
     * only if we weren't already in NOT_PRESENT or ENUMERATING.
     */
    for (auto& rejectStatus : rejectSourceStates) {
        if (oldStatus == rejectStatus) {
            ALOGV("%s: Rejecting status transition for Camera ID %s,  since the source "
                    "state was was in one of the bad states.", __FUNCTION__, cameraId.string());
            mStatus = oldStatus;
            return;
        }
    }

    onStatusUpdatedLocked(cameraId, status);
}


template<class CALLBACK, class CLIENT>
status_t CameraService::connectHelper(const sp<CALLBACK>& cameraCb, const String8& cameraId,
        int halVersion, const String16& clientPackageName, int clientUid,
        apiLevel effectiveApiLevel, bool legacyMode, bool shimUpdateOnly,
        /*out*/sp<CLIENT>& device) {
    status_t ret = NO_ERROR;
    String8 clientName8(clientPackageName);
    int clientPid = getCallingPid();

    ALOGI("CameraService::connect call (PID %d \"%s\", camera ID %s) for HAL version %s and "
            "Camera API version %d", clientPid, clientName8.string(), cameraId.string(),
            (halVersion == -1) ? "default" : std::to_string(halVersion).c_str(),
            static_cast<int>(effectiveApiLevel));

    sp<CLIENT> client = nullptr;
    {
        // Acquire mServiceLock and prevent other clients from connecting
        std::unique_ptr<AutoConditionLock> lock =
                AutoConditionLock::waitAndAcquire(mServiceLockWrapper, DEFAULT_CONNECT_TIMEOUT_NS);

        if (lock == nullptr) {
            ALOGE("CameraService::connect X (PID %d) rejected (too many other clients connecting)."
                    , clientPid);
            return -EBUSY;
        }

        // Enforce client permissions and do basic sanity checks
        if((ret = validateConnectLocked(cameraId, /*inout*/clientUid)) != NO_ERROR) {
            return ret;
        }

        // Check the shim parameters after acquiring lock, if they have already been updated and
        // we were doing a shim update, return immediately
        if (shimUpdateOnly) {
            auto cameraState = getCameraState(cameraId);
            if (cameraState != nullptr) {
                if (!cameraState->getShimParams().isEmpty()) return NO_ERROR;
            }
        }

        sp<BasicClient> clientTmp = nullptr;
        std::shared_ptr<resource_policy::ClientDescriptor<String8, sp<BasicClient>>> partial;
        if ((ret = handleEvictionsLocked(cameraId, clientPid, effectiveApiLevel,
                IInterface::asBinder(cameraCb), clientName8, /*out*/&clientTmp,
                /*out*/&partial)) != NO_ERROR) {
            return ret;
        }

        if (clientTmp.get() != nullptr) {
            // Handle special case for API1 MediaRecorder where the existing client is returned
            device = static_cast<CLIENT*>(clientTmp.get());
            return NO_ERROR;
        }

        // give flashlight a chance to close devices if necessary.
        mFlashlight->prepareDeviceOpen(cameraId);

        // TODO: Update getDeviceVersion + HAL interface to use strings for Camera IDs
        int id = cameraIdToInt(cameraId);
        if (id == -1) {
            ALOGE("%s: Invalid camera ID %s, cannot get device version from HAL.", __FUNCTION__,
                    cameraId.string());
            return BAD_VALUE;
        }

        int facing = -1;
        int deviceVersion = getDeviceVersion(id, /*out*/&facing);
        sp<BasicClient> tmp = nullptr;
        if((ret = makeClient(this, cameraCb, clientPackageName, cameraId, facing, clientPid,
                clientUid, getpid(), legacyMode, halVersion, deviceVersion, effectiveApiLevel,
                /*out*/&tmp)) != NO_ERROR) {
            return ret;
        }
        client = static_cast<CLIENT*>(tmp.get());

        LOG_ALWAYS_FATAL_IF(client.get() == nullptr, "%s: CameraService in invalid state",
                __FUNCTION__);

        if ((ret = client->initialize(mModule)) != OK) {
            ALOGE("%s: Could not initialize client from HAL module.", __FUNCTION__);
            return ret;
        }

        // Update shim paremeters for legacy clients
        if (effectiveApiLevel == API_1) {
            // Assume we have always received a Client subclass for API1
            sp<Client> shimClient = reinterpret_cast<Client*>(client.get());
            String8 rawParams = shimClient->getParameters();
            CameraParameters params(rawParams);

            auto cameraState = getCameraState(cameraId);
            if (cameraState != nullptr) {
                cameraState->setShimParams(params);
            } else {
                ALOGE("%s: Cannot update shim parameters for camera %s, no such device exists.",
                        __FUNCTION__, cameraId.string());
            }
        }

        if (shimUpdateOnly) {
            // If only updating legacy shim parameters, immediately disconnect client
            mServiceLock.unlock();
            client->disconnect();
            mServiceLock.lock();
        } else {
            // Otherwise, add client to active clients list
            finishConnectLocked(client, partial);
        }
    } // lock is destroyed, allow further connect calls

    // Important: release the mutex here so the client can call back into the service from its
    // destructor (can be at the end of the call)
    device = client;
    return NO_ERROR;
}

} // namespace android

#endif
