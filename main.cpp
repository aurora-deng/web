#include "backend/application/bootstrap/ApplicationRuntime/ApplicationRuntime.h"
#include <exception>
#include <iostream>

/** Process boundary only: assemble via ApplicationRuntime and report failure. */
int main(){
    try {
        webserver::bootstrap::ApplicationRuntime application;
        return application.run();
    } catch(const std::exception& error) {
        std::cerr<<"webserver failed to start: "<<error.what()<<'\n';
        return 1;
    }
}
