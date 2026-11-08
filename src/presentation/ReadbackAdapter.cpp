#include "ReadbackAdapter.h"
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <limits>
#include <stdexcept>
namespace demo {
namespace {
void gui() {
 if (!QCoreApplication::instance() || QThread::currentThread()!=QCoreApplication::instance()->thread())
  throw std::logic_error("Presentation controls require GUI owner");
}
template<class F> void post(F f) { QTimer::singleShot(0,QCoreApplication::instance(),std::move(f)); }
RenderError error(std::string message) { return {ErrorStage::Surface,true,std::move(message),"readback presentation lifecycle"}; }
bool storageValid(lr::render::ImageRgba8 const& image) {
 return image.extent.x>0 && image.extent.y>0 && image.extent.x<=std::numeric_limits<int>::max()/4 &&
  image.pixels.size()==std::size_t(image.extent.x)*std::size_t(image.extent.y);
}
}
struct ReadbackAdapter::Core {
 enum Phase { Cold,Preparing,Prepared,Ready,Draining,Drained,Releasing,Released } phase=Cold;
 PresentationEvents events; PresentationBoundary boundary;
 std::uint64_t operation=0; SessionGeneration generation=0; ViewportExtent viewport;
 Done preparing; std::vector<Done> drains,releases;
 bool preparePending=false,drainStarted=false;
 void startDrain(std::shared_ptr<Core>);
 void report(RenderError const& e) { if(events.onError) events.onError(e); }
};
// Boundary may complete inline or from a worker; public completions are ALWAYS
// queued to GUI and fenced once. Core survives adapter/UI destruction until Done.
template<class C,class F> void boundary(std::shared_ptr<C> c,std::string name,F finish) {
 auto once=std::make_shared<std::atomic<bool>>(false);
 Done done=[c,once,finish=std::move(finish)](auto result) mutable {
  if(once->exchange(true)) return;
  post([c,finish=std::move(finish),result=std::move(result)]() mutable {finish(*c,std::move(result));});
 };
 try { if(c->boundary) c->boundary(std::move(name),done); else done(std::monostate{}); }
 catch(std::exception const& e) {done(error(e.what()));}
}
void ReadbackAdapter::Core::startDrain(std::shared_ptr<Core> c) {
 if(drainStarted) return;drainStarted=true;
 demo::boundary(std::move(c),"drain",[](Core& c,auto result) {
  c.drainStarted=false;
  c.phase=std::holds_alternative<RenderError>(result)?Core::Cold:Core::Drained;
  auto callbacks=std::move(c.drains);c.drains.clear();
  if(auto e=std::get_if<RenderError>(&result)) c.report(*e);
  for(auto& cb:callbacks) cb(result);
 });
}
ReadbackAdapter::ReadbackAdapter(PresentationEvents events,PresentationBoundary driver):core_(std::make_shared<Core>()) {
 gui(); core_->events=std::move(events);core_->boundary=std::move(driver);
}
ReadbackAdapter::~ReadbackAdapter()=default; // no device resource or wait
void ReadbackAdapter::preparePath(PresentationPath path,std::shared_ptr<NativeHostLease>,Done cb) {
 gui();auto c=core_;
 if(c->phase!=Core::Cold && c->phase!=Core::Released) {post([cb=std::move(cb)]{cb(error("prepare overlaps an existing path; drain/release first"));});return;}
 if(path!=PresentationPath::Readback) {
  post([c,cb=std::move(cb)] {auto e=error("Unsupported: VulkanShared requires the P9 native adapter");c->report(e);cb(e);});return;
 }
 c->phase=Core::Preparing;c->preparePending=true;auto operation=++c->operation;c->preparing=std::move(cb);
 boundary(c,"prepare",[operation,weak=std::weak_ptr<Core>(c)](Core& c,auto result) {
  c.preparePending=false;
  if(c.operation!=operation || c.phase!=Core::Preparing) {
   if(auto e=std::get_if<RenderError>(&result)) c.report(*e);
   if(c.phase==Core::Draining) c.startDrain(weak.lock());
   return;
  }
  c.phase=std::holds_alternative<RenderError>(result)?Core::Cold:Core::Prepared;
  auto done=std::move(c.preparing);
  if(auto e=std::get_if<RenderError>(&result)) c.report(*e);
  done(std::move(result));
 });
}
void ReadbackAdapter::probe(tc::DeviceInfo const& info,std::function<void(Result<ProbeReport>)> cb) {
 gui();auto c=core_;auto operation=c->operation;auto backend=std::string(info.backend);auto device=info.deviceName;
 if(c->phase!=Core::Prepared) {post([cb=std::move(cb)]{cb(error("probe requires prepared candidate"));});return;}
 post([c,operation,backend=std::move(backend),device=std::move(device),cb=std::move(cb)] {
  if(c->operation!=operation || c->phase!=Core::Prepared) {cb(error("probe cancelled by drain"));return;}
  c->phase=Core::Ready;cb(ProbeReport{false,"Owned readback; renderer backend="+backend,device});
 });
}
TargetDesc ReadbackAdapter::describeTarget(ViewportExtent const& extent)const {
 gui();if(core_->phase!=Core::Prepared && core_->phase!=Core::Ready) throw std::logic_error("describe requires prepared path");
 if(extent.physical.x<=0 || extent.physical.y<=0) throw std::invalid_argument("positive actual target extent required");
 return lr::render::ImageTargetDesc{extent.physical};
}
void ReadbackAdapter::setFrameContext(SessionGeneration generation,ViewportExtent const& extent) {
 gui();if(core_->phase!=Core::Ready) throw std::logic_error("frame acceptance requires probed path");
 if(generation==0 || extent.physical.x<0 || extent.physical.y<0 || extent.logical.x<0 || extent.logical.y<0)
  throw std::invalid_argument("invalid frame context");
 core_->generation=generation;core_->viewport=extent;
}
void ReadbackAdapter::consumeImage(OwnedFrame frame) {
 gui();auto c=core_;auto const& e=c->viewport;
 if(c->phase!=Core::Ready || !c->generation || frame.stamp.generation!=c->generation ||
    e.logical.x<=0 || e.logical.y<=0 || e.physical.x<=0 || e.physical.y<=0) return;
 if(!frame.image) {c->report(error("null owned image"));return;}
 if(frame.image->extent.x!=e.physical.x || frame.image->extent.y!=e.physical.y) return;
 if(!storageValid(*frame.image)) {c->report(error("malformed owned image storage"));return;}
 if(c->events.onImage) c->events.onImage(std::move(frame));
}
void ReadbackAdapter::beginDrain(Done cb) {
 gui();auto c=core_;
 if(c->phase==Core::Drained || c->phase==Core::Released) {post([cb=std::move(cb)]{cb(std::monostate{});});return;}
 if(c->phase==Core::Releasing) {post([cb=std::move(cb)]{cb(error("release already in progress"));});return;}
 c->drains.push_back(std::move(cb));if(c->phase==Core::Draining) return;
 c->phase=Core::Draining;++c->operation;c->generation=0;
 if(c->preparing) {auto done=std::move(c->preparing);post([done=std::move(done)]{done(error("prepare cancelled by drain"));});}
 if(!c->preparePending) c->startDrain(c);
}
void ReadbackAdapter::releasePath(Done cb) {
 gui();auto c=core_;
 if(c->phase==Core::Released) {post([cb=std::move(cb)]{cb(std::monostate{});});return;}
 if(c->phase!=Core::Drained && c->phase!=Core::Releasing) {post([cb=std::move(cb)]{cb(error("release requires successful drain and Session target destruction"));});return;}
 c->releases.push_back(std::move(cb));if(c->phase==Core::Releasing) return;c->phase=Core::Releasing;
 boundary(c,"release",[](Core& c,auto result) {
  c.phase=std::holds_alternative<RenderError>(result)?Core::Drained:Core::Released;
  auto callbacks=std::move(c.releases);c.releases.clear();
  if(auto e=std::get_if<RenderError>(&result)) c.report(*e);
  for(auto& cb:callbacks) cb(result);
 });
}
QImage ReadbackAdapter::copyImage(lr::render::ImageRgba8 const& image) {
 gui();if(!storageValid(image)) throw std::invalid_argument("invalid RGBA8 image");
 QImage result(image.extent.x,image.extent.y,QImage::Format_RGBA8888);
 if(result.isNull()) throw std::runtime_error("QImage allocation failed");
 // Portable explicit unpack: ImageRgba8 low-byte red, rows top first; no endian
 // assumption or borrowed pixels survive. Qt owns this newly allocated storage.
 for(int y=0;y<image.extent.y;++y) {
  auto row=result.scanLine(y);
  for(int x=0;x<image.extent.x;++x) {
   auto pixel=image.pixels[std::size_t(y)*image.extent.x+x];
   for(int channel=0;channel!=4;++channel) row[4*x+channel]=static_cast<unsigned char>(pixel>>(8*channel));
  }
 }
 return result;
}
void PresentationTransition::fallback(RenderError reason) {
 if(closing_ || action_!=TransitionAction::Idle) return;
 reason_=std::move(reason);action_=TransitionAction::AwaitOldFrames;
}
void PresentationTransition::close() {
 closing_=true;
 if(action_==TransitionAction::Idle) action_=TransitionAction::AwaitOldFrames;
 if(action_==TransitionAction::PublishNew) action_=TransitionAction::DrainOld;
}
void PresentationTransition::completed(TransitionAction action) {
 if(action!=action_) throw std::logic_error("out-of-order presentation handoff");
 switch(action_) {
 case TransitionAction::AwaitOldFrames:action_=TransitionAction::DrainOld;break;
 case TransitionAction::DrainOld:action_=TransitionAction::DestroyOldTarget;break;
 case TransitionAction::DestroyOldTarget:action_=TransitionAction::ReleaseOld;break;
 case TransitionAction::ReleaseOld:action_=closing_?TransitionAction::Closed:TransitionAction::PrepareNew;break;
 case TransitionAction::PrepareNew:action_=closing_?TransitionAction::DrainOld:TransitionAction::PublishNew;break;
 case TransitionAction::PublishNew:action_=TransitionAction::Idle;break;
 default:throw std::logic_error("no presentation operation to complete");
 }
}
}
