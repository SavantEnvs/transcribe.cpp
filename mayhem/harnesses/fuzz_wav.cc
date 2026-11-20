// Fuzzes the CLI's audio input: transcribe_cli::load_wav_mono_16k
// (examples/common/wav.cpp over the vendored dr_wav: RIFF/RF64/W64/AIFF,
// PCM/float/ADPCM/A-law/mu-law, downmix and rate check).
#include "wav.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
    const char * dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    std::string tmpl = std::string(dir) + "/transcribe-wav-XXXXXX.wav";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = mkstemps(buf.data(), 4);
    if (fd < 0) return 0;
    size_t off = 0;
    while (off < size) {
        ssize_t w = write(fd, data + off, size - off);
        if (w <= 0) break;
        off += (size_t) w;
    }
    close(fd);
    std::vector<float> pcm;
    std::string err;
    if (transcribe_cli::load_wav_mono_16k(buf.data(), pcm, err)) {
        volatile float acc = 0;
        for (float v : pcm) acc += v;
        (void) acc;
    }
    unlink(buf.data());
    return 0;
}
