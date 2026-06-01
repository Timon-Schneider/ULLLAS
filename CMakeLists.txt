cmake_minimum_required(VERSION 3.15)
project(ulllas VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SOURCES
    src/main.c
    src/config.c
    src/ringbuffer.c
    src/pcm_proto.c
    src/udp_tx.c
    src/udp_rx.c
    src/plc.c
    src/fec.c
    src/rt_thread.c
)

set(HEADERS
    src/audio.h
    src/config.h
    src/network.h
    src/pcm_proto.h
    src/plc.h
    src/fec.h
    src/ringbuffer.h
    src/rt_thread.h
    src/asio_backend.h
    src/jack_backend.h
)

if(WIN32)
    if(EXISTS "${CMAKE_SOURCE_DIR}/asio/asio.h")
        list(APPEND SOURCES src/asio_backend.cpp asio/asio.cpp asio/asiodrivers.cpp asio/asiolist.cpp)
        add_compile_definitions(HAS_ASIO)
        message(STATUS "ASIO SDK found, building with ASIO support")
    else()
        message(WARNING "ASIO SDK not found in asio/ directory. Place asio.h, asiodrivers.h, asiodrivers.cpp, asiosys.h, asio.cpp there for Windows ASIO support.")
        add_compile_definitions(NO_ASIO)
    endif()
elseif(APPLE)
    list(APPEND SOURCES src/coreaudio_backend.cpp)
    list(APPEND HEADERS src/coreaudio_backend.h)
    add_compile_definitions(HAS_COREAUDIO)
    message(STATUS "CoreAudio support enabled (macOS)")

    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(JACK jack)
    endif()
    if(JACK_FOUND)
        list(APPEND SOURCES src/jack_backend.cpp)
        add_compile_definitions(HAS_JACK)
        message(STATUS "JACK found, building with JACK support")
    else()
        add_compile_definitions(NO_JACK)
    endif()
else()
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(JACK jack)
    endif()
    if(JACK_FOUND)
        list(APPEND SOURCES src/jack_backend.cpp)
        add_compile_definitions(HAS_JACK)
        message(STATUS "JACK found, building with JACK support")
    else()
        message(WARNING "JACK development headers not found. Install libjack-dev or jack2-devel for Linux JACK support.")
        add_compile_definitions(NO_JACK)
    endif()
endif()

find_package(Threads)

add_executable(ulllas MACOSX_BUNDLE ${SOURCES} ${HEADERS})

target_include_directories(ulllas PRIVATE src)

if(APPLE)
    set_target_properties(ulllas PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.ulllas.audio"
        MACOSX_BUNDLE_BUNDLE_NAME "ULLLAS"
        MACOSX_BUNDLE_BUNDLE_VERSION "2.0.0"
    )
endif()

if(WIN32)
    target_include_directories(ulllas PRIVATE asio)
    # iphlpapi is pulled in by ws2tcpip.h indirectly on some toolchains.
    target_link_libraries(ulllas PRIVATE ws2_32 winmm ole32)
elseif(APPLE)
    find_library(COREAUDIO_LIB CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_LIB AudioToolbox REQUIRED)
    find_library(COREFOUNDATION_LIB CoreFoundation REQUIRED)
    target_link_libraries(ulllas PRIVATE
        ${COREAUDIO_LIB}
        ${AUDIOTOOLBOX_LIB}
        ${COREFOUNDATION_LIB}
    )
    if(Threads_FOUND)
        target_link_libraries(ulllas PRIVATE Threads::Threads)
    endif()
    if(JACK_FOUND)
        target_include_directories(ulllas PRIVATE ${JACK_INCLUDE_DIRS})
        target_link_libraries(ulllas PRIVATE ${JACK_LIBRARIES})
    endif()
else()
    # _GNU_SOURCE for pthread_setschedparam, strcasestr, mlockall, etc.
    target_compile_definitions(ulllas PRIVATE _GNU_SOURCE)
    target_link_libraries(ulllas PRIVATE m)
    if(Threads_FOUND)
        target_link_libraries(ulllas PRIVATE Threads::Threads)
    endif()
    if(JACK_FOUND)
        target_include_directories(ulllas PRIVATE ${JACK_INCLUDE_DIRS})
        target_link_libraries(ulllas PRIVATE ${JACK_LIBRARIES})
    endif()
endif()

if(MSVC)
    target_compile_options(ulllas PRIVATE /W4 /O2)
else()
    target_compile_options(ulllas PRIVATE -Wall -Wextra -O3)
    if(WIN32)
        target_link_options(ulllas PRIVATE -static)
    endif()
endif()

install(TARGETS ulllas
    RUNTIME DESTINATION bin
    BUNDLE  DESTINATION .
)
