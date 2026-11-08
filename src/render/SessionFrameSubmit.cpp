#include "SessionFrameSubmit.h"
#include <QDebug>
namespace demo {
void submitSessionFrame(lr::render::Renderer &renderer, lr::render::FrameDesc frame, FrameStamp stamp,
                        lqtexec::ObjectScope &scope, lexec::counting_scope &root,
                        std::function<void(FrameCompletion)> done) {
  auto sender = renderer.submit(std::move(frame)); // may throw AFTER queueing; caller must fault-latch
  auto settled = std::move(sender) | lexec::then([stamp](lr::render::FrameResult result) {
                   return FrameCompletion{stamp, std::move(result)};
                 }) |
                 lexec::upon_error([stamp](std::exception_ptr failure) {
                   std::string message = "unknown renderer failure";
                   try {
                     if (failure)
                       std::rethrow_exception(failure);
                   } catch (std::exception const &e) {
                     message = e.what();
                   } catch (...) {
                   }
                   return FrameCompletion{
                       stamp, RenderError{ErrorStage::Submit, false, "frame failed", std::move(message)}};
                 }) |
                 lexec::upon_stopped([stamp] {
                   return FrameCompletion{stamp, RenderError{ErrorStage::Submit, false, "producer stopped",
                                                             "renderer completion stopped"}};
                 });
  auto hopped =
      lqtexec::then_on(std::move(settled), scope, [done](FrameCompletion c) { done(std::move(c)); });
  // spawn explicitly rejects an unhandled error channel. Preserve the same receipt
  // on hop/materialization error; done normalizes delivery back onto the GUI.
  lexec::spawn(std::move(hopped) |
                   lexec::upon_error([done = std::move(done), stamp](std::exception_ptr failure) noexcept {
                     try {
                       std::string cause = "completion dispatch failed";
                       try {
                         if (failure)
                           std::rethrow_exception(failure);
                       } catch (std::exception const &e) {
                         cause = e.what();
                       } catch (...) {
                       }
                       done({stamp, RenderError{ErrorStage::Submit, false, "completion dispatch failed",
                                                std::move(cause)}});
                     } catch (...) {
                       qFatal("cannot report frame completion failure");
                     }
                   }),
               root.get_token());
}
} // namespace demo
