@echo off
rem Starts the game with logging (play.err). Close the game window when you are done.
cd /d C:\itc-work
set ITC_STATUS=1
rt_build\itc.exe extracted\default.xex 2> play.err
