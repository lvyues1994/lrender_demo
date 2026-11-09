#include "FramePump.h"
#include <limits>
#include <stdexcept>
namespace demo {
FramePump::FramePump(unsigned capacity):capacity_(capacity) {
 if(capacity!=1 && capacity!=2) throw std::invalid_argument("FramePump capacity must be 1 or 2");
}
void FramePump::markDirty() {if(!stopped_) dirty_=true;}
void FramePump::clearDirty() {dirty_=false;}
void FramePump::setContinuous(bool enabled) {if(!stopped_) continuous_=enabled;}
void FramePump::setPaused(bool paused) {if(!stopped_) paused_=paused;}
void FramePump::stop() {stopped_=true;dirty_=false;continuous_=false;}
std::optional<FrameStamp> FramePump::reserve(lr::core::ViewId view,SessionGeneration generation) {
 if(stopped_ || paused_ || count_==capacity_ || (!dirty_ && !continuous_)) return std::nullopt;
 if(!view.isValid() || generation==0) throw std::invalid_argument("FramePump requires valid view and nonzero generation");
 if(sequence_==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("FramePump sequence exhausted");
 for(auto& ticket:tickets_) if(!ticket) {
  FrameStamp stamp{view,generation,sequence_+1};
  ticket=stamp;sequence_=stamp.sequence;++count_;dirty_=false;
  return stamp;
 }
 throw std::logic_error("FramePump capacity invariant violated");
}
void FramePump::complete(FrameStamp stamp) {
 for(auto& ticket:tickets_) {
  if(ticket && ticket->sequence==stamp.sequence && ticket->view==stamp.view && ticket->generation==stamp.generation) {
   ticket.reset();--count_;return;
  }
 }
}
unsigned FramePump::inFlight()const {return count_;}
}
