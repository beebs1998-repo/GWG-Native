// In-memory code patches for game.exe.
//
// The game runs a hardware self-test after bringing the arcade I/O board up.
// Two of its checks compare a result code and, on failure, store "coinerror" or
// "i/oerror" into the error field at game.exe + 0xFBC (see 0x43E3FC..0x43E482).
// Later, 0x44A085 compares that field against "i/oerror" and, on a match, prints
// the I/O ERROR screen before quitting (0x44A0C5).
//
// Both failure stores sit behind a short conditional jump:
//
//   0x43E426  74 17   je  0x43E43F    ; do not set "coinerror"
//   0x43E469  74 17   je  0x43E482    ; do not set "i/oerror"
//
// Flipping the opcode from 74 (jump if equal) to EB (jump always) leaves the
// displacement untouched and takes the same "all fine" path the game takes when
// the hardware check passes. That is the same kind of fix OpenParrot applies
// ("error display routine noping").
//
// Patches are data-driven rather than hard-coded so they are easy to audit,
// disable or retarget: the launcher passes the [Patches] section of the ini as
// "ADDR=OLD>NEW" entries separated by semicolals.
#pragma once

#include <windows.h>

namespace patches {

// Applies every patch in GWG_NATIVE_PATCHES. Returns the number applied.
int Apply();

}  // namespace patches