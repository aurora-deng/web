add_library(nebula_agent_domain
    domain/SkillCatalog/SkillCatalog.cpp
    domain/FleetProposal/FleetProposal.cpp)
target_link_libraries(nebula_agent_domain PUBLIC nebula_fleet_domain)

add_library(nebula_agent_application application/FleetAgent/FleetAgent.cpp)
target_link_libraries(nebula_agent_application PUBLIC nebula_agent_domain nebula_workflow)

if(TARGET nebula_fleet_rpc)
    add_library(nebula_agent_grpc
        infrastructure/FleetStatusGrpc/FleetStatusGrpc.cpp
        infrastructure/FleetReviewGrpc/FleetReviewGrpc.cpp)
    target_link_libraries(nebula_agent_grpc PUBLIC nebula_agent_application nebula_fleet_rpc)

    add_executable(nebula_fleet_agent
        program/FleetAgentCli/FleetAgentCli.cpp
        program/FleetAgentCli/FleetApprovalCommand.cpp
        infrastructure/SqliteAgentSession/SqliteAgentSession.cpp)
    target_link_libraries(nebula_fleet_agent PRIVATE nebula_agent_grpc nebula_workflow_observer nebula_workflow_sqlite)

    if(NEBULA_AGENT_OLLAMA)
        find_package(CURL 8.0 REQUIRED)
        set(NEBULA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
        add_library(nebula_agent_ollama
            infrastructure/OllamaProposalProvider/OllamaProposalProvider.cpp
            ${NEBULA_ROOT}/backend/ai/models/OllamaModelProvider/OllamaModelProvider.cpp
            ${NEBULA_ROOT}/backend/ai/models/OllamaStreamDecoder/OllamaStreamDecoder.cpp
            ${NEBULA_ROOT}/backend/application/transport/FlatJson/FlatJson.cpp)
        target_link_libraries(nebula_agent_ollama PUBLIC nebula_agent_application CURL::libcurl)
        target_link_libraries(nebula_fleet_agent PRIVATE nebula_agent_ollama)
        target_compile_definitions(nebula_fleet_agent PRIVATE NEBULA_AGENT_OLLAMA=1)
    endif()
endif()
