#include "render/FramePump.h"
#include "render/SessionState.h"
#include <doctest/doctest.h>
#include <vector>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
using namespace demo;
namespace { constexpr lr::core::ViewId view{0}; }
TEST_CASE("frame_pump/cap-two") {
 FramePump pump{2};pump.setContinuous(true);
 auto a=pump.reserve(view,1),b=pump.reserve(view,1);REQUIRE(a);REQUIRE(b);
 CHECK_FALSE(pump.reserve(view,1));CHECK(pump.inFlight()==2);
 pump.complete(*a);CHECK(pump.inFlight()==1);CHECK(pump.reserve(view,2));CHECK(pump.inFlight()==2);
}
TEST_CASE("frame_pump/only-one-or-two-capacity") {
 CHECK_THROWS_AS(FramePump{0},std::invalid_argument);CHECK_THROWS_AS(FramePump{3},std::invalid_argument);
 CHECK_THROWS_AS(FramePump{std::numeric_limits<unsigned>::max()},std::invalid_argument);
 FramePump pump{1};pump.setContinuous(true);auto ticket=pump.reserve(view,1);REQUIRE(ticket);
 CHECK_FALSE(pump.reserve(view,1));pump.complete(*ticket);CHECK(pump.inFlight()==0);CHECK(pump.reserve(view,1));
}
TEST_CASE("frame_pump/dirty-is-one-coalesced-opportunity-not-a-loop") {
 FramePump pump{2};CHECK_FALSE(pump.reserve(view,1));
 for(int i=0;i!=1000;++i)pump.markDirty();
 auto a=pump.reserve(view,1);REQUIRE(a);CHECK_FALSE(pump.reserve(view,1));
 pump.markDirty();auto b=pump.reserve(view,1);REQUIRE(b);
 for(int i=0;i!=1000;++i)pump.markDirty();
 CHECK_FALSE(pump.reserve(view,1)); // full capacity must NOT consume dirty
 pump.complete(*a);auto latest=pump.reserve(view,1);REQUIRE(latest);
 CHECK(latest->sequence>b->sequence);CHECK_FALSE(pump.reserve(view,1));
 pump.complete(*latest);pump.complete(*b);CHECK_FALSE(pump.reserve(view,1));CHECK(pump.inFlight()==0);
}
TEST_CASE("frame_pump/completion-does-not-invent-dirty") {
 FramePump pump{1};pump.markDirty();auto a=pump.reserve(view,1);REQUIRE(a);
 pump.complete(*a);CHECK_FALSE(pump.reserve(view,1));
 pump.setContinuous(true);auto b=pump.reserve(view,1);REQUIRE(b);pump.complete(*b);CHECK(pump.reserve(view,1));
 pump.setContinuous(false);CHECK_FALSE(pump.reserve(view,1));
}
TEST_CASE("frame_pump/pause-preserves-dirty-and-continuous-intent") {
 FramePump pump{1};pump.markDirty();pump.setPaused(true);CHECK_FALSE(pump.reserve(view,1));
 pump.setPaused(false);auto a=pump.reserve(view,1);REQUIRE(a);pump.markDirty();pump.setPaused(true);
 pump.complete(*a);CHECK_FALSE(pump.reserve(view,1));pump.setPaused(false);auto b=pump.reserve(view,1);REQUIRE(b);
 pump.complete(*b);CHECK_FALSE(pump.reserve(view,1));pump.setContinuous(true);pump.setPaused(true);
 CHECK_FALSE(pump.reserve(view,1));pump.setPaused(false);CHECK(pump.reserve(view,1));
}
TEST_CASE("frame_pump/stop-never-cancels-outstanding-work") {
 FramePump pump{2};pump.setContinuous(true);auto a=pump.reserve(view,1),b=pump.reserve(view,1);REQUIRE(a);REQUIRE(b);
 pump.stop();pump.stop();pump.markDirty();pump.setPaused(false);pump.setContinuous(true);
 CHECK_FALSE(pump.reserve(view,2));CHECK(pump.inFlight()==2);
 pump.complete(*a);CHECK(pump.inFlight()==1);pump.complete(*b);CHECK(pump.inFlight()==0);
 CHECK_FALSE(pump.reserve(view,2));pump.complete(*a);CHECK(pump.inFlight()==0);
}
TEST_CASE("frame_pump/forged-unknown-and-duplicate-completions-cannot-free-slots") {
 FramePump pump{2};pump.setContinuous(true);auto a=pump.reserve(view,3),b=pump.reserve(lr::core::ViewId{1},4);REQUIRE(a);REQUIRE(b);
 auto wrong=*a;++wrong.generation;pump.complete(wrong);CHECK(pump.inFlight()==2);
 wrong=*a;wrong.view=lr::core::ViewId{7};pump.complete(wrong);CHECK(pump.inFlight()==2);
 wrong=*a;wrong.sequence=999;pump.complete(wrong);CHECK(pump.inFlight()==2);
 pump.complete({});CHECK(pump.inFlight()==2);pump.complete(*a);pump.complete(*a);CHECK(pump.inFlight()==1);
 auto c=pump.reserve(view,5);REQUIRE(c);CHECK(c->sequence>b->sequence);
 pump.complete(*a);CHECK(pump.inFlight()==2);pump.complete(*c);pump.complete(*b);CHECK(pump.inFlight()==0);
}
TEST_CASE("frame_pump/old-generation-settles-but-production-state-refuses-display") {
 SessionState state;state.setViewport({{2,2},{2,2},1});state.start();
 REQUIRE(state.presentationReady(state.snapshot()->generation,PresentationPath::Readback));
 FramePump pump{2};pump.setContinuous(true);auto old=pump.reserve(view,state.snapshot()->generation);REQUIRE(old);
 state.setViewport({{3,2},{3,2},1});REQUIRE(state.presentationReady(state.snapshot()->generation,PresentationPath::Readback));
 auto current=pump.reserve(view,state.snapshot()->generation);REQUIRE(current);
 CHECK_FALSE(state.acceptsFrame(*old));CHECK(state.acceptsFrame(*current));
 pump.complete(*old);CHECK(pump.inFlight()==1);pump.complete(*current);CHECK(pump.inFlight()==0);
}
TEST_CASE("frame_pump/preaccept-failure-keeps-extracted-delta-outside-pump") {
 SessionState state;state.setViewport({{2,2},{2,2},1});state.start();
 REQUIRE(state.presentationReady(state.snapshot()->generation,PresentationPath::Readback));
 FramePump pump{1};pump.markDirty();auto first=pump.reserve(view,state.snapshot()->generation);REQUIRE(first);
 // Caller contract example, NOT a mock World/extractor or real submit claim.
 lr::core::RenderWorldDelta pending;pending.environment=lr::core::EnvironmentDesc{lr::core::vec3{0.25f}};
 pump.complete(*first);pump.markDirty(); // known failure before accepted submit, pending still owned by caller
 CHECK(pending.environment.has_value());CHECK(state.historyResetPending());
 auto retry=pump.reserve(view,state.snapshot()->generation);REQUIRE(retry);CHECK(retry->sequence>first->sequence);
 CHECK(pending.environment->radiance.x==0.25f);pump.complete(*retry); // terminal completion, not auto-retry
 CHECK_FALSE(pump.reserve(view,state.snapshot()->generation));CHECK(state.historyResetPending());
}
TEST_CASE("frame_pump/ambiguous-acceptance-pauses-without-replaying-or-freeing-ticket") {
 FramePump pump{1};pump.markDirty();auto a=pump.reserve(view,1);REQUIRE(a);
 pump.setPaused(true);pump.markDirty();CHECK_FALSE(pump.reserve(view,1));CHECK(pump.inFlight()==1);
 pump.setPaused(false);CHECK_FALSE(pump.reserve(view,1)); // even resume cannot double-submit an unaccounted delta
 pump.complete(*a);auto b=pump.reserve(view,1);REQUIRE(b);CHECK(b->sequence>a->sequence);
}
TEST_CASE("frame_pump/inline-reentrant-completion-uses-prepublished-ticket") {
 FramePump pump{2};pump.markDirty();auto a=pump.reserve(view,1);REQUIRE(a);
 // Represents an injected owner's inline callback; pump itself invokes NO callbacks.
 auto inlineCompletion=[&]{pump.complete(*a);pump.markDirty();auto b=pump.reserve(view,2);REQUIRE(b);
  CHECK(b->sequence>a->sequence);pump.complete(*b);pump.stop();};
 inlineCompletion();pump.complete(*a); // late outer bookkeeping must not reinsert the old ticket
 CHECK(pump.inFlight()==0);CHECK_FALSE(pump.reserve(view,3));
}
TEST_CASE("frame_pump/invalid-admitted-context-is-atomic") {
 FramePump pump{1};pump.markDirty();CHECK_THROWS_AS(pump.reserve(lr::core::ViewId{},1),std::invalid_argument);
 CHECK_THROWS_AS(pump.reserve(view,0),std::invalid_argument);CHECK(pump.inFlight()==0);
 auto a=pump.reserve(view,1);REQUIRE(a);CHECK(a->sequence==1);
}
TEST_CASE("frame_pump/fixed-seed-operation-traces-match-independent-reference") {
 for(unsigned cap:{1u,2u})for(unsigned seed:{17u,907u,65537u}) {
  FramePump pump{cap};std::mt19937 random(seed);bool dirty=false,continuous=false,paused=false,stopped=false;
  std::uint64_t sequence=0,generation=1;std::map<std::uint64_t,FrameStamp> outstanding;std::vector<FrameStamp> history;
  unsigned reservations=0,duplicates=0,stale=0;
  for(unsigned step=0;step!=500;++step) {
   INFO("cap=",cap," seed=",seed," step=",step);
   auto op=random()%8;
   if(op==0){pump.markDirty();if(!stopped)dirty=true;}
   if(op==1){continuous=!continuous;pump.setContinuous(continuous);}
   if(op==2){paused=!paused;pump.setPaused(paused);}
   if(op==3)++generation;
   if(op==4 || op==5) {
    lr::core::ViewId id{static_cast<std::uint32_t>(random()%3)};auto actual=pump.reserve(id,generation);
    bool allowed=!stopped&&!paused&&(dirty||continuous)&&outstanding.size()<cap;
    REQUIRE(actual.has_value()==allowed);
    if(actual){++sequence;++reservations;dirty=false;CHECK(actual->sequence==sequence);CHECK(actual->view==id);
     CHECK(actual->generation==generation);outstanding.emplace(sequence,*actual);history.push_back(*actual);}
   }
   if(op==6 && !history.empty()) {
    auto ticket=history[random()%history.size()];bool forged=random()%4==0;
    if(forged){++ticket.generation;++duplicates;}
    auto found=outstanding.find(ticket.sequence);
    if(found!=outstanding.end() && found->second.generation==ticket.generation && found->second.view==ticket.view) {
     if(ticket.generation!=generation)++stale;outstanding.erase(found);
    }else ++duplicates;
    pump.complete(ticket);
   }
   if(op==7 && step>440){pump.stop();stopped=true;dirty=false;}
   REQUIRE(pump.inFlight()==outstanding.size());
  }
  CHECK(reservations>0);CHECK(duplicates>0);CHECK(stale>0); // seeded trace contains real admission and forged/repeated completion work
  for(auto const& item:outstanding)pump.complete(item.second);
  CHECK(pump.inFlight()==0);
 }
}
