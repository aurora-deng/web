#include "tests/function3/unit/WorkspaceContract.h"
#include "backend/application/storage/InMemoryStore/InMemoryStore.h"
#include <iostream>
int main() {
    try { webserver::phase11::InMemoryStore store; webserver::chat::test::runWorkspaceContract(store); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    std::cout<<"workspace memory contract passed\n";
}
