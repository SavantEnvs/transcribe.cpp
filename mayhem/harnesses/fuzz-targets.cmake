# Fuzz harness targets, injected into the upstream top-level build with
# -DCMAKE_PROJECT_transcribe_INCLUDE=<this file> (runs right after upstream's
# project() call; upstream's CMake relies on being the top-level project, so
# it cannot be add_subdirectory()'d). Target references resolve at generate
# time, so linking `transcribe` before it is defined is fine.
set(FUZZ_HARNESS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(TRANSCRIBE_ROOT ${CMAKE_SOURCE_DIR})

if(NOT DEFINED FUZZ_ENGINE)
    message(FATAL_ERROR "FUZZ_ENGINE (libFuzzer link flag) required")
endif()
if(NOT DEFINED STANDALONE_MAIN)
    message(FATAL_ERROR "STANDALONE_MAIN (run-once driver source) required")
endif()
if(NOT DEFINED FUZZ_GEN_DIR)
    message(FATAL_ERROR "FUZZ_GEN_DIR (dir holding fuzz_models.inc) required")
endif()

if(NOT DEFINED FUZZ_LSAN_OFF)
    message(FATAL_ERROR "FUZZ_LSAN_OFF (mayhem/lsan_off.cc) required")
endif()
add_library(fuzz_lsan_off OBJECT ${FUZZ_LSAN_OFF})
add_library(fuzz_standalone_main OBJECT ${STANDALONE_MAIN})
set_source_files_properties(${STANDALONE_MAIN} PROPERTIES LANGUAGE C)
add_library(fuzz_wavlib STATIC ${TRANSCRIBE_ROOT}/examples/common/wav.cpp)
target_include_directories(fuzz_wavlib PUBLIC ${TRANSCRIBE_ROOT}/examples/common)

function(fuzz_target name src)
    foreach(variant fuzz standalone)
        if(variant STREQUAL "fuzz")
            set(exe ${name})
        else()
            set(exe ${name}-standalone)
        endif()
        add_executable(${exe} ${FUZZ_HARNESS_DIR}/${src} $<TARGET_OBJECTS:fuzz_lsan_off>)
        target_include_directories(${exe} PRIVATE ${TRANSCRIBE_ROOT}/include ${FUZZ_GEN_DIR}
                                                  ${FUZZ_HARNESS_DIR})
        target_link_libraries(${exe} PRIVATE transcribe fuzz_wavlib)
        set_target_properties(${exe} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FUZZ_OUT_DIR})
        if(variant STREQUAL "fuzz")
            target_link_options(${exe} PRIVATE ${FUZZ_ENGINE})
        else()
            target_sources(${exe} PRIVATE $<TARGET_OBJECTS:fuzz_standalone_main>)
        endif()
    endforeach()
endfunction()

fuzz_target(fuzz_model_gguf        fuzz_model.cc)
fuzz_target(fuzz_model_vad         fuzz_model.cc)
fuzz_target(fuzz_model_langid      fuzz_model.cc)
fuzz_target(fuzz_model_whisper_bin fuzz_model.cc)
fuzz_target(fuzz_pcm_run           fuzz_pcm_run.cc)
fuzz_target(fuzz_stream_pcm        fuzz_stream_pcm.cc)
fuzz_target(fuzz_tokenize          fuzz_tokenize.cc)
fuzz_target(fuzz_wav               fuzz_wav.cc)

# UBSan's `function` check is relaxed for the vendored ggml ONLY (every other
# check stays on and halting, and `function` stays on for transcribe's src/,
# examples/common/ and the harnesses). ggml fills its per-type dispatch tables
# with casts to generic function-pointer types and calls through them, e.g.
# ggml/src/ggml-cpu/ggml-cpu.c:218 `(ggml_vec_dot_t) ggml_vec_dot_f32`
# (const float * vs const void *; also lines 217, 224, 397) and
# ggml/src/ggml.c:674-805 `(ggml_to_float_t) dequantize_row_q8_0` and others.
# The check fires at the call site (first: the vec_dot call at
# ggml/src/ggml-cpu/ggml-cpu.c:1279, reached by every F32 matmul, i.e. every
# input that runs a model), and all such call sites are in ggml/src. ggml is
# add_subdirectory()'d after this file runs, so the per-target flag is applied
# by a call deferred to the end of the top-level CMakeLists.txt. It comes after
# CMAKE_C(XX)_FLAGS on the compile line, so it overrides -fsanitize=undefined.
function(fuzz_relax_ubsan_function_in dir)
    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS targets)
        get_target_property(type ${t} TYPE)
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${t} PRIVATE -fno-sanitize=function)
            list(APPEND FUZZ_RELAXED_TARGETS ${t})
        endif()
    endforeach()
    get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(d IN LISTS subdirs)
        fuzz_relax_ubsan_function_in(${d})
    endforeach()
    set(FUZZ_RELAXED_TARGETS ${FUZZ_RELAXED_TARGETS} PARENT_SCOPE)
endfunction()
function(fuzz_relax_ubsan_function_ggml)
    set(FUZZ_RELAXED_TARGETS "")
    fuzz_relax_ubsan_function_in(${TRANSCRIBE_ROOT}/ggml)
    if(NOT "ggml-base" IN_LIST FUZZ_RELAXED_TARGETS OR NOT "ggml-cpu" IN_LIST FUZZ_RELAXED_TARGETS)
        message(FATAL_ERROR "ggml-base/ggml-cpu not found under ggml/: ${FUZZ_RELAXED_TARGETS}")
    endif()
    message(STATUS "fuzz: -fno-sanitize=function on ggml targets: ${FUZZ_RELAXED_TARGETS}")
endfunction()
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL fuzz_relax_ubsan_function_ggml)
