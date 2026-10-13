@echo off
rem Same as play.bat plus the sampling profiler (play.err gets call-stack samples of the busy threads every 2 s; costs a few percent of fps).
rem Use it when the game slows down somewhere and tell me where/when it happened.
cd /d C:\itc-work
set ITC_STATUS=1
set ITC_PROF=1
rt_build\itc.exe extracted\default.xex 2> play_prof.err
