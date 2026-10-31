#pragma once
namespace webserver::bootstrap {
/** Process entry coordinator. Domain runtimes own all business resources. */
class ApplicationRuntime final { public: int run(); };
}
