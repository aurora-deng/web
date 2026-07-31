# ==============================================================================
# CMake 最低版本约束
# 项目中用到的 CMake 特性（INTERFACE 库、生成器表达式、gtest_discover_tests 等）要求至少3.16
# ==============================================================================
cmake_minimum_required(VERSION 3.16)

# 定义项目名称 webserver，指定编程语言仅使用 C++
project(webserver LANGUAGES CXX)

# ==============================================================================
# C++ 标准配置
# 项目基于 C++20，大量使用协程（std::coroutine_handle）、新范围for、concept等C++20特性
# ==============================================================================
# 指定C++标准为C++20
set(CMAKE_CXX_STANDARD 20)
# 强制要求编译器必须支持指定C++标准，不自动回退到低版本
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 关闭编译器私有扩展（如GNU扩展 -std=c++20 vs -std=gnu++20）
# 保证代码严格遵循ISO C++标准，提升跨编译器可移植性，防止无意间依赖GCC拓展语法
set(CMAKE_CXX_EXTENSIONS OFF)

# 启用CTest测试框架（后续 enable_testing() 由 include(CTest) 自动引入）
include(CTest)
# 查找系统线程库，Linux下封装pthread，必须依赖
find_package(Threads REQUIRED)

# ==============================================================================
# 可选编译选项：各类Sanitizer内存/线程检查工具
# ASan：内存越界、野指针、内存泄漏
# UBSan：未定义行为（整数溢出、空指针解引用、类型转换错误）
# TSan：线程竞争、数据竞争（多线程竞态条件）
# fuzzer：libFuzzer模糊测试，仅Clang支持
# option(变量名 提示文本 默认值)，编译时可通过 -Dxxx=ON/OFF 控制开关
# ==============================================================================
option(WEBSERVER_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(WEBSERVER_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(WEBSERVER_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(WEBSERVER_BUILD_FUZZER "Build the libFuzzer HttpParser harness (Clang only)" OFF)

# TSan 和 ASan/UBSan 运行时库冲突，无法同时启用，直接报错终止构建
if(WEBSERVER_ENABLE_TSAN AND (WEBSERVER_ENABLE_ASAN OR WEBSERVER_ENABLE_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan or UBSan; use separate build directories.")
endif()

# 创建INTERFACE接口库：webserver_sanitizers
# INTERFACE库不生成目标文件，仅统一承载编译选项、链接选项，供其他target链接复用
add_library(webserver_sanitizers INTERFACE)

# 启用AddressSanitizer配置
if(WEBSERVER_ENABLE_ASAN)
    # 编译参数：INTERFACE 开启asan，保留调用栈信息,方便定位崩溃位置
    target_compile_options(webserver_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    # 链接参数：链接asan运行时库
    target_link_options(webserver_sanitizers INTERFACE -fsanitize=address)
endif()

# 启用未定义行为检查UBSan
if(WEBSERVER_ENABLE_UBSAN)
    target_compile_options(webserver_sanitizers INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(webserver_sanitizers INTERFACE -fsanitize=undefined)
endif()

# 启用线程竞争检查TSan
if(WEBSERVER_ENABLE_TSAN)
    target_compile_options(webserver_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(webserver_sanitizers INTERFACE -fsanitize=thread)
endif()

# ==============================================================================
# 核心业务静态库 webserver_core
# 架构设计分离：业务逻辑全部收敛进库，main.cpp只负责组装逻辑、启动服务
# 优势：单元测试、模糊测试、主程序可以复用同一套源码，避免代码重复；便于模块化拆分
# ==============================================================================
add_library(
    webserver_core
    # 对象池
    server/ObjectPool/ObjectPool.cpp

    # HTTP协议基础模块
    server/http/http.cpp
    server/http/HttpCodec/HttpCodec.cpp
    server/http/HttpParser/HttpParser.cpp
    server/http/ResponseSender/ResponseSender.cpp
    server/http/HttpSession/HttpSession.cpp
    server/http/RequestContext/RequestContext.cpp

    # 路由模块
    server/Route/Router.cpp

    # 缓冲区模块
    server/Buffer/Buffer.cpp
    server/BufferPoll/BufferPoll.cpp

    # 普通线程池
    server/threadpoll/thread_pool.cpp

    # 日志模块
    log/logger/logger.cpp

    # HTTP响应体实现：多种body类型
    server/Repsonse/ChunkedBody.cpp
    server/Repsonse/FileBody.cpp
    server/Repsonse/StringBody.cpp
    server/Repsonse/HeaderBody.cpp

    # Reactor IO多路复用框架
    server/SubReactor/SubReactor.cpp
    server/Reactor/ReactorGroup.cpp
    server/Runtime/ServerRuntime.cpp

    # 时间轮定时器
    server/timer/TimeWheel.cpp
    # 内存分段池
    server/SegmentPool/SegmentPool.cpp

    # C++20协程调度模块
    server/CoroutineScheduler/CoroutineScheduler.cpp
    server/CoroutineScheduler/AWaiter.cpp
)

# 设置头文件搜索路径：项目根目录对外暴露
# PUBLIC：链接这个库的可执行文件，也会继承该头文件路径
target_include_directories(webserver_core PUBLIC ${PROJECT_SOURCE_DIR})

# 链接依赖：系统线程库 + sanitizer编译选项接口库
target_link_libraries(webserver_core PUBLIC Threads::Threads webserver_sanitizers)

# ==============================================================================
# 主程序可执行文件 webserver
# 服务入口，main.cpp，只负责初始化组件、启动Reactor
# ==============================================================================
add_executable(webserver main.cpp)
# 链接核心业务库
target_link_libraries(webserver PRIVATE webserver_core)

# 构建完成后自动复制静态资源目录static到程序输出目录
# $<TARGET_FILE_DIR:webserver> 生成器表达式：自动获取webserver可执行文件所在目录
add_custom_command(
    TARGET webserver POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/static
            $<TARGET_FILE_DIR:webserver>/static
    COMMENT "Copying static assets next to webserver"
)

# ==============================================================================
# 单元测试配置 BUILD_TESTING=ON 生效（cmake -DBUILD_TESTING=ON）
# 使用GoogleTest单元测试框架
# 策略：只寻找系统预装GTest，不自动下载源码编译，保证构建环境可控、支持离线编译
# ==============================================================================
if(BUILD_TESTING)
    # 优先寻找CONFIG模式GTest（现代系统cmake包），找不到再使用旧版模块模式查找
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        find_package(GTest QUIET)
    endif()

    # 未找到GTest直接抛出致命错误，提示用户安装库或者关闭测试
    if(NOT GTest_FOUND)
        message(FATAL_ERROR
            "BUILD_TESTING=ON requires a system-installed GoogleTest. "
            "Install it, for example:\n"
            "  Debian/Ubuntu: sudo apt install libgtest-dev\n"
            "  CentOS/RHEL:   sudo dnf install gtest-devel\n"
            "Or configure with -DBUILD_TESTING=OFF. "
            "This project never downloads test dependencies.")
    endif()

    # 兼容不同系统GTest导出target名称差异：部分环境目标名为GTest::Main而非gtest_main
    if(NOT TARGET GTest::gtest_main)
        if(TARGET GTest::Main)
            add_library(GTest::gtest_main ALIAS GTest::Main)
        else()
            message(FATAL_ERROR "GoogleTest was found, but no gtest_main CMake target is available.")
        endif()
    endif()
    if(NOT TARGET GTest::gtest AND TARGET GTest::GTest)
        add_library(GTest::gtest ALIAS GTest::GTest)
    endif()

    # 单元测试可执行程序
    add_executable(webserver_unit_tests tests/unit_tests.cpp)
    # 部分发行版的 gtest_main 不会自动带上 gtest 本体，显式链接更稳妥。
    if(TARGET GTest::gtest)
        target_link_libraries(webserver_unit_tests PRIVATE webserver_core GTest::gtest GTest::gtest_main)
    else()
        target_link_libraries(webserver_unit_tests PRIVATE webserver_core GTest::gtest_main)
    endif()
    # 自动扫描所有TEST宏，注册测试用例，不需要手动add_test
    include(GoogleTest)
    # PRE_TEST：避免在构建阶段执行测试二进制做用例发现（全局线程等可能导致 POST_BUILD 失败）。
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.18")
        gtest_discover_tests(webserver_unit_tests DISCOVERY_MODE PRE_TEST)
    else()
        gtest_discover_tests(webserver_unit_tests)
    endif()

    # ==========================================================================
    # 集成黑盒测试：Python脚本启动真实服务，发送网络请求测试完整链路
    # 限制：仅Linux平台启用，依赖Python3解释器
    # ==========================================================================
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(
            NAME http_blackbox
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/integration/http_blackbox.py
                    --server $<TARGET_FILE:webserver>
        )
        # 设置测试超时时间90秒，防止测试卡死阻塞CI
        set_tests_properties(http_blackbox PROPERTIES TIMEOUT 90)
    else()
        message(STATUS "http_blackbox test disabled (requires Linux and Python 3)")
    endif()
endif()

# ==============================================================================
# libFuzzer 模糊测试目标：HTTP解析器压力测试，挖掘解析器漏洞
# 限制：仅Clang编译器支持libFuzzer，GCC不支持
# ==============================================================================
if(WEBSERVER_BUILD_FUZZER)
    # 校验编译器类型，非Clang直接报错
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "WEBSERVER_BUILD_FUZZER requires Clang with libFuzzer support.")
    endif()
    # 模糊测试程序
    add_executable(http_parser_fuzz fuzz/http_parser_fuzz.cpp)
    target_link_libraries(http_parser_fuzz PRIVATE webserver_core)
    # 开启libFuzzer编译与链接标志
    target_compile_options(http_parser_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(http_parser_fuzz PRIVATE -fsanitize=fuzzer)
endif()