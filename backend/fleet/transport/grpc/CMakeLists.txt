find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG QUIET)
if(TARGET gRPC::grpc++)
    set(fleet_grpc_target gRPC::grpc++)
else()
    # Ubuntu Jammy publishes pkg-config metadata in its gRPC development package.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(fleet_grpc REQUIRED IMPORTED_TARGET GLOBAL grpc++)
    set(fleet_grpc_target PkgConfig::fleet_grpc)
endif()
if(TARGET gRPC::grpc_cpp_plugin)
    set(fleet_grpc_plugin gRPC::grpc_cpp_plugin)
    set(fleet_grpc_plugin_path $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
else()
    find_program(fleet_grpc_plugin grpc_cpp_plugin REQUIRED)
    set(fleet_grpc_plugin_path "${fleet_grpc_plugin}")
endif()
if(NOT TARGET protobuf::protoc)
    message(FATAL_ERROR "fleet requires installed protoc")
endif()

set(fleet_proto "${CMAKE_CURRENT_SOURCE_DIR}/proto/fleet.proto")
set(fleet_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${fleet_generated}")
add_custom_command(
    OUTPUT "${fleet_generated}/fleet.pb.cc" "${fleet_generated}/fleet.pb.h"
           "${fleet_generated}/fleet.grpc.pb.cc" "${fleet_generated}/fleet.grpc.pb.h"
    COMMAND protobuf::protoc
    ARGS "--cpp_out=${fleet_generated}" "--grpc_out=${fleet_generated}"
         "--plugin=protoc-gen-grpc=${fleet_grpc_plugin_path}"
         "-I${CMAKE_CURRENT_SOURCE_DIR}/proto" "${fleet_proto}"
    DEPENDS "${fleet_proto}" protobuf::protoc ${fleet_grpc_plugin}
    VERBATIM)
add_library(nebula_fleet_protocol "${fleet_generated}/fleet.pb.cc" "${fleet_generated}/fleet.grpc.pb.cc")
target_include_directories(nebula_fleet_protocol PUBLIC "${fleet_generated}")
target_link_libraries(nebula_fleet_protocol PUBLIC ${fleet_grpc_target} protobuf::libprotobuf)

add_library(nebula_fleet_rpc FleetWire/FleetWire.cpp FleetService/FleetService.cpp EdgeSession/EdgeSession.cpp)
target_link_libraries(nebula_fleet_rpc PUBLIC nebula_fleet_protocol nebula_fleet_application)

add_executable(nebula_fleet_coordinator program/CoordinatorMain.cpp)
target_link_libraries(nebula_fleet_coordinator PRIVATE nebula_fleet_rpc nebula_fleet_ledger)
add_executable(nebula_fleet_edge program/EdgeMain.cpp)
target_link_libraries(nebula_fleet_edge PRIVATE nebula_fleet_rpc nebula_fleet_ledger
    nebula_fleet_simulator nebula_workflow_observer)
add_executable(nebula_fleet_ctl program/FleetCtl.cpp)
target_link_libraries(nebula_fleet_ctl PRIVATE nebula_fleet_rpc)

