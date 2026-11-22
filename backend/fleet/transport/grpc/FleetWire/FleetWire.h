#pragma once

#include "../../../domain/FleetTypes/FleetTypes.h"
#include "fleet.grpc.pb.h"

#include <grpcpp/channel.h>

namespace nebula::fleet::wire {

inline constexpr unsigned protocolVersion = 1;
inline constexpr int messageLimit = 2048;
inline constexpr auto tokenHeader = "x-nebula-fleet-token";
inline constexpr auto robotHeader = "x-nebula-robot";

Command decode(const v1::Command& value);
Receipt decode(const v1::Receipt& value);
void encode(const Command& command, v1::Command& value);
void encode(const Receipt& receipt, v1::Receipt& value);
void encode(const Task& task, v1::TaskStatus& value);

std::shared_ptr<grpc::Channel> localChannel(std::string_view address);
unsigned localPort(std::string_view address, bool allowEphemeral = false);
std::string environmentToken(const char* variable);
bool tokenMatches(std::string_view candidate, std::string_view allowed);
}
