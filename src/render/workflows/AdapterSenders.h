#pragma once

#include "presentation/PresentationAdapter.h"

#include <lexec/execution.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace demo::workflows {

// Done callbacks become set_value(Result). Unexpected throws become RenderError values.
// No set_error channel so lexec::spawn can attach without upon_error plumbing.
struct DoneOpSender {
  using sender_concept = lexec::sender_t;
  using completion_signatures =
      lexec::completion_signatures<lexec::set_value_t(Result<std::monostate>), lexec::set_stopped_t()>;

  std::function<void(Done)> launch;

  template <class Rcvr>
  struct operation {
    using operation_state_concept = lexec::operation_state_t;
    Rcvr rcvr;
    std::function<void(Done)> launch;
    std::shared_ptr<std::atomic_bool> armed = std::make_shared<std::atomic_bool>(true);

    void start() & noexcept {
      try {
        auto gate = armed;
        launch([this, gate](Result<std::monostate> result) mutable {
          if (!gate->exchange(false))
            return;
          try {
            auto token = lexec::get_stop_token(lexec::get_env(rcvr));
            if (token.stop_requested()) {
              lexec::set_stopped(static_cast<Rcvr &&>(rcvr));
              return;
            }
            lexec::set_value(static_cast<Rcvr &&>(rcvr), std::move(result));
          } catch (...) {
            try {
              lexec::set_value(static_cast<Rcvr &&>(rcvr),
                               Result<std::monostate>{RenderError{ErrorStage::Shutdown, false, "done hop failed",
                                                                  "exception while completing DoneOpSender"}});
            } catch (...) {
            }
          }
        });
      } catch (...) {
        try {
          lexec::set_value(
              static_cast<Rcvr &&>(rcvr),
              Result<std::monostate>{
                  RenderError{ErrorStage::Shutdown, false, "done launch failed", "exception starting DoneOpSender"}});
        } catch (...) {
        }
      }
    }
  };

  template <class Rcvr>
  operation<std::decay_t<Rcvr>> connect(Rcvr &&rcvr) && {
    return {static_cast<Rcvr &&>(rcvr), std::move(launch)};
  }
};

inline DoneOpSender beginDrainSender(PresentationAdapter &adapter) {
  return DoneOpSender{[&adapter](Done done) { adapter.beginDrain(std::move(done)); }};
}

inline DoneOpSender releasePathSender(PresentationAdapter &adapter) {
  return DoneOpSender{[&adapter](Done done) { adapter.releasePath(std::move(done)); }};
}

} // namespace demo::workflows
