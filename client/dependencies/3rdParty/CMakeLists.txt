# Initialize optional Android external-build install variables so they exist
# even if the ExternalProject_Add steps are skipped or not yet executed.
# These are cached to allow overriding from the command line or Gradle toolchain.
set(OPENSSL_INSTALL_DIR "" CACHE PATH "OpenSSL install dir (set by openssl_external)")
set(OPENSSL_SSL_LIBRARY "" CACHE FILEPATH "OpenSSL SSL library (set after external build)")
set(OPENSSL_CRYPTO_LIBRARY "" CACHE FILEPATH "OpenSSL Crypto library (set after external build)")
set(CURL_INSTALL_DIR "" CACHE PATH "cURL install dir (set by curl_external)")

# Ensure ANDROID-specific variables have sensible defaults so ExternalProject steps
# won't fail when Gradle/AGP doesn't provide them explicitly.
if(NOT DEFINED ANDROID_NATIVE_API_LEVEL)
    if(DEFINED ANDROID_PLATFORM)
        set(ANDROID_NATIVE_API_LEVEL ${ANDROID_PLATFORM})
    else()
        # fallback to a reasonable default matching this project's minSdk
        set(ANDROID_NATIVE_API_LEVEL 26)
    endif()
endif()

if(NOT DEFINED ANDROID_PLATFORM)
    # keep these in sync if one is set and the other isn't
    set(ANDROID_PLATFORM ${ANDROID_NATIVE_API_LEVEL})
endif()

if(NOT DEFINED JTHREADS)
    include(ProcessorCount)
    ProcessorCount(NPROC)
    if(NPROC GREATER 1)
        math(EXPR JTHREADS "${NPROC} - 1")
    else()
        set(JTHREADS 1)
    endif()
endif()

function(find_ktx)
    # use prebuild library for Android to avoid runtime crash, further
    # investigation needed on building from ktx source
    if (ANDROID)
        set(KTX_DIR ${CMAKE_CURRENT_LIST_DIR}/khronos/ktx)

        add_library(ktx SHARED IMPORTED GLOBAL)
        set(KTXLIB ${KTX_DIR}/lib/android/${ANDROID_ABI}/libktx.so)
        set_target_properties(ktx PROPERTIES
            IMPORTED_LOCATION ${KTXLIB}
            INTERFACE_INCLUDE_DIRECTORIES ${KTX_DIR}/include)
    elseif (WIN32)
        set(KTX_DIR ${CMAKE_CURRENT_LIST_DIR}/khronos/ktx_src)
        include(FetchContent)

        FetchContent_Declare(
            libktx
            GIT_REPOSITORY  https://github.com/KhronosGroup/KTX-Software.git
            GIT_TAG         v4.2.1
            SOURCE_DIR      ${KTX_DIR}
        )

        FetchContent_GetProperties(libktx)
        if(NOT libktx_POPULATED)
            FetchContent_Populate(libktx)
        endif()

        if (WIN32)
            set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "Build static ktx target to avoid DLL hell on Windows")
        endif()

        # Common compile definitions
        add_subdirectory(${libktx_SOURCE_DIR} ${libktx_BINARY_DIR})
    endif()
endfunction()

function(find_zlib)
    set(Z_DIR ${CMAKE_CURRENT_LIST_DIR}/zlib)

    include(FetchContent)

    FetchContent_Declare(
        zlib
        GIT_REPOSITORY  https://github.com/madler/zlib.git
        GIT_TAG         v1.2.13
        SOURCE_DIR      ${Z_DIR}
    )

    FetchContent_GetProperties(zlib)
    if(NOT zlib_POPULATED)
        FetchContent_Populate(zlib)
    endif()

    # Common compile definitions
    add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
endfunction()

function(find_minizip)
    # Define the source files
    set(MINIZIP_SRCS
        ${CMAKE_CURRENT_LIST_DIR}/minizip/src/ioapi.c
        ${CMAKE_CURRENT_LIST_DIR}/minizip/src/miniunz.c
        ${CMAKE_CURRENT_LIST_DIR}/minizip/src/mztools.c
        ${CMAKE_CURRENT_LIST_DIR}/minizip/src/unzip.c
        ${CMAKE_CURRENT_LIST_DIR}/minizip/src/zip.c
    )

    # Add the library
    add_library(minizip STATIC ${MINIZIP_SRCS})

    # Add the include directories
    target_include_directories(minizip PUBLIC ${CMAKE_CURRENT_LIST_DIR}/minizip/src)

    # no-shadow doesn't exist on MSVC
    if(NOT MSVC)
        target_compile_options(minizip PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wno-shadow>
        )
    endif()

    if(ANDROID)
        target_compile_options(minizip PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wno-unused-command-line-argument;-marm;-mfpu=neon>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-command-line-argument;-marm>
        )
        target_link_libraries(minizip z)
    elseif (WIN32)
        target_include_directories(minizip
            PUBLIC
                ${CMAKE_CURRENT_LIST_DIR}/zlib
                ${zlib_BINARY_DIR})
        target_link_libraries(minizip zlibstatic)
    endif()
endfunction()

function(find_stb)
    set(STB_SRC
        ${CMAKE_CURRENT_LIST_DIR}/stb/src/stb_image.c
        ${CMAKE_CURRENT_LIST_DIR}/stb/src/stb_image_write.c
        ${CMAKE_CURRENT_LIST_DIR}/stb/src/stb_vorbis.c
    )

    add_library(stb STATIC ${STB_SRC})

    target_include_directories(stb PUBLIC ${CMAKE_CURRENT_LIST_DIR}/stb/src)
endfunction()

function(find_openxr_loader)
    # Pull openxr loader for windows build as there is no aar method on windows
    if(WIN32)
        include(FetchContent)

        FetchContent_Declare(
            openxr
            GIT_REPOSITORY  https://github.com/KhronosGroup/OpenXR-SDK.git
            GIT_TAG         release-1.1.46
            SOURCE_DIR      ${CMAKE_CURRENT_LIST_DIR}/openxr
        )

        FetchContent_GetProperties(openxr)
        if(NOT openxr_POPULATED)
            FetchContent_Populate(openxr)
        endif()

        # Common compile definitions
        add_subdirectory(${openxr_SOURCE_DIR} ${openxr_BINARY_DIR})
    endif()
endfunction()

function(find_curl)
    if(ANDROID)
        include(FetchContent)

        FetchContent_Declare(
        curl
        URL https://github.com/curl/curl/releases/download/curl-8_17_0/curl-8.17.0.tar.gz
        )

        set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

        # Disable all optional deps
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
        set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
        set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
        set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
        set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
        set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
        set(CURL_USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
        set(CURL_USE_ANDROID_SYSTEM ON CACHE BOOL "" FORCE)

        # Disable features not needed for Android native apps
        set(CURL_DISABLE_FTP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_FILE ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
        set(CURL_DISABLE_SMB ON CACHE BOOL "" FORCE)

        FetchContent_MakeAvailable(curl)
    endif()
endfunction()

function(find_jsoncpp)
    if(ANDROID)
        include(FetchContent)

        FetchContent_Declare(
            jsoncpp
            URL https://github.com/open-source-parsers/jsoncpp/archive/refs/tags/1.9.6.tar.gz
        )

        FetchContent_MakeAvailable(jsoncpp)

        # Build jsoncpp as a static library
        add_library(jsoncpp_static
                ${jsoncpp_SOURCE_DIR}/src/lib_json/json_reader.cpp
                ${jsoncpp_SOURCE_DIR}/src/lib_json/json_value.cpp
                ${jsoncpp_SOURCE_DIR}/src/lib_json/json_writer.cpp
        )

        target_include_directories(jsoncpp_static PUBLIC
                ${jsoncpp_SOURCE_DIR}/include
        )

        # Expose as interface for other targets
        add_library(jsoncpp_external INTERFACE)
        target_link_libraries(jsoncpp_external INTERFACE jsoncpp_static)
    endif()
endfunction()

function(find_glm)
    set(GLM_DIR ${CMAKE_CURRENT_LIST_DIR}/glm)

    include(FetchContent)

    FetchContent_Declare(
        glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG         1.0.2
        SOURCE_DIR      ${GLM_DIR}
    )

    FetchContent_GetProperties(glm)
    if(NOT glm_POPULATED)
        FetchContent_Populate(glm)
    endif()

    #add_library(glm SHARED IMPORTED GLOBAL)
    #add_library(glm_external INTERFACE)
    #target_include_directories(glm
    #        PRIVATE
    #        ${GLM_DIR}
    #)

    add_library(glm INTERFACE)
    target_include_directories( glm INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/glm
    )

endfunction()

function(find_dav1d)
    set(DAV1D_DIR ${CMAKE_CURRENT_LIST_DIR}/dav1d)

    include(FetchContent)

    FetchContent_Declare(
            dav1d
            GIT_REPOSITORY https://github.com/videolan/dav1d.git
            GIT_TAG        1.5.2
            SOURCE_DIR      ${DAV1D_DIR}
    )

    FetchContent_GetProperties(dav1d)
    if(NOT dav1d_POPULATED)
        FetchContent_Populate(dav1d)
    endif()

    set(DAV1D_BUILD_DIR ${dav1d_BINARY_DIR})

    # Use Meson inside CMake
    set(MESON_EXECUTABLE meson)
    set(NINJA_EXECUTABLE ninja)

    # Output directory per ABI
    set(DAV1D_OUT ${CMAKE_CURRENT_BINARY_DIR}/dav1d/${ANDROID_ABI})
    set(DAV1D_INSTALL_DIR ${DAV1D_OUT}/install)

    set(DAV1D_MESON_CROSS_FILE ${CMAKE_CURRENT_BINARY_DIR}/meson-android-${ANDROID_ABI}.ini)

    # Detect host-tag automatically (Linux, macOS, Windows)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        set(NDK_HOST_TAG "linux-x86_64")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(NDK_HOST_TAG "darwin-x86_64")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(NDK_HOST_TAG "windows-x86_64")
    else()
        message(FATAL_ERROR "Unsupported host: ${CMAKE_HOST_SYSTEM_NAME}")
    endif()

    set(NDK_TOOLCHAIN_BIN
            ${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${NDK_HOST_TAG}/bin
    )

    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(ANDROID_C_COMPILER     "aarch64-linux-android21-clang")
        set(ANDROID_CXX_COMPILER   "aarch64-linux-android21-clang++")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(ANDROID_C_COMPILER     "armv7a-linux-androideabi21-clang")
        set(ANDROID_CXX_COMPILER   "armv7a-linux-androideabi21-clang++")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(ANDROID_C_COMPILER     "x86_64-linux-android21-clang")
        set(ANDROID_CXX_COMPILER   "x86_64-linux-android21-clang++")
    endif()

    file(WRITE ${DAV1D_MESON_CROSS_FILE} "
[host_machine]
system = 'android'
cpu_family = '${CMAKE_SYSTEM_PROCESSOR}'
cpu = '${CMAKE_SYSTEM_PROCESSOR}'
endian = 'little'

[binaries]
c = '${NDK_TOOLCHAIN_BIN}/${ANDROID_C_COMPILER}'
cpp = '${NDK_TOOLCHAIN_BIN}/${ANDROID_CXX_COMPILER}'
ar = '${NDK_TOOLCHAIN_BIN}/llvm-ar'
strip = '${NDK_TOOLCHAIN_BIN}/llvm-strip'
nm = '${NDK_TOOLCHAIN_BIN}/llvm-nm'

[properties]
needs_exe_wrapper = true
")

    execute_process(
            COMMAND ${MESON_EXECUTABLE}
            setup
            ${DAV1D_OUT}
            ${dav1d_SOURCE_DIR}
            --buildtype=release
            --default-library=shared
            --cross-file=${DAV1D_MESON_CROSS_FILE}
            --prefix=${DAV1D_INSTALL_DIR}
            RESULT_VARIABLE r
    )
    if(NOT r EQUAL 0)
        message(FATAL_ERROR "Meson configure failed")
    endif()

    execute_process(
            COMMAND ${NINJA_EXECUTABLE} -C ${DAV1D_OUT}
            RESULT_VARIABLE r2
    )
    if(NOT r2 EQUAL 0)
        message(FATAL_ERROR "ninja build failed for dav1d")
    endif()

    execute_process(
            COMMAND ${NINJA_EXECUTABLE} -C ${DAV1D_OUT} install
            RESULT_VARIABLE r3
    )
    if(NOT r3 EQUAL 0)
        message(FATAL_ERROR "ninja build install failed for dav1d")
    endif()

    add_library(dav1d SHARED IMPORTED GLOBAL)
    set_target_properties(dav1d PROPERTIES
            IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/dav1d/${ANDROID_ABI}/install/lib/libdav1d.so
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/dav1d/${ANDROID_ABI}/include"
    )

endfunction()

function(find_ffmpeg)
    set(FFMPEG_DIR ${CMAKE_CURRENT_LIST_DIR}/FFmpeg)
    set(DAV1D_OUT ${CMAKE_CURRENT_BINARY_DIR}/dav1d/${ANDROID_ABI})
    set(FFMPEG_OUT ${CMAKE_CURRENT_BINARY_DIR}/FFmpeg/${ANDROID_ABI})

    set(ENV{ANDROID_NDK} ${CMAKE_ANDROID_NDK})

    include(FetchContent)

    FetchContent_Declare(
            ffmpeg
            GIT_REPOSITORY https://github.com/FFmpeg/FFmpeg.git
            GIT_TAG        n8.0.1
            SOURCE_DIR      ${FFMPEG_DIR}
    )

    FetchContent_GetProperties(dav1d)
    if(NOT dav1d_POPULATED)
        FetchContent_Populate(dav1d)
    endif()

    execute_process(
            COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/build_ffmpeg_android.sh ${DAV1D_OUT} ${FFMPEG_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
            RESULT_VARIABLE r
    )
    if (NOT r EQUAL 0)
        message(FATAL_ERROR "FFmpeg build failed")
    endif()

    add_library(ffmpeg SHARED IMPORTED)
    set_target_properties(ffmpeg PROPERTIES
        IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavformat.so
    )

    include_directories(${FFMPEG_INCLUDE_DIR})

    add_library(avformat SHARED IMPORTED)
    add_library(avcodec SHARED IMPORTED)
    add_library(avutil SHARED IMPORTED)

    set_target_properties(avformat PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavformat.so)
    set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavcodec.so)
    set_target_properties(avutil  PROPERTIES IMPORTED_LOCATION ${FFMPEG_LIB_DIR}/libavutil.so)
endfunction()

find_ktx()
find_stb()

# this need to be defined before minizip since minizip depends on zlib
if (WIN32)
    find_zlib()
endif()
find_minizip()
find_openxr_loader()
find_curl()
find_jsoncpp()
find_glm()
find_dav1d()
find_ffmpeg()
