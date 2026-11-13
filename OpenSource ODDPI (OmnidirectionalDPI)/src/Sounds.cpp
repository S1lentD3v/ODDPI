#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>

#include "Sounds.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace oddpi::sfx {

namespace {

    constexpr int kRate = 44100;
    constexpr double kTau = 6.28318530717958647692;

    bool g_enabled = false;
    double g_volume = 0.6;
    std::vector<std::vector<BYTE>> g_wavs;   // one WAV file image per Sound
    double g_builtVolume = -1;

    std::vector<BYTE> ToWav(const std::vector<double>& x, double volume)
    {
        double peak = 1e-9;
        for (double v : x) peak = std::max(peak, std::fabs(v));
        const double gain = 0.9 * volume / peak;

        const uint32_t dataBytes = uint32_t(x.size() * 2);
        std::vector<BYTE> wav(44 + dataBytes);
        auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
        auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
        std::memcpy(&wav[0], "RIFF", 4);
        put32(4, 36 + dataBytes);
        std::memcpy(&wav[8], "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);              // PCM
        put16(22, 1);              // mono
        put32(24, kRate);
        put32(28, kRate * 2);
        put16(32, 2);
        put16(34, 16);
        std::memcpy(&wav[36], "data", 4);
        put32(40, dataBytes);
        for (size_t i = 0; i < x.size(); ++i) {
            const double v = std::clamp(x[i] * gain, -1.0, 1.0);
            const int16_t s = int16_t(std::lround(v * 32767));
            std::memcpy(&wav[44 + i * 2], &s, 2);
        }
        return wav;
    }

    std::vector<double> Buffer(double seconds) { return std::vector<double>(size_t(seconds * kRate), 0.0); }

    double Attack(double t, double a) { return t < a ? t / a : 1.0; }

    // Crisp "pop": a fast downward pitch sweep, a soft low body and a tiny click transient.
    std::vector<double> Hit()
    {
        auto x = Buffer(0.12);
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> noise(-1, 1);
        double ph = 0, body = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = double(i) / kRate;
            const double f = 620 + 1100 * std::exp(-t / 0.016);
            ph += kTau * f / kRate;
            body += kTau * 190 / kRate;
            const double env = Attack(t, 0.0015) * std::exp(-t / 0.030);
            double v = (std::sin(ph) + 0.30 * std::sin(2 * ph)) * env;
            v += 0.45 * std::sin(body) * Attack(t, 0.002) * std::exp(-t / 0.045);
            if (t < 0.004) v += 0.25 * noise(rng) * (1 - t / 0.004);
            x[i] = v;
        }
        return x;
    }

    // Soft dull thud.
    std::vector<double> Miss()
    {
        auto x = Buffer(0.10);
        std::mt19937 rng(11);
        std::uniform_real_distribution<double> noise(-1, 1);
        double ph = 0, lp = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double t = double(i) / kRate;
            ph += kTau * (95 + 60 * std::exp(-t / 0.02)) / kRate;
            lp += 0.08 * (noise(rng) - lp);
            const double env = Attack(t, 0.003) * std::exp(-t / 0.032);
            x[i] = (std::sin(ph) + 0.6 * lp) * env * 0.55;
        }
        return x;
    }

    void Bell(std::vector<double>& x, double start, double freq, double decay, double amp)
    {
        double ph = 0;
        for (size_t i = size_t(start * kRate); i < x.size(); ++i) {
            const double t = double(i) / kRate - start;
            ph += kTau * freq / kRate;
            const double env = Attack(t, 0.003) * std::exp(-t / decay);
            x[i] += amp * env * (std::sin(ph) + 0.25 * std::sin(2 * ph) + 0.08 * std::sin(3 * ph));
        }
    }

    std::vector<double> Switch()
    {
        auto x = Buffer(0.40);
        Bell(x, 0.00, 659.25, 0.09, 1.0);
        Bell(x, 0.09, 987.77, 0.12, 0.9);
        return x;
    }

    std::vector<double> Start()
    {
        auto x = Buffer(0.30);
        Bell(x, 0.0, 880.0, 0.10, 1.0);
        Bell(x, 0.0, 1320.0, 0.07, 0.35);
        return x;
    }

    std::vector<double> Tick()
    {
        auto x = Buffer(0.12);
        Bell(x, 0.0, 587.33, 0.035, 1.0);
        return x;
    }

    std::vector<double> Done()
    {
        auto x = Buffer(0.95);
        const double notes[] = { 523.25, 659.25, 783.99, 1046.50 };
        for (int i = 0; i < 4; ++i) Bell(x, 0.085 * i, notes[i], i == 3 ? 0.30 : 0.18, i == 3 ? 1.0 : 0.8);
        return x;
    }

    void Build()
    {
        if (g_builtVolume == g_volume && !g_wavs.empty()) return;
        g_wavs = { ToWav(Hit(), g_volume * 0.9), ToWav(Miss(), g_volume * 0.6), ToWav(Switch(), g_volume * 0.7),
                   ToWav(Start(), g_volume * 0.7), ToWav(Tick(), g_volume * 0.55), ToWav(Done(), g_volume * 0.75) };
        g_builtVolume = g_volume;
    }
}

void Configure(bool enabled, double volume)
{
    g_enabled = enabled && volume > 0;
    g_volume = std::clamp(volume, 0.0, 1.0);
    if (g_enabled) Build();
}

void Play(Sound s)
{
    if (!g_enabled) return;
    Build();
    const auto& wav = g_wavs[size_t(s)];
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

}  // namespace oddpi::sfx
