cmake_minimum_required(VERSION 3.16)
project(nebula_actuator_ros_sim LANGUAGES CXX)

include(CTest)
add_subdirectory(../actuator_control actuator_control)
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(std_srvs REQUIRED)

add_library(nebula_actuator_runtime ActuatorRuntime/ActuatorRuntime.cpp)
target_link_libraries(nebula_actuator_runtime PUBLIC nebula_actuator_control)
target_include_directories(nebula_actuator_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(nebula_actuator_ros_sim ActuatorNode/ActuatorNode.cpp)
target_link_libraries(nebula_actuator_ros_sim PRIVATE nebula_actuator_runtime)
ament_target_dependencies(nebula_actuator_ros_sim PUBLIC rclcpp geometry_msgs std_msgs std_srvs)
