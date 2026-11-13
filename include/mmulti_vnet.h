// mmulti over a vnet relay: the interface that is not part of mmulti.h.
//
// mmulti.h is unchanged (a vnet transport sits behind it, chosen at run time
// by the -net arguments below). This header is for the code around mmulti:
// the game (src/game.c, src/netlobby.c) and the browser lobby.
//
// Arguments, given among the -net arguments (they are read by
// initmultiplayersparms, so  sw -net -relay HOST:PORT -host-room 3 -- ...):
//
//   -relay HOST:PORT    the relay's UDP address (default port 7777). A numeric
//                       address never blocks; a host name is looked up once.
//                       "udp://HOST:PORT" is accepted too. A ws:// or wss://
//                       URL is the browser's form and is refused natively.
//   -host-room N        create a room and be the master (player 0, 10.0.0.1)
//                       of an N player game. N is 2 to 8; the game allows 4
//                       on the shareware GRP and 8 on registered, and the
//                       relay has a room size of its own: a larger N is
//                       clamped with a message once those limits are known.
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

// ---- The lobby: the start-up wait of a relay game (brief 05) ------------
// Defined in mmulti.c. All of them do nothing (or return 0) without a relay.

// Players in the room so far, this machine included. On the master: itself and
// every slave that has checked in and not left (a place whose player has been
// silent for 3 s during the wait is freed, and the later players move down a
// place, so the places in use are always the first ones). On a slave: the
// count in the master's last answer to its ping (0 until the first answer).
int mmulti_vnet_lobbyplayers(void);
// The places in the game being waited for (the master's numplayers; on a slave,
// the count in the master's last answer). After a "start now" it is the final
// player count, which every machine learns before the game starts.
int mmulti_vnet_lobbyslots(void);
// 1 once this machine's wait is over (the master gave the go-ahead).
int mmulti_vnet_lobbystarted(void);

// Master, during the wait: start with the players who are in (at least 2)
// instead of waiting for every place ("start now"). The request stays until it
// can be met: every player who is in has answered within the last 2 s. The
// game then has that many places; the slaves learn the count from the same
// answer to their ping that gives them the go-ahead, so every machine starts
// with the same count and the same player numbers. A newcomer after that is
// told the game has already started. Returns 1 if at least 2 are in now.
int mmulti_vnet_startnow(void);

// Leaving during the wait (Cancel): the master tells every slave the room is
// closed; a slave tells the master it is leaving (its place is freed at once).
// The caller then closes the session (uninitmultiplayers or exit).
void mmulti_vnet_leave(void);

// How long ago this machine last heard from player i, in ms (-1: never).
// In master/slave mode a slave hears only from the master.
int mmulti_vnet_silentms(int player);
// Master, during the game: tell every other slave that player i has left
// (its machine stopped answering); they end their game with that reason.
void mmulti_vnet_tellleft(int player);

// The notices (an 0xac start-up packet: what, arg), relay games only.
#define MMULTI_VNET_NOTE_FULL     1   // master to a newcomer: every place is taken
#define MMULTI_VNET_NOTE_STARTED  2   // master to a newcomer: the game has already started
#define MMULTI_VNET_NOTE_CLOSED   3   // master to the slaves: the host left the room (wait)
#define MMULTI_VNET_NOTE_LEAVING  4   // slave to the master: I am leaving the room (wait)
#define MMULTI_VNET_NOTE_LEFT     5   // master to the slaves: player arg left the game

// ---- For mmulti.c only --------------------------------------------------

// A notice from the master reached this slave: fail with its reason
// (mmulti_vnet_errormsg()). Defined in mmulti_vnet.c.
void mmulti_vnet_notice(int what, int arg);

// Is a relay session wanted / in use? (Wanted from the arguments on, active
// from a successful mmulti_vnet_open until mmulti_vnet_close.)
int mmulti_vnet_wanted(void);
int mmulti_vnet_active(void);

// Report an error in how a relay game was asked for: prints "mmulti error: ...",
// sets the FAILED state (so that the game exits 1 instead of playing single
// player) and returns -1.
int mmulti_vnet_fail(const char *fmt, ...);

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
// Close the relay session (the state goes back to OFF unless it FAILED).
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

// Has mmulti queued data (sendpacket) that its send-rate limit (one datagram per peer every
// 25 ms, PAKRATE) has not let out yet? (The browser's hidden-tab test counts yields entered
// with data held back.) Defined in mmulti.c.
int mmulti_vnet_sendpending(void);
// Send it now, ignoring that limit; returns how many peers. Not specific to vnet: it works for
// plain UDP peers too. The game calls it right after it hands mmulti a new input or the master's
// combined packet (brief 04b; brief 04 called it once per hidden-tab pass), so an input does not
// wait for the next window. Defined in mmulti.c.
int mmulti_vnet_flush(void);

// Test-only (brief 04b, the game's -testlag): called when sendpacket() queues a packet (ocnt is its
// index in the peer's output queue) and when a datagram to a peer goes out (ocnt1: queue index one
// past the newest packet, nbytes: the datagram's size). NULL unless the game sets them; they only
// observe. Defined in mmulti.c.
extern void (*mmulti_test_queuehook)(int other, int ocnt);
extern void (*mmulti_test_sendhook)(int other, int ocnt1, int nbytes);

#ifdef __cplusplus
}
#endif

#endif
