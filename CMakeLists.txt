cmake_minimum_required(VERSION 3.20)
project(MapleAudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    whisper
    GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
    GIT_TAG v1.7.1
    GIT_SHALLOW TRUE
)

# We only need the library, not the examples or tests
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(whisper)

add_executable(MapleAudio
    src/MapleAudio.cpp
    src/Transcriber.cpp
)

target_include_directories(MapleAudio PRIVATE src)
target_link_libraries(MapleAudio PRIVATE ole32 whisper)

include(CTest)
if(BUILD_TESTING)
    add_executable(queue_tests tests/QueueTests.cpp)
    target_include_directories(queue_tests PRIVATE src)
    add_test(NAME queue_tests COMMAND queue_tests)
endif()