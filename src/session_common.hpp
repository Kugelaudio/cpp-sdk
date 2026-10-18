#pragma once

// Diagnostics bookkeeping shared by the two TTS sessions: one operation per
// turn (keyed per context for multi-context), settled by the turn's
// final/done frame, an error, or a cancellation.

#include <chrono>
#include <exception>
#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "diagnostics.hpp"
#include "error_mapping.hpp"

namespace kugelaudio::detail {

class TurnTracker {
 public:
  TurnTracker(std::shared_ptr<diag::Reporter> reporter, const char* operation)
      : reporter_(std::move(reporter)), operation_(operation) {}

  /// The live turn for `key`, minting one when the last turn settled.
  diag::Operation& turn(const std::string& key = {}) {
    auto it = ops_.find(key);
    if (it == ops_.end() || it->second.settled()) {
      ops_.erase(key);
      it =
          ops_.emplace(key, reporter_->operation(std::string(operation_), diag::kTransportWebsocket,
                                                 std::string(diag::kStageSendingRequest)))
              .first;
    }
    return it->second;
  }

  /// The live turn for `key`, or nullptr.
  diag::Operation* live(const std::string& key = {}) {
    auto it = ops_.find(key);
    return it != ops_.end() && !it->second.settled() ? &it->second : nullptr;
  }

  /// The server's final/done frame for `key`: the turn succeeded.
  void finish(const std::string& key = {}) {
    if (auto* op = live(key)) op->succeed();
    ops_.erase(key);
  }

  /// Caller-initiated cancellation of `key`'s turn only.
  void cancel(const std::string& key = {}) {
    if (auto* op = live(key)) op->cancel();
    ops_.erase(key);
  }

  void cancel_all() {
    for (auto& [key, op] : ops_) op.cancel();
    ops_.clear();
  }

  /// Fail the live turn of `key` with `error` (when there is one).
  void fail(const std::exception_ptr& error, const std::string& key = {}) {
    if (auto* op = live(key)) op->fail(error);
  }

  /// Throw the typed error of a server error frame, marking the live turn.
  [[noreturn]] void raise_frame_error(const nlohmann::json& frame, const std::string& key = {}) {
    auto error = classify_ws_frame(frame);
    if (auto* op = live(key)) op->mark_server_error(error);
    std::rethrow_exception(error);
  }

  /// A connect operation of its own (establishing the socket).
  diag::Operation connect_operation() {
    return reporter_->operation(std::string(operation_), diag::kTransportWebsocket,
                                std::string(diag::kStageHandshake));
  }

 private:
  std::shared_ptr<diag::Reporter> reporter_;
  const char* operation_;
  std::map<std::string, diag::Operation> ops_;
};

/// Seconds to wait before reconnecting after a rolling-deploy close: the
/// error's retry_after hint, else one second.
inline std::chrono::milliseconds restart_delay(const std::exception_ptr& error) {
  const ErrorSummary summary = summarize(error);
  if (summary.details.retry_after) {
    return std::chrono::milliseconds(std::max(0, *summary.details.retry_after) * 1000);
  }
  return std::chrono::milliseconds(1000);
}

}  // namespace kugelaudio::detail
