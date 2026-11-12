# Clean-prefix C++ consumer: builds only against the installed module.
cmake_minimum_required(VERSION 3.20)
project(rwsrt_install_consumer LANGUAGES CXX)
find_package(Gnuradio "3.10" REQUIRED)
find_package(gnuradio-robotweax_srt REQUIRED)
add_executable(rwsrt_install_consumer main.cc)
target_link_libraries(rwsrt_install_consumer PRIVATE gnuradio::gnuradio-robotweax_srt)
