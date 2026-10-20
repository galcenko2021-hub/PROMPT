#include "audio.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xaudio2.h>

#include <cmath>
#include <unordered_map>
#include <vector>

#include "core.h"

namespace audio {
namespace {

constexpr int RATE = 22050;
constexpr int VOICES = 12;

enum class Wave { Square, Triangle, Sine, Noise };

struct Note {
    float freq, duration;
};

IXAudio2* g_xaudio = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voices[VOICES] = {};
int g_next = 0;
std::unordered_map<std::string, std::vector<int16_t>> g_sounds;

float sample(Wave w, float phase) {
    const float p = phase - std::floor(phase);
    switch (w) {
        case Wave::Square: return p < 0.5f ? 1.f : -1.f;
        case Wave::Triangle: return p < 0.5f ? 4 * p - 1 : 3 - 4 * p;
        case Wave::Noise: return rng().uniform(-1.f, 1.f);
        default: return std::sin(p * TAU);
    }
}

std::vector<int16_t> synth(std::initializer_list<Note> notes, Wave w, float volume, float sweepTo = 0.f) {
    std::vector<int16_t> out;
    float phase = 0.f;
    for (const Note& n : notes) {
        const int count = std::max(1, static_cast<int>(RATE * n.duration));
        for (int i = 0; i < count; ++i) {
            const float t = static_cast<float>(i) / count;
            const float freq = sweepTo > 0.f ? n.freq + (sweepTo - n.freq) * t : n.freq;
            phase += freq / RATE;
            const float env = std::min(1.f, i / 60.f) * std::pow(1.f - t, 1.6f);
            out.push_back(static_cast<int16_t>(sample(w, phase) * env * volume * 32767.f));
        }
    }
    return out;
}

}  // namespace

bool init() {
    if (FAILED(XAudio2Create(&g_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) return false;
    if (FAILED(g_xaudio->CreateMasteringVoice(&g_master))) {
        shutdown();
        return false;
    }
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = RATE * 2;
    for (auto& v : g_voices)
        if (FAILED(g_xaudio->CreateSourceVoice(&v, &fmt))) v = nullptr;

    g_sounds["blip"] = synth({{880, 0.05f}}, Wave::Square, 0.12f);
    g_sounds["select"] = synth({{660, 0.04f}, {990, 0.05f}}, Wave::Square, 0.12f);
    g_sounds["summon"] = synth({{440, 0.06f}, {554, 0.06f}, {659, 0.06f}, {880, 0.12f}}, Wave::Triangle, 0.3f);
    g_sounds["error"] = synth({{180, 0.09f}, {130, 0.14f}}, Wave::Square, 0.16f);
    g_sounds["success"] = synth({{523, 0.07f}, {659, 0.07f}, {784, 0.07f}, {1046, 0.16f}}, Wave::Triangle, 0.3f);
    g_sounds["door"] = synth({{200, 0.45f}}, Wave::Triangle, 0.3f, 640.f);
    g_sounds["detected"] = synth({{880, 0.08f}, {660, 0.08f}, {880, 0.08f}, {660, 0.12f}}, Wave::Square, 0.16f);
    g_sounds["mark"] = synth({{1250, 0.035f}}, Wave::Square, 0.1f);
    g_sounds["push"] = synth({{110, 0.12f}}, Wave::Noise, 0.12f);
    g_sounds["hit"] = synth({{90, 0.35f}}, Wave::Noise, 0.3f);
    g_sounds["glitch"] = synth({{300, 0.16f}}, Wave::Noise, 0.18f);
    g_sounds["beam"] = synth({{300, 0.32f}}, Wave::Triangle, 0.22f, 1300.f);
    g_sounds["rec"] = synth({{1000, 0.05f}, {1500, 0.05f}}, Wave::Sine, 0.25f);
    g_sounds["think"] = synth({{520, 0.05f}, {780, 0.05f}, {650, 0.06f}}, Wave::Sine, 0.18f);
    g_sounds["learn"] = synth({{660, 0.06f}, {880, 0.06f}, {1320, 0.1f}}, Wave::Sine, 0.22f);
    g_sounds["upgrade"] = synth({{392, 0.08f}, {523, 0.08f}, {659, 0.08f}, {784, 0.08f}, {1046, 0.08f}, {1318, 0.22f}}, Wave::Triangle, 0.3f);
    g_sounds["victory"] =
        synth({{523, 0.12f}, {659, 0.12f}, {784, 0.12f}, {1046, 0.2f}, {784, 0.1f}, {1046, 0.35f}}, Wave::Triangle, 0.32f);
    g_sounds["reshuffle"] = synth({{900, 0.3f}}, Wave::Triangle, 0.2f, 200.f);
    const float tones[4] = {523, 659, 784, 988};
    for (int i = 0; i < 4; ++i) g_sounds["tone" + std::to_string(i)] = synth({{tones[i], 0.2f}}, Wave::Sine, 0.35f);
    return true;
}

void shutdown() {
    for (auto& v : g_voices)
        if (v) {
            v->DestroyVoice();
            v = nullptr;
        }
    if (g_master) g_master->DestroyVoice();
    g_master = nullptr;
    if (g_xaudio) g_xaudio->Release();
    g_xaudio = nullptr;
}

void play(const std::string& name) {
    if (!g_xaudio) return;
    auto it = g_sounds.find(name);
    if (it == g_sounds.end()) return;
    IXAudio2SourceVoice* v = g_voices[g_next];
    g_next = (g_next + 1) % VOICES;
    if (!v) return;
    v->Stop();
    v->FlushSourceBuffers();
    XAUDIO2_BUFFER buf{};
    buf.AudioBytes = static_cast<UINT32>(it->second.size() * sizeof(int16_t));
    buf.pAudioData = reinterpret_cast<const BYTE*>(it->second.data());
    buf.Flags = XAUDIO2_END_OF_STREAM;
    v->SubmitSourceBuffer(&buf);
    v->Start();
}

}  // namespace audio
