# Native tests of gr-robotweax-srt.
#
# Part of the GNU Radio module build, and also a standalone project so the
# same tests run without GNU Radio under sanitizers and a fuzzer:
#
#   cmake -S tests -B build-native -DRWSRT_SANITIZE=address,undefined
#   cmake -S tests -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DRWSRT_BUILD_FUZZERS=ON
cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(rwsrt_native LANGUAGES CXX)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo)
    endif()
    enable_testing()
    set(RWSRT_SANITIZE "" CACHE STRING "Sanitizers for every native target, e.g. address,undefined")
    if(RWSRT_SANITIZE)
        add_compile_options(-fsanitize=${RWSRT_SANITIZE} -fno-omit-frame-pointer
                            -fno-sanitize-recover=all)
        add_link_options(-fsanitize=${RWSRT_SANITIZE})
    endif()
    option(RWSRT_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
    if(RWSRT_BUILD_FUZZERS)
        # Coverage instrumentation for the code under test, not only the
        # fuzz entry points; without it the fuzzer is blind.
        add_compile_options(-fsanitize=fuzzer-no-link)
    endif()
    include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/RwsrtNative.cmake")
endif()

find_package(Boost REQUIRED)

add_executable(rwsrt_unit_tests
    unit/main.cc
    unit/test_wire_codec.cc
    unit/test_sample_time.cc
    unit/test_continuity.cc
    unit/test_queue.cc
    unit/test_assembler.cc
    unit/test_session_state.cc)
target_link_libraries(rwsrt_unit_tests PRIVATE rwsrt_core Boost::boost)
target_compile_definitions(rwsrt_unit_tests PRIVATE
    RWSRT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/rwsr_v1")
target_compile_options(rwsrt_unit_tests PRIVATE -Wall -Wextra)
add_test(NAME rwsrt_unit COMMAND rwsrt_unit_tests)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME rwsrt_golden_vectors
        COMMAND ${Python3_EXECUTABLE} -I
                "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_golden_vectors.py" --check)
endif()

# Headless reference peers: same codec, queue and worker as the blocks.
foreach(peer sender receiver)
    add_executable(rwsrt_ref_${peer} ../tools/reference_peer/${peer}.cc
                   ../tools/reference_peer/baseline_link.cc)
    target_link_libraries(rwsrt_ref_${peer} PRIVATE rwsrt_transport)
    target_compile_options(rwsrt_ref_${peer} PRIVATE -Wall -Wextra)
endforeach()
if(Python3_FOUND)
    add_test(NAME rwsrt_end_to_end
        COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_e2e.py"
                --sender $<TARGET_FILE:rwsrt_ref_sender>
                --receiver $<TARGET_FILE:rwsrt_ref_receiver>)
    set_tests_properties(rwsrt_end_to_end PROPERTIES TIMEOUT 300)
endif()

if(RWSRT_BUILD_FUZZERS)
    foreach(name decode record_stream)
        add_executable(rwsrt_fuzz_${name} fuzz/fuzz_${name}.cc)
        target_link_libraries(rwsrt_fuzz_${name} PRIVATE rwsrt_core)
        target_compile_options(rwsrt_fuzz_${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(rwsrt_fuzz_${name} PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()
