#pragma once
#include "presentation/ReadbackAdapter.h"
#include <deque>
#include <stdexcept>
namespace demo {
// Only asynchronous platform boundary is fake. ALL production readback lifecycle,
// admission, callback fencing and errors are inherited, not reimplemented here.
class FakePresentation final : public ReadbackAdapter {
 struct Pending { std::deque<Done> callbacks; std::vector<std::string> stages; };
 std::shared_ptr<Pending> pending_;
 explicit FakePresentation(std::shared_ptr<Pending> p)
  : ReadbackAdapter({},[p](std::string stage,Done done){p->stages.push_back(std::move(stage));p->callbacks.push_back(std::move(done));}),pending_(std::move(p)) {}
public:
 FakePresentation():FakePresentation(std::make_shared<Pending>()){}
 void complete(Result<std::monostate> result=std::monostate{}) {
  if(pending_->callbacks.empty()) throw std::logic_error("no boundary pending");
  auto done=std::move(pending_->callbacks.front());pending_->callbacks.pop_front();done(std::move(result));
 }
 auto const& stages() const { return pending_->stages; }
};
}
