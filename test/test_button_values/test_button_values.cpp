// A button's message with values and «Only when» (ButtonValues.h), on the PC:
//
//   pio test -e native -f test_button_values
//
// The same cases as the designer's e2e/button-values.spec.ts «the rules»,
// which tests lib/button-values.ts - the original this header is ported from.

#include <unity.h>

#include <map>
#include <string>

#include "project/ButtonValues.h"

void setUp() {}
void tearDown() {}

static const std::string MESSAGE = "{\"action\":\"DISARM\",\"code\":\"{topic:local/e2e-alarm/code}\"}";

static buttonvalues::Lookup from(std::map<std::string, std::string> values) {
  return [values](const std::string& key) -> std::optional<std::string> {
    auto found = values.find(key);
    if (found == values.end()) return std::nullopt;
    return found->second;
  };
}

static void onlyTopicPlaceholdersAreFilled() {
  const auto v = from({{"a/b", "12"}, {"c#d", "x"}});
  TEST_ASSERT_EQUAL_STRING("{\"action\":\"DISARM\",\"code\":\"1234\"}",
                           buttonvalues::fillMessage(MESSAGE, from({{"local/e2e-alarm/code", "1234"}})).c_str());
  TEST_ASSERT_EQUAL_STRING("12-x", buttonvalues::fillMessage("{topic:a/b}-{topic:c#d}", v).c_str());
  TEST_ASSERT_EQUAL_STRING("", buttonvalues::fillMessage("{topic:none}", v).c_str());
  TEST_ASSERT_EQUAL_STRING("{\"x\":{{value}}}", buttonvalues::fillMessage("{\"x\":{{value}}}", v).c_str());
  TEST_ASSERT_EQUAL_STRING("{topic:a/b", buttonvalues::fillMessage("{topic:a/b", v).c_str());
}

static void sentOnlyWhenYes() {
  const auto topic = from({{"local/e2e-alarm/code", "1234"}});
  TEST_ASSERT_FALSE(buttonvalues::pressedMessage(MESSAGE, "code_ok", from({{"code_ok", "false"}}), topic).has_value());
  TEST_ASSERT_FALSE(buttonvalues::pressedMessage(MESSAGE, "code_ok", from({}), topic).has_value());
  const auto sent = buttonvalues::pressedMessage(MESSAGE, "code_ok", from({{"code_ok", "true"}}), topic);
  TEST_ASSERT_TRUE(sent.has_value());
  TEST_ASSERT_EQUAL_STRING("{\"action\":\"DISARM\",\"code\":\"1234\"}", sent->c_str());
  // Without «Only when» it always sends.
  TEST_ASSERT_EQUAL_STRING("ARM_AWAY", buttonvalues::pressedMessage("ARM_AWAY", "", from({}), from({}))->c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(onlyTopicPlaceholdersAreFilled);
  RUN_TEST(sentOnlyWhenYes);
  return UNITY_END();
}
