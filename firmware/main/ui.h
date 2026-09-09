// Owns the front panel: status display, LED, and the mode button.
//
// One task drives all three so the timings live together and nothing else in
// the firmware has to know a display exists.
#pragma once

void ui_start(void);
