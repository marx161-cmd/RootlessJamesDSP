// rt_config.h -- lock-free parameter handoff from the control (Binder/Lua)
// thread to the realtime audio thread, for the LSP dynamics ports.
//
// Why: every lsp_*_configure() used to run the real plugin's update_settings()
// (filter/oversampler/limiter recompute) on the Binder thread, and the audio
// thread took the engine's global pthread mutex around process() to stay
// coherent. That mutex is also held by the convolver IR / DDC / LiveProg
// loaders, so the audio thread could stall behind a multi-hundred-ms load
// (review 2026-10-21, finding 4). The real LSP plugins never do this: the
// host updates port values, and update_settings() runs on the audio thread
// right before process(). Same structure here:
//   control thread:  configure() = sanitise + publish(params)   (never blocks audio)
//   audio thread:    process()   = take(params) ? apply(params) : nothing, then DSP
// The audio thread only ever try-locks; if a publish is mid-flight it keeps
// the previous settings for one more block. apply() paths were checked for
// allocations (none: Gate/Compressor/Limiter/Oversampler/Sidechain/Delay/
// Equalizer update_settings all recompute in place).
#ifndef JDSP_RT_CONFIG_H
#define JDSP_RT_CONFIG_H

#include <atomic>

template <typename Cfg>
struct RtConfig {
    Cfg              staging{};
    std::atomic<bool> pending{false};
    std::atomic_flag  lock = ATOMIC_FLAG_INIT;

    // Control thread. Spins only against another control-thread publish or a
    // take() in progress (a few dozen ns).
    void publish(const Cfg &c) {
        while (lock.test_and_set(std::memory_order_acquire)) { }
        staging = c;
        pending.store(true, std::memory_order_release);
        lock.clear(std::memory_order_release);
    }
    // Audio thread. Never blocks: returns false if nothing is pending or a
    // publish is in flight (retry next block).
    bool take(Cfg &out) {
        if (!pending.load(std::memory_order_acquire)) return false;
        if (lock.test_and_set(std::memory_order_acquire)) return false;
        out = staging;
        pending.store(false, std::memory_order_relaxed);
        lock.clear(std::memory_order_release);
        return true;
    }
    // Control thread read-back of the last published values (so a
    // get->modify->configure sequence from Lua sees its own writes even
    // before the audio thread applied them).
    Cfg peek() {
        while (lock.test_and_set(std::memory_order_acquire)) { }
        Cfg c = staging;
        lock.clear(std::memory_order_release);
        return c;
    }
};

#endif
