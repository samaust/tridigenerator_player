project(tridigenerator_player LANGUAGES C CXX)

if(UNIX AND NOT ANDROID)
    set(OpenGL_GL_PREFERENCE GLVND)
    find_package(PkgConfig REQUIRED)
    find_package(OpenGL REQUIRED)
    find_package(X11 REQUIRED)
    pkg_check_modules(OPENXR REQUIRED IMPORTED_TARGET openxr)
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
    pkg_check_modules(DAV1D REQUIRED IMPORTED_TARGET dav1d)
    pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

    add_executable(${PROJECT_NAME}
        Src/Linux/LinuxDesktopApp.cpp
        Src/Linux/LinuxOpenXrBackend.cpp
        Src/Data/VipeDataset.cpp
        Src/Videos/WebmInMemoryDemuxer.cpp)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        GL_GLEXT_PROTOTYPES=1
        XR_USE_PLATFORM_XLIB=1
        XR_USE_GRAPHICS_API_OPENGL=1)
    target_include_directories(${PROJECT_NAME} PRIVATE
        Src
        ${CMAKE_CURRENT_LIST_DIR}/../dependencies/meta/SampleXrFramework/Src)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenGL::GL X11::X11 pthread
        PkgConfig::OPENXR PkgConfig::AVFORMAT PkgConfig::AVCODEC
        PkgConfig::AVUTIL PkgConfig::SWSCALE PkgConfig::DAV1D PkgConfig::JSONCPP)
    if(BUILD_TESTING)
        add_executable(vipe_dataset_tests
            Tests/VipeDatasetTests.cpp
            Src/Data/VipeDataset.cpp)
        target_include_directories(vipe_dataset_tests PRIVATE Src)
        target_compile_definitions(vipe_dataset_tests PRIVATE
            VIPE_TEST_MANIFEST="${CMAKE_SOURCE_DIR}/../vipe_encoded/dog-example.json")
        target_link_libraries(vipe_dataset_tests PRIVATE PkgConfig::JSONCPP)
        add_test(NAME vipe_dataset_tests COMMAND vipe_dataset_tests)
        add_executable(vipe_decoder_tests
            Tests/VipeDecoderTests.cpp
            Src/Videos/WebmInMemoryDemuxer.cpp)
        target_include_directories(vipe_decoder_tests PRIVATE
            Src ${CMAKE_CURRENT_LIST_DIR}/../dependencies/meta/SampleXrFramework/Src)
        target_compile_definitions(vipe_decoder_tests PRIVATE
            VIPE_TEST_VIDEO="${CMAKE_SOURCE_DIR}/../vipe_encoded/dog-example.mkv")
        target_link_libraries(vipe_decoder_tests PRIVATE
            PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL
            PkgConfig::SWSCALE PkgConfig::DAV1D)
        add_test(NAME vipe_decoder_tests COMMAND vipe_decoder_tests)
    endif()
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
    return()
endif()

if(NOT TARGET OpenXR::openxr_loader)
    find_package(OpenXR REQUIRED)
endif()

file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS
    Src/*.c
    Src/*.cpp
    Src/Components/*.cpp
    Src/Core/*.cpp
    Src/States/*.cpp
    Src/Systems/*.cpp
    Src/Videos/*.cpp
)
if(ANDROID OR WIN32)
    list(FILTER SRC_FILES EXCLUDE REGEX "/Linux/")
endif()

if(ANDROID)
    add_library(${PROJECT_NAME} MODULE ${SRC_FILES})

    # Show what CMake received from Gradle for debugging
    message(STATUS "ANDROID_NDK = ${ANDROID_NDK}")
    message(STATUS "ANDROID_ABI = ${ANDROID_ABI}")
    message(STATUS "ANDROID_PLATFORM = ${ANDROID_PLATFORM}")
    message(STATUS "CURL_INSTALL_DIR = ${CURL_INSTALL_DIR}")
    message(STATUS "OPENSSL_INSTALL_DIR = ${OPENSSL_INSTALL_DIR}")
    message(STATUS "OPENSSL_SSL_LIBRARY = ${OPENSSL_SSL_LIBRARY}")
    message(STATUS "OPENSSL_CRYPTO_LIBRARY = ${OPENSSL_CRYPTO_LIBRARY}")

    target_include_directories(${PROJECT_NAME} PUBLIC
            ${ANDROID_NDK}/sources/android/native_app_glue
    )

    # Link libraries (only link files that actually exist)
    set(ANDROID_LINK_LIBS
            android
            CURL::libcurl
            EGL
            GLESv3
            jsoncpp_external
            glm
            log
            ktx
            dav1d
            ffmpeg
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ANDROID_LINK_LIBS})
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
elseif(WIN32)
    add_definitions(-D_USE_MATH_DEFINES)
    add_executable(${PROJECT_NAME} ${SRC_FILES})
    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_LIST_DIR}/assets"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/assets"
        VERBATIM)

    add_custom_command(TARGET ${PROJECT_NAME} PRE_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_SOURCE_DIR}/dependencies/meta/SampleXrFramework/res/raw"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/font/res/raw"
        VERBATIM)
endif()

# Common across platforms
target_include_directories(${PROJECT_NAME} PRIVATE Src)
target_link_libraries(${PROJECT_NAME} PRIVATE samplexrframework)
