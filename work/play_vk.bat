@echo off
rem Starts the game with the Vulkan renderer (log: play_vk.err). Close the game window when you are done.
cd /d C:\itc-work
set ITC_STATUS=1
set ITC_RENDERER=vulkan
rt_build\itc.exe extracted\default.xex 2> play_vk.err
