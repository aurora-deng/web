cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


include_directories(${PROJECT_SOURCE_DIR})


add_executable(
    webserver
    main.cpp
    server/threadpoll/thread_pool.cpp
    server/SubReactor/SubReactor.cpp
    server/http/http.cpp
    server/Route/Router.cpp
    server/Buffer/Buffer.cpp
    log/logger/logger.cpp
    server/timer/TimeWheel.cpp
    
    )


target_link_libraries(webserver pthread)