add_library(nebula_workflow
    WorkflowGraph/WorkflowGraph.cpp
    DagExecutor/DagExecutor.cpp
    DurableWorkflow/DurableWorkflow.cpp
    WorkerPool/WorkerPool.cpp)
target_compile_features(nebula_workflow PUBLIC cxx_std_20)
target_include_directories(nebula_workflow PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(nebula_workflow PUBLIC Threads::Threads)
if(TARGET webserver_sanitizers)
    target_link_libraries(nebula_workflow PUBLIC webserver_sanitizers)
endif()

add_library(nebula_workflow_observer ObserverEvents/ObserverEvents.cpp)
target_link_libraries(nebula_workflow_observer PUBLIC nebula_workflow)
