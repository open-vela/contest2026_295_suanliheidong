#pragma once
#include <cmath>
#include <cstdint>
#include "robot_wakeword_model_params.h"
// Standalone utility only. robot_wakeword.c already owns this state machine:
// never run both processors on the same score stream.
struct RobotWakewordDetection { bool wake; double score; };
class RobotWakewordDetector {
 public:
  RobotWakewordDetector() { Reset(); }
  void Reset() {
    for (unsigned i=0;i<ROBOT_WAKEWORD_SCORE_AVERAGE_WINDOW;i++) history_[i]=0;
    sum_=0; count_=pos_=hits_=0; old_=candidate_=last_=0;
    armed_=true; has_trigger_=false;
  }
  RobotWakewordDetection Update(float raw, double level, double seconds) {
    const uint64_t end=static_cast<uint64_t>(std::llround(seconds*16000));
    if(end<=old_ || end%ROBOT_WAKEWORD_HOP_SAMPLES) return {false, count_?sum_/count_:0};
    const uint64_t skipped=(end-old_)/ROBOT_WAKEWORD_HOP_SAMPLES-1;
    old_=end;
    const float gated=static_cast<float>(level)>=ROBOT_WAKEWORD_MIN_RMS?raw:0;
    if(count_==ROBOT_WAKEWORD_SCORE_AVERAGE_WINDOW) sum_-=history_[pos_];
    else count_++;
    history_[pos_]=gated; sum_+=gated;
    pos_=(pos_+1)%ROBOT_WAKEWORD_SCORE_AVERAGE_WINDOW;
    const float score=sum_/count_;
    const float threshold=ROBOT_WAKEWORD_MODEL_THRESHOLD_OVERRIDE>0?
        ROBOT_WAKEWORD_MODEL_THRESHOLD_OVERRIDE:ROBOT_WAKEWORD_THRESHOLD;
    if(count_<ROBOT_WAKEWORD_SCORE_AVERAGE_WINDOW) hits_=0,candidate_=0;
    else if(score<=std::fmax(0.f,threshold-ROBOT_WAKEWORD_HYSTERESIS))
      armed_=true,hits_=0,candidate_=0;
    else if(score>=threshold) {
      if(skipped || !hits_ || end-candidate_!=ROBOT_WAKEWORD_HOP_SAMPLES) hits_=1;
      else hits_++;
      candidate_=end;
    } else hits_=0,candidate_=0;
    bool wake=armed_ && hits_>=ROBOT_WAKEWORD_CONFIRM_HITS &&
        (!has_trigger_ || static_cast<double>(end-last_)>=
          static_cast<double>(ROBOT_WAKEWORD_COOLDOWN_SECONDS)*16000);
    if(wake) armed_=false,has_trigger_=true,last_=end,hits_=0,candidate_=0;
    return {wake,score};
  }
 private:
  float history_[ROBOT_WAKEWORD_SCORE_AVERAGE_WINDOW],sum_;
  unsigned count_,pos_,hits_;
  uint64_t old_,candidate_,last_;
  bool armed_,has_trigger_;
};
