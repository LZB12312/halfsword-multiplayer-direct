// Direct voice chat: the Windows default or selected microphone through waveIn (16 kHz mono, sent as IMA ADPCM,
// voice/hsm_adpcm.h); playback through waveOut on its own thread. The decisions (level gate, who hears whom, gains)
// are in shared/voice/hsm_voice.h; the session feeds this class what it may send and how loud each speaker is.
// Game thread, except the audio thread inside.
#pragma once
#include "voice/hsm_voice.h"
#include "voice/hsm_voice_ui.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hsm::game {

struct VoiceSettings {
    voice::Mic mic = voice::Mic::PushToTalk;
    float levelDb = -40.f;
    int key = 'V';
    float volume = 1.f;
    std::string device;   // a waveIn device's name; empty: the Windows default (through Steam when Steam is running)
};

class Voice {
public:
    ~Voice() { Stop(); }
    void Configure(const VoiceSettings& s);
    const VoiceSettings& settings() const { return set_; }

    struct CaptureOut {
        std::vector<std::vector<uint8_t>> chunks;   // to send, oldest first
        bool spurt = false;                         // the first of them starts a talk spurt
        bool adpcm = false;                         // they are IMA ADPCM (a picked device), not Steam-compressed
    };
    // Each frame. mayTalk: this machine's fighter may talk now (voice on at the host, alive, conscious enough).
    // keyAllowed: the game window is in front and nobody is typing (push to talk reads the key only then). meter: a menu
    // shows the microphone, so record for its level meter and HEAR MYSELF even while nothing may be sent.
    void Capture(bool mayTalk, bool keyAllowed, bool meter, uint32_t nowMs, CaptureOut& out);

    // A chunk from a speaker (an entity) that this machine may hear. Late or repeated chunks are dropped.
    void Receive(uint8_t entity, uint16_t seq, bool adpcm, const uint8_t* data, size_t size, uint32_t nowMs);
    void SetGains(uint8_t entity, voice::Gains g);
    void Forget(uint8_t entity);
    bool Talking(uint8_t entity, uint32_t nowMs, uint32_t withinMs = 300) const;   // heard in the last withinMs
    bool MeTalking() const { return sending_; }

    // HEAR MYSELF: what passes the gate (or the key) is played back here, through the same codec as it is sent.
    void SetMonitor(bool on);
    bool Monitor() const { return monitor_; }
    float MicLevelDb(uint32_t nowMs) const { return levelAt_ && nowMs - levelAt_ <= 300 ? level_ : -120.f; }
    bool GateOpen() const { return gateOpen_; }
    bool Recording() const { return recording_; }
    // The recording devices Windows lists now (refreshed at most every 2 s), by name.
    const std::vector<std::string>& Devices(uint32_t nowMs);
    // What records now: "Steam" (the Windows default), a device's name, "Windows default" (no Steam), or empty.
    std::string SourceName() const;

    void Stop();   // recording and the audio device off, every queue cleared (the session ended, the menu closed)

    struct Stats {
        uint32_t sent = 0, sentBytes = 0, gateOpenMs = 0, received = 0, receivedBytes = 0, late = 0, badChunks = 0;
        uint64_t mixerDropped = 0;
        bool recording = false, device = false, adpcm = false;
    };
    Stats TakeStats();   // since the last call

private:
    enum class Source : uint8_t { None, Steam, WaveIn };
    bool StartDevice();
    void StopDevice();
    void AudioLoop();
    void SetRecording(Source s, int waveInIndex);
    bool OpenWaveIn(int index);
    void CloseWaveIn();
    void PollWaveIn();
    // One recorded chunk: through the gate, maybe to the monitor, maybe out. pcm24: what it sounds like at 24 kHz.
    void Chunk(std::vector<uint8_t> enc, const std::vector<int16_t>* pcm24, float db, bool keyDown, bool mayTalk, uint32_t nowMs, CaptureOut& out);

    VoiceSettings set_;
    voice::LevelGate gate_;
    voice::PreRoll preroll_;
    bool recording_ = false, sending_ = false, steamWarned_ = false, monitor_ = false, gateOpen_ = false;
    Source source_ = Source::None;
    int waveInIndex_ = -2;
    uint32_t lastCaptureMs_ = 0;
    float level_ = -120.f;
    uint32_t levelAt_ = 0;
    std::vector<int16_t> pcm_;
    std::array<voice::SeqGuard, voice::kMaxSpeakers> seq_{};
    std::array<uint32_t, voice::kMaxSpeakers> heardAt_{};
    Stats stats_;
    uint64_t mixerDroppedSeen_ = 0;
    std::vector<std::string> devices_;
    uint32_t devicesAt_ = 0;
    bool devicesRead_ = false;
    std::string missingWarned_;

    // waveIn (a picked device): eight 20 ms buffers, polled each frame.
    void* waveIn_ = nullptr;   // HWAVEIN
    struct InBuf;
    std::vector<InBuf*> inBufs_;
    size_t inNext_ = 0;
    std::vector<int16_t> inAcc_;   // 16 kHz samples not yet in a chunk

    mutable std::mutex mu_;          // guards mixer_ (game thread pushes, audio thread mixes)
    voice::Mixer mixer_;
    std::thread audio_;
    std::atomic<bool> audioRun_{false};
    std::atomic<bool> deviceFailed_{false};   // set by the audio thread: no output device, do not retry
};

}  // namespace hsm::game
