# M0 spike: public-header compile, package discovery and SRT API behaviour.
# Standalone on purpose: it has no GNU Radio dependency, so a failure here is
# a transport/package problem and not a scheduler problem.
cmake_minimum_required(VERSION 3.20)
project(rwsrt_m0_srt_api LANGUAGES CXX)

find_package(RobotweaxSRT 0.3 CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_executable(rwsrt_m0_srt_api srt_api_spike.cc)
target_compile_features(rwsrt_m0_srt_api PRIVATE cxx_std_20)
set_target_properties(rwsrt_m0_srt_api PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(rwsrt_m0_srt_api PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(rwsrt_m0_srt_api PRIVATE
    RWSRT_FOUND_PACKAGE_VERSION="${RobotweaxSRT_VERSION}")
target_link_libraries(rwsrt_m0_srt_api PRIVATE RobotweaxSRT::srt Threads::Threads)
