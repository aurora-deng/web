cmake_minimum_required(VERSION 3.16)
project(nebula_actuator_control LANGUAGES CXX)

include(CTest)
find_package(Threads REQUIRED)

add_library(nebula_actuator_control
    ControlLoop/ControlLoop.cpp
    PeriodicClock/PeriodicClock.cpp
    SimDrive/SimDrive.cpp
)
target_compile_features(nebula_actuator_control PUBLIC cxx_std_17)
target_include_directories(nebula_actuator_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nebula_actuator_control PUBLIC Threads::Threads)

add_executable(nebula_control_probe ControlProbe/ControlProbe.cpp)
target_link_libraries(nebula_control_probe PRIVATE nebula_actuator_control)

if(BUILD_TESTING)
    add_executable(nebula_actuator_contract tests/ActuatorContract/ActuatorContract.cpp)
    target_link_libraries(nebula_actuator_contract PRIVATE nebula_actuator_control)
    add_test(NAME nebula_actuator_contract COMMAND nebula_actuator_contract)
    set_tests_properties(nebula_actuator_contract PROPERTIES TIMEOUT 20 LABELS actuator-sim)
endif()
