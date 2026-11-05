#include "gameplay/voice.h"
#include "log.h"
#include "net/direct_api.h"
#include "voice/hsm_adpcm.h"
#include <windows.h>
#include <mmsystem.h>

namespace hsm::game {
namespace {
constexpr size_t kBufferFrames = 480;   // 20 ms at 24 kHz
constexpr int kBuffers = 4;
constexpr size_t kInBufSamples = 320;   // 20 ms at 16 kHz
constexpr int kInBufs = 8;
constexpr size_t kSelf = 0;             // the mixer's speaker for HEAR MYSELF (fighters are entities 1..)

std::string Utf8(const wchar_t* w) {
    char out[256];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof out, nullptr, nullptr)) return {};
    return out;
}
}  // namespace

struct Voice::InBuf { WAVEHDR hdr{}; std::array<int16_t, kInBufSamples> pcm{}; };

void Voice::Configure(const VoiceSettings& s) {
    const bool deviceChanged = s.device != set_.device;
    set_ = s;
    gate_.Configure({s.levelDb, 400});
    if (deviceChanged && source_ != Source::None) SetRecording(Source::None, -2);   // the next Capture opens the new one
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.SetVolume(s.volume);
}

void Voice::SetMonitor(bool on) {
    monitor_ = on;
    if (!on) { std::lock_guard<std::mutex> lock(mu_); mixer_.Forget(kSelf); }
}

const std::vector<std::string>& Voice::Devices(uint32_t nowMs) {
    if (devicesRead_ && nowMs - devicesAt_ < 2000) return devices_;
    devicesRead_ = true; devicesAt_ = nowMs;
    devices_.clear();
    const UINT n = waveInGetNumDevs();
    for (UINT i = 0; i < n && i < 32; ++i) {
        WAVEINCAPSW caps{};
        devices_.push_back(waveInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR ? Utf8(caps.szPname) : std::string());
    }
    return devices_;
}

std::string Voice::SourceName() const {
    if (source_ == Source::Steam) return "Steam";
    if (source_ != Source::WaveIn) return {};
    if (waveInIndex_ >= 0 && size_t(waveInIndex_) < devices_.size()) return devices_[size_t(waveInIndex_)];
    return "Windows default";
}

void Voice::SetRecording(Source s, int waveInIndex) {
    if (s == source_ && (s != Source::WaveIn || waveInIndex == waveInIndex_)) return;
    if (source_ == Source::Steam) direct::StopVoice();
    if (source_ == Source::WaveIn) CloseWaveIn();
    source_ = Source::None; waveInIndex_ = -2;
    gate_.Reset(); preroll_.Clear(); sending_ = false; gateOpen_ = false; inAcc_.clear();
    if (s == Source::Steam) { direct::StartVoice(); source_ = s; }
    else if (s == Source::WaveIn && OpenWaveIn(waveInIndex)) { source_ = s; waveInIndex_ = waveInIndex; }
    recording_ = source_ != Source::None;
}

bool Voice::OpenWaveIn(int index) {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 1; fmt.nSamplesPerSec = voice::kAdpcmRate; fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2; fmt.nAvgBytesPerSec = voice::kAdpcmRate * 2;
    HWAVEIN h = nullptr;
    const MMRESULT r = waveInOpen(&h, index >= 0 ? UINT(index) : WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (r != MMSYSERR_NOERROR) {
        HSM_LOG("voice: could not open the microphone (waveInOpen %u, device %d)", unsigned(r), index);
        return false;
    }
    waveIn_ = h;
    for (int i = 0; i < kInBufs; ++i) {
        InBuf* b = new InBuf();
        b->hdr.lpData = reinterpret_cast<LPSTR>(b->pcm.data());
        b->hdr.dwBufferLength = DWORD(b->pcm.size() * sizeof(int16_t));
        waveInPrepareHeader(h, &b->hdr, sizeof(WAVEHDR));
        waveInAddBuffer(h, &b->hdr, sizeof(WAVEHDR));
        inBufs_.push_back(b);
    }
    inNext_ = 0;
    waveInStart(h);
    HSM_LOG("voice: recording from %s (16 kHz, sent as ADPCM)", index >= 0 ? "the picked device" : "the Windows default device (no Steam voice)");
    return true;
}

void Voice::CloseWaveIn() {
    HWAVEIN h = static_cast<HWAVEIN>(waveIn_);
    if (h) {
        waveInReset(h);
        for (InBuf* b : inBufs_) waveInUnprepareHeader(h, &b->hdr, sizeof(WAVEHDR));
        waveInClose(h);
    }
    for (InBuf* b : inBufs_) delete b;
    inBufs_.clear();
    waveIn_ = nullptr;
}

// The buffers finish in the order they were added: take each one done, then give it back.
void Voice::PollWaveIn() {
    HWAVEIN h = static_cast<HWAVEIN>(waveIn_);
    if (!h || inBufs_.empty()) return;
    for (size_t k = 0; k < inBufs_.size(); ++k) {
        InBuf* b = inBufs_[inNext_];
        if (!(b->hdr.dwFlags & WHDR_DONE)) break;
        const size_t n = b->hdr.dwBytesRecorded / sizeof(int16_t);
        inAcc_.insert(inAcc_.end(), b->pcm.begin(), b->pcm.begin() + long(n < b->pcm.size() ? n : b->pcm.size()));
        b->hdr.dwFlags &= ~DWORD(WHDR_DONE);
        b->hdr.dwBytesRecorded = 0;
        waveInAddBuffer(h, &b->hdr, sizeof(WAVEHDR));
        inNext_ = (inNext_ + 1) % inBufs_.size();
    }
    const size_t keep = voice::kAdpcmChunkSamples * 8;   // a long hitch: the oldest audio goes
    if (inAcc_.size() > keep) inAcc_.erase(inAcc_.begin(), inAcc_.begin() + long(inAcc_.size() - keep));
}

void Voice::Chunk(std::vector<uint8_t> enc, const std::vector<int16_t>* pcm24, float db, bool keyDown, bool mayTalk, uint32_t nowMs, CaptureOut& out) {
    if (db > -120.f) { level_ = db; levelAt_ = nowMs ? nowMs : 1; }
    const bool open = set_.mic == voice::Mic::Open ? gate_.Feed(db, nowMs) : keyDown;
    gateOpen_ = open;
    if (open && monitor_ && pcm24 && !pcm24->empty()) {
        if (!audioRun_ && !deviceFailed_) StartDevice();
        std::lock_guard<std::mutex> lock(mu_);
        mixer_.SetGains(kSelf, {1.f, 1.f});
        mixer_.Push(kSelf, pcm24->data(), pcm24->size());
    }
    if (!open || !mayTalk) {
        sending_ = false;
        if (!open && mayTalk && set_.mic == voice::Mic::Open) preroll_.Keep(std::move(enc));
        return;
    }
    if (!sending_) {
        sending_ = true;
        out.spurt = out.chunks.empty();
        for (std::vector<uint8_t>& c : preroll_.Take()) out.chunks.push_back(std::move(c));
    }
    out.chunks.push_back(std::move(enc));
}

void Voice::Capture(bool mayTalk, bool keyAllowed, bool meter, uint32_t nowMs, CaptureOut& out) {
    out.chunks.clear(); out.spurt = false; out.adpcm = false;
    const bool keyDown = set_.mic == voice::Mic::PushToTalk && keyAllowed && (GetAsyncKeyState(set_.key) & 0x8000) != 0;
    // Record while something may come of it: an open microphone while talking is allowed, the key held, or a menu's meter.
    const bool want = set_.mic != voice::Mic::Off && (meter || (mayTalk && (set_.mic == voice::Mic::Open || keyDown)));
    Source src = Source::None;
    int index = -1;
    if (want) {
        index = set_.device.empty() ? -1 : voice::DeviceIndex(Devices(nowMs), set_.device);
        if (!set_.device.empty() && index < 0 && missingWarned_ != set_.device) {
            missingWarned_ = set_.device;
            HSM_LOG("voice: the picked microphone is not connected; using the Windows default");
        }
        if (index >= 0) src = Source::WaveIn;
        else if (direct::VoiceAvailable()) src = Source::Steam;
        else {
            if (mayTalk && !steamWarned_) { steamWarned_ = true; HSM_LOG("voice: Steam voice is not available here; recording the Windows default device"); }
            src = Source::WaveIn;
        }
    }
    if (src != source_ || (src == Source::WaveIn && index != waveInIndex_)) {
        if ((src != Source::None) != recording_) HSM_LOG("voice: microphone %s", src != Source::None ? "on" : "off");
        SetRecording(src, index);
    }
    const uint32_t dt = lastCaptureMs_ ? nowMs - lastCaptureMs_ : 0;
    lastCaptureMs_ = nowMs;
    if (sending_) stats_.gateOpenMs += dt;
    if (source_ == Source::None) { sending_ = false; gateOpen_ = false; return; }

    if (source_ == Source::Steam) {
        std::vector<uint8_t> chunk;
        if (!direct::TakeVoice(chunk) || chunk.empty()) {
            if (sending_ && (!mayTalk || (set_.mic == voice::Mic::Open && !gate_.Feed(-120.f, nowMs)) || (set_.mic == voice::Mic::PushToTalk && !keyDown))) sending_ = false;
            if (set_.mic == voice::Mic::PushToTalk) gateOpen_ = keyDown;
            return;
        }
        const bool needPcm = set_.mic == voice::Mic::Open || meter || monitor_;
        float db = -120.f;
        const bool decoded = needPcm && direct::DecompressVoice(chunk.data(), chunk.size(), voice::kSampleRate, pcm_);
        if (decoded) db = voice::RmsDbfs(pcm_.data(), pcm_.size());
        Chunk(std::move(chunk), decoded ? &pcm_ : nullptr, db, keyDown, mayTalk, nowMs, out);
    } else {
        out.adpcm = true;
        PollWaveIn();
        std::vector<int16_t> dec, up;
        while (inAcc_.size() >= voice::kAdpcmChunkSamples) {
            const int16_t* p = inAcc_.data();
            const float db = voice::RmsDbfs(p, voice::kAdpcmChunkSamples);
            std::vector<uint8_t> enc = voice::EncodeAdpcm(p, voice::kAdpcmChunkSamples);
            const std::vector<int16_t>* mon = nullptr;
            if (monitor_ && voice::DecodeAdpcm(enc.data(), enc.size(), dec)) { voice::Upsample16To24(dec.data(), dec.size(), up); mon = &up; }
            inAcc_.erase(inAcc_.begin(), inAcc_.begin() + long(voice::kAdpcmChunkSamples));
            Chunk(std::move(enc), mon, db, keyDown, mayTalk, nowMs, out);
        }
        if (set_.mic == voice::Mic::PushToTalk) { gateOpen_ = keyDown; if (!keyDown || !mayTalk) sending_ = false; }
    }
    for (const std::vector<uint8_t>& c : out.chunks) { ++stats_.sent; stats_.sentBytes += uint32_t(c.size()); }
    if (!out.chunks.empty()) stats_.adpcm = out.adpcm;
}

void Voice::Receive(uint8_t entity, uint16_t seq, bool adpcm, const uint8_t* data, size_t size, uint32_t nowMs) {
    if (entity == kSelf || entity >= voice::kMaxSpeakers) return;
    if (!seq_[entity].Accept(seq, nowMs)) { ++stats_.late; return; }
    std::vector<int16_t> pcm;
    if (adpcm) {
        std::vector<int16_t> raw;
        if (!voice::DecodeAdpcm(data, size, raw)) { ++stats_.badChunks; return; }
        voice::Upsample16To24(raw.data(), raw.size(), pcm);
    } else if (!direct::DecompressVoice(data, size, voice::kSampleRate, pcm)) {
        ++stats_.badChunks; return;
    }
    if (pcm.empty()) { ++stats_.badChunks; return; }
    ++stats_.received; stats_.receivedBytes += uint32_t(size);
    heardAt_[entity] = nowMs ? nowMs : 1;
    if (!audioRun_ && !deviceFailed_) StartDevice();
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.Push(entity, pcm.data(), pcm.size());
}

void Voice::SetGains(uint8_t entity, voice::Gains g) {
    if (entity == kSelf) return;
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.SetGains(entity, g);
}

void Voice::Forget(uint8_t entity) {
    if (entity == kSelf || entity >= voice::kMaxSpeakers) return;
    seq_[entity].Reset(); heardAt_[entity] = 0;
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.Forget(entity);
}

bool Voice::Talking(uint8_t entity, uint32_t nowMs, uint32_t withinMs) const {
    return entity < voice::kMaxSpeakers && heardAt_[entity] && nowMs - heardAt_[entity] <= withinMs;
}

Voice::Stats Voice::TakeStats() {
    Stats s = stats_;
    stats_ = Stats{};
    s.recording = recording_;
    s.device = audioRun_;
    { std::lock_guard<std::mutex> lock(mu_); s.mixerDropped = mixer_.dropped() - mixerDroppedSeen_; mixerDroppedSeen_ = mixer_.dropped(); }
    return s;
}

void Voice::Stop() {
    SetRecording(Source::None, -2);
    StopDevice();
    for (voice::SeqGuard& g : seq_) g.Reset();
    heardAt_.fill(0);
    level_ = -120.f; levelAt_ = 0;
    std::lock_guard<std::mutex> lock(mu_);
    mixer_.Clear();
}

bool Voice::StartDevice() {
    if (audioRun_) return true;
    if (audio_.joinable()) audio_.join();   // a loop that ended on its own (no device)
    audioRun_ = true;
    audio_ = std::thread([this] { AudioLoop(); });
    return true;
}

void Voice::StopDevice() {
    audioRun_ = false;
    if (audio_.joinable()) audio_.join();
}

// waveOut, 24 kHz stereo, four 20 ms buffers: each one is refilled from the mixer as the device finishes it.
void Voice::AudioLoop() {
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 2; fmt.nSamplesPerSec = voice::kSampleRate; fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4; fmt.nAvgBytesPerSec = voice::kSampleRate * 4;
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT out = nullptr;
    const MMRESULT r = done ? waveOutOpen(&out, WAVE_MAPPER, &fmt, DWORD_PTR(done), 0, CALLBACK_EVENT) : MMSYSERR_ERROR;
    if (r != MMSYSERR_NOERROR) {
        HSM_LOG("voice: could not open the audio output (waveOutOpen %u); other players' voices are not played", unsigned(r));
        if (done) CloseHandle(done);
        deviceFailed_ = true;
        audioRun_ = false;
        return;
    }
    HSM_LOG("voice: audio output open (24 kHz stereo, %d x 20 ms buffers)", kBuffers);
    std::array<std::vector<int16_t>, kBuffers> buf;
    std::array<WAVEHDR, kBuffers> hdr{};
    for (int i = 0; i < kBuffers; ++i) {
        buf[i].assign(kBufferFrames * 2, 0);
        hdr[i].lpData = reinterpret_cast<LPSTR>(buf[i].data());
        hdr[i].dwBufferLength = DWORD(buf[i].size() * sizeof(int16_t));
        waveOutPrepareHeader(out, &hdr[i], sizeof(WAVEHDR));
        hdr[i].dwFlags |= WHDR_DONE;   // first pass: every buffer is free
    }
    while (audioRun_) {
        for (int i = 0; i < kBuffers; ++i) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            { std::lock_guard<std::mutex> lock(mu_); mixer_.Mix(buf[i].data(), kBufferFrames); }
            hdr[i].dwFlags &= ~DWORD(WHDR_DONE);
            waveOutWrite(out, &hdr[i], sizeof(WAVEHDR));
        }
        WaitForSingleObject(done, 50);
    }
    waveOutReset(out);
    for (WAVEHDR& h : hdr) waveOutUnprepareHeader(out, &h, sizeof(WAVEHDR));
    waveOutClose(out);
    CloseHandle(done);
    HSM_LOG("voice: audio output closed");
}

}  // namespace hsm::game
