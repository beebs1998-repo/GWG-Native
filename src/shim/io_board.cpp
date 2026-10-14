// Emulation of the arcade I/O board (coin counter / LED / service switch).
//
// GigaWing Generations talks to its I/O board over a Win32 serial port on
// "COM2". From static analysis of game.exe:
//
//   0x520933  CreateFileA("COM2", GENERIC_READ|GENERIC_WRITE, 0, NULL,
//                         OPEN_EXISTING, 0, NULL)  -> stored in global 0x5C4714
//   0x520952  SetupComm(h, 0x204, 0x204)
//   0x5209A9  SetCommState(h, &dcb)      -> 115200 baud, 8 data bits
//   0x5209BC  SetCommMask(h, EV_RXCHAR)
//   0x5209FF  SetCommTimeouts(h, &to)    -> ReadTotalTimeoutConstant = 20ms
//   0x5207F0  GetCommModemStatus(h,&msr) -> returns (msr >> 4) & 1, i.e. CTS
//   0x520850  read helper: ClearCommError + ReadFile
//   0x520380  packet parser (see below)
//   0x520820  write helper: WriteFile
//
// Wire protocol (decoded from the parser at 0x520380):
//
//   packet := 0xE0 <data bytes...> <checksum>
//   checksum == (sum of the data bytes) & 0xFF
//
// The game's own traffic confirms this. It emits exactly
//
//   E0 FF 03 F0 D9 CB     and  FF+03+F0+D9 = 0x3CB -> 0xCB (checksum)
//
// 0xD0 may also appear and switches the parser into a mode where received
// bytes have 1 subtracted before being stored.
//
// Handshake (0x51fc40 .. 0x51fd42):
//
//   1. game sends a 0xF0 command packet, Sleep(1000)
//   2. game polls CTS, re-sending while CTS is still asserted, and *requires*
//      CTS to be LOW before it will continue
//   3. game then loops up to 32 times: send a 0xF1 packet, read a reply via
//      0x520380, and stop as soon as CTS goes HIGH again
//   4. the decoded reply must have 0x01 at payload bytes 8 and 9
//
// So the emulated board has to drop CTS when it is handed a command and raise
// it again once the reply is queued - holding CTS asserted forever is what
// leaves the game spinning in its once-per-second retry loop.

#include "io_board.h"

#include <stdlib.h>

#include "game_thread.h"
#include "log.h"

namespace ioboard {
namespace {

constexpr DWORD kQueueSize = 0x204;      // what SetupComm is called with
constexpr size_t kRxCapacity = 0x2000;   // board -> game ring
DWORD g_busyMs = 2500;          // how long CTS stays low after a command
constexpr DWORD kMaxPacket = 0x204;

struct Board {
    CRITICAL_SECTION lock;
    bool initialised = false;
    HANDLE port = INVALID_HANDLE_VALUE;
    DWORD commMask = 0;

    bool cts = true;          // CTS as seen by the game (MS_CTS = 0x10)
    DWORD readyAt = 0;        // tick at which CTS rises again

    // The 0xF1 handshake rounds count the I/O channels the board reports.
    // The game increments its channel counter once per round and stops when
    // CTS goes high, then reads per-channel data with commands 0x10..0x14.
    // Raise CTS too early and the game concludes there is only one channel and
    // skips every per-channel read.
    DWORD roundsWanted = 2;
    DWORD roundsSeen = 0;



    unsigned char rx[kRxCapacity];
    size_t rxHead = 0;
    size_t rxTail = 0;

    unsigned char reply[kMaxPacket];
    size_t replyLen = 0;

    bool echoMode = false;
    DWORD txCount = 0;
    DWORD rxCount = 0;
};

Board g;



size_t RxAvailable() {
    return (g.rxHead + kRxCapacity - g.rxTail) % kRxCapacity;
}

void RxPush(const unsigned char* data, DWORD len) {
    for (DWORD i = 0; i < len; ++i) {
        size_t next = (g.rxHead + 1) % kRxCapacity;
        if (next == g.rxTail) break;
        g.rx[g.rxHead] = data[i];
        g.rxHead = next;
    }
}

// Raises CTS once the simulated "processing" window has elapsed *and* the board
// has seen enough 0xF1 handshakes. Called from every entry point the game might
// poll while waiting.
void Service() {
    if (g.cts) return;
    if (g.roundsSeen < g.roundsWanted) return;
    if (GetTickCount() >= g.readyAt) g.cts = true;
}

void FillDefaultDcb(LPDCB dcb) {
    if (!dcb) return;
    ZeroMemory(dcb, sizeof(*dcb));
    dcb->DCBlength = sizeof(DCB);
    dcb->BaudRate = CBR_115200;
    dcb->ByteSize = 8;
    dcb->fBinary = TRUE;
    dcb->Parity = NOPARITY;
    dcb->StopBits = ONESTOPBIT;
    dcb->fParity = FALSE;
    dcb->fOutxCtsFlow = FALSE;
    dcb->fOutxDsrFlow = FALSE;
    dcb->fOutX = FALSE;
    dcb->fInX = FALSE;
    dcb->fErrorChar = FALSE;
    dcb->fNull = FALSE;
    dcb->fAbortOnError = FALSE;
    dcb->fDtrControl = DTR_CONTROL_DISABLE;
    dcb->fRtsControl = RTS_CONTROL_DISABLE;
}

const char* EscapeName(DWORD func) {
    switch (func) {
        case SETXOFF:  return "SETXOFF";
        case SETXON:   return "SETXON";
        case SETRTS:   return "SETRTS";
        case CLRRTS:   return "CLRRTS";
        case SETDTR:   return "SETDTR";
        case CLRDTR:   return "CLRDTR";
        case SETBREAK: return "SETBREAK";
        case CLRBREAK: return "CLRBREAK";
        default:       return "UNKNOWN";
    }
}

// Parses "E00101..." into the canned reply buffer.
bool LoadReply(const char* hex) {
    if (!hex) return false;
    size_t len = strlen(hex);
    if (len < 2 || (len & 1)) return false;
    size_t n = len / 2;
    if (n > kMaxPacket) n = kMaxPacket;

    unsigned char tmp[kMaxPacket];
    for (size_t i = 0; i < n; ++i) {
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        tmp[i] = (unsigned char)strtoul(pair, NULL, 16);
    }
    memcpy(g.reply, tmp, n);
    g.replyLen = n;
    return true;
}

// --- keyboard input ------------------------------------------------------
//
// Bit layout is the standard JVS digital input byte (confirmed against
// TeknoParrotUI's JvsPackageEmulator.GetPlayerControls):
//
//   0x80 start   0x40 coin 1   0x20 up    0x10 down
//   0x08 left    0x04 right    0x02 shot   0x01 bomb
//
// The preceding byte carries the cabinet switches, of which only the test
// switch (0x80) is used.
//
// Note on coins: the game credits from this digital byte, not from the board's
// 0x21 coin-counter command. No coin counter is kept here - the JVS coin path
// is never exercised by this title, and wiring a key to it looked like a
// missing feature when it was simply the wrong key.

bool Down(int vk) { return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }

unsigned char CabinetSwitches() {
    // F11 - test switch.
    return Down(VK_F11) ? 0x80 : 0x00;
}

unsigned char PlayerSwitches(int player) {
    // Player 2 uses the numpad so both stick slots can be driven at once.
    const bool p2 = player != 0;

    unsigned char v = 0;
    if (Down(p2 ? VK_NUMPAD0 : VK_F1)) v |= 0x80;  // start
    if (Down(VK_F2)) v |= 0x40;                     // coin 1
    if (Down(VK_UP)) v |= 0x20;
    if (Down(VK_DOWN)) v |= 0x10;
    if (Down(VK_LEFT)) v |= 0x08;
    if (Down(VK_RIGHT)) v |= 0x04;
    if (Down(p2 ? VK_NUMPAD1 : 'Z')) v |= 0x02;      // shot
    if (Down(p2 ? VK_NUMPAD2 : 'X')) v |= 0x01;      // bomb
    return v;
}

// --- per-command replies -------------------------------------------------
//
// Wire format, as implemented by TeknoParrotUI's JvsHelper:
//
//   E0 <node=0> <len> <status=01> <report=01> <payload...> <checksum>
//   len      == payload + 3
//   checksum == len + 2 + sum(payload)
//
// The reset command (0xF0) is deliberately answered with silence, which is what
// the real board does; the game never reads a reply to it.

constexpr unsigned char kJvsVersion = 0x20;
constexpr unsigned char kJvsCommVersion = 0x10;
constexpr unsigned char kJvsCommandRevision = 0x13;
constexpr unsigned char kJvsSwitchCount = 0x0E;  // 14 switches per player
constexpr const char kJvsIdentifier[] =
    "SEGA CORPORATION;I/O BD JVS;837-14572;Ver1.00;2005/10";

void FinishReply(const unsigned char* payload, size_t len) {
    if (len + 6 > kMaxPacket) return;

    const size_t lenField = len + 3;
    unsigned int sum = (unsigned int)(lenField + 2);
    for (size_t i = 0; i < len; ++i) sum += payload[i];

    size_t n = 0;
    g.reply[n++] = 0xE0;
    g.reply[n++] = 0x00;
    g.reply[n++] = (unsigned char)lenField;
    g.reply[n++] = 0x01;  // JVSStatus.OK
    g.reply[n++] = 0x01;  // JVSReport.OK
    for (size_t i = 0; i < len; ++i) g.reply[n++] = payload[i];
    g.reply[n++] = (unsigned char)(sum & 0xFF);
    g.replyLen = n;
}

// Builds the reply for one command. Returns false when the command is
// unanswered, in which case nothing is queued for the game to read.
bool BuildCommandReply(const unsigned char* req, DWORD reqLen) {
    if (reqLen < 4) return false;

    const unsigned char node = req[1];
    const unsigned char cmd = req[3];
    const unsigned char arg0 = reqLen > 4 ? req[4] : 0;  // player / slot count
    const unsigned char arg1 = reqLen > 5 ? req[5] : 0;  // bytes per unit

    unsigned char payload[64];
    size_t n = 0;

    switch (cmd) {
        case 0xF0:  // OP_RESET: the real board stays silent.
            return false;

        case 0xF1:  // OP_ADDRESS: acknowledge, no payload.
            break;

        case 0x10:  // ID_DATA: the board's identification string.
            n = sizeof(kJvsIdentifier);  // includes the terminating NUL
            memcpy(payload, kJvsIdentifier, n);
            break;

        case 0x11:  // command revision
            payload[n++] = kJvsCommandRevision;
            break;
        case 0x12:  // JVS version
            payload[n++] = kJvsVersion;
            break;
        case 0x13:  // communication version
            payload[n++] = kJvsCommVersion;
            break;

        case 0x14: {  // slave feature descriptor
            static const unsigned char kFeatures[] = {
                0x01, 0x02, kJvsSwitchCount, 0x00,  // switch input: 2 players
                0x02, 0x02, 0x00, 0x00,            // coin: 2 slots
                0x03, 0x08, 0x0A, 0x00,            // analog: 8 ch, 10 bit
                0x12, 0x14,                        // general purpose out
                0x00, 0x00, 0x00,                  // exit code + padding
            };
            n = sizeof(kFeatures);
            memcpy(payload, kFeatures, n);
            break;
        }

        case 0x20: {  // digital inputs
            payload[n++] = CabinetSwitches();
            unsigned players = arg0 ? arg0 : 1;
            if (players > 2) players = 2;
            unsigned perPlayer = arg1 ? arg1 : 1;
            for (unsigned p = 0; p < players; ++p) {
                // Node 2 addresses the second pair of players.
                int slot = (node == 2 ? 2 : 0) + (int)p;
                payload[n++] = PlayerSwitches(slot);
                for (unsigned k = 1; k < perPlayer && n < sizeof(payload); ++k)
                    payload[n++] = 0x00;  // "extended" switch bytes, unused here
            }
            break;
        }

        case 0x22: {  // analog inputs: one byte per channel, centred on 0x80
            unsigned channels = arg0 ? arg0 : 8;
            for (unsigned i = 0; i < channels && i < 8; ++i)
                payload[n++] = 0x80;
            break;
        }

        default:
            // Notably 0x04, which the game polls on node 0 at the same rate as
            // the digital read. It is not a JVS command in any published table
            // (TeknoParrot treats it as Taito-specific and answers it as an
            // error), and an empty reply is what the game copes with.
            break;  // empty payload: a valid, minimal acknowledgement
    }

    FinishReply(payload, n);
    return true;
}

}  // namespace

bool Init(HMODULE self) {
    (void)self;
    if (g.initialised) return true;

    InitializeCriticalSection(&g.lock);
    g.initialised = true;

    // Start with CTS asserted: this is the "I/O board is present" signal the
    // game polls in GetCommModemStatus. MS_CTS == 0x10.
    g.cts = true;
    g.roundsSeen = 0;

    wchar_t echo[8] = {};
    if (GetEnvironmentVariableW(L"GWG_NATIVE_ECHO", echo, 8) > 0)
        g.echoMode = (echo[0] == L'1');

    // Default reply, in the framing the game's parser at 0x520380 expects:
    //
    //   E0 00 <len> <data...> <checksum>
    //   checksum == (len + sum(data)) & 0xFF
    //
    // A non-zero second byte selects the *transmit* framing, which makes the
    // parser ignore payload bytes entirely, so the second byte must be 0x00.
    // The length byte seeds a countdown that is decremented *before* each byte
    // is examined, so the last of the `len` bytes after it is consumed as the
    // checksum and only len-1 bytes are stored as payload. To deliver ten
    // payload bytes the length must therefore be 0x0B, and the checksum is
    // (0x0B + 10) = 0x15.
    //
    // The payload must have 0x01 at indices 0, 8 and 9: index 0 selects the
    // response handler (it dispatches on dest[0]-1), and 8/9 are the status
    // bytes the game checks after every exchange.
    LoadReply("E0000B0101010101010101010115");

    // The game polls CTS about once a second, so the "busy" window has to be
    // long enough for it to actually observe CTS low.
    wchar_t rounds[16] = {};
    if (GetEnvironmentVariableW(L"GWG_NATIVE_IO_ROUNDS", rounds, 16) > 0) {
        int v = _wtoi(rounds);
        if (v >= 0) g.roundsWanted = (DWORD)v;
    }

    wchar_t busy[16] = {};
    if (GetEnvironmentVariableW(L"GWG_NATIVE_IO_BUSYMS", busy, 16) > 0) {
        int v = _wtoi(busy);
        if (v >= 0) g_busyMs = (DWORD)v;
    }

    wchar_t replyHex[2048] = {};
    if (GetEnvironmentVariableW(L"GWG_NATIVE_IO_REPLY", replyHex,
                               sizeof(replyHex) / sizeof(replyHex[0])) > 0) {
        char narrow[2048] = {};
        WideCharToMultiByte(CP_ACP, 0, replyHex, -1, narrow, sizeof(narrow), NULL,
                            NULL);
        if (LoadReply(narrow)) {
            log::Write("ioboard: reply packet overridden (%u bytes)", g.replyLen);
        }
    }

    log::Write("ioboard: initialised (echo=%d, replyLen=%u, modemStatus=0x%02X)",
               g.echoMode ? 1 : 0, (unsigned)g.replyLen, 0x10);
    return true;
}

void Shutdown() {
    if (!g.initialised) return;
    EnterCriticalSection(&g.lock);
    if (g.port != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g.port);
        g.port = INVALID_HANDLE_VALUE;
    }
    log::Write("ioboard: shutdown (%lu tx / %lu rx)", g.txCount, g.rxCount);
    LeaveCriticalSection(&g.lock);
    DeleteCriticalSection(&g.lock);
    g.initialised = false;
}

bool Owns(HANDLE h) {
    return g.initialised && h != INVALID_HANDLE_VALUE && h == g.port;
}

HANDLE CreateFile(const char* path, DWORD access, DWORD share,
                  LPSECURITY_ATTRIBUTES sa, DWORD creation, DWORD flags,
                  HANDLE tmpl) {
    if (!path) return INVALID_HANDLE_VALUE;

    // Only the arcade serial link is emulated; everything else (the 174 MB
    // YOKUSHIN.mbd, saves, logs) must reach the real filesystem untouched.
    if (_stricmp(path, "COM2") != 0)
        return ::CreateFileA(path, access, share, sa, creation, flags, tmpl);

    // The hardware bring-up runs on the game's main thread.
    NotePossibleMainThread();

    EnterCriticalSection(&g.lock);
    if (g.port != INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g.lock);
        return g.port;
    }
    // A genuine, waitable kernel object keeps us robust against any serial API
    // the game might call that we do not intercept.
    g.port = ::CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g.port) {
        LeaveCriticalSection(&g.lock);
        log::Write("ioboard: CreateFile(COM2) FAILED to allocate handle");
        return INVALID_HANDLE_VALUE;
    }
    g.rxHead = g.rxTail = 0;
    log::Write("ioboard: CreateFile(\"COM2\") -> emulated board, handle=%p",
               (void*)g.port);
    LeaveCriticalSection(&g.lock);
    return g.port;
}

BOOL SetupComm(HANDLE h, DWORD inQueue, DWORD outQueue) {
    (void)inQueue;
    (void)outQueue;
    return Owns(h) ? TRUE : FALSE;
}

BOOL GetCommState(HANDLE h, LPDCB dcb) {
    if (!Owns(h)) return FALSE;
    FillDefaultDcb(dcb);
    return TRUE;
}

BOOL SetCommState(HANDLE h, LPDCB dcb) {
    if (!Owns(h)) return FALSE;
    if (dcb) {
        log::Write("ioboard: SetCommState baud=%u byteSize=%u parity=%u stop=%u",
                   dcb->BaudRate, dcb->ByteSize, dcb->Parity, dcb->StopBits);
    }
    return TRUE;
}

BOOL SetCommMask(HANDLE h, DWORD mask) {
    if (!Owns(h)) return FALSE;
    g.commMask = mask;
    return TRUE;
}

BOOL GetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS to) {
    if (!Owns(h)) return FALSE;
    if (to) {
        to->ReadIntervalTimeout = 0;
        to->ReadTotalTimeoutMultiplier = 0;
        to->ReadTotalTimeoutConstant = 20;
        to->WriteTotalTimeoutMultiplier = 0;
        to->WriteTotalTimeoutConstant = 0;
    }
    return TRUE;
}

BOOL SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS to) {
    (void)to;
    return Owns(h) ? TRUE : FALSE;
}

BOOL ClearCommError(HANDLE h, LPDWORD errors, LPCOMSTAT stat) {
    if (!Owns(h)) return FALSE;
    EnterCriticalSection(&g.lock);
    Service();
    if (errors) *errors = 0;
    if (stat) {
        ZeroMemory(stat, sizeof(*stat));
        stat->cbInQue = (DWORD)RxAvailable();
        stat->cbOutQue = 0;
    }
    LeaveCriticalSection(&g.lock);
    return TRUE;
}

BOOL GetCommModemStatus(HANDLE h, LPDWORD msr) {
    if (!Owns(h)) return FALSE;
    EnterCriticalSection(&g.lock);
    Service();
    if (msr) *msr = g.cts ? 0x10u : 0u;  // MS_CTS
    const int cts = g.cts ? 1 : 0;
    LeaveCriticalSection(&g.lock);
    log::Write("ioboard: GetCommModemStatus -> CTS=%d (tx=%lu rx=%lu)", cts,
               g.txCount, g.rxCount);
    return TRUE;
}

BOOL EscapeCommFunction(HANDLE h, DWORD func) {
    if (!Owns(h)) return FALSE;
    log::Write("ioboard: EscapeCommFunction(%s)", EscapeName(func));
    return TRUE;
}

BOOL ReadFile(HANDLE h, void* buffer, DWORD toRead, LPDWORD read,
              LPOVERLAPPED overlapped) {
    if (!Owns(h)) return FALSE;
    if (overlapped) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    EnterCriticalSection(&g.lock);
    Service();
    size_t avail = RxAvailable();
    DWORD n = (DWORD)(avail < toRead ? avail : toRead);
    for (DWORD i = 0; i < n; ++i) {
        ((unsigned char*)buffer)[i] = g.rx[g.rxTail];
        g.rxTail = (g.rxTail + 1) % kRxCapacity;
    }
    g.rxCount++;
    LeaveCriticalSection(&g.lock);

    if (read) *read = n;
    if (n) log::WriteHex("RX", (const unsigned char*)buffer, n);
    return TRUE;
}

BOOL WriteFile(HANDLE h, const void* buffer, DWORD toWrite, LPDWORD written,
               LPOVERLAPPED overlapped) {
    if (!Owns(h)) return FALSE;
    if (overlapped) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    log::WriteHex("TX", (const unsigned char*)buffer, toWrite);

    EnterCriticalSection(&g.lock);
    g.txCount++;

    // Packet layout is  E0 <b1> <b2> <cmd> <param...> <checksum>, so the command
    // byte lands at index 3.
    const unsigned char cmd = (toWrite > 3) ? ((const unsigned char*)buffer)[3] : 0;

    // CTS modelling.
    //
    // A real board drops CTS while it handles a command and raises it again as
    // soon as the reply is queued - at 115200 baud that is a couple of
    // milliseconds, not seconds. Three cases matter to the game:
    //
    //   0xF0  The game re-sends the reset until CTS goes LOW, then waits for it
    //         to come back, so it has to drop here and rise after busyMs.
    //   0xF1  One round per channel the board reports. The game's handshake loop
    //         ends when CTS rises, so it must stay low for the first
    //         roundsWanted-1 rounds and go high on the last one.
    //   other Per-channel reads (0x10-0x14) and input polling. The command has
    //         already been answered, so CTS stays asserted. Dropping it here
    //         made the game read a board failure at 0x520087 and give up.
    if (cmd == 0xF0) {
        g.cts = false;
        g.readyAt = GetTickCount() + g_busyMs;
    } else if (cmd == 0xF1) {
        g.roundsSeen++;
        if (g.roundsSeen >= g.roundsWanted) {
            g.cts = true;
        } else {
            g.cts = false;
            g.readyAt = GetTickCount() + g_busyMs;
        }
    } else {
        g.cts = true;
    }

    // The game does not always read every reply (the 0xF0 init command is
    // answered but never drained), so a real board would discard the stale
    // frame when the next command arrives. Drop anything left over, otherwise
    // two packets end up in one read and the parser sees a corrupt stream.
    g.rxHead = 0;
    g.rxTail = 0;

    if (g.echoMode) {
        RxPush((const unsigned char*)buffer, toWrite);
    } else {
        // Answer each JVS command with the reply that command actually calls
        // for. The board's identification string alone is 52 bytes, so a single
        // canned packet can never satisfy the 0x10-0x14 exchange.
        if (BuildCommandReply((const unsigned char*)buffer, toWrite)) {
            RxPush(g.reply, (DWORD)g.replyLen);
        } else {
            log::Write("ioboard: command 0x%02X is unanswered (no reply queued)",
                       ((const unsigned char*)buffer)[3]);
        }
    }
    LeaveCriticalSection(&g.lock);

    if (written) *written = toWrite;
    return TRUE;
}

BOOL CloseHandle(HANDLE h) {
    if (!Owns(h)) return FALSE;
    EnterCriticalSection(&g.lock);
    if (g.port != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g.port);
        g.port = INVALID_HANDLE_VALUE;
    }
    log::Write("ioboard: CloseHandle(COM2)");
    LeaveCriticalSection(&g.lock);
    return TRUE;
}

}  // namespace ioboard