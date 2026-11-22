add_executable(nebula_workflow_contract WorkflowContract/WorkflowContract.cpp)
target_link_libraries(nebula_workflow_contract PRIVATE nebula_workflow_observer)
add_test(NAME nebula_workflow_contract COMMAND nebula_workflow_contract)
set_tests_properties(nebula_workflow_contract PROPERTIES TIMEOUT 30 LABELS cpp-systems)

if(TARGET nebula_fleet_application)
    add_executable(nebula_fleet_contract FleetContract/FleetContract.cpp)
    target_link_libraries(nebula_fleet_contract PRIVATE
        nebula_fleet_application nebula_fleet_ledger nebula_fleet_simulator)
    add_test(NAME nebula_fleet_contract COMMAND nebula_fleet_contract)
    set_tests_properties(nebula_fleet_contract PROPERTIES TIMEOUT 30 LABELS cpp-systems)
endif()

if(TARGET nebula_fleet_coordinator)
    add_executable(nebula_fleet_test_peer FleetTestPeer/FleetTestPeer.cpp)
    target_link_libraries(nebula_fleet_test_peer PRIVATE nebula_fleet_rpc nebula_fleet_ledger)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME nebula_fleet_process_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
            --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
            --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer>)
    set_tests_properties(nebula_fleet_process_contract PROPERTIES TIMEOUT 120 LABELS cpp-systems)
    if(TARGET nebula_nav2_contract)
        add_test(NAME nebula_nav2_fleet_process
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
                --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
                --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer>
                --nav2-fixture $<TARGET_FILE:nebula_nav2_contract>)
        set_tests_properties(nebula_nav2_fleet_process PROPERTIES TIMEOUT 60 LABELS ros2-sim
            ENVIRONMENT "ROS_LOCALHOST_ONLY=1;ROS_DOMAIN_ID=88")
    endif()
endif()
