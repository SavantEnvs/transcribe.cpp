// Fuzzes the model-file parser: the input is a whole model file (GGUF, or a
// legacy whisper.cpp / Silero `ggml` .bin) loaded through the public
// transcribe_model_load_file(). A model that loads is then driven through
// every role it advertises (ASR one-shot + streaming, VAD, langid, diarize)
// on a fixed short PCM clip, so per-architecture hparam/tokenizer/weight
// validation and the inference graphs see the fuzzed metadata.
//
// Built as several targets (fuzz_model_gguf, fuzz_model_vad,
// fuzz_model_langid, fuzz_model_whisper_bin) that differ only in seeds.
#include "fuzz_common.h"
#include "transcribe/diarize.h"
#include "transcribe/langid.h"
#include "transcribe/vad.h"

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
    fuzz::quiet_logs();
    return 0;
}

static void drive_asr(transcribe_model * model, const std::vector<float> & pcm) {
    transcribe_session * s = fuzz::open_session(model);
    if (!s) return;
    transcribe_run_params rp;
    transcribe_run_params_init(&rp);
    if (transcribe_run(s, pcm.data(), (int) pcm.size(), &rp) == TRANSCRIBE_OK) fuzz::walk_results(s);

    int32_t toks[64];
    transcribe_tokenize(model, "hello world", toks, 64);

    transcribe_capabilities caps;
    transcribe_capabilities_init(&caps);
    if (transcribe_model_get_capabilities(model, &caps) == TRANSCRIBE_OK && caps.supports_streaming) {
        transcribe_stream_params sp;
        transcribe_stream_params_init(&sp);
        if (transcribe_stream_begin(s, &rp, &sp) == TRANSCRIBE_OK) {
            size_t half = pcm.size() / 2;
            transcribe_stream_feed(s, pcm.data(), (int) half, nullptr);
            transcribe_stream_feed(s, pcm.data() + half, (int) (pcm.size() - half), nullptr);
            transcribe_stream_finalize(s, nullptr);
            transcribe_stream_text tx;
            transcribe_stream_text_init(&tx);
            transcribe_stream_get_text(s, &tx);
        }
        transcribe_stream_reset(s);
    }
    transcribe_session_free(s);
}

static void drive_vad(transcribe_model * model, const std::vector<float> & pcm) {
    transcribe_vad_info info;
    transcribe_vad_info_init(&info);
    if (transcribe_vad_get_info(model, &info) != TRANSCRIBE_OK) return;
    transcribe_vad_session * s = nullptr;
    if (transcribe_vad_session_init(model, nullptr, &s) != TRANSCRIBE_OK) return;
    transcribe_vad_params vp;
    transcribe_vad_params_init(&vp);
    if (transcribe_vad_run(s, pcm.data(), (int) pcm.size(), &vp) == TRANSCRIBE_OK) {
        transcribe_vad_result r;
        transcribe_vad_result_init(&r);
        transcribe_vad_get_result(s, &r);
        for (int i = 0; i < r.n_segments && i < 64; ++i) {
            transcribe_vad_segment seg;
            transcribe_vad_segment_init(&seg);
            transcribe_vad_get_segment(s, i, &seg);
        }
    }
    transcribe_vad_stream_feed(s, pcm.data(), (int) pcm.size() / 3);
    transcribe_vad_stream_feed(s, pcm.data() + pcm.size() / 3, (int) (pcm.size() - pcm.size() / 3));
    transcribe_vad_stream_flush(s);
    transcribe_vad_session_free(s);
}

static void drive_langid(transcribe_model * model, const std::vector<float> & pcm) {
    transcribe_langid_info info;
    transcribe_langid_info_init(&info);
    if (transcribe_langid_get_info(model, &info) != TRANSCRIBE_OK) return;
    for (int i = 0; i < 8; ++i) {
        transcribe_langid_label_code(model, i);
        transcribe_langid_label_name(model, i);
    }
    transcribe_langid_label_index(model, "en");
    transcribe_langid_session * s = nullptr;
    if (transcribe_langid_session_init(model, nullptr, &s) != TRANSCRIBE_OK) return;
    if (transcribe_langid_run(s, pcm.data(), (int) pcm.size(), nullptr) == TRANSCRIBE_OK) {
        transcribe_langid_result r;
        transcribe_langid_result_init(&r);
        transcribe_langid_get_result(s, &r);
        for (int i = 0; i < r.n_candidates && i < 16; ++i) {
            transcribe_langid_candidate c;
            transcribe_langid_candidate_init(&c);
            transcribe_langid_get_candidate(s, i, &c);
        }
    }
    transcribe_langid_session_free(s);
}

static void drive_diarize(transcribe_model * model, const std::vector<float> & pcm) {
    transcribe_diarize_info info;
    transcribe_diarize_info_init(&info);
    if (transcribe_diarize_get_info(model, &info) != TRANSCRIBE_OK) return;
    transcribe_diarize_session * s = nullptr;
    if (transcribe_diarize_session_init(model, nullptr, &s) != TRANSCRIBE_OK) return;
    if (transcribe_diarize_run(s, pcm.data(), (int) pcm.size(), nullptr) == TRANSCRIBE_OK) {
        int n = transcribe_diarize_n_segments(s);
        for (int i = 0; i < n && i < 64; ++i) {
            transcribe_speaker_segment seg;
            transcribe_speaker_segment_init(&seg);
            transcribe_diarize_get_segment(s, i, &seg);
        }
    }
    transcribe_diarize_session_free(s);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    // mmap is the library default; the copying loader is a separate path and
    // is exercised on inputs whose size is odd.
    transcribe_model * model = fuzz::load_bytes(data, size, (size & 1) != 0);
    if (!model) return 0;

    volatile size_t sink = 0;
    const char * s = transcribe_model_arch_string(model);
    if (s) sink += std::strlen(s);
    s = transcribe_model_variant_string(model);
    if (s) sink += std::strlen(s);
    s = transcribe_model_meta_val_str(model, "general.architecture");
    if (s) sink += std::strlen(s);
    transcribe_capabilities caps;
    transcribe_capabilities_init(&caps);
    if (transcribe_model_get_capabilities(model, &caps) == TRANSCRIBE_OK) {
        for (int i = 0; i < caps.n_languages && i < 512 && caps.languages; ++i)
            if (caps.languages[i]) sink += std::strlen(caps.languages[i]);
    }
    (void) sink;

    static const std::vector<float> pcm = fuzz::fixed_pcm(8000);  // 0.5 s
    uint32_t roles = transcribe_model_roles(model);
    if (roles & TRANSCRIBE_ROLE_ASR) drive_asr(model, pcm);
    if (roles & TRANSCRIBE_ROLE_VAD) drive_vad(model, pcm);
    if (roles & TRANSCRIBE_ROLE_LANGID) drive_langid(model, pcm);
    if (roles & TRANSCRIBE_ROLE_DIARIZE) drive_diarize(model, pcm);

    transcribe_model_free(model);
    return 0;
}
