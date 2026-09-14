#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Who the CPU time went to since the last call, per FreeRTOS task.
//
// Written 2026-09-14 for a 4.3B that moved snapshots at 20-30 KB/s for
// several minutes after some project installs and at 600 KB/s otherwise -
// same access point, same signal, same firmware - after the radio, power
// save, the access point and the client had each been measured away. The
// counters the Arduino core already compiles in
// (CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS) answer "what is this board busy
// with" from the inside, which no amount of timing from the outside does.
//
// Deltas, not totals: a boot's worth of accumulated time hides whatever
// started a minute ago. The first call only primes the baseline.
class TaskLoad {
public:
  String report()
  {
    const UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = (TaskStatus_t *)malloc(sizeof(TaskStatus_t) * (count + 4));
    if (!tasks) return "  tasks: out of memory\n";
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t n = uxTaskGetSystemState(tasks, count + 4, &total);

    String out;
    const configRUN_TIME_COUNTER_TYPE window = total - lastTotal_;
    if (lastTotal_ == 0 || window == 0)
    {
      out = "  tasks: baseline taken, ask again for the load since now\n";
    }
    else
    {
      // Per core the idle tasks together make 100% of one core each, so a
      // task's share is of the window times the core count.
      out = "  tasks over the last " + String(window / 1000000.0f, 1) + " s (% of one core):\n";
      for (UBaseType_t i = 0; i < n; i++)
      {
        const configRUN_TIME_COUNTER_TYPE before = previous(tasks[i].xHandle);
        const configRUN_TIME_COUNTER_TYPE used = tasks[i].ulRunTimeCounter - before;
        const float pct = 100.0f * (float)used / (float)window;
        if (pct < 0.5f) continue;
        char line[96];
        snprintf(line, sizeof(line), "    %-16s core %2d prio %2u  %5.1f%%\n", tasks[i].pcTaskName,
                 tasks[i].xCoreID == tskNO_AFFINITY ? -1 : (int)tasks[i].xCoreID,
                 (unsigned)tasks[i].uxCurrentPriority, pct);
        out += line;
      }
    }

    // Keep this call's counters as the next baseline.
    count_ = 0;
    for (UBaseType_t i = 0; i < n && count_ < MAX_TASKS; i++)
    {
      handles_[count_] = tasks[i].xHandle;
      counters_[count_] = tasks[i].ulRunTimeCounter;
      count_++;
    }
    lastTotal_ = total;
    free(tasks);
    return out;
  }

private:
  static const int MAX_TASKS = 48;
  TaskHandle_t handles_[MAX_TASKS];
  configRUN_TIME_COUNTER_TYPE counters_[MAX_TASKS];
  int count_ = 0;
  configRUN_TIME_COUNTER_TYPE lastTotal_ = 0;

  configRUN_TIME_COUNTER_TYPE previous(TaskHandle_t h) const
  {
    for (int i = 0; i < count_; i++)
      if (handles_[i] == h) return counters_[i];
    return 0;
  }
};
