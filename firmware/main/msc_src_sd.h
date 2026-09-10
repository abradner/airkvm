// The microSD slot as the mass-storage medium.
//
// Boards that have a slot (BOARD_HAS_SDCARD in board.h) expose the card to
// the target as the LUN's medium: insert a card, the target sees a disk;
// pull it, the disk is gone. Read-only in this first version — write, and
// an ISO-file-on-card CD-ROM mode, are issue #34's territory.
//
// Exclusive access is the rule: while the card is the target's disk the
// dongle never mounts its filesystem. Two writers to one FAT is corruption,
// and the target owns it.
//
// Boundary (docs/repo-map.md, R-008): a card is input the firmware did not
// author, accepted with the physical-presence argument (inserting a card is
// the same kind of act as plugging in). Read-only means the dongle writes
// nothing to it — the target cannot leave data on the card through us — but a
// card still leaves carrying whatever was on it when it arrived.
#pragma once

#include "esp_err.h"

// Starts the host, the slot, and the probe task. Never blocks on the card:
// probing is the probe task's job and never app_main's.
esp_err_t msc_src_sd_start(void);
