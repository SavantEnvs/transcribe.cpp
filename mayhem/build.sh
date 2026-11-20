#!/usr/bin/env bash
# mayhem/build.sh — build transcribe.cpp's fuzz harnesses (sanitized + SanCov)
# and the upstream test suite (normal flags). Re-runnable, offline.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export CC CXX
cd "$SRC"

# Common CMake switches: CPU only, no system BLAS / OpenMP, portable x86
# baseline (no -march=native), no install rules.
COMMON=(-G Ninja -DTRANSCRIBE_USE_SYSTEM_BLAS=OFF -DTRANSCRIBE_USE_OPENMP=OFF
        -DTRANSCRIBE_X86_CONSERVATIVE=ON -DGGML_NATIVE=OFF -DTRANSCRIBE_INSTALL=OFF
        -DTRANSCRIBE_METAL=OFF)

# 1) Toy GGUF fixtures (stdlib-only generator; *.gguf is gitignored upstream,
#    so they are always regenerated here) and the embedded-model table.
GEN="$SRC/build-fuzz/gen"
mkdir -p "$GEN/fixtures"
cp tests/fixtures/make_gguf_fixtures.py "$GEN/fixtures/"
(cd "$GEN/fixtures" && python3 -I make_gguf_fixtures.py >/dev/null)
F="$GEN/fixtures"
python3 -I mayhem/harnesses/embed_models.py "$GEN/fuzz_models.inc.tmp" \
    granite5_ctc="$F/arch_granite5_ctc_minimal.gguf" \
    cohere="$F/arch_cohere_minimal.gguf" \
    qwen3_asr="$F/arch_qwen3_asr_minimal.gguf" \
    parakeet="$F/tokenizer_minimal.gguf" \
    parakeet_streaming="$F/tokenizer_minimal_streaming.gguf" \
    parakeet_streaming_cache_aware="$F/tokenizer_minimal_streaming_cache_aware.gguf" \
    parakeet_streaming_buffered="$F/tokenizer_minimal_streaming_buffered.gguf"
# keep the header's mtime stable on a re-run so Ninja doesn't recompile
if ! cmp -s "$GEN/fuzz_models.inc.tmp" "$GEN/fuzz_models.inc"; then
    mv "$GEN/fuzz_models.inc.tmp" "$GEN/fuzz_models.inc"
else
    rm -f "$GEN/fuzz_models.inc.tmp"
fi

# 2) Sanitized, coverage-instrumented library + harnesses (DWARF 3), each
#    linked with mayhem/lsan_off.cc (leak detection off, SPEC 6.2 item 15). The
#    "Fuzz" build type has no per-config flags, so assertions stay enabled.
#    UBSan's `function` check is relaxed ONLY for the vendored ggml targets
#    (see the end of mayhem/harnesses/fuzz-targets.cmake for why); it stays on
#    for transcribe's own code and the harnesses. ASan and the rest of UBSan
#    stay on and halting everywhere.
FUZZ_FLAGS="-O1 $SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS"
cmake -S . -B build-fuzz/cmake "${COMMON[@]}" \
    -DCMAKE_PROJECT_transcribe_INCLUDE="$SRC/mayhem/harnesses/fuzz-targets.cmake" \
    -DCMAKE_BUILD_TYPE=Fuzz \
    -DCMAKE_C_FLAGS="$FUZZ_FLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_FLAGS" \
    -DTRANSCRIBE_BUILD_TESTS=OFF -DTRANSCRIBE_BUILD_EXAMPLES=OFF \
    -DFUZZ_ENGINE="$LIB_FUZZING_ENGINE" -DSTANDALONE_MAIN="$STANDALONE_FUZZ_MAIN" \
    -DFUZZ_GEN_DIR="$GEN" -DFUZZ_OUT_DIR="$SRC/build-fuzz/bin" \
    -DFUZZ_LSAN_OFF="$SRC/mayhem/lsan_off.cc"
cmake --build build-fuzz/cmake -j"$MAYHEM_JOBS" --target \
    fuzz_model_gguf fuzz_model_vad fuzz_model_langid fuzz_model_whisper_bin \
    fuzz_pcm_run fuzz_stream_pcm fuzz_tokenize fuzz_wav \
    fuzz_model_gguf-standalone fuzz_model_vad-standalone fuzz_model_langid-standalone \
    fuzz_model_whisper_bin-standalone fuzz_pcm_run-standalone fuzz_stream_pcm-standalone \
    fuzz_tokenize-standalone fuzz_wav-standalone
ls -l build-fuzz/bin

# 3) Upstream test suite with its normal (Release) flags. The fixture-driven
#    tests are registered only when `uv` is found; point that at a shim that
#    runs the stdlib-only generator with python3.
cmake -S . -B build-tests "${COMMON[@]}" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS" \
    -DTRANSCRIBE_BUILD_TESTS=ON -DTRANSCRIBE_BUILD_EXAMPLES=ON \
    -DTRANSCRIBE_UV="$SRC/mayhem/uv-shim.sh"
cmake --build build-tests -j"$MAYHEM_JOBS"
