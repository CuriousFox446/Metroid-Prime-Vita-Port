#pragma once

#include <algorithm>
#include <cmath>

namespace PortTiming {
// Accumulates wall-clock time and releases whole fixed-size simulation steps.
// The step defaults to the GameCube's 60 Hz tick; the port can raise it to run
// the simulation at the display rate (see PortDebug::SimRate()).
class FixedStepClock {
public:
  static constexpr double kPeriod = 1.0 / 60.0;

  void SetPeriod(double period) {
    if (std::isfinite(period) && period > 0.0) {
      mPeriod = period;
    }
  }
  double Period() const { return mPeriod; }

  // `cadenceSteps`: the steps a frame paced to a frame cap below the step rate that divides it runs
  // (2 at a 30 FPS cap on 60 Hz steps), 0 for none.
  unsigned Advance(double elapsed, bool oneTick = false, bool cappedCadence = false, unsigned cadenceSteps = 0) {
    if (oneTick) {
      mRemainder = 0.0;
      return 1;
    }
    if (!std::isfinite(elapsed) || elapsed < 0.0) return 0;
    // Bound long stalls, but retain fractional time through ordinary jitter.
    mRemainder = std::min(mRemainder + elapsed, 0.25);
    if (cadenceSteps > 0) {
      // Such a frame runs its steps while the clock stays within a whole step of them either way: the
      // remainder carries the difference (the pacing's jitter, the display's own rate) and only a whole
      // step of it, behind or ahead, adds a step or holds one back, after which it is near nothing again.
      // At the step's edge microseconds of jitter alternated one and three steps at the same 30 frames
      // a second (0.46.0 test 69's log), a judder on everything moving.
      const double left = mRemainder - cadenceSteps * mPeriod;
      unsigned steps = cadenceSteps;
      if (left >= mPeriod) {
        steps += static_cast<unsigned>(left / mPeriod);
      } else if (left <= -mPeriod) {
        const unsigned fewer = static_cast<unsigned>(-left / mPeriod);
        steps = fewer >= steps ? 0 : steps - fewer;
      }
      mRemainder -= steps * mPeriod;
      return steps;
    }
    // A precise deadline still jitters by microseconds. Without a little
    // scheduling leeway it can alternate zero/two ticks at the same rendered
    // frame rate. Keep the borrowed time as debt; no time is lost.
    const double leeway = cappedCadence ? 0.00025 : 0.0;
    const unsigned steps = static_cast<unsigned>(std::max(0.0, mRemainder + leeway) / mPeriod);
    mRemainder -= steps * mPeriod;
    return steps;
  }
  // Adaptive stepping: a step already covered the frame, so leftover time (from
  // a clamped hitch or a change of period) would only cause a stray extra step.
  void DropRemainder() { mRemainder = 0.0; }
  float Interpolation() const {
    return static_cast<float>(std::clamp(mRemainder / mPeriod, 0.0, 1.0));
  }

private:
  double mPeriod = kPeriod;
  double mRemainder = 0.0;
};
} // namespace PortTiming
