/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "FifoProducerPartition.h"
#include "absl/synchronization/mutex.h"
#include <atomic>
#include <memory>
#include <system_error>
#include "FifoContext.h"
#include "rocketmq/Message.h"
#include "rocketmq/RocketMQ.h"
#include "rocketmq/SendCallback.h"
#include "rocketmq/SendReceipt.h"
#include "spdlog/spdlog.h"

ROCKETMQ_NAMESPACE_BEGIN

void FifoProducerPartition::add(FifoContext&& context) {
  {
    absl::MutexLock lock(&messages_mtx_);
    messages_.emplace_back(std::move(context));
  }
  trySend();
}

void FifoProducerPartition::trySend() {
  bool expected = false;
  if (!inflight_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
  MessageConstPtr message;
  SendCallback callback;
  {
    absl::MutexLock lock(&messages_mtx_);
    if (messages_.empty()) {
      inflight_.store(false, std::memory_order_release);
      return;
    }
    auto& context = messages_.front();
    message = std::move(context.message);
    callback = context.callback;
    messages_.pop_front();
  }
  // A synchronous failure may invoke onComplete on this very thread.
  // Release the queue mutex before entering the internal producer.
  const auto partition = shared_from_this();
  try {
    producer_->send(std::move(message), [partition, callback](const std::error_code& error,
                                                           SendReceipt&& receipt) mutable {
      partition->onComplete(error, std::move(receipt), callback);
    });
  } catch (const std::exception& error) {
    SPDLOG_ERROR("FIFO send failed before dispatch: {}", error.what());
    SendReceipt receipt;
    onComplete(std::make_error_code(std::errc::operation_canceled), std::move(receipt), callback);
  }
}

void FifoProducerPartition::onComplete(const std::error_code& error, SendReceipt&& receipt,
                                     SendCallback& callback) {
  // Function3 policy: internal Producer already performs its finite RPC attempts.
  // Report final failure exactly once; durable Outbox owns retry and dead letters.
  // Requeueing here forever hides failure and retains messages beyond its budget.
  try { callback(error, std::move(receipt)); }
  catch (...) { SPDLOG_ERROR("FIFO completion callback threw"); }
  inflight_.store(false, std::memory_order_release);
  trySend();
}

ROCKETMQ_NAMESPACE_END
