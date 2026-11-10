cmake_minimum_required(VERSION 3.24)
project(agent_io LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The current target is native Windows.")
endif()

add_library(agent_io_contracts OBJECT tools/contracts/header-smoke.cpp)
target_compile_features(agent_io_contracts PRIVATE cxx_std_20)

add_executable(windows_environment_probe tools/environment/windows-probe.cpp)
target_compile_features(windows_environment_probe PRIVATE cxx_std_20)
target_compile_definitions(windows_environment_probe PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

add_executable(job_runner tools/environment/job-runner.cpp)
target_compile_features(job_runner PRIVATE cxx_std_20)
target_compile_definitions(job_runner PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(job_runner PRIVATE psapi)

if(MSVC)
    foreach(target agent_io_contracts windows_environment_probe job_runner)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
    endforeach()
endif()

enable_testing()
add_test(NAME windows_environment_probe COMMAND windows_environment_probe)
set_tests_properties(windows_environment_probe PROPERTIES TIMEOUT 20)
add_test(NAME job_process_tree COMMAND job_runner 64 10
    "${CMAKE_CURRENT_BINARY_DIR}/job-tree.json" $<TARGET_FILE:job_runner> --child tree)
add_test(NAME job_commit_rejection COMMAND job_runner 64 10
    "${CMAKE_CURRENT_BINARY_DIR}/job-reject.json" $<TARGET_FILE:job_runner> --child reject)
set_tests_properties(job_process_tree job_commit_rejection PROPERTIES TIMEOUT 15)

set(AGENT_IO_DISKANN_ROOT "" CACHE PATH "Pinned DiskANN 0.6.1 source")
set(AGENT_IO_DISKANN_PACKAGES "" CACHE PATH "DiskANN NuGet packages directory")
if(AGENT_IO_DISKANN_ROOT)
    add_executable(diskann_probe tools/environment/diskann-probe.cpp)
    target_compile_features(diskann_probe PRIVATE cxx_std_20)
    target_compile_definitions(diskann_probe PRIVATE
        _WINDOWS NOMINMAX UNICODE _WIN32_WINNT=0x0A00)
    target_compile_options(diskann_probe PRIVATE /utf-8 /EHsc /arch:AVX2 /W4)
    target_include_directories(diskann_probe PRIVATE include)
    target_include_directories(diskann_probe SYSTEM PRIVATE
        "${AGENT_IO_DISKANN_ROOT}/include"
        "${AGENT_IO_DISKANN_PACKAGES}/boost/lib/native/include"
        "${AGENT_IO_DISKANN_PACKAGES}/intelopenmp.devel.win/lib/native/include")
    target_link_libraries(diskann_probe PRIVATE
        "${AGENT_IO_DISKANN_ROOT}/x64/Release/diskann.lib" psapi)
    foreach(runtime diskann.dll libiomp5md.dll)
        add_custom_command(TARGET diskann_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AGENT_IO_DISKANN_ROOT}/x64/Release/${runtime}"
                "$<TARGET_FILE_DIR:diskann_probe>")
    endforeach()
endif()
