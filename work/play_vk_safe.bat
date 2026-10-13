@echo off
rem Vulkan renderer with every draw-path shortcut turned off (ITC_NO_PERF=1: all vertex/texture data re-checked every frame). Log: play_vk_safe.err.
cd /d C:\itc-work
set ITC_STATUS=1
set ITC_RENDERER=vulkan
set ITC_NO_PERF=1
rt_build\itc.exe extracted\default.xex 2> play_vk_safe.err
