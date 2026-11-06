// mmulti over a vnet relay: the interface that is not part of mmulti.h.
//
// mmulti.h is unchanged (a vnet transport sits behind it, chosen at run time
// by the -net arguments below). This header is for the code around mmulti:
// the game (src/game.c) and later the browser lobby.
//
// Arguments, given among the -net arguments (they are read by
// initmultiplayersparms, so  sw -net -relay HOST:PORT -host-room 3 -- ...):
//
//   -relay HOST:PORT    the relay's UDP address (default port 7777). A numeric
//                       address never blocks; a host name is looked up once.
//                       "udp://HOST:PORT" is accepted too. A ws:// or wss://
//                       URL is the browser's form and is refused natively.
//   -host-room N        create a room and be the master (player 0, 10.0.0.1)
//                       of an N player game. N is 2 up to what the game
//                       allows (4 on the shareware GRP, 8 on registered) and
//                       what the relay allows per room; a larger N is clamped
//                       with a message once those limits are known.
//   -room CODE          join the room CODE as a slave, taking an open slot as
//                       a newcomer does on plain UDP.
//   -p PORT             (as before) the port inside the room, default 0x5bd9.
//                       Everyone in a room must use the same one.
//
// Impairment for tests comes from vnet itself: the VNET_IMPAIR environment
// variable ("lat=40,jit=10,loss=5,dup=2,reorder=3,seed=7"; give every process
// its own seed). It is read at start up and affects what a process sends.
//
// Without -relay nothing here is used and mmulti behaves exactly as before.

#ifndef MMULTI_VNET_H_
#define MMULTI_VNET_H_

#ifdef __cplusplus
extern "C" {
#endif

struct sockaddr_storage;

// State of the relay connection, as returned by mmulti_vnet_state().
#define MMULTI_VNET_OFF         0   // not using a relay (plain UDP, or not started)
#define MMULTI_VNET_CONNECTING  1   // connecting to the relay / entering the room
#define MMULTI_VNET_READY       2   // in the room
#define MMULTI_VNET_FAILED      3   // failed: see mmulti_vnet_error*()

// ---- For the game and the lobby ----------------------------------------

// One of MMULTI_VNET_*. Once FAILED it stays FAILED (until the next
// initmultiplayersparms). The game's start-up wait ends when this is FAILED;
// src/game.c InitGame then exits with status 1.
int mmulti_vnet_state(void);

// The 6 character room code once the room has been entered (READY), else "".
// A host prints it (stdout and the on-screen console) as
// "mmulti: room code ABC234" the moment it is known.
const char *mmulti_vnet_roomcode(void);

// Why the connection failed: a VNET_E* code (negative, 0 when none) and a
// sentence for the player ("the relay has no room with that code").
int mmulti_vnet_error(void);
const char *mmulti_vnet_errormsg(void);

// The most players the game can have, set by the game before the start-up
// wait (src/game.c InitGame: MAX_SW_PLAYERS, 4 on shareware, 8 registered).
// A -host-room N larger than this (or than the relay's room size) is clamped,
// with a message. 0 means no limit but mmulti's own (16).
void mmulti_vnet_setplayercap(int cap);

// The number of players the host asked for with -host-room (0 for a joiner
// or without -relay), and how many were left after clamping.
int mmulti_vnet_hostplayers(void);

// ---- For mmulti.c only --------------------------------------------------

// Is a relay session wanted / in use? (Wanted from the arguments on, active
// from a successful mmulti_vnet_open until mmulti_vnet_close.)
int mmulti_vnet_wanted(void);
int mmulti_vnet_active(void);

// Reset the option state; called at the start of initmultiplayersparms.
void mmulti_vnet_reset(void);

// How many arguments, starting at argv[i], belong to a vnet option (-relay X,
// -host-room N, -room CODE), 0 if argv[i] is not one.
int mmulti_vnet_optlen(int argc, char const * const argv[], int i);

// Record the option at argv[i]. Returns 0, or -1 after printing why not.
int mmulti_vnet_setopt(int argc, char const * const argv[], int i);

// Check the options as a set (once they are all read). 0 or -1 (printed).
int mmulti_vnet_checkopts(void);

// Create the relay context and open the game socket on PORT. 1 or 0.
int mmulti_vnet_open(int port);
void mmulti_vnet_close(void);

// The address of the room creator, 10.0.0.1:PORT, as an AF_INET sockaddr.
void mmulti_vnet_hostaddr(struct sockaddr_storage *out, int port);

// Datagram I/O in the shape of mmulti.c's netsend/netread. Peer addresses are
// AF_INET sockaddrs holding 10.0.0.N and the port. Both pump the relay.
int mmulti_vnet_send(const struct sockaddr_storage *to, const void *buf, int len);
// Returns the length (> 0) and fills *from, or 0 when nothing is waiting.
int mmulti_vnet_recv(void *buf, int cap, struct sockaddr_storage *from);

// One step of the start-up wait (initmultiplayerscycle): pump the relay, note
// the state change, apply the player cap. SILENT_MS is how long ago the master
// last answered a slave's ping (-1: never; ignored for the master).
// Returns the state; the caller keeps waiting while CONNECTING and stops when
// FAILED (a slave also fails here when its host stays silent too long).
int mmulti_vnet_cycle(int isslave, int silent_ms);

#ifdef __cplusplus
}
#endif

#endif
