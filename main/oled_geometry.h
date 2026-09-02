// Pure page-offset arithmetic for an SSD1306 panel window, no hardware
// dependency — separated out so it is host-testable (AGENTS.md §8: "every
// conditional branch that encodes real logic gets a test"). This exact
// formula already cost one wasted flash-and-observe cycle to derive
// correctly: the controller addresses 8 pages (64 rows) regardless of panel
// size, and a panel window that isn't centered in that space (this board's
// isn't — it sits flush against one end) needs its offset recomputed on
// rotation, not just the segment-remap/COM-scan-direction bits flipped.
// Flipping only the bits mirrors which pages the glass shows around the
// *center* of the full 8-page space, not around the window's own center.
#pragma once

// `total_pages` is the controller's full page count (8 for the SSD1306's
// 64-row multiplex). `panel_pages` is the visible window's height in pages.
// `y_offset_pages` is the window's un-rotated page offset from page 0.
// `rotate_180` selects the same rotation the segment-remap/COM-scan-direction
// bits are flipped for — see BOARD_OLED_ROTATE_180 in board.h.
#define OLED_PAGE_OFFSET_FOR(total_pages, panel_pages, y_offset_pages, \
                             rotate_180)                               \
  ((rotate_180) ? ((total_pages) - (panel_pages) - (y_offset_pages))   \
                : (y_offset_pages))
