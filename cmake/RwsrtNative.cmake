# Native (GNU Radio independent) libraries of gr-robotweax-srt.
#
#   rwsrt_core       wire codec, continuity, queues, framing, state machine
#   rwsrt_transport  SRT adapter and session worker (needs RobotweaxSRT::srt)
#
# Included by the GNU Radio module build and by the standalone project in
# tests/, which builds the same sources without GNU Radio for sanitizers.
include_guard(GLOBAL)

set(RWSRT_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../lib")
set(RWSRT_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

add_library(rwsrt_core STATIC
    "${RWSRT_LIB_DIR}/wire_codec.cc"
    "${RWSRT_LIB_DIR}/sample_time.cc"
    "${RWSRT_LIB_DIR}/continuity.cc"
    "${RWSRT_LIB_DIR}/bounded_record_queue.cc"
    "${RWSRT_LIB_DIR}/record_assembler.cc"
    "${RWSRT_LIB_DIR}/session_state.cc"
    "${RWSRT_LIB_DIR}/session_config.cc")
target_include_directories(rwsrt_core PUBLIC "${RWSRT_LIB_DIR}")
target_compile_features(rwsrt_core PUBLIC cxx_std_20)
target_compile_options(rwsrt_core PRIVATE ${RWSRT_WARNINGS})
set_target_properties(rwsrt_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden)
find_package(Threads REQUIRED)
target_link_libraries(rwsrt_core PUBLIC Threads::Threads)

# Transport: the only target that includes <srt/srt.h>.
if(NOT TARGET RobotweaxSRT::srt)
    find_package(RobotweaxSRT 0.3 CONFIG REQUIRED)
endif()
add_library(rwsrt_transport STATIC
    "${RWSRT_LIB_DIR}/runtime_owner.cc"
    "${RWSRT_LIB_DIR}/srt_adapter.cc"
    "${RWSRT_LIB_DIR}/session_worker.cc")
target_compile_options(rwsrt_transport PRIVATE ${RWSRT_WARNINGS})
set_target_properties(rwsrt_transport PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden)
target_link_libraries(rwsrt_transport PUBLIC rwsrt_core PRIVATE RobotweaxSRT::srt)
