// #include <MtpTypes.h>
#include <system/audio.h>
#include <StrongPointer.h>
#include <media/IAudioFlinger.h>
#include <hardware/audio.h>
#include <stdlib.h>
#include <dlfcn.h>

namespace android {

typedef void (*audio_error_callback)(status_t err);

class AudioSystem
{
public:
    static bool getVoiceUnlockDLInstance();
    static int GetVoiceUnlockDLLatency();
    static int SetVoiceUnlockSRC(uint outSR, uint outChannel);
    static bool stopVoiceUnlockDL();
    static bool startVoiceUnlockDL();
    static int ReadRefFromRing(void*buf, uint32_t datasz,void* DLtime);
    static int GetVoiceUnlockULTime(void* DLtime);
    static void freeVoiceUnlockDLInstance();

}; // class

bool AudioSystem::getVoiceUnlockDLInstance()
{
  return 0;
}

int AudioSystem::GetVoiceUnlockDLLatency()
{
  return 0;
}

int AudioSystem::SetVoiceUnlockSRC(uint outSR __unused, uint outChannel __unused)
{
  return 0;
}

bool AudioSystem::stopVoiceUnlockDL()
{
  return 0;
}

bool AudioSystem::startVoiceUnlockDL()
{
  return 0;
}

int AudioSystem::ReadRefFromRing(
        void *buf __unused,
        uint32_t datasz __unused,
        void* DLtime __unused
        )
{
  return 0;
}

int AudioSystem::GetVoiceUnlockULTime(void* DLtime __unused)
{
  return 0;
}

void AudioSystem::freeVoiceUnlockDLInstance()
{
  return;
}

class IATVCtrlClient
{
};

class IATVCtrlService: public IInterface
{
public:
  DECLARE_META_INTERFACE(ATVCtrlService);
};

class BpATVCtrlService : public BpInterface<IATVCtrlService>
{
public:
    BpATVCtrlService(const sp<IBinder>& impl)
        : BpInterface<IATVCtrlService>(impl)
    {
    }
    virtual ~BpATVCtrlService()
    {
    }
    virtual int ATVCS_matv_init()
    {
        return 0;
    }
    virtual int ATVCS_matv_ps_init(int on __unused)
    {
        return 0;
    }
    virtual int ATVCS_matv_set_parameterb(int in __unused)
    {
        return 0;
    }
    virtual int ATVCS_matv_suspend(int on __unused)
    {
        return 0;
    }
    virtual int ATVCS_matv_shutdown()
    {
        return 0;
    }
    virtual void ATVCS_matv_chscan(int mode __unused)
    {
    }
    virtual void ATVCS_matv_chscan_stop()
    {
    }
    virtual int ATVCS_matv_get_chtable(int ch __unused, void *entry __unused, int len __unused)
    {
        return 0;
    }
    virtual int ATVCS_matv_set_chtable(int ch __unused, void *entry __unused, int len __unused)
    {
        return 0;
    }
    virtual int ATVCS_matv_clear_chtable()
    {
        return 0;
    }
    virtual void ATVCS_matv_change_channel(int ch __unused)
    {
    }
    virtual void ATVCS_matv_set_country(int country __unused)
    {
    }
    virtual void ATVCS_matv_set_tparam(int mode __unused)
    {
    }
    virtual void ATVCS_matv_audio_play()
    {
    }
    virtual void ATVCS_matv_audio_stop()
    {
    }
    virtual int ATVCS_matv_audio_get_format()
    {
        return 0;
    }
    virtual void ATVCS_matv_audio_set_format(int val __unused)
    {
    }
    virtual int ATVCS_matv_audio_get_sound_system()
    {
        return 0;
    }
    virtual int ATVCS_matv_adjust(int item __unused, int val __unused)
    {
        return 0;
    }
    virtual int ATVCS_matv_get_chipdep(int item __unused)
    {
        return 0;
    }
    virtual int ATVCS_matv_set_chipdep(int item __unused, int val __unused)
    {
        return 0;
    }
    virtual void ATVCS_matv_register_callback()
    {
    }
    virtual void registerClient(const sp<IATVCtrlClient>& client __unused)
    {
    }
    virtual void registerClient_FM(const sp<IATVCtrlClient>& client __unused)
    {
    }
    virtual void CLI(char input __unused)
    {
    }
    virtual int ATVCS_fm_powerup(void *parm __unused, int len __unused)
    {
        return 0;
    }
    virtual int ATVCS_fm_powerdown()
    {
        return 0;
    }
    virtual int ATVCS_fm_getrssi()
    {
        return 0;
    }
    virtual int ATVCS_fm_tune(void *parm __unused, int len __unused)
    {
        return 0;
    }
    virtual int ATVCS_fm_seek(void *parm __unused, int len __unused)
    {
        return 0;
    }
    virtual int ATVCS_fm_scan(void *parm __unused, int len __unused)
    {
        return 0;
    }
    virtual int ATVCS_fm_mute(int val __unused)
    {
        return 0;
    }
    virtual int ATVCS_fm_getchipid()
    {
        return 0;
    }
    virtual int ATVCS_fm_isFMPowerUp()
    {
        return 0;
    }
};

IMPLEMENT_META_INTERFACE(ATVCtrlService, "android.media.IATVCtrlService");

} // namespace
