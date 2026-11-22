#include "FleetWire.h"

#include <charconv>
#include <cstdlib>
#include <grpcpp/grpcpp.h>
#include <utility>

namespace nebula::fleet::wire {

Command decode(const v1::Command& value) {
    Command command{value.task_id(), value.trace_id(), static_cast<Station>(value.station()), value.epoch()};
    checkCommand(command);
    return command;
}

Receipt decode(const v1::Receipt& value) {
    Receipt receipt{value.task_id(), value.epoch(), static_cast<Phase>(value.phase()), value.outcome()};
    checkReceipt(receipt);
    return receipt;
}

void encode(const Command& command, v1::Command& value) {
    value.set_task_id(command.taskId);
    value.set_trace_id(command.traceId);
    value.set_station(static_cast<v1::Station>(command.station));
    value.set_epoch(command.epoch);
}

void encode(const Receipt& receipt, v1::Receipt& value) {
    value.set_task_id(receipt.taskId);
    value.set_epoch(receipt.epoch);
    value.set_phase(static_cast<v1::Phase>(receipt.phase));
    value.set_outcome(receipt.outcome);
}

void encode(const Task& task, v1::TaskStatus& value) {
    encode(task.command, *value.mutable_command());
    value.set_robot_id(task.robotId);
    value.set_phase(static_cast<v1::Phase>(task.phase));
    value.set_outcome(task.outcome);
    value.set_connected(task.connected);
    value.set_needs_reconciliation(task.needsReconciliation);
}

unsigned localPort(std::string_view address, bool allowEphemeral) {
    constexpr std::string_view loopback = "127.0.0.1:";
    if (!address.starts_with(loopback)) throw Error(ErrorCode::invalid, "fleet lab requires an IPv4 loopback endpoint");
    address.remove_prefix(loopback.size());
    unsigned port{};
    const auto end = address.data() + address.size();
    const auto result = std::from_chars(address.data(), end, port);
    if (result.ec != std::errc{} || result.ptr != end || port > 65535 || (!allowEphemeral && port == 0)) {
        throw Error(ErrorCode::invalid, "invalid fleet port");
    }
    return port;
}

std::shared_ptr<grpc::Channel> localChannel(std::string_view address) {
    localPort(address);
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(messageLimit);
    args.SetMaxSendMessageSize(messageLimit);
    return grpc::CreateCustomChannel(std::string(address), grpc::InsecureChannelCredentials(), args);
}

std::string environmentToken(const char* variable) {
    const auto* value = std::getenv(variable);
    std::string result = value ? value : "";
    if (result.size() < 24 || result.size() > 128) {
        throw Error(ErrorCode::invalid, "fleet token environment variable must contain 24-128 characters");
    }
    for (unsigned char ch : result) {
        if (ch <= 0x20 || ch >= 0x7f) throw Error(ErrorCode::invalid, "fleet token is not printable ASCII");
    }
    return result;
}

bool tokenMatches(std::string_view candidate, std::string_view allowed) {
    if (candidate.size() != allowed.size()) return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < allowed.size(); ++i) {
        diff |= static_cast<unsigned char>(candidate[i]) ^ static_cast<unsigned char>(allowed[i]);
    }
    return diff == 0;
}
}
