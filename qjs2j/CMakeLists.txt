cmake_minimum_required(VERSION 3.10)

project(quickjs_java LANGUAGES C)

set(QUICKJS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../quickjs" CACHE PATH "Path to the QuickJS source tree")

add_library(quickjs_jni SHARED
    src/main/c/quickjs_jni.c
    src/main/c/qjs_util.c
    src/main/c/qjs_console.c
    src/main/c/qjs_timer.c
    src/main/c/qjs_callback.c
    "${QUICKJS_ROOT}/dtoa.c"
    "${QUICKJS_ROOT}/libregexp.c"
    "${QUICKJS_ROOT}/libunicode.c"
    "${QUICKJS_ROOT}/quickjs.c"
)

target_compile_features(quickjs_jni PRIVATE c_std_11)
target_compile_definitions(quickjs_jni PRIVATE _GNU_SOURCE)
target_include_directories(quickjs_jni PRIVATE "${QUICKJS_ROOT}" "${CMAKE_CURRENT_LIST_DIR}/src/main/c")

if(NOT ANDROID)
    set(_jni_include_dirs)
    set(_java_home "$ENV{JAVA_HOME}")

    if(NOT _java_home AND APPLE AND EXISTS "/usr/libexec/java_home")
        execute_process(
            COMMAND /usr/libexec/java_home
            OUTPUT_VARIABLE _java_home
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()

    if(_java_home)
        list(APPEND _jni_include_dirs "${_java_home}/include")
        if(APPLE)
            list(APPEND _jni_include_dirs "${_java_home}/include/darwin")
        elseif(WIN32)
            list(APPEND _jni_include_dirs "${_java_home}/include/win32")
        else()
            list(APPEND _jni_include_dirs "${_java_home}/include/linux")
        endif()
    else()
        find_package(JNI QUIET)
        if(JNI_FOUND)
            list(APPEND _jni_include_dirs ${JNI_INCLUDE_DIRS})
        endif()
    endif()

    if(NOT _jni_include_dirs)
        message(FATAL_ERROR "Could not find JNI headers. Set JAVA_HOME to a JDK path.")
    endif()
    target_include_directories(quickjs_jni PRIVATE ${_jni_include_dirs})
endif()

find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(quickjs_jni PRIVATE Threads::Threads)
endif()

find_library(M_LIBRARIES m)
if(M_LIBRARIES)
    target_link_libraries(quickjs_jni PRIVATE ${M_LIBRARIES})
endif()

if(NOT WIN32)
    target_link_libraries(quickjs_jni PRIVATE ${CMAKE_DL_LIBS})
endif()
