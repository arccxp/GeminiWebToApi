cmake_minimum_required(VERSION 3.16)
project(gemini_web_to_api LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GWA_CORE_SOURCES
    src/cookies.cpp
    src/gemini_web.cpp
    src/jspb.cpp
    src/modelname.cpp
    src/relay.cpp
    src/session.cpp
    src/transport.cpp
    src/usage.cpp
    src/util.cpp
)

add_library(gemini_web STATIC ${GWA_CORE_SOURCES})
target_include_directories(gemini_web PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(WIN32)
    target_link_libraries(gemini_web PUBLIC winhttp bcrypt crypt32)
else()
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(gemini_web PUBLIC CURL::libcurl Threads::Threads)
endif()

add_library(gemini_web_ffi SHARED src/c_api.cpp)
target_compile_definitions(gemini_web_ffi PRIVATE GWA_BUILD_DLL)
target_link_libraries(gemini_web_ffi PRIVATE gemini_web)

option(GWA_BUILD_TESTS "Build unit tests" ON)
if(GWA_BUILD_TESTS)
    enable_testing()
    add_executable(gemini_web_tests tests/test_core.cpp src/c_api.cpp)
    target_link_libraries(gemini_web_tests PRIVATE gemini_web)
    add_test(NAME gemini_web_tests COMMAND gemini_web_tests)
endif()
