project(tridigenerator_player)

if(NOT TARGET OpenXR::openxr_loader)
    find_package(OpenXR REQUIRED)
endif()

file(GLOB_RECURSE SRC_FILES
    Src/*.c
    Src/*.cpp
    Src/Core/*.cpp
    Src/Dav1d/*.cpp
    Src/Systems/*.cpp
    Src/Unsorted/*.cpp
)

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
