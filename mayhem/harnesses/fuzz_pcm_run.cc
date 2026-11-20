// Fuzzes the audio path of the one-shot and batch ASR APIs: fuzzed PCM goes
// through transcribe_run / transcribe_run_batch on fixed toy models
// (granite5 CTC, cohere, qwen3-asr, parakeet), reaching the mel / fbank
// front ends, the encoders, the decoders and the input-length caps.
//
// Input: byte 0 selects the model (bits 0-1) and one-shot vs batch (bit 2);
// the batch split point is byte 1; the rest is little-endian int16 PCM.
#include "embedded_models.h"

static const char * k_names[4] = {"granite5_ctc", "cohere", "qwen3_asr", "parakeet"};
static transcribe_model * g_models[4];

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    fuzz::quiet_logs();
    for (int i = 0; i < 4; ++i) g_models[i] = fuzz::embedded_model(k_names[i]);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    if (size < 2) return -1;
    const uint8_t sel = data[0], split = data[1];
    std::vector<float> pcm = fuzz::bytes_to_pcm(data + 2, size - 2, 32000);  // <= 2 s
    if (pcm.empty()) return -1;

    transcribe_session * s = fuzz::open_session(g_models[sel & 3]);
    if (!s) return 0;
    transcribe_run_params rp;
    transcribe_run_params_init(&rp);
    if (sel & 8) rp.timestamps = TRANSCRIBE_TIMESTAMPS_WORD;
    if (!(sel & 4)) {
        if (transcribe_run(s, pcm.data(), (int) pcm.size(), &rp) == TRANSCRIBE_OK) fuzz::walk_results(s);
    } else {
        size_t cut = pcm.size() * split / 256;
        if (cut == 0) cut = 1;
        const float * bufs[2] = {pcm.data(), pcm.data() + cut};
        int lens[2] = {(int) cut, (int) (pcm.size() - cut)};
        int n = lens[1] > 0 ? 2 : 1;
        if (transcribe_run_batch(s, bufs, lens, n, &rp) == TRANSCRIBE_OK) {
            for (int i = 0; i < transcribe_batch_n_results(s); ++i) {
                transcribe_batch_status(s, i);
                const char * t = transcribe_batch_full_text(s, i);
                if (t) (void) std::strlen(t);
                transcribe_batch_n_segments(s, i);
                transcribe_batch_n_tokens(s, i);
            }
        }
    }
    transcribe_session_free(s);
    return 0;
}
