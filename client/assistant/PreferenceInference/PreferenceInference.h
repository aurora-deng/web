#pragma once

#include "client/companion/UpstreamTransport/UpstreamTransport.h"
#include <string_view>

namespace nebula::client {

// Auxiliary inference does not create local chat messages or take document context.
ProxyResponse handlePreferenceInference(const ProxyRequest& request, std::string_view route, std::string_view query,
    const BoundServerSession& identity, UpstreamTransport& upstream);

}
