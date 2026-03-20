#include <unity.h>

#include "LogRing.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

void test_append_before_wrap() {
  LogRing::State state{4, 0, 0};

  LogRing::advanceAfterAppend(state);
  TEST_ASSERT_EQUAL_UINT32(1, state.head);
  TEST_ASSERT_EQUAL_UINT32(1, state.count);

  LogRing::advanceAfterAppend(state);
  TEST_ASSERT_EQUAL_UINT32(2, state.head);
  TEST_ASSERT_EQUAL_UINT32(2, state.count);
  TEST_ASSERT_EQUAL_UINT32(0, LogRing::oldestIndex(state));
  TEST_ASSERT_EQUAL_UINT32(0, LogRing::physicalIndexForLogical(state, 0));
  TEST_ASSERT_EQUAL_UINT32(1, LogRing::physicalIndexForLogical(state, 1));
}

void test_append_after_wrap() {
  LogRing::State state{4, 0, 0};
  for (int i = 0; i < 6; ++i) {
    LogRing::advanceAfterAppend(state);
  }

  TEST_ASSERT_EQUAL_UINT32(2, state.head);
  TEST_ASSERT_EQUAL_UINT32(4, state.count);
  TEST_ASSERT_EQUAL_UINT32(2, LogRing::oldestIndex(state));
  TEST_ASSERT_EQUAL_UINT32(2, LogRing::physicalIndexForLogical(state, 0));
  TEST_ASSERT_EQUAL_UINT32(3, LogRing::physicalIndexForLogical(state, 1));
  TEST_ASSERT_EQUAL_UINT32(0, LogRing::physicalIndexForLogical(state, 2));
  TEST_ASSERT_EQUAL_UINT32(1, LogRing::physicalIndexForLogical(state, 3));
}

void test_invalid_logical_index_rejected() {
  LogRing::State state{8, 3, 5};
  TEST_ASSERT_TRUE(LogRing::isValidLogicalIndex(state, 4));
  TEST_ASSERT_FALSE(LogRing::isValidLogicalIndex(state, 5));
}

#ifdef ARDUINO
void setup() {
  delay(2000);
  UNITY_BEGIN();
  RUN_TEST(test_append_before_wrap);
  RUN_TEST(test_append_after_wrap);
  RUN_TEST(test_invalid_logical_index_rejected);
  UNITY_END();
}

void loop() {
}
#else
int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_append_before_wrap);
  RUN_TEST(test_append_after_wrap);
  RUN_TEST(test_invalid_logical_index_rejected);
  return UNITY_END();
}
#endif
