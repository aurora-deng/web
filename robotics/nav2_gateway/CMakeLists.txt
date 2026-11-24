cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nebula_nav2_gateway LANGUAGES CXX)
    include(CTest)
    add_subdirectory(../../backend/fleet/domain fleet_domain)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(lifecycle_msgs REQUIRED)
find_package(nav2_msgs REQUIRED)
find_package(rosgraph_msgs REQUIRED)
find_package(std_msgs REQUIRED)
find_package(tf2_ros REQUIRED)

add_library(nebula_nav2_navigation BodyLifecycle/BodyLifecycle.cpp Nav2Navigation/Nav2Navigation.cpp)
target_link_libraries(nebula_nav2_navigation PUBLIC nebula_fleet_domain)
ament_target_dependencies(nebula_nav2_navigation PUBLIC rclcpp rclcpp_action rclcpp_lifecycle
    lifecycle_msgs nav2_msgs rosgraph_msgs std_msgs tf2_ros)

if(BUILD_TESTING)
    add_executable(nebula_nav2_contract tests/Nav2Contract/Nav2Contract.cpp)
    target_link_libraries(nebula_nav2_contract PRIVATE nebula_nav2_navigation)
    add_test(NAME nebula_nav2_contract COMMAND nebula_nav2_contract)
    set_tests_properties(nebula_nav2_contract PROPERTIES TIMEOUT 35 LABELS ros2-sim
        ENVIRONMENT "ROS_LOCALHOST_ONLY=1;ROS_DOMAIN_ID=87")
endif()
