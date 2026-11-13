#pragma once

// Small synthesized sound effects for the trainers (no audio files shipped).
// Played asynchronously with PlaySound from memory; a new sound replaces the one playing.

namespace oddpi::sfx {

    enum class Sound { Hit, Miss, Switch, Start, Tick, Done };

    // enabled = false makes Play a no-op. volume 0..1. Safe to call repeatedly.
    void Configure(bool enabled, double volume);
    void Play(Sound s);
}
