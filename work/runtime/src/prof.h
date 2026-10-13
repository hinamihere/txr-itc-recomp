#pragma once
// Sampling profiler (enabled with ITC_PROF=1). ProfNameThread labels the calling thread in the reports.
void ProfStart();
void ProfNameThread(const char* name);
void ProfNameThreadId(unsigned tid, const char* name);
