# ============================================================
# 文件名：CMakeLists.txt
# ------------------------------------------------------------
# 【职责比喻：工厂图纸 / 装配清单】
#   本文件是整座工厂的"施工图纸"和"装配清单"——它告诉构建系统 CMake：
#   要生产哪些产品（target）、每个产品需要哪些原料（源文件）、用什么工艺
#   （编译选项）、装配顺序（依赖链接）。图纸写好后，一句 cmake -B build
#   就能自动调度编译器、链接器把整座工厂搭起来。
#
# 关键技术点（初学者重点理解）：
#   1. C++20 标准：项目大量使用协程（std::coroutine_handle）、concept 等
#      C++20 特性，必须强制要求 C++20 且关闭编译器扩展。
#   2. 核心库分离（webserver_core）：业务逻辑全部收敛进静态库，主程序和
#      测试程序都链接它，避免源码重复。
#   3. Sanitizer 可选开关：ASan/UBSan/TSan 是内存与线程安全检查工具，
#      INTERFACE 库统一承载编译/链接选项，按需启用。
#   4. 三层测试：GTest 单元测试 + Python 黑盒测试 + libFuzzer 模糊测试，
#      从单元到协议到漏洞挖掘全覆盖。
#   5. WebSocket 模块：握手 / 编解码 / 单帧解析 / 消息组装 / 会话 / 会话工厂 / 管理器 /
#      分发器分层；出站体系使用 OutboundTask + OutboundQueue + TransportWriter，
#      HTTP->WS 协议升级经 SessionFactory 依赖倒置。
#      webserver_core；后续又加入 HTTP/2 与可选 TLS/ALPN 传输层。
# ============================================================

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
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)

# TLS 传输层依赖 SSL_read_ex / SSL_write_ex（OpenSSL 1.1.1+）。仅仅找到名为
# libssl 的库还不够：用户目录可能同时残留旧 OpenSSL 或 BoringSSL，CMake 随后
# 会把不匹配的头文件和库拼在一起，直到编译 TlsTransport.cpp 才出现难懂的报错。
# 在配置阶段检查实际选中的头文件和库，让依赖选错时立即给出可操作的诊断。
include(CheckCXXSymbolExists)
unset(WEBSERVER_HAS_SSL_READ_EX CACHE)
unset(WEBSERVER_HAS_SSL_WRITE_EX CACHE)
set(CMAKE_REQUIRED_INCLUDES "${OPENSSL_INCLUDE_DIR}")
set(CMAKE_REQUIRED_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
check_cxx_symbol_exists(SSL_read_ex "openssl/ssl.h" WEBSERVER_HAS_SSL_READ_EX)
check_cxx_symbol_exists(SSL_write_ex "openssl/ssl.h" WEBSERVER_HAS_SSL_WRITE_EX)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT WEBSERVER_HAS_SSL_READ_EX OR NOT WEBSERVER_HAS_SSL_WRITE_EX)
    message(FATAL_ERROR
        "The selected OpenSSL does not provide SSL_read_ex/SSL_write_ex. "
        "OPENSSL_INCLUDE_DIR='${OPENSSL_INCLUDE_DIR}', "
        "OPENSSL_SSL_LIBRARY='${OPENSSL_SSL_LIBRARY}', "
        "OPENSSL_CRYPTO_LIBRARY='${OPENSSL_CRYPTO_LIBRARY}'. "
        "Use one OpenSSL 1.1.1+ installation for both headers and libraries. "
        "If ~/.local shadows the system installation, set OPENSSL_ROOT_DIR, "
        "OPENSSL_INCLUDE_DIR, OPENSSL_SSL_LIBRARY and OPENSSL_CRYPTO_LIBRARY explicitly.")
endif()

find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)
find_library(NGHTTP2_LIBRARY NAMES nghttp2)
if(NOT NGHTTP2_INCLUDE_DIR OR NOT NGHTTP2_LIBRARY)
    message(FATAL_ERROR "Native HTTP/2 requires libnghttp2-dev (Debian/Ubuntu: sudo apt install libnghttp2-dev).")
endif()

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
option(WEBSERVER_ENABLE_GRPC "Build the optional gRPC service on a separate listener" OFF)
option(WEBSERVER_BUILD_BENCHMARKS "Build repeatable Phase 10 benchmark clients" OFF)

if(WEBSERVER_BUILD_BENCHMARKS AND NOT WEBSERVER_ENABLE_GRPC)
    message(FATAL_ERROR "WEBSERVER_BUILD_BENCHMARKS requires WEBSERVER_ENABLE_GRPC=ON")
endif()

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

# 跨 HTTP/SSE/WebSocket/gRPC 共用的安全基础层。独立成库后，gRPC 组件测试
# 不必为了复用令牌校验而链接 Linux epoll 网络核心。
add_library(webserver_security
    server/security/AuthToken.cpp)
target_include_directories(webserver_security PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(webserver_security PUBLIC OpenSSL::Crypto webserver_sanitizers)

# ==============================================================================
# 核心业务静态库 webserver_core
# 架构设计分离：业务逻辑全部收敛进库，main.cpp只负责组装逻辑、启动服务
# 优势：单元测试、模糊测试、主程序可以复用同一套源码，避免代码重复；便于模块化拆分
# ==============================================================================
add_library(webserver_core)

target_sources(webserver_core PRIVATE
    # core：调度、Reactor、内存与基础设施
    server/ObjectPool/ObjectPool.cpp
    server/Route/Router.cpp
    server/Buffer/Buffer.cpp
    server/buffer_pool/BufferPoll.cpp
    server/thread_pool/thread_pool.cpp
    log/logger/logger.cpp
    server/SubReactor/SubReactor.cpp
    server/Reactor/ReactorGroup.cpp
    server/Runtime/ServerRuntime.cpp
    server/timer/TimeWheel.cpp
    server/SegmentPool/SegmentPool.cpp
    server/CoroutineScheduler/CoroutineScheduler.cpp
    server/CoroutineScheduler/AWaiter.cpp
)

target_sources(webserver_core PRIVATE
    # transport：协议无关出站队列与唯一 socket 写入口
    server/transport/OutboundTask.cpp
    server/transport/OutboundAdmission.cpp
    server/transport/TransportWriter.cpp
    server/transport/OutboundQueue.cpp
)

target_sources(webserver_core PRIVATE
    # http：协议、会话和响应体
    server/http/http.cpp
    server/http/HttpCodec/HttpCodec.cpp
    server/http/HttpParser/HttpParser.cpp
    server/http/HttpSession/HttpSession.cpp
    server/http2/Http2Codec.cpp
    server/http2/Http2Session.cpp
    server/tls/TlsContext.cpp
    server/tls/TlsTransport.cpp
    server/tls/TlsSession.cpp
    server/http/RequestContext/RequestContext.cpp
    server/response/ChunkedBody.cpp
    server/response/FileBody.cpp
    server/response/StringBody.cpp
    server/response/HeaderBody.cpp
    # sse：事件编码、长会话和跨 Reactor 订阅目录
    server/sse/SseCodec.cpp
    server/sse/SseReplayBuffer.cpp
    server/sse/SseSession.cpp
    server/sse/SseSessionManager.cpp
    server/session/ProtocolSessionFactory.cpp
)

target_sources(webserver_core PRIVATE
    # websocket：握手、编解码、分发、会话、会话工厂与跨 Reactor 定位
    server/websocket/WebSocketHandshake/WebSocketHandshake.cpp
    server/websocket/WebSocketCodec/WebSocketCodec.cpp
    server/websocket/WebSocketParser/WebSocketParser.cpp
    server/websocket/WebSocketMessageAssembler/WebSocketMessageAssembler.cpp
    server/websocket/WebSocketValidation/WebSocketValidation.cpp
    server/websocket/WebSocketDelivery/WebSocketDeliveryTracker.cpp
    server/websocket/WebSocketDelivery/WebSocketDeliveryService.cpp
    server/websocket/WebSocketSession/WebSocketSession.cpp
    server/websocket/WebSocketSessionManager/WebSocketSessionManager.cpp
    server/websocket/WebSocketDispatcher/WebSocketDispatcher.cpp
)

# 设置头文件搜索路径：项目根目录对外暴露
# PUBLIC：链接这个库的可执行文件，也会继承该头文件路径
target_include_directories(webserver_core PUBLIC ${PROJECT_SOURCE_DIR})
target_include_directories(webserver_core PUBLIC ${NGHTTP2_INCLUDE_DIR})

# 链接依赖：系统线程库 + sanitizer编译选项接口库
target_link_libraries(webserver_core PUBLIC Threads::Threads webserver_sanitizers webserver_security ${NGHTTP2_LIBRARY} OpenSSL::SSL OpenSSL::Crypto)

# ==============================================================================
# 主程序可执行文件 webserver
# 服务入口，main.cpp，只负责初始化组件、启动Reactor
# ==============================================================================
add_executable(webserver main.cpp)
# 链接核心业务库
target_link_libraries(webserver PRIVATE webserver_core)

# ==============================================================================
# 可选 gRPC 服务：官方 gRPC C++ 负责它自己的 HTTP/2、HPACK、stream 与 trailers。
# 现有 webserver_core 不链接 gRPC，避免把两套 HTTP/2 状态机耦合在一起。
# ==============================================================================
if(WEBSERVER_ENABLE_GRPC)
    # 与 gRPC 官方 C++ 示例一致，要求已安装的 CMake package，并使用 protoc
    # 与 grpc_cpp_plugin 在构建目录生成代码，不提交生成文件。
    set(protobuf_MODULE_COMPATIBLE TRUE)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(gRPC CONFIG REQUIRED)

    if(CMAKE_CROSSCOMPILING)
        find_program(WEBSERVER_PROTOC protoc REQUIRED)
        find_program(WEBSERVER_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
    else()
        set(WEBSERVER_PROTOC $<TARGET_FILE:protobuf::protoc>)
        set(WEBSERVER_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
    endif()

    set(WEBSERVER_GRPC_PROTO
        "${PROJECT_SOURCE_DIR}/server/grpc/proto/web_learning.proto")
    set(WEBSERVER_GRPC_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/grpc")
    file(MAKE_DIRECTORY "${WEBSERVER_GRPC_GENERATED_DIR}")

    set(WEBSERVER_GRPC_PROTO_SRCS
        "${WEBSERVER_GRPC_GENERATED_DIR}/web_learning.pb.cc")
    set(WEBSERVER_GRPC_PROTO_HDRS
        "${WEBSERVER_GRPC_GENERATED_DIR}/web_learning.pb.h")
    set(WEBSERVER_GRPC_SERVICE_SRCS
        "${WEBSERVER_GRPC_GENERATED_DIR}/web_learning.grpc.pb.cc")
    set(WEBSERVER_GRPC_SERVICE_HDRS
        "${WEBSERVER_GRPC_GENERATED_DIR}/web_learning.grpc.pb.h")

    add_custom_command(
        OUTPUT
            "${WEBSERVER_GRPC_PROTO_SRCS}"
            "${WEBSERVER_GRPC_PROTO_HDRS}"
            "${WEBSERVER_GRPC_SERVICE_SRCS}"
            "${WEBSERVER_GRPC_SERVICE_HDRS}"
        COMMAND ${WEBSERVER_PROTOC}
        ARGS
            --grpc_out "${WEBSERVER_GRPC_GENERATED_DIR}"
            --cpp_out "${WEBSERVER_GRPC_GENERATED_DIR}"
            -I "${PROJECT_SOURCE_DIR}/server/grpc/proto"
            "--plugin=protoc-gen-grpc=${WEBSERVER_GRPC_CPP_PLUGIN}"
            "${WEBSERVER_GRPC_PROTO}"
        DEPENDS "${WEBSERVER_GRPC_PROTO}"
        COMMENT "Generating C++ and gRPC sources for web_learning.proto"
        VERBATIM)

    add_library(webserver_grpc
        server/grpc/GrpcServer.cpp
        ${WEBSERVER_GRPC_PROTO_SRCS}
        ${WEBSERVER_GRPC_PROTO_HDRS}
        ${WEBSERVER_GRPC_SERVICE_SRCS}
        ${WEBSERVER_GRPC_SERVICE_HDRS})
    target_include_directories(webserver_grpc PUBLIC
        ${PROJECT_SOURCE_DIR}
        ${WEBSERVER_GRPC_GENERATED_DIR})
    target_link_libraries(webserver_grpc PUBLIC
        gRPC::grpc++
        protobuf::libprotobuf
        Threads::Threads
        webserver_security)
    target_link_libraries(webserver_grpc PRIVATE webserver_sanitizers)

    target_compile_definitions(webserver PRIVATE WEBSERVER_HAS_GRPC=1)
    target_link_libraries(webserver PRIVATE webserver_grpc)

    # 可独立部署/压测的 gRPC 进程。集成 webserver 仍保留，便于对比同进程资源竞争。
    add_executable(webserver-grpc server/grpc/GrpcMain.cpp)
    target_link_libraries(webserver-grpc PRIVATE webserver_grpc)

    if(WEBSERVER_BUILD_BENCHMARKS)
        add_executable(grpc_benchmark benchmarks/grpc_benchmark.cpp)
        target_link_libraries(grpc_benchmark PRIVATE webserver_grpc)
    endif()
endif()

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
    add_executable(phase9_foundation_tests
        tests/phase9_foundation.cpp
        server/sse/SseReplayBuffer.cpp)
    target_include_directories(phase9_foundation_tests PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(phase9_foundation_tests PRIVATE webserver_security)
    add_test(NAME phase9_foundation_tests COMMAND phase9_foundation_tests)

    add_executable(grpc_message_codec_tests
        tests/grpc_message_codec.cpp
        server/grpc/learn/GrpcMessageCodec.cpp)
    target_include_directories(grpc_message_codec_tests PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/server/grpc/learn)
    add_test(NAME grpc_message_codec_tests COMMAND grpc_message_codec_tests)

    if(WEBSERVER_ENABLE_GRPC)
        add_executable(grpc_integration tests/grpc_integration.cpp)
        target_link_libraries(grpc_integration PRIVATE webserver_grpc)
        # Windows 的 gRPC 包通常是 DLL；把传递依赖复制到测试程序旁边，
        # 让 CTest 不依赖用户手工修改 PATH。该生成器表达式要求 CMake 3.21。
        if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
            add_custom_command(TARGET grpc_integration POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:grpc_integration>
                        $<TARGET_FILE_DIR:grpc_integration>
                COMMAND_EXPAND_LISTS)
        endif()
        add_test(NAME grpc_integration COMMAND grpc_integration)
        set_tests_properties(grpc_integration PROPERTIES TIMEOUT 30)
    endif()

    add_executable(http2_stream_coroutine_lifecycle
        tests/http2_stream_coroutine_lifecycle.cpp)
    target_include_directories(http2_stream_coroutine_lifecycle PRIVATE
        ${PROJECT_SOURCE_DIR})
    add_test(NAME http2_stream_coroutine_lifecycle
        COMMAND http2_stream_coroutine_lifecycle)

    add_executable(http2_codec_roundtrip
        tests/http2_codec_roundtrip.cpp server/http2/Http2Codec.cpp)
    target_include_directories(http2_codec_roundtrip PRIVATE
        ${PROJECT_SOURCE_DIR} ${NGHTTP2_INCLUDE_DIR})
    target_link_libraries(http2_codec_roundtrip PRIVATE ${NGHTTP2_LIBRARY})
    add_test(NAME http2_codec_roundtrip COMMAND http2_codec_roundtrip)

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
    add_executable(
        webserver_unit_tests
        tests/unit_tests.cpp
        tests/transport_tests.cpp
        tests/coroutine_lifecycle_tests.cpp
    )
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
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME reliable_inbox_unit
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/python/reliable_websocket_consumer_test.py
        )
    endif()
    if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(OPENSSL_COMMAND openssl)
        if(OPENSSL_COMMAND)
            add_test(
                NAME tls_blackbox
                COMMAND ${Python3_EXECUTABLE}
                        ${PROJECT_SOURCE_DIR}/tests/integration/tls_blackbox.py
                        --server $<TARGET_FILE:webserver>
            )
            set_tests_properties(tls_blackbox PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
        endif()
        add_test(
            NAME http_blackbox
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/integration/http_blackbox.py
                    --server $<TARGET_FILE:webserver>
        )
        add_test(
            NAME websocket_blackbox
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/integration/websocket_blackbox.py
                    --server $<TARGET_FILE:webserver>
        )
        add_test(
            NAME sse_blackbox
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/integration/sse_blackbox.py
                    --server $<TARGET_FILE:webserver>
        )
        add_test(
            NAME phase9_security_blackbox
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/integration/phase9_security_blackbox.py
                    --server $<TARGET_FILE:webserver>
        )
        add_test(
            NAME phase9_shutdown_blackbox
            COMMAND ${Python3_EXECUTABLE}
                    ${PROJECT_SOURCE_DIR}/tests/integration/phase9_shutdown_blackbox.py
                    --server $<TARGET_FILE:webserver>
        )
        # 设置测试超时时间90秒，防止测试卡死阻塞CI
        set_tests_properties(
            http_blackbox websocket_blackbox sse_blackbox
            phase9_security_blackbox phase9_shutdown_blackbox
            PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE
        )
    else()
        message(STATUS "HTTP/WebSocket/SSE blackbox tests disabled (require Linux and Python 3)")
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

    add_executable(websocket_parser_fuzz fuzz/websocket_parser_fuzz.cpp)
    target_link_libraries(websocket_parser_fuzz PRIVATE webserver_core)
    target_compile_options(websocket_parser_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(websocket_parser_fuzz PRIVATE -fsanitize=fuzzer)
endif()
