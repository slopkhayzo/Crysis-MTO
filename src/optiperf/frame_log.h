#pragma once
// A frame-rate log, to measure play sessions (it works in every mode, with every other part off too): each call of
// CSystem::Update on the main thread starts a frame, so the time between two is a frame's time. Every second a line of
// optiperf_frames.csv (next to the log): the seconds since the first frame, that second's frames and frame rate, its
// longest frame, and its frames with the game paused (the menus over a level). At each report a summary in the log: the
// frames counted (not the paused ones, nor those over 1 s: loads, the exit), their average rate, the frame times' median,
// 1 % and 0.1 % lows, the longest, and the share of the time below 100 and 60 fps.
//
// [Debug] FrameLog = off (default) | on   (env OPTIPERF_FRAME_LOG)

#include <cstddef>

namespace optiperf {

void FrameLog_OnModuleLoaded(const wchar_t* baseName, char* base, size_t size);
void FrameLog_Report(const char* when);

}  // namespace optiperf
