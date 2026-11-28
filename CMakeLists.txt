cmake_minimum_required(VERSION 3.24)
project(agent_io LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The current target is native Windows.")
endif()

add_library(agent_io_contracts OBJECT tools/contracts/header-smoke.cpp)
target_compile_features(agent_io_contracts PRIVATE cxx_std_20)

add_library(agent_io_index_contracts OBJECT tools/contracts/header-smoke-b.cpp)
target_compile_features(agent_io_index_contracts PRIVATE cxx_std_20)

add_library(agent_io_index STATIC
    src/index/json.cpp src/index/digest.cpp src/index/format.cpp
    src/index/manifest.cpp src/index/scratch.cpp src/index/adapter.cpp
    src/index/builder.cpp src/index/delta.cpp src/index/recovery.cpp
    src/index/graph.cpp src/index/segment_writer.cpp src/index/publish.cpp)
target_include_directories(agent_io_index PUBLIC include)
target_compile_features(agent_io_index PUBLIC cxx_std_20)
target_compile_definitions(agent_io_index PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(agent_io_index PUBLIC agent_io_storage PRIVATE bcrypt)

add_library(agent_io_engine STATIC src/engine/engine.cpp)
target_include_directories(agent_io_engine PUBLIC include)
target_compile_features(agent_io_engine PUBLIC cxx_std_20)
target_compile_definitions(agent_io_engine PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(agent_io_engine PUBLIC agent_io_index)

add_executable(vector_build tools/build/main.cpp)
target_link_libraries(vector_build PRIVATE agent_io_index)

add_library(agent_io_storage STATIC
    src/storage/synchronous_storage.cpp src/storage/synchronous_block_reader.cpp
    src/storage/optimized_block_reader.cpp src/storage/wal_codec.cpp src/storage/wal_writer.cpp)
target_include_directories(agent_io_storage PUBLIC include)
target_compile_features(agent_io_storage PUBLIC cxx_std_20)
target_compile_definitions(agent_io_storage PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(agent_io_storage PRIVATE ole32)

add_library(agent_io_benchmark STATIC src/benchmark/runner.cpp)
target_include_directories(agent_io_benchmark PUBLIC include)
target_compile_features(agent_io_benchmark PUBLIC cxx_std_20)
target_compile_definitions(agent_io_benchmark PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(agent_io_benchmark PUBLIC agent_io_storage PRIVATE psapi)

add_executable(benchmark_runner_test tests/benchmark/benchmark-runner-test.cpp)
target_link_libraries(benchmark_runner_test PRIVATE agent_io_benchmark)

add_executable(index_engine_test tests/index/index-engine-test.cpp)
target_link_libraries(index_engine_test PRIVATE agent_io_engine)

add_executable(dynamic_engine_test tests/index/dynamic-engine-test.cpp)
add_executable(maintenance_test tests/index/maintenance-test.cpp)
target_link_libraries(maintenance_test PRIVATE agent_io_engine)
target_link_libraries(dynamic_engine_test PRIVATE agent_io_engine)

add_executable(write_probe tools/dynamic/write-probe.cpp)
target_link_libraries(write_probe PRIVATE agent_io_engine)

add_executable(synchronous_storage_test tests/storage/synchronous-storage-test.cpp)
target_link_libraries(synchronous_storage_test PRIVATE agent_io_storage)
target_compile_definitions(synchronous_storage_test PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

add_executable(wal_test tests/storage/wal-test.cpp)
target_link_libraries(wal_test PRIVATE agent_io_storage)
target_include_directories(wal_test PRIVATE src/storage)
target_compile_definitions(wal_test PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

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
    foreach(target agent_io_contracts agent_io_index_contracts
                   agent_io_storage agent_io_index agent_io_engine
                   agent_io_benchmark benchmark_runner_test index_engine_test
                   dynamic_engine_test maintenance_test
                   synchronous_storage_test wal_test vector_build write_probe
                   windows_environment_probe job_runner)
        target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
    endforeach()
endif()

# Reuse the pinned Boost 1.78 headers already installed for DiskANN. Core
# storage and runner builds remain independent of this CLI-only JSON library.
set(AGENT_IO_TEST_PYTHON "${CMAKE_CURRENT_SOURCE_DIR}/.venv/Scripts/python.exe"
    CACHE FILEPATH "Python for native producer and contract integration tests")
find_path(AGENT_IO_BOOST_INCLUDE boost/json/src.hpp HINTS
    "${AGENT_IO_DISKANN_PACKAGES}/boost/lib/native/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/build/diskann/packages/boost/lib/native/include")
if(AGENT_IO_BOOST_INCLUDE)
    add_library(agent_io_benchmark_json STATIC tools/benchmark/boost-json.cpp)
    target_include_directories(agent_io_benchmark_json SYSTEM PUBLIC "${AGENT_IO_BOOST_INCLUDE}")
    target_compile_definitions(agent_io_benchmark_json PUBLIC
        BOOST_JSON_NO_LIB BOOST_CONTAINER_NO_LIB)
    target_compile_features(agent_io_benchmark_json PUBLIC cxx_std_20)
    add_executable(vector_bench tools/benchmark/main.cpp tools/benchmark/config.cpp
        tools/benchmark/engine-target.cpp)
    target_link_libraries(vector_bench PRIVATE agent_io_engine)
    target_link_libraries(vector_bench PRIVATE agent_io_benchmark agent_io_benchmark_json)
    target_compile_definitions(vector_bench PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
        AGENT_IO_BUILD_CONFIG="$<CONFIG>")
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE AGENT_IO_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    execute_process(COMMAND git status --porcelain WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE AGENT_IO_GIT_STATUS
        OUTPUT_VARIABLE AGENT_IO_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT AGENT_IO_BUILD_COMMIT OR NOT AGENT_IO_GIT_STATUS STREQUAL "0")
        set(AGENT_IO_DIRTY_VALUE "unavailable")
    elseif(AGENT_IO_DIRTY)
        set(AGENT_IO_DIRTY_VALUE "true")
    else()
        set(AGENT_IO_DIRTY_VALUE "false")
    endif()
    # A source snapshot without git metadata still has to disclose provenance:
    # the field is required by EVAL-1.1 and must never be an empty string.
    if(NOT AGENT_IO_BUILD_COMMIT)
        set(AGENT_IO_BUILD_COMMIT "unavailable-no-git-metadata")
    endif()
    target_compile_definitions(vector_bench PRIVATE
        AGENT_IO_BUILD_COMMIT="${AGENT_IO_BUILD_COMMIT}"
        AGENT_IO_BUILD_DIRTY="${AGENT_IO_DIRTY_VALUE}")
    if(MSVC)
        target_compile_options(vector_bench PRIVATE /W4 /WX /permissive- /EHsc)
    endif()
else()
    message(STATUS "vector_bench CLI disabled: set AGENT_IO_BOOST_INCLUDE to pinned Boost 1.78")
endif()

enable_testing()
if(EXISTS "${AGENT_IO_TEST_PYTHON}")
    add_test(NAME stage3_b_contracts COMMAND "${AGENT_IO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/contracts/check-stage3-b.py")
    add_test(NAME stage3_b_regressions COMMAND "${AGENT_IO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/contracts/test-stage3-b.py")
    add_test(NAME stage3_manifest_contract COMMAND "${AGENT_IO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/contracts/test-stage3-freeze.py")
    add_test(NAME stage5_read_path_checks COMMAND "${AGENT_IO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/storage/test-verify-read-path.py")
    set_tests_properties(stage3_b_contracts stage3_b_regressions stage3_manifest_contract
        stage5_read_path_checks PROPERTIES TIMEOUT 60)
endif()
add_test(NAME benchmark_runner COMMAND benchmark_runner_test)
set_tests_properties(benchmark_runner PROPERTIES TIMEOUT 30)
if(TARGET vector_bench AND EXISTS "${AGENT_IO_TEST_PYTHON}")
    add_test(NAME benchmark_cli COMMAND "${AGENT_IO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/benchmark/check-benchmark.py"
        --binary $<TARGET_FILE:vector_bench> --job-runner $<TARGET_FILE:job_runner>
        --output-parent "${CMAKE_CURRENT_BINARY_DIR}/benchmark-cli-$<CONFIG>")
    set_tests_properties(benchmark_cli PROPERTIES TIMEOUT 60)
endif()
add_test(NAME index_engine COMMAND index_engine_test)
set_tests_properties(index_engine PROPERTIES TIMEOUT 120)
add_test(NAME dynamic_engine COMMAND dynamic_engine_test)
set_tests_properties(dynamic_engine PROPERTIES TIMEOUT 180)
add_test(NAME maintenance COMMAND maintenance_test)
set_tests_properties(maintenance PROPERTIES TIMEOUT 300)
if(TARGET vector_build AND EXISTS "${AGENT_IO_TEST_PYTHON}")
    # The refusal matrix: every rejected configuration must leave the published
    # index byte-identical, so this has to run against the real binaries.
    add_test(NAME build_contract COMMAND "${AGENT_IO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/contracts/check-build.py"
        --binary $<TARGET_FILE:vector_build> --job-runner $<TARGET_FILE:job_runner>
        --output "${CMAKE_CURRENT_BINARY_DIR}/build-contract-$<CONFIG>")
    set_tests_properties(build_contract PROPERTIES TIMEOUT 120)
endif()
add_test(NAME synchronous_storage COMMAND synchronous_storage_test)
set_tests_properties(synchronous_storage PROPERTIES TIMEOUT 30)
add_test(NAME wal COMMAND wal_test)
set_tests_properties(wal PROPERTIES TIMEOUT 30)
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
        agent_io_storage "${AGENT_IO_DISKANN_ROOT}/x64/Release/diskann.lib" psapi)
    foreach(runtime diskann.dll libiomp5md.dll)
        add_custom_command(TARGET diskann_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${AGENT_IO_DISKANN_ROOT}/x64/Release/${runtime}"
                "$<TARGET_FILE_DIR:diskann_probe>")
    endforeach()
endif()
