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
// Since 0.46.0 test 18 the game thread runs side jobs too, those no worker has taken by the time it
// waits for them (vita_side_jobs.cpp, wait_game). While one runs, the part of the game thread's stack
// below the wait is slot 3: the job's code sees itself on a side job, as on a worker (slot not 0: a
// projectile's step does not wait for itself), and the game thread's own state in slot 0, that of the
// code around the wait, stays as it was. Test 17 ran them in slot 0, and a projectile's step waited
// for itself: the game froze.
//
// Elsewhere, and on the Vita before the side workers start, every thread reads slot 0.

#include <cstdint>

// The slots: 0 the game thread and the rest, 1 the side worker, 2 the second side worker, 3 a side
// job the game thread runs while it waits.
inline constexpr int kPortThreadSlots = 4;

#if defined(__vita__)
extern "C" {
// Each side worker's stack, written by it before its first job (both 0 until then).
extern uintptr_t gPortSideStackLow;
extern uintptr_t gPortSideStackSpan;
extern uintptr_t gPortSide2StackLow;
extern uintptr_t gPortSide2StackSpan;
// The game thread's stack below the wait where it runs a side job: its span 0 while it runs none.
extern uintptr_t gPortGameJobStackLow;
extern uintptr_t gPortGameJobStackSpan;
}

// 1 on the side worker, 2 on the second, 3 in a side job the game thread runs, 0 everywhere else.
inline int PortThreadSlot() {
  uintptr_t sp;
  __asm__("mov %0, sp" : "=r"(sp));
  if (sp - gPortSideStackLow < gPortSideStackSpan) {
    return 1;
  }
  if (sp - gPortSide2StackLow < gPortSide2StackSpan) {
    return 2;
  }
  return sp - gPortGameJobStackLow < gPortGameJobStackSpan ? 3 : 0;
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
  constexpr explicit PortThreadValue(T initial) : x_cells{{initial}, {initial}, {initial}, {initial}} {}
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
