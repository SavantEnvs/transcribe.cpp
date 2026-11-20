// Shared helpers for the transcribe.cpp fuzz harnesses (mayhem/harnesses).
#pragma once
#include "transcribe.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

namespace fuzz {

// Model loading is path-based only (transcribe_model_load_file), so stage
// the bytes in a private temp file under $TMPDIR.
inline std::string write_temp(const uint8_t * data, size_t size, const char * suffix) {
    const char * dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    std::string tmpl = std::string(dir) + "/transcribe-fuzz-XXXXXX" + suffix;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), (int) std::strlen(suffix));
    if (fd < 0) return std::string();
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if (w <= 0) { close(fd); unlink(buf.data()); return std::string(); }
        off += (size_t) w;
    }
    close(fd);
    return std::string(buf.data());
}

inline transcribe_model * load_bytes(const uint8_t * data, size_t size, bool no_mmap) {
    std::string path = write_temp(data, size, ".gguf");
    if (path.empty()) return nullptr;
    transcribe_model_load_params lp;
    transcribe_model_load_params_init(&lp);
    lp.backend = TRANSCRIBE_BACKEND_CPU;
    lp.no_mmap = no_mmap;
    transcribe_model * model = nullptr;
    transcribe_status st = transcribe_model_load_file(path.c_str(), &lp, &model);
    unlink(path.c_str());  // an mmap'd model keeps its mapping after unlink
    if (st != TRANSCRIBE_OK) return nullptr;
    return model;
}

// Deterministic 16 kHz test signal: a chirp with a little harmonic content.
inline std::vector<float> fixed_pcm(int n) {
    std::vector<float> pcm((size_t) n);
    for (int i = 0; i < n; ++i) {
        double t = i / 16000.0;
        pcm[(size_t) i] = (float) (0.3 * std::sin(2 * M_PI * (200 + 400 * t) * t) +
                                   0.1 * std::sin(2 * M_PI * 1300 * t));
    }
    return pcm;
}

// Fuzz bytes -> PCM: little-endian int16 samples scaled to [-1, 1) (always finite).
inline std::vector<float> bytes_to_pcm(const uint8_t * data, size_t size, size_t max_samples) {
    size_t n = size / 2;
    if (n > max_samples) n = max_samples;
    std::vector<float> pcm(n);
    for (size_t i = 0; i < n; ++i) {
        int16_t v = (int16_t) (data[2 * i] | (data[2 * i + 1] << 8));
        pcm[i] = v / 32768.0f;
    }
    return pcm;
}

inline void walk_results(transcribe_session * s) {
    volatile size_t sink = 0;
    const char * t = transcribe_full_text(s);
    if (t) sink += std::strlen(t);
    t = transcribe_raw_text(s);
    if (t) sink += std::strlen(t);
    t = transcribe_detected_language(s);
    if (t) sink += std::strlen(t);
    int ns = transcribe_n_segments(s), nw = transcribe_n_words(s), nt = transcribe_n_tokens(s);
    for (int i = 0; i < ns && i < 256; ++i) {
        transcribe_segment seg; transcribe_segment_init(&seg);
        transcribe_get_segment(s, i, &seg);
        sink += (size_t) seg.n_words;
    }
    for (int i = 0; i < nw && i < 256; ++i) {
        transcribe_word w; transcribe_word_init(&w);
        transcribe_get_word(s, i, &w);
        sink += (size_t) w.n_tokens;
    }
    for (int i = 0; i < nt && i < 256; ++i) {
        transcribe_token tok; transcribe_token_init(&tok);
        transcribe_get_token(s, i, &tok);
        sink += (size_t) tok.id;
    }
    (void) sink;
}

inline transcribe_session * open_session(transcribe_model * model) {
    transcribe_session_params sp;
    transcribe_session_params_init(&sp);
    sp.n_threads = 1;
    transcribe_session * s = nullptr;
    if (transcribe_session_init(model, &sp, &s) != TRANSCRIBE_OK) return nullptr;
    return s;
}

inline void quiet_logs() { transcribe_log_set(nullptr, nullptr); }

}  // namespace fuzz
