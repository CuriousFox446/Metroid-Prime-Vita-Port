#pragma once

// Per-thread statics for the particle code (PS Vita, 0.42.0 test 19).
//
// The side worker (Aurora's thread on the fourth core, extern/aurora/lib/vita_side_jobs.cpp) steps
// the actors' particle systems (CParticleDatabase::Update) while the game thread goes on with the
// tick, its own particle code included. That code keeps its state between calls in statics: the
// particle and system being evaluated and their lifetime tweens (CParticleGlobals), the global
// random number generator (CRandom16, CGlobalRandom), the variables in scope, the child seeds, the
// update depth. Each of those holds a value for each thread that runs it: the game thread's (and
// every other thread's), the side worker's, and since 0.46.0 test 16 the second side worker's, on
// the first core. A thread's stack pointer tells which it reads: it lies inside a side worker's
// stack only on that worker. No lookup, no lock: two compares.
//
// Elsewhere, and on the Vita before the side workers start, every thread reads slot 0.

#include <cstdint>

// The slots: 0 the game thread and the rest, 1 the side worker, 2 the second side worker.
inline constexpr int kPortThreadSlots = 3;

#if defined(__vita__)
extern "C" {
// Each side worker's stack, written by it before its first job (both 0 until then).
extern uintptr_t gPortSideStackLow;
extern uintptr_t gPortSideStackSpan;
extern uintptr_t gPortSide2StackLow;
extern uintptr_t gPortSide2StackSpan;
}

// 1 on the side worker, 2 on the second, 0 on every other thread.
inline int PortThreadSlot() {
  uintptr_t sp;
  __asm__("mov %0, sp" : "=r"(sp));
  if (sp - gPortSideStackLow < gPortSideStackSpan) {
    return 1;
  }
  return sp - gPortSide2StackLow < gPortSide2StackSpan ? 2 : 0;
}
#else
inline int PortThreadSlot() { return 0; }
#endif

// A static with a value for each slot, each in a cache line of its own (the threads write theirs at
// the same time). It reads and writes as the T it stands for.
template < typename T >
class PortThreadValue {
public:
  constexpr PortThreadValue() : x_cells{} {}
  constexpr explicit PortThreadValue(T initial) : x_cells{{initial}, {initial}, {initial}} {}
  PortThreadValue(const PortThreadValue&) = delete;

  T& Get() { return x_cells[PortThreadSlot()].value; }
  operator T&() { return Get(); }
  PortThreadValue& operator=(const T& value) {
    Get() = value;
    return *this;
  }
  T operator->() { return Get(); }
  T& operator++() { return ++Get(); }
  T& operator--() { return --Get(); }
  T operator++(int) { return Get()++; }
  T operator--(int) { return Get()--; }

private:
  struct alignas(64) Cell {
    T value;
  };
  Cell x_cells[kPortThreadSlots];
};
