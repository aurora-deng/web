cmake_minimum_required(VERSION 3.16)
project(webserver LANGUAGES CXX)

# 核心库依赖 C++20 协程；显式关闭编译器扩展，可避免代码无意间依赖某个工具链的私有语法，
# 也让后续学习或迁移到其他 Linux 编译器时更容易区分“标准能力”和“平台扩展”。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)

# Sanitizer 统一做成可选构建能力：正常构建不承担运行时开销，排查内存越界、未定义行为和数据竞争时
# 则可在独立构建目录启用。TSan 与 ASan/UBSan 的运行时不能可靠共存，因此下面主动阻止混用。
option(WEBSERVER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(WEBSERVER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(WEBSERVER_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(WEBSERVER_BUILD_FUZZER "Build the libFuzzer HttpParser harness (Clang only)" OFF)

if(WEBSERVER_ENABLE_TSAN AND (WEBSERVER_ENABLE_ASAN OR WEBSERVER_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan or UBSan; use separate build directories.")
endif()

add_library(webserver_sanitizers INTERFACE)
if(WEBSERVER_ENABLE_ASAN)
    target_compile_options(webserver_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(webserver_sanitizers INTERFACE -fsanitize=address)
endif()
if(WEBSERVER_ENABLE_UBSAN)
    target_compile_options(webserver_sanitizers INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(webserver_sanitizers INTERFACE -fsanitize=undefined)
endif()
if(WEBSERVER_ENABLE_TSAN)
    target_compile_options(webserver_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(webserver_sanitizers INTERFACE -fsanitize=thread)
endif()

# 将协议、路由、Reactor、响应体等实现收敛为核心库，而 main.cpp 只负责组装和启动。
# 这种边界既让单元测试复用真实服务器代码，也方便后续把入口替换成示例程序或其他部署方式。
add_library(
    webserver_core


    server/ObjectPool/ObjectPool.cpp
    server/http/http.cpp
    server/http/HttpCodec/HttpCodec.cpp
    server/http/HttpParser/HttpParser.cpp
    server/http/ResponseSender/ResponseSender.cpp
    server/http/HttpSession/HttpSession.cpp
    server/http/RequestContext/RequestContext.cpp

    server/Route/Router.cpp

    server/Buffer/Buffer.cpp
    server/BufferPoll/BufferPoll.cpp
    server/threadpoll/thread_pool.cpp

    log/logger/logger.cpp


    server/Repsonse/ChunkedBody.cpp
    server/Repsonse/FileBody.cpp
    server/Repsonse/StringBody.cpp
    server/Repsonse/HeaderBody.cpp

    server/SubReactor/SubReactor.cpp
    server/Reactor/ReactorGroup.cpp
    server/Runtime/ServerRuntime.cpp

    server/timer/TimeWheel.cpp
    server/SegmentPool/SegmentPool.cpp

 
    
    server/CoroutineScheduler/CoroutineScheduler.cpp
    server/CoroutineScheduler/AWaiter.cpp
    
)

target_include_directories(webserver_core PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(webserver_core PUBLIC Threads::Threads webserver_sanitizers)

add_executable(webserver main.cpp)
target_link_libraries(webserver PRIVATE webserver_core)
add_custom_command(
    TARGET webserver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/static
            $<TARGET_FILE_DIR:webserver>/static
    COMMENT "Copying static assets next to webserver"
)

if(BUILD_TESTING)
    # 测试依赖只从系统查找，不在配置阶段联网下载；这样离线/教学环境的构建结果更可重复，
    # 依赖缺失时也能立即得到明确错误，而不是在第三方下载阶段失败。
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        find_package(GTest QUIET)
    endif()
    if(NOT GTest_FOUND)
        message(FATAL_ERROR
            "BUILD_TESTING=ON requires a system-installed GoogleTest. "
            "Install it (for example: apt install libgtest-dev) or configure "
            "with -DBUILD_TESTING=OFF. This project never downloads test dependencies.")
    endif()
    if(NOT TARGET GTest::gtest_main)
        if(TARGET GTest::Main)
            add_library(GTest::gtest_main ALIAS GTest::Main)
        else()
            message(FATAL_ERROR "GoogleTest was found, but no gtest_main CMake target is available.")
        endif()
    endif()

    add_executable(webserver_unit_tests tests/unit_tests.cpp)
    target_link_libraries(webserver_unit_tests PRIVATE webserver_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(webserver_unit_tests)

    # 黑盒测试依赖 Linux 的网络/进程语义，因此仅在平台条件满足时注册；
    # 其他平台仍可构建核心库和单元测试，不会因不适用的集成测试阻塞开发。
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(
            NAME http_blackbox
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/integration/http_blackbox.py
                    --server $<TARGET_FILE:webserver>
        )
        set_tests_properties(http_blackbox PROPERTIES TIMEOUT 90)
    else()
        message(STATUS "http_blackbox test disabled (requires Linux and Python 3)")
    endif()
endif()

if(WEBSERVER_BUILD_FUZZER)
    # 模糊测试入口直接链接核心库，保证它验证的是生产解析器；libFuzzer 属于 Clang 能力，
    # 配置期限制编译器可以避免生成一个到链接阶段才失败的目标。
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "WEBSERVER_BUILD_FUZZER requires Clang with libFuzzer support.")
    endif()
    add_executable(http_parser_fuzz fuzz/http_parser_fuzz.cpp)
    target_link_libraries(http_parser_fuzz PRIVATE webserver_core)
    target_compile_options(http_parser_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(http_parser_fuzz PRIVATE -fsanitize=fuzzer)
endif()