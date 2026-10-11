#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
// Orbis audio shims: cubeb/SDL drivers don't exist here (libSceAudioOut later).
#include "Host/AudioStream.h"
#include "common/Error.h"
#include "USB/usb-mic/audiodev.h"
#include "USB/usb-mic/audiodev-cubeb.h"
#include "ProsperoMic.h" // vk-285-141

// (AudioDevice::CreateDevice/GetInputDeviceList live in usb-mic.cpp already.)

std::vector<std::pair<std::string, std::string>> AudioStream::GetCubebDriverNames()
{
  return {};
}

std::vector<AudioStream::DeviceInfo> AudioStream::GetCubebOutputDevices(const char* driver)
{
  (void)driver;
  return {};
}

// eerec-256: real audio via libSceAudioOut (float stereo, 48 kHz, 256-frame grains, own thread).
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void* p);
int sceAudioOutClose(int handle);
}

namespace
{
class OrbisAudioStream final : public AudioStream
{
public:
  OrbisAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
    : AudioStream(sample_rate, parameters)
  {
  }
  ~OrbisAudioStream() override
  {
    m_quit.store(true);
    if (m_thread.joinable())
      m_thread.join();
    if (m_handle >= 0)
      sceAudioOutClose(m_handle);
  }
  void SetPaused(bool paused) override { m_paused = paused; }

  bool Open(bool stretch_enabled, Error* error)
  {
    static bool s_inited = false;
    if (!s_inited)
    {
      const int r = sceAudioOutInit();
      std::printf("[audio] sceAudioOutInit=%x\n", r);
      s_inited = true;
    }
    // userId 255 = system, port type 0 = main, param 4 = FLOAT_STEREO (2 = S16_8CH, 1 = S16_STEREO)
    m_handle = sceAudioOutOpen(255, 0, 0, GRAIN, m_sample_rate, 4);
    std::printf("[audio] sceAudioOutOpen rate=%u ch=%u -> %x\n", m_sample_rate, (unsigned)m_output_channels, m_handle);
    std::fflush(stdout);
    if (m_handle < 0)
    {
      Error::SetString(error, "sceAudioOutOpen failed");
      return false;
    }
    BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
    m_thread = std::thread([this]() { Run(); });
    return true;
  }

private:
  static constexpr u32 GRAIN = 256;
  void Run()
  {
    alignas(64) float buf[GRAIN * 2];
    while (!m_quit.load(std::memory_order_relaxed))
    {
      if (m_paused)
        std::memset(buf, 0, sizeof(buf));
      else
        ReadFrames(buf, GRAIN);
      sceAudioOutOutput(m_handle, buf); // blocks until the previous grain has been consumed
    }
  }
  int m_handle = -1;
  std::atomic<bool> m_quit{false};
  std::thread m_thread;
};
} // namespace

std::unique_ptr<AudioStream> AudioStream::CreateCubebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
  const char* driver_name, const char* device_name, bool stretch_enabled, Error* error)
{
  (void)driver_name;
  (void)device_name;
  AudioStreamParameters p = parameters;
  p.expansion_mode = AudioExpansionMode::Disabled;
  auto stream = std::make_unique<OrbisAudioStream>(sample_rate, p);
  if (!stream->Open(stretch_enabled, error))
    return nullptr;
  return stream;
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
  bool stretch_enabled, Error* error)
{
  (void)sample_rate;
  (void)parameters;
  (void)stretch_enabled;
  Error::SetString(error, "SDL audio unavailable on Orbis");
  return nullptr;
}

// No cubeb on Orbis. vk-285-141 (AI-assisted): the USB microphone's and headset's audio source is the controller's microphone
// (ProsperoMic.cpp); a sink (the headset's speaker) takes what it is given and plays nothing. mDeviceId, which cubeb would
// use, marks a started source here (the header stays PCSX2's).
namespace usb_mic
{
namespace audiodev_cubeb
{
static const int kStarted = 1;
CubebAudioDevice::CubebAudioDevice(AudioDir dir, u32 channels, std::string devname, s32 latency)
  : AudioDevice(dir, channels)
  , mDeviceName(std::move(devname))
{
  (void)latency;
  mContext = nullptr;
  mDeviceId = nullptr;
  std::printf("[mic] USB audio %s \"%s\", %u channel%s\n", dir == AUDIODIR_SOURCE ? "source" : "sink", mDeviceName.c_str(),
    (unsigned)channels, channels == 1 ? "" : "s");
}
CubebAudioDevice::~CubebAudioDevice()
{
  Stop();
}
std::vector<std::pair<std::string, std::string>> CubebAudioDevice::GetDeviceList(bool input)
{
  if (input)
    return {{"DualSense", "The controller's microphone"}};
  return {};
}
uint32_t CubebAudioDevice::GetBuffer(int16_t* buff, uint32_t frames)
{
  if (mAudioDir != AUDIODIR_SOURCE || !mDeviceId || !buff)
    return 0;
  return static_cast<uint32_t>(orbis_mic::Read(static_cast<int>(mSampleRate), buff, frames, GetChannels()));
}
uint32_t CubebAudioDevice::SetBuffer(int16_t* buff, uint32_t frames)
{
  (void)buff;
  return frames; // the headset's speaker: taken and dropped
}
bool CubebAudioDevice::GetFrames(uint32_t* size)
{
  if (size)
    *size = (mAudioDir == AUDIODIR_SOURCE && mDeviceId) ? static_cast<uint32_t>(orbis_mic::Available(static_cast<int>(mSampleRate))) : 0;
  return true;
}
void CubebAudioDevice::SetResampling(int samplerate)
{
  if (samplerate > 0)
    mSampleRate = static_cast<u32>(samplerate);
  std::printf("[mic] USB audio %s at %d Hz\n", mAudioDir == AUDIODIR_SOURCE ? "source" : "sink", samplerate);
  if (mAudioDir == AUDIODIR_SOURCE && mDeviceId)
    orbis_mic::Reset();
}
bool CubebAudioDevice::Start()
{
  if (mAudioDir == AUDIODIR_SOURCE && !mDeviceId)
  {
    orbis_mic::Acquire();
    mDeviceId = &kStarted;
  }
  return true;
}
void CubebAudioDevice::Stop()
{
  if (mAudioDir == AUDIODIR_SOURCE && mDeviceId)
  {
    mDeviceId = nullptr;
    orbis_mic::Release();
  }
}
void CubebAudioDevice::ResetBuffers()
{
  if (mAudioDir == AUDIODIR_SOURCE && mDeviceId)
    orbis_mic::Reset();
}
long CubebAudioDevice::DataCallback(struct cubeb_stream* stream, void* user_ptr, void const* input_buffer,
  void* output_buffer, long nframes)
{
  (void)stream;
  (void)user_ptr;
  (void)input_buffer;
  (void)output_buffer;
  return nframes;
}
} // namespace audiodev_cubeb
} // namespace usb_mic
