// Fuzzes the streaming ASR state machine (transcribe_stream_begin / feed /
// finalize) on the fixed parakeet streaming toy models (plain, cache-aware,
// buffered) with fuzzed audio and fuzzed chunking.
//
// Input: byte 0 selects the model (bits 0-1, mod 3) and commit policy
// (bits 2-3); then repeated records of [chunk-length byte][int16 PCM ...].
#include "embedded_models.h"

static const char * k_names[3] = {"parakeet_streaming", "parakeet_streaming_cache_aware",
                                  "parakeet_streaming_buffered"};
static transcribe_model * g_models[3];

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    fuzz::quiet_logs();
    for (int i = 0; i < 3; ++i) g_models[i] = fuzz::embedded_model(k_names[i]);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    if (size < 1) return -1;
    const uint8_t sel = data[0];
    transcribe_session * s = fuzz::open_session(g_models[(sel & 3) % 3]);
    if (!s) return 0;
    transcribe_run_params rp;
    transcribe_run_params_init(&rp);
    transcribe_stream_params sp;
    transcribe_stream_params_init(&sp);
    sp.commit_policy = (transcribe_stream_commit_policy) ((sel >> 2) & 3);
    if (transcribe_stream_begin(s, &rp, &sp) == TRANSCRIBE_OK) {
        size_t off = 1, total = 0;
        while (off < size && total < 64000) {
            size_t want = ((size_t) data[off++] + 1) * 64;  // samples
            size_t avail = (size - off) / 2;
            if (want > avail) want = avail;
            if (want == 0) break;
            std::vector<float> pcm = fuzz::bytes_to_pcm(data + off, want * 2, want);
            off += want * 2;
            total += want;
            transcribe_stream_update up;
            transcribe_stream_update_init(&up);
            if (transcribe_stream_feed(s, pcm.data(), (int) pcm.size(), &up) != TRANSCRIBE_OK) break;
            transcribe_stream_revision(s);
            transcribe_stream_n_committed_tokens(s);
        }
        if (transcribe_stream_get_state(s) == TRANSCRIBE_STREAM_ACTIVE) transcribe_stream_finalize(s, nullptr);
        transcribe_stream_text tx;
        transcribe_stream_text_init(&tx);
        if (transcribe_stream_get_text(s, &tx) == TRANSCRIBE_OK && tx.full_text) (void) std::strlen(tx.full_text);
        transcribe_stream_n_committed_segments(s);
        transcribe_stream_n_committed_words(s);
        fuzz::walk_results(s);
    }
    transcribe_stream_reset(s);
    transcribe_session_free(s);
    return 0;
}
