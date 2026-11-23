add_subdirectory(domain)

add_library(nebula_fleet_application
    application/FleetCoordinator/FleetCoordinator.cpp
    application/TaskWorkflow/TaskWorkflow.cpp
    application/EdgeWorker/EdgeWorker.cpp)
target_link_libraries(nebula_fleet_application PUBLIC nebula_fleet_domain nebula_workflow)

add_library(nebula_fleet_ledger
    infrastructure/SqliteDatabase/SqliteDatabase.cpp
    infrastructure/SqliteFleetLedger/SqliteFleetLedger.cpp
    infrastructure/SqliteFleetLedger/SimulationReviewSql.cpp
    infrastructure/SqliteEdgeLedger/SqliteEdgeLedger.cpp)
target_link_libraries(nebula_fleet_ledger PUBLIC nebula_fleet_domain Threads::Threads)
if(NOT WIN32)
    find_package(SQLite3 REQUIRED)
    target_link_libraries(nebula_fleet_ledger PRIVATE SQLite::SQLite3)
endif()

add_library(nebula_fleet_simulator infrastructure/SimNavigation/SimNavigation.cpp)
target_link_libraries(nebula_fleet_simulator PUBLIC nebula_fleet_domain Threads::Threads)

if(NEBULA_FLEET_GRPC)
    add_subdirectory(transport/grpc)
endif()
