// Fuzzes text handling: transcribe_tokenize (qwen byte-level BPE) on the
// fixed qwen3-asr toy model with small and large output buffers, and the
// same text as run_params prompt / vocabulary / language on a short run.
//
// Input: byte 0 = flags (bit 0: also run; bit 1: use as language), rest = text.
#include "embedded_models.h"

static transcribe_model * g_model;

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    fuzz::quiet_logs();
    g_model = fuzz::embedded_model("qwen3_asr");
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    if (size < 1) return -1;
    const uint8_t flags = data[0];
    std::string text(reinterpret_cast<const char *>(data + 1), size - 1);
    const char * ctext = text.c_str();  // stops at the first NUL, as a C caller would

    int32_t small[8];
    transcribe_tokenize(g_model, ctext, small, 8);
    std::vector<int32_t> big(4096);
    transcribe_tokenize(g_model, ctext, big.data(), big.size());

    if (flags & 1) {
        std::vector<std::string> words;
        size_t start = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n') {
                words.push_back(text.substr(start, i - start));
                start = i + 1;
            }
        }
        std::vector<const char *> vocab;
        for (auto & w : words) vocab.push_back(w.c_str());
        static const std::vector<float> pcm = fuzz::fixed_pcm(4000);
        transcribe_session * s = fuzz::open_session(g_model);
        if (!s) return 0;
        transcribe_run_params rp;
        transcribe_run_params_init(&rp);
        rp.prompt = ctext;
        rp.vocabulary = vocab.data();
        rp.n_vocabulary = (int32_t) vocab.size();
        if (flags & 2) rp.language = ctext;
        if (flags & 4) rp.prefix = ctext;
        if (transcribe_run(s, pcm.data(), (int) pcm.size(), &rp) == TRANSCRIBE_OK) fuzz::walk_results(s);
        transcribe_session_free(s);
    }
    return 0;
}
