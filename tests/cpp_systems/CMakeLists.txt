add_executable(nebula_workflow_contract WorkflowContract/WorkflowContract.cpp)
target_link_libraries(nebula_workflow_contract PRIVATE nebula_workflow_observer)
add_test(NAME nebula_workflow_contract COMMAND nebula_workflow_contract)
set_tests_properties(nebula_workflow_contract PROPERTIES TIMEOUT 30 LABELS cpp-systems)

if(TARGET nebula_workflow_sqlite)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(nebula_workflow_recovery WorkflowRecovery/WorkflowRecovery.cpp)
    target_link_libraries(nebula_workflow_recovery PRIVATE nebula_workflow_sqlite)
    add_test(NAME nebula_workflow_recovery
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/workflow_recovery.py"
            $<TARGET_FILE:nebula_workflow_recovery>)
    set_tests_properties(nebula_workflow_recovery PROPERTIES TIMEOUT 35 LABELS cpp-systems)
endif()

if(TARGET nebula_fleet_application)
    add_executable(nebula_fleet_contract FleetContract/FleetContract.cpp)
    target_link_libraries(nebula_fleet_contract PRIVATE
        nebula_fleet_application nebula_fleet_ledger nebula_fleet_simulator)
    add_test(NAME nebula_fleet_contract COMMAND nebula_fleet_contract)
    set_tests_properties(nebula_fleet_contract PROPERTIES TIMEOUT 30 LABELS cpp-systems)
endif()

if(TARGET nebula_agent_application)
    add_executable(nebula_agent_contract AgentContract/AgentContract.cpp)
    target_link_libraries(nebula_agent_contract PRIVATE nebula_agent_application nebula_workflow_observer)
    add_test(NAME nebula_agent_contract COMMAND nebula_agent_contract)
    set_tests_properties(nebula_agent_contract PROPERTIES TIMEOUT 15 LABELS cpp-systems)
endif()

if(TARGET nebula_fleet_coordinator)
    add_executable(nebula_fleet_test_peer FleetTestPeer/FleetTestPeer.cpp)
    target_link_libraries(nebula_fleet_test_peer PRIVATE nebula_fleet_rpc nebula_fleet_ledger)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME nebula_fleet_process_contract
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
            --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
            --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer>
            --agent $<TARGET_FILE:nebula_fleet_agent>)
    set_tests_properties(nebula_fleet_process_contract PROPERTIES TIMEOUT 120 LABELS cpp-systems)
    add_test(NAME nebula_agent_approval_handoff
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
            --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
            --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer>
            --agent $<TARGET_FILE:nebula_fleet_agent> --agent-approval)
    set_tests_properties(nebula_agent_approval_handoff PROPERTIES TIMEOUT 45 LABELS cpp-systems)
    add_test(NAME nebula_agent_model_adapter
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
            --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
            --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer>
            --agent $<TARGET_FILE:nebula_fleet_agent> --agent-model)
    set_tests_properties(nebula_agent_model_adapter PROPERTIES TIMEOUT 35 LABELS cpp-systems)
    if(TARGET nebula_agent_ollama)
        add_test(NAME nebula_agent_ollama_native
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
                --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
                --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer>
                --agent $<TARGET_FILE:nebula_fleet_agent> --native-ollama)
        set_tests_properties(nebula_agent_ollama_native PROPERTIES TIMEOUT 35 LABELS cpp-systems)
    endif()
    add_test(NAME nebula_fleet_backpressure
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
            --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
            --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer> --backpressure)
    set_tests_properties(nebula_fleet_backpressure PROPERTIES TIMEOUT 75 LABELS cpp-systems)
    add_test(NAME nebula_simulation_models_and_replay
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fleet_process.py"
            --coordinator $<TARGET_FILE:nebula_fleet_coordinator> --edge $<TARGET_FILE:nebula_fleet_edge>
            --ctl $<TARGET_FILE:nebula_fleet_ctl> --peer $<TARGET_FILE:nebula_fleet_test_peer> --simulation-models)
    set_tests_properties(nebula_simulation_models_and_replay PROPERTIES TIMEOUT 45 LABELS cpp-systems)
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
if(TARGET nebula_workflow_worker AND NEBULA_WORKFLOW_POSTGRES_PREFIX)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME nebula_workflow_pg_leases
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/workflow_pg.py"
            --worker $<TARGET_FILE:nebula_workflow_worker> --postgres "${NEBULA_WORKFLOW_POSTGRES_PREFIX}"
            --report "${CMAKE_BINARY_DIR}/workflow-pg.json")
    set_tests_properties(nebula_workflow_pg_leases PROPERTIES TIMEOUT 65 LABELS cpp-systems)
endif()
