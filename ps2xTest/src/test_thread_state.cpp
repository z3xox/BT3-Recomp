#include "ps2_runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Syscalls/Helpers/State.h"

extern "C" void ps2xTestSetCurrentThreadId(int tid)
{
    g_currentThreadId = tid;
}
