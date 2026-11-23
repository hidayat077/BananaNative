// nn::codec::HardwareOpusDecoder: the real device decodes in hardware via the hwopus service (the game's audio and cutscene tracks are all Opus; this is not an online feature).
// Host: the constructor instead constructs the SDK's software OpusDecoder in the same memory (the sdk bundles libopus, native ARM).
// Basis: both are implementations of nn::codec::IOpusDecoder, with the 10 vtable slots identical one by one in order and signature (destructor×2, GetWorkBufferSize, IsInitialized,
// Initialize, Finalize, GetSampleRate, GetChannelCount, Reset, DecodeInterleaved); the game allocates 0x40 bytes and the software object is 0x30 bytes.
#include "hle.h"
#include <stdatomic.h>

HLE_FORCE("_ZN2nn5codec19HardwareOpusDecoderC1Ei", "hwopus service; replaced by the SDK software OpusDecoder (same IOpusDecoder virtual interface)",
          void, h_codec_HwOpusCtor, void *self, int option) {
    (void)option;
    static void (*soft_ctor)(void *);
    if (!soft_ctor && !(soft_ctor = guest_sym("_ZN2nn5codec11OpusDecoderC1Ev"))) die("missing the OpusDecoder constructor");
    soft_ctor(self);
}

// The C interface to the same hardware decoder (Tears of the Kingdom, SDK 20.5.22: GetHardwareOpusDecoderWorkBufferSize opens hwopus and aborts in sm::Initialize).
// Each call goes to the sdk's software C interface instead. The software decoder state lives in a host block kept per game decoder object (objmap), so the size of the
// game's HardwareOpusDecoderType does not matter; the game's work buffer is sized by GetOpusDecoderWorkBufferSize and handed to the software decoder as is.
#define SOFT_OPUS_STATE 0x200   // bytes for the software OpusDecoderType (larger than the SDK's)
// What the Opus decoders did, for a log that has to settle "the music is missing" or "the voices stop" without a recording. One line every 10 s while any decoder works (the figures of that
// window, then reset), and a line for the first 32 of each decoder event: its time, the interface (the older C interface / a manager's decoder) and the decoders alive after it. Nothing
// here changes what a decoder does; the counts are atomics, the line is taken by whichever thread passes the 10 s mark first
static _Atomic uint64_t g_op_dec, g_op_fail, g_op_zero, g_op_enq, g_op_refused, g_op_samples, g_op_resets, g_op_fins, g_op_inits, g_op_next_ns;
static _Atomic int g_op_peak, g_op_io_peak, g_op_live;
static int opus_pcm_peak(const int16_t *p, size_t n) { int pk = 0; for (size_t i = 0; i < n; i++) { int v = p[i] < 0 ? -p[i] : p[i]; if (v > pk) pk = v; } return pk; }
static void opus_note_pcm(const int16_t *p, size_t n) {   // one decoded frame: all-zero frames and the loudest sample of the window
    int pk = opus_pcm_peak(p, n); if (!pk) atomic_fetch_add(&g_op_zero, 1);
    int old = atomic_load(&g_op_peak); while (pk > old && !atomic_compare_exchange_weak(&g_op_peak, &old, pk)) {}
}
static void opus_tick(void) {
    uint64_t now = now_ns(), due = atomic_load(&g_op_next_ns);
    if (now < due || !atomic_compare_exchange_strong(&g_op_next_ns, &due, now + 10000000000ull)) return;
    if (!due) return;   // the first call only starts the clock
    uint64_t dec = atomic_exchange(&g_op_dec, 0), fail = atomic_exchange(&g_op_fail, 0), zero = atomic_exchange(&g_op_zero, 0), enq = atomic_exchange(&g_op_enq, 0), ref = atomic_exchange(&g_op_refused, 0),
             smp = atomic_exchange(&g_op_samples, 0), rs = atomic_exchange(&g_op_resets, 0), fi = atomic_exchange(&g_op_fins, 0), in = atomic_exchange(&g_op_inits, 0);
    int pk = atomic_exchange(&g_op_peak, 0), io = atomic_exchange(&g_op_io_peak, 0);
    if (!(dec | fail | enq | rs | fi | in)) return;
    LOG("[codec] Opus at %.1f s, the last 10 s: %llu frames decoded (%llu samples a channel, %llu failed, %llu all zero, peak %d of 32767), %llu packets enqueued (%llu refused, I/O buffers in use at most %d), %llu resets, %llu finalizes, %llu inits, %d decoders alive",
        dm_rel_s(), (unsigned long long)dec, (unsigned long long)smp, (unsigned long long)fail, (unsigned long long)zero, pk, (unsigned long long)enq, (unsigned long long)ref, io, (unsigned long long)rs, (unsigned long long)fi,
        (unsigned long long)in, atomic_load(&g_op_live));
}
static void opus_event(const char *what, const char *iface, int live_delta) {   // a decoder's init / reset / finalize, logged for the first 32 of each kind
    static _Atomic int n[3]; int k = what[0] == 'i' ? 0 : what[0] == 'r' ? 1 : 2;
    atomic_fetch_add(k == 0 ? &g_op_inits : k == 1 ? &g_op_resets : &g_op_fins, 1);
    int live = atomic_fetch_add(&g_op_live, live_delta) + live_delta;
    int c = atomic_fetch_add(&n[k], 1);
    if (c < 32) LOG("[codec] Opus decoder %s at %.1f s (%s interface, %d decoders alive)%s", what, dm_rel_s(), iface, live, c == 31 ? "; not logged from here on" : "");
}
static void *opus_fn(const char *name) { void *f = guest_sym(name); if (!f) die("sdk lacks %s", name); return f; }
static void *opus_soft(const void *hw) { void *s = objmap_get(hw); if (!s) die("Opus decoder %p was not initialized by the host", hw); return s; }
// The sdk's libopus (fixed point, nncodecLibopus_*) keeps its tables under libopus's plain names (eMeans, tf_select_table, silk_*) with default
// visibility, and the loader binds a name to the first module that defines it: in Hogwarts Legacy that is main's own libopus 1.1-beta (floating
// point), so the sdk's CELT decoder read main's float eMeans as bytes and decoded every Opus stream 15-20 dB low with three of four CELT bands
// nearly gone (speech and music muted and thin). On the console this code never runs: the hwopus device decodes. When a game module exports the
// plain libopus decoder, the hardware decoder's C interface decodes with it, one state per game decoder; DM_OPUS_SDK=1 keeps the sdk's.
// Only a definition outside sdk counts: older sdks (3.5.1, 5.5.0) export libopus under the plain opus_* names themselves, and their software
// OpusDecoder already decodes with its own tables, so that path must stay as it is.
typedef struct { uint32_t magic; int rate, ch; void *st; } popus;
#define POPUS_MAGIC 0x53554f50u
static struct { int (*size)(int); int (*init)(void *, int32_t, int); int (*dec)(void *, const uint8_t *, int32_t, int16_t *, int, int); int (*ctl)(void *, int, ...); } g_po;
static int popus_game(const void *f) { module_t *m = f ? module_of(f) : NULL; return m && strcmp(m->name, "sdk"); }
static int popus_on(void) {
    static _Atomic int on = -1; int v = atomic_load(&on); if (v >= 0) return v;
    g_po.size = guest_sym("opus_decoder_get_size"); g_po.init = guest_sym("opus_decoder_init"); g_po.dec = guest_sym("opus_decode"); g_po.ctl = guest_sym("opus_decoder_ctl");
    const char *e = getenv("DM_OPUS_SDK"); int game = popus_game(g_po.size) && popus_game(g_po.init) && popus_game(g_po.dec) && popus_game(g_po.ctl);
    v = game && !(e && atoi(e));
    if (atomic_exchange(&on, v) < 0) {
        module_t *m = g_po.dec ? module_of(g_po.dec) : NULL;
        LOG("codec: hardware Opus decoder through %s%s%s", v ? "the game's own libopus (opus_decode exported by " : "the sdk's software OpusDecoder", v && m ? m->name : "",
            v ? ")" : g_po.dec && !game ? " (the plain opus_* names are the sdk's own)" : "");
    }
    return v;
}
static popus *popus_of(const void *hw) { popus *p = objmap_get(hw); return p && p->magic == POPUS_MAGIC ? p : NULL; }
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusWorkSize, int rate, int ch, int option) {
    (void)option; size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii"); return f(rate, ch);
}
// SDK 11's form without the option (Big Brain Academy: Brain vs. Brain sizes its decoders with it early in nnMain, main+0x31f8e8; the SDK's own opens hwopus and aborts resolving the service)
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEii", "hwopus service; work buffer computed for the software decoder (SDK 11 two-argument version)", size_t, h_codec_HwOpusWorkSize2,
          int rate, int ch) {
    size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii"); return f(rate, ch);
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", "hwopus service; replaced by the software OpusDecoder", int, h_codec_HwOpusInit,
          void *hw, int rate, int ch, void *buf, size_t size, int option) {
    (void)option; int r;
    if (popus_on()) {   // the game's own libopus
        popus *p = popus_of(hw); if (!p) { if (objmap_get(hw)) die("Opus decoder %p initialized twice", hw); p = calloc(1, sizeof *p); objmap_put(hw, p); }
        int n = g_po.size(ch); free(p->st);
        *p = (popus){POPUS_MAGIC, rate, ch, n > 0 ? calloc(1, (size_t)n) : NULL};
        r = p->st && !g_po.init(p->st, rate, ch) ? 0 : 8;   // 8: the sdk's answer to a rate or channel count it does not take
        if (r) { objmap_del(hw); free(p->st); free(p); }   // a failed decoder is forgotten: its later calls answer as for one never initialized
    } else {
        int (*f)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
        void *s = objmap_get(hw); if (!s) { s = calloc(1, SOFT_OPUS_STATE); objmap_put(hw, s); }
        r = f(s, rate, ch, buf, size);
    }
    { static _Atomic int n; if (atomic_fetch_add(&n, 1) < 16) LOG("codec: hardware Opus decoder %p: %d Hz, %d channels, work buffer %#zx: result %#x (%s)", hw, rate, ch, size, r, popus_on() ? "the game's libopus" : "software OpusDecoder"); }
    if (!r) opus_event("initialized", "older", 1);
    return r;
}
static int opus_mgr_dec_fini(void *gd); static int opus_mgr_dec_reset(void *gd);   // a decoder of a manager (below)
HLE_FORCE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusFini, void *hw) {
    if (opus_mgr_dec_fini(hw)) return;
    if (popus_of(hw)) { popus *p = objmap_del(hw); free(p->st); free(p); opus_event("finalized", "older", -1); return; }
    if (popus_on()) return;   // a decoder whose initialization failed: nothing to finalize
    void (*f)(void *) = opus_fn("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    void *s = objmap_del(hw); if (s) { f(s); free(s); opus_event("finalized", "older", -1); }
}
HLE_FORCE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusRate, const void *hw) {
    { popus *p = popus_of(hw); if (p) return p->rate; if (popus_on()) return 0; }
    int (*f)(const void *) = opus_fn("_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusCh, const void *hw) {
    { popus *p = popus_of(hw); if (p) return p->ch; if (popus_on()) return 0; }
    int (*f)(const void *) = opus_fn("_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusReset, void *hw) {
    if (opus_mgr_dec_reset(hw)) return;
    { popus *p = popus_of(hw); if (p) { g_po.ctl(p->st, 4028); opus_event("reset", "older", 0); return; } if (popus_on()) return; }   // OPUS_RESET_STATE
    void (*f)(void *) = opus_fn("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE"); f(opus_soft(hw)); opus_event("reset", "older", 0);
}
HLE_FORCE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", "hwopus service; software OpusDecoder", int, h_codec_HwOpusDecode,
          void *hw, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    int r, ch = 0; popus *p = popus_of(hw);
    if (p) {   // Nintendo's packet header (big-endian payload size, final range), then the packet, as the sdk's C interface takes it
        const uint8_t *b = in; uint32_t n = in && in_size >= 8 ? (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3] : 0; int ok = in && in_size >= 8 && n <= in_size - 8;
        int got = ok ? g_po.dec(p->st, b + 8, (int32_t)n, out, (int)(out_size / 2 / (size_t)p->ch), 0) : -1;
        r = !ok ? 6 : got < 0 ? 4 : 0;   // the sdk's codes: 6 a short packet, 4 a decode error
        if (!r) { if (consumed) *consumed = 8 + (size_t)n; if (samples) *samples = got; }
        ch = p->ch;
    } else if (popus_on()) {
        r = 4;   // a decoder whose initialization failed: a decode error, not a call on no state
    } else {
        int (*f)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
        void *soft = opus_soft(hw);
        r = f(soft, consumed, samples, out, out_size, in, in_size);
        int (*chf)(const void *) = opus_fn("_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE"); ch = chf(soft);
    }
    atomic_fetch_add(&g_op_dec, 1);
    if (r) { atomic_fetch_add(&g_op_fail, 1); static int nred; if (nred++ < 4) LOG("[RED] codec: Opus decode of a %zu-byte packet failed (%#x)", in_size, r); }
    else if (samples && *samples > 0) {
        size_t n = (size_t)*samples * (size_t)(ch > 0 ? ch : 1); if (n * 2 > out_size) n = out_size / 2;
        atomic_fetch_add(&g_op_samples, (uint64_t)*samples); opus_note_pcm(out, n);
    }
    opus_tick();
    return r;
}

// nn::codec::HardwareOpusMultiStreamDecoder (Hyrule Warriors: Age of Calamity, SDK 9.6.0: its sound streams are multi-stream Opus): the same swap as HardwareOpusDecoder above.
// Both classes implement one interface: their 12 vtable slots match one by one (destructor×2, GetWorkBufferSize, IsInitialized, Initialize, Finalize, GetSampleRate, GetChannelCount,
// GetTotalStreamCount, GetStereoStreamCount, Reset, DecodeInterleaved); the hardware object is 0x48 bytes, the software one 0x38. Both destructors are a bare ret.
// Without it the game's KIDS resource loader never finished the first sound stream (asset/data/0x272c6efb.file) and the main thread waited for it forever (main+0x8d9bc8)
HLE_FORCE("_ZN2nn5codec30HardwareOpusMultiStreamDecoderC1Ei", "hwopus service; replaced by the SDK software OpusMultiStreamDecoder (same virtual interface)",
          void, h_codec_HwOpusMsCtor, void *self, int option) {
    (void)option; void (*f)(void *) = opus_fn("_ZN2nn5codec22OpusMultiStreamDecoderC1Ev"); f(self);
}
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusMsWorkSize,
          int rate, int ch, int streams, int stereo) {
    size_t (*f)(int, int, int, int) = opus_fn("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii"); return f(rate, ch, streams, stereo);
}

// The C interface to the hardware multi-stream decoder (Unicorn Overlord, SDK 16.2.4: CRI's Opus streams; GetHardwareOpusMultiStreamDecoderWorkBufferSize opened hwopus and the
// game aborted in sm::Initialize at nnMain). The same swap as the single-stream C interface above: software state per game decoder object, the work buffer sized for the software decoder.
// The software OpusMultiStreamDecoderType is 0x28 bytes (InitializeOpusMultiStreamDecoder writes up to +0x24), well inside SOFT_OPUS_STATE.
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusMsWorkSize5,
          int rate, int ch, int streams, int stereo, int option) {
    (void)option; size_t (*f)(int, int, int, int) = opus_fn("_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii"); return f(rate, ch, streams, stereo);
}
HLE_FORCE("_ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi", "hwopus service; replaced by the software OpusMultiStreamDecoder",
          int, h_codec_HwOpusMsInit, void *hw, int rate, int ch, int streams, int stereo, const uint8_t *mapping, void *buf, size_t size, int option) {
    (void)option; int (*f)(void *, int, int, int, int, const uint8_t *, void *, size_t) = opus_fn("_ZN2nn5codec32InitializeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeEiiiiPKhPvm");
    void *s = objmap_get(hw); if (!s) { s = calloc(1, SOFT_OPUS_STATE); objmap_put(hw, s); }
    return f(s, rate, ch, streams, stereo, mapping, buf, size);
}
HLE_FORCE("_ZN2nn5codec38FinalizeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", void, h_codec_HwOpusMsFini, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec30FinalizeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE");
    void *s = objmap_del(hw); if (s) { f(s); free(s); }
}
HLE_FORCE("_ZN2nn5codec43GetHardwareOpusMultiStreamDecoderSampleRateEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsRate, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec35GetOpusMultiStreamDecoderSampleRateEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec45GetHardwareOpusMultiStreamDecoderChannelCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsCh, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec37GetOpusMultiStreamDecoderChannelCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusMultiStreamDecoderTotalStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsTotal, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec41GetOpusMultiStreamDecoderTotalStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec50GetHardwareOpusMultiStreamDecoderStereoStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsStereo, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec42GetOpusMultiStreamDecoderStereoStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE"); return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec35ResetHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; software OpusMultiStreamDecoder", void, h_codec_HwOpusMsReset, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec27ResetOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE"); f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec44DecodeOpusMultiStreamInterleavedWithHardwareEPNS0_34HardwareOpusMultiStreamDecoderTypeEPmPiPsmPKvm", "hwopus service; software OpusMultiStreamDecoder", int, h_codec_HwOpusMsDecode,
          void *hw, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    int (*f)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec32DecodeOpusMultiStreamInterleavedEPNS0_26OpusMultiStreamDecoderTypeEPmPiPsmPKvm");
    return f(opus_soft(hw), consumed, samples, out, out_size, in, in_size);
}
// The hardware's decode time of the last frame (nn::TimeSpan); the software decoder runs inline on the caller's thread, so none is reported
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderProcessingTimeEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", "hwopus service; no hardware decode time", int64_t, h_codec_HwOpusMsTime, const void *hw) {
    (void)hw; return 0;
}

// ---------- SDK 21's hardware Opus decoder manager (Xenoblade Chronicles 2's Switch 2 Edition, SDK 21.4.4) ----------
// The asynchronous interface over hwopus: a manager (the game's 0x18-byte HardwareOpusDecoderManagerType, with a system event) owns decoders (0x30-byte HardwareOpusDecoderType each)
// and a pool of I/O buffers. The game enqueues one packet per call (Nintendo's 8-byte header, big-endian size and final range, then the packet: what DecodeOpusInterleaved takes),
// calls ExecuteHardwareOpusDecoderDecoding on each manager once per audio frame, then dequeues each finished frame (960 samples a channel at 48 kHz) and reads the request counts
// (main+0xad2810, main+0xad2e80, main+0xad3140). Host: Execute decodes every waiting packet with the sdk's software OpusDecoder on the caller's thread and signals the event, so a request is
// waiting until Execute, never executing, and completed until dequeued; each holds one I/O buffer from Enqueue to Dequeue. The game tells an initialized manager by its first word
// (main+0xad2828), so that holds the host state. Without these the SDK opened hwopus itself and aborted with 0xe15 (sdk+0x323180, under GetHardwareOpusDecoderManagerWorkBufferSize)
#define OPUS_MAX_PACKET 0x1800                 // header + packet: a 20 ms frame of at most 6 streams' bytes is far below it
#define OPUS_MAX_PCM (960 * 2 * 2 * 3)         // a frame of up to 60 ms, stereo, 16-bit
typedef struct opus_req { struct opus_req *next; size_t n; int samples; uint8_t data[]; } opus_req;   // a waiting packet, then (decoded) its PCM
typedef struct opus_mgr opus_mgr;
typedef struct opus_dec { const void *gd; struct opus_dec *next_all; opus_mgr *m; void *soft; void *work; int ch; opus_req *wait, *wait_tail, *done, *done_tail; int nwait, ndone; } opus_dec;
struct opus_mgr { pthread_mutex_t lock; void *event; int io_total, io_used, ndec, cap; opus_dec **dec; };
static opus_mgr *opus_mgr_of(const void *gm) { opus_mgr *m = *(opus_mgr *const *)gm; if (!m) die("Opus decoder manager %p was not initialized by the host", gm); return m; }
// A manager's decoders are listed here by the game's object, apart from objmap: FinalizeHardwareOpusDecoder and ResetHardwareOpusDecoder serve both interfaces, and objmap's
// entry of a decoder of the older interface is the software OpusDecoderType itself (work buffer at +0 and +8, its size at +0x10), which read as an opus_dec looked like one with
// a manager. Its Reset then locked a "mutex" inside the game's libopus state and reset the work buffer as a decoder: Ocarina of Time's sound streams decoded to silence (0.3.12)
static pthread_mutex_t g_mdec_lock = PTHREAD_MUTEX_INITIALIZER;
static opus_dec *g_mdecs;
static opus_dec *opus_mdec_find(const void *gd, int take) {
    pthread_mutex_lock(&g_mdec_lock);
    opus_dec **pp = &g_mdecs; while (*pp && (*pp)->gd != gd) pp = &(*pp)->next_all;
    opus_dec *d = *pp; if (d && take) *pp = d->next_all;
    pthread_mutex_unlock(&g_mdec_lock);
    return d;
}
static opus_dec *opus_dec_of(const void *gd) { opus_dec *d = opus_mdec_find(gd, 0); if (!d) die("Opus decoder %p was not initialized by the host", gd); return d; }
static void opus_free_list(opus_req *r) { while (r) { opus_req *n = r->next; free(r); r = n; } }

HLE_FORCE("_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", "hwopus service; the host keeps the manager's state, the work buffer is not used",
          size_t, h_codec_HwOpusMgrWorkSize, int rate, int ch, int64_t frame, int decoders, int io) {
    (void)rate; (void)ch; (void)frame; (void)decoders; (void)io; return 0x1000;
}
HLE_FORCE("_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm",
          "hwopus service; decoded by the software OpusDecoder in ExecuteHardwareOpusDecoderDecoding", int, h_codec_HwOpusMgrInit,
          void *gm, void *event, int rate, int ch, int64_t frame, int decoders, int io, void *buf, size_t size) {
    (void)rate; (void)ch; (void)frame; (void)buf; (void)size;
    opus_mgr *m = calloc(1, sizeof *m); if (!m) die("Opus manager calloc");
    pthread_mutex_init(&m->lock, NULL); m->event = event; m->io_total = io; m->cap = decoders > 0 ? decoders : 1;
    if (!(m->dec = calloc((size_t)m->cap, sizeof *m->dec))) die("Opus manager calloc");
    hle_bind_system_event(event, 1);   // the SDK creates it; the game destroys it after FinalizeHardwareOpusDecoderManager (main+0xad27c4)
    *(opus_mgr **)gm = m;
    return 0;
}
HLE_FORCE("_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service; host manager state", void, h_codec_HwOpusMgrFini, void *gm) {
    opus_mgr *m = *(opus_mgr **)gm; if (!m) return;
    for (int i = 0; i < m->ndec; i++) if (m->dec[i]) m->dec[i]->m = NULL;
    *(opus_mgr **)gm = NULL; pthread_mutex_destroy(&m->lock); free(m->dec); free(m);
}
HLE_FORCE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service; host manager state", int, h_codec_HwOpusMgrFreeIo, const void *gm) {
    opus_mgr *m = opus_mgr_of(gm); pthread_mutex_lock(&m->lock); int n = m->io_total - m->io_used; pthread_mutex_unlock(&m->lock); return n;
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE",
          "hwopus service; replaced by the software OpusDecoder (manager form)", int, h_codec_HwOpusMgrDecInit, void *gd, int rate, int ch, int64_t frame, void *gm) {
    (void)frame;
    opus_mgr *m = opus_mgr_of(gm);
    size_t (*wsize)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii");
    int (*init)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    opus_dec *d = calloc(1, sizeof *d); if (!d) die("Opus decoder calloc");
    size_t ws = wsize(rate, ch);
    if (!(d->soft = calloc(1, SOFT_OPUS_STATE)) || !(d->work = calloc(1, ws))) die("Opus decoder calloc");
    int r = init(d->soft, rate, ch, d->work, ws);
    if (r) { free(d->work); free(d->soft); free(d); return r; }
    d->m = m; d->ch = ch; d->gd = gd;
    pthread_mutex_lock(&m->lock);
    if (m->ndec == m->cap) { m->cap *= 2; if (!(m->dec = realloc(m->dec, (size_t)m->cap * sizeof *m->dec))) die("Opus manager realloc"); }
    m->dec[m->ndec++] = d;
    pthread_mutex_unlock(&m->lock);
    if (opus_mdec_find(gd, 0)) die("Opus decoder %p initialized twice without Finalize", gd);
    pthread_mutex_lock(&g_mdec_lock); d->next_all = g_mdecs; g_mdecs = d; pthread_mutex_unlock(&g_mdec_lock);
    opus_event("initialized", "manager", 1);
    return 0;
}
static void opus_dec_clear(opus_dec *d) {   // under the manager's lock: every request of the decoder dropped, its I/O buffers back to the pool
    if (d->m) d->m->io_used -= d->nwait + d->ndone;
    opus_free_list(d->wait); opus_free_list(d->done);
    d->wait = d->wait_tail = d->done = d->done_tail = NULL; d->nwait = d->ndone = 0;
}
// FinalizeHardwareOpusDecoder and ResetHardwareOpusDecoder are the hooks above (the older C interface): both look for a manager's decoder first; 0 = not one, the older interface's
static int opus_mgr_dec_fini(void *gd) {
    opus_dec *d = opus_mdec_find(gd, 1); if (!d) return 0;
    void (*fini)(void *) = opus_fn("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    opus_mgr *m = d->m;
    if (m) {
        pthread_mutex_lock(&m->lock); opus_dec_clear(d);
        for (int i = 0; i < m->ndec; i++) if (m->dec[i] == d) { m->dec[i] = m->dec[--m->ndec]; break; }
        pthread_mutex_unlock(&m->lock);
    } else opus_dec_clear(d);
    fini(d->soft); free(d->soft); free(d->work); free(d);
    opus_event("finalized", "manager", -1);
    return 1;
}
static int opus_mgr_dec_reset(void *gd) {
    opus_dec *d = opus_mdec_find(gd, 0); if (!d) return 0;
    void (*reset)(void *) = opus_fn("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE");
    if (d->m) pthread_mutex_lock(&d->m->lock);
    opus_dec_clear(d); reset(d->soft);
    if (d->m) pthread_mutex_unlock(&d->m->lock);
    opus_event("reset", "manager", 0);
    return 1;
}
HLE_FORCE("_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", "hwopus service; queued for ExecuteHardwareOpusDecoderDecoding", int, h_codec_HwOpusEnqueue,
          void *gd, size_t *consumed, const void *in, size_t in_size) {
    opus_dec *d = opus_dec_of(gd); opus_mgr *m = d->m;
    if (consumed) *consumed = 0;
    if (!m) return 0x3a6f;   // the manager was finalized
    if (in_size < 8 || in_size > OPUS_MAX_PACKET) { LOG("[RED] codec: Opus packet of %zu bytes enqueued", in_size); return 0x3a6f; }
    opus_req *r = malloc(sizeof *r + (in_size > OPUS_MAX_PCM ? in_size : OPUS_MAX_PCM)); if (!r) die("Opus request malloc");
    r->next = NULL; r->n = in_size; r->samples = 0; memcpy(r->data, in, in_size);
    pthread_mutex_lock(&m->lock);
    if (m->io_used >= m->io_total) { pthread_mutex_unlock(&m->lock); free(r); atomic_fetch_add(&g_op_refused, 1); return 0x3a6f; }   // no free I/O buffer: the game checks the count first (main+0xad2ec4)
    m->io_used++; d->nwait++;
    { int pk = atomic_load(&g_op_io_peak); while (m->io_used > pk && !atomic_compare_exchange_weak(&g_op_io_peak, &pk, m->io_used)) {} }
    atomic_fetch_add(&g_op_enq, 1);
    if (d->wait_tail) d->wait_tail->next = r; else d->wait = r;
    d->wait_tail = r;
    pthread_mutex_unlock(&m->lock);
    if (consumed) *consumed = in_size;
    return 0;
}
HLE_FORCE("_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service; the software OpusDecoder decodes the waiting packets here",
          int, h_codec_HwOpusExecute, void *gm) {
    opus_mgr *m = opus_mgr_of(gm);
    int (*dec)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    int any = 0;
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < m->ndec; i++) {
        opus_dec *d = m->dec[i];
        while (d->wait) {
            opus_req *r = d->wait; d->wait = r->next; if (!d->wait) d->wait_tail = NULL; d->nwait--;
            uint8_t pkt[OPUS_MAX_PACKET]; size_t n = r->n, used = 0; int samples = 0; memcpy(pkt, r->data, n);
            int res = dec(d->soft, &used, &samples, (int16_t *)r->data, OPUS_MAX_PCM, pkt, n);
            if (res || samples <= 0) {   // a frame the decoder refuses still completes, as silence of a 20 ms frame, so the game's stream stays in step
                static int nred; if (nred++ < 4) LOG("[RED] codec: software Opus decode of a %zu-byte packet failed (%#x), silence instead", n, res);
                samples = 960; memset(r->data, 0, (size_t)samples * d->ch * 2);
                atomic_fetch_add(&g_op_fail, 1);
            }
            atomic_fetch_add(&g_op_dec, 1); atomic_fetch_add(&g_op_samples, (uint64_t)samples); opus_note_pcm((const int16_t *)r->data, (size_t)samples * (size_t)d->ch);
            r->samples = samples; r->n = (size_t)samples * d->ch * 2; r->next = NULL;
            if (d->done_tail) d->done_tail->next = r; else d->done = r;
            d->done_tail = r; d->ndone++; any = 1;
        }
    }
    pthread_mutex_unlock(&m->lock);
    if (any) hle_signal_system_event_binding(hle_system_event_binding(m->event));
    opus_tick();
    return 0;
}
HLE_FORCE("_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", "hwopus service; frames the software OpusDecoder decoded",
          int, h_codec_HwOpusDequeue, void *gd, int *samples, int16_t *out, size_t out_size) {
    opus_dec *d = opus_dec_of(gd); opus_mgr *m = d->m;
    if (samples) *samples = 0;
    if (!m) return 0x3a6f;
    pthread_mutex_lock(&m->lock);
    opus_req *r = d->done;
    if (!r) { pthread_mutex_unlock(&m->lock); return 0x3a6f; }   // nothing completed: the game dequeues only as many as the completed count (main+0xad31ac)
    if (r->n > out_size) { pthread_mutex_unlock(&m->lock); LOG("[RED] codec: a %zu-byte Opus frame for a %zu-byte buffer", r->n, out_size); return 0x3a6f; }
    d->done = r->next; if (!d->done) d->done_tail = NULL; d->ndone--; m->io_used--;
    pthread_mutex_unlock(&m->lock);
    memcpy(out, r->data, r->n); if (samples) *samples = r->samples;
    free(r);
    return 0;
}
static int opus_count(const void *gd, int which) {
    opus_dec *d = opus_dec_of(gd); if (!d->m) return 0;
    pthread_mutex_lock(&d->m->lock); int n = which == 0 ? d->nwait : which == 2 ? d->ndone : 0; pthread_mutex_unlock(&d->m->lock);
    return n;
}
HLE_FORCE("_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; packets not yet decoded", int, h_codec_HwOpusWaiting, const void *gd) {
    return opus_count(gd, 0);
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; Execute decodes synchronously, none run in the background",
          int, h_codec_HwOpusExecuting, const void *gd) {
    return opus_count(gd, 1);
}
HLE_FORCE("_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; decoded frames not yet dequeued", int, h_codec_HwOpusCompleted, const void *gd) {
    return opus_count(gd, 2);
}
