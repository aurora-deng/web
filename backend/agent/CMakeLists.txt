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
        program/FleetAgentCli/FleetApprovalCommand.cpp)
    target_link_libraries(nebula_fleet_agent PRIVATE nebula_agent_grpc nebula_workflow_observer)
endif()
