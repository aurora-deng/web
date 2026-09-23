cmake_minimum_required(VERSION 3.16)
project(grpc_message_learning LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(grpc_message_codec GrpcMessageCodec.cpp)
target_include_directories(grpc_message_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(grpc_message_learn main.cpp)
target_link_libraries(grpc_message_learn PRIVATE grpc_message_codec)
