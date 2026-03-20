#pragma once

#include <stddef.h>

namespace LogRing {

struct State {
  size_t capacity;
  size_t head;
  size_t count;
};

inline size_t oldestIndex(const State& state) {
  return state.count == state.capacity ? state.head : 0;
}

inline bool isValidLogicalIndex(const State& state, size_t logicalIndex) {
  return logicalIndex < state.count;
}

inline size_t physicalIndexForLogical(const State& state, size_t logicalIndex) {
  return (oldestIndex(state) + logicalIndex) % state.capacity;
}

inline void advanceAfterAppend(State& state) {
  state.head = (state.head + 1) % state.capacity;
  if (state.count < state.capacity) {
    ++state.count;
  }
}

}  // namespace LogRing
