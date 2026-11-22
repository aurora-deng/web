cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nebula_nav2_gateway LANGUAGES CXX)
    include(CTest)
    add_subdirectory(../../backend/fleet/domain fleet_domain)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(nav2_msgs REQUIRED)

add_library(nebula_nav2_navigation Nav2Navigation/Nav2Navigation.cpp)
target_link_libraries(nebula_nav2_navigation PUBLIC nebula_fleet_domain)
ament_target_dependencies(nebula_nav2_navigation PUBLIC rclcpp rclcpp_action nav2_msgs)

if(BUILD_TESTING)
    find_package(rosgraph_msgs REQUIRED)
    add_executable(nebula_nav2_contract tests/Nav2Contract/Nav2Contract.cpp)
    target_link_libraries(nebula_nav2_contract PRIVATE nebula_nav2_navigation)
    ament_target_dependencies(nebula_nav2_contract PUBLIC rosgraph_msgs)
    add_test(NAME nebula_nav2_contract COMMAND nebula_nav2_contract)
    set_tests_properties(nebula_nav2_contract PROPERTIES TIMEOUT 35 LABELS ros2-sim
        ENVIRONMENT "ROS_LOCALHOST_ONLY=1;ROS_DOMAIN_ID=87")
endif()
