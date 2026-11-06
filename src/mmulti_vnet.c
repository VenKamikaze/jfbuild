// mmulti over a vnet relay (see include/mmulti_vnet.h).
//
// mmulti.c's four socket users (netinit, netsend, netread, netuninit) hand
// over to the functions here when a relay was asked for with -relay. Peer
// addresses stay AF_INET sockaddrs inside mmulti.c's sockaddr_storage arrays:
// the relay's virtual address 10.0.0.N and the port, so mmulti's address
// comparison, printing and bookkeeping are unchanged.
//
// vnet is built into the engine library from ../ext/vnet (see the Makefile);
// without it (USE_VNET not defined) every function here is a stub and -relay
// is refused.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat.h"
#include "build.h"
#include "mmulti.h"
#include "mmulti_vnet.h"

#define printf buildprintf

#define DEFAULT_RELAY_PORT 7777
#define JOIN_TIMEOUT_MS    15000   // a joiner that never hears from the master
#define HOSTLOST_TIMEOUT_MS 10000  // a joiner whose master stops answering

static int playercap;        // set by the game

#ifndef USE_VNET

static int stubfailed;
static char stubmsg[120];

static int stubfail(void)
{
	stubfailed = 1;
	snprintf(stubmsg, sizeof(stubmsg), "this build has no relay support (vnet was not built in)");
	printf("mmulti error: %s\n", stubmsg);
	return -1;
}

int mmulti_vnet_state(void) { return stubfailed ? MMULTI_VNET_FAILED : MMULTI_VNET_OFF; }
const char *mmulti_vnet_roomcode(void) { return ""; }
int mmulti_vnet_error(void) { return stubfailed ? -2 : 0; }
const char *mmulti_vnet_errormsg(void) { return stubfailed ? stubmsg : ""; }
void mmulti_vnet_setplayercap(int cap) { playercap = cap; }
int mmulti_vnet_hostplayers(void) { return 0; }
int mmulti_vnet_wanted(void) { return 0; }
int mmulti_vnet_active(void) { return 0; }
void mmulti_vnet_reset(void) { stubfailed = 0; }
int mmulti_vnet_optlen(int argc, char const * const argv[], int i)
{
	(void)argc;
	if (argv[i][0] != '-' && argv[i][0] != '/') return 0;
	if (!Bstrcasecmp(argv[i]+1, "relay") || !Bstrcasecmp(argv[i]+1, "room") ||
			!Bstrcasecmp(argv[i]+1, "host-room")) return 2;
	return 0;
}
int mmulti_vnet_setopt(int argc, char const * const argv[], int i)
{
	(void)argc; (void)argv; (void)i;
	return stubfail();
}
int mmulti_vnet_checkopts(void) { return 0; }
int mmulti_vnet_open(int port) { (void)port; return 0; }
void mmulti_vnet_close(void) {}
void mmulti_vnet_hostaddr(struct sockaddr_storage *out, int port) { (void)port; memset(out, 0, sizeof(*out)); }
int mmulti_vnet_send(const struct sockaddr_storage *to, const void *buf, int len)
	{ (void)to; (void)buf; (void)len; return 0; }
int mmulti_vnet_recv(void *buf, int cap, struct sockaddr_storage *from)
	{ (void)buf; (void)cap; (void)from; return 0; }
int mmulti_vnet_cycle(int isslave, int silent_ms)
	{ (void)isslave; (void)silent_ms; return MMULTI_VNET_OFF; }

#else

#include "vnet.h"

static int hostplayers;      // -host-room N, after clamping
static int wantedflag;
static char optrelay[256];   // the relay address as given
static char optroom[16];     // -room CODE (upper-cased); "" when hosting
static int opthost;          // -host-room N, as given
static int sawrelay, sawroom, sawhost;

static vnet *ctx;
static int sock = -1;
static int sockport;
static int state = MMULTI_VNET_OFF;
static int errcode;
static char errmsg[240];
static char roomcode[8];
static unsigned long long readyat;   // vnet_time() at READY
static int atexitset;

#include <stdarg.h>

// An argument or start-up error: say so, and remember it (state FAILED) so
// that the game can tell a bad -relay start from a plain -net failure.
static int optfail(const char *fmt, ...)
{
	char msg[200];
	va_list va;

	va_start(va, fmt);
	vsnprintf(msg, sizeof(msg), fmt, va);
	va_end(va);
	printf("mmulti error: %s\n", msg);
	state = MMULTI_VNET_FAILED;
	errcode = VNET_EINVAL;
	snprintf(errmsg, sizeof(errmsg), "%s", msg);
	return -1;
}

static int streq_ci(const char *a, const char *b)
{
	return Bstrcasecmp(a, b) == 0;
}

static int isoptname(const char *arg, const char *name)
{
	if (arg[0] != '-' && arg[0] != '/') return 0;
	return streq_ci(arg+1, name);
}

int mmulti_vnet_state(void) { return state; }
const char *mmulti_vnet_roomcode(void) { return roomcode; }
int mmulti_vnet_error(void) { return errcode; }
const char *mmulti_vnet_errormsg(void) { return errmsg; }
void mmulti_vnet_setplayercap(int cap) { playercap = cap; }
int mmulti_vnet_hostplayers(void) { return hostplayers; }
int mmulti_vnet_wanted(void) { return wantedflag; }
int mmulti_vnet_active(void) { return ctx != NULL; }

void mmulti_vnet_reset(void)
{
	mmulti_vnet_close();
	optrelay[0] = optroom[0] = 0;
	opthost = hostplayers = 0;
	sawrelay = sawroom = sawhost = 0;
	wantedflag = 0;
	state = MMULTI_VNET_OFF;
	errcode = 0;
	errmsg[0] = 0;
	roomcode[0] = 0;
}

int mmulti_vnet_optlen(int argc, char const * const argv[], int i)
{
	(void)argc;
	if (isoptname(argv[i], "relay") || isoptname(argv[i], "room") || isoptname(argv[i], "host-room"))
		return 2;
	return 0;
}

int mmulti_vnet_setopt(int argc, char const * const argv[], int i)
{
	const char *val;
	int n;

	if (i + 1 >= argc || argv[i+1][0] == '-' || argv[i+1][0] == '/') {
		return optfail("%s needs a value", argv[i]);
	}
	val = argv[i+1];

	if (isoptname(argv[i], "relay")) {
		if (!strncmp(val, "udp://", 6)) val += 6;
		if (strstr(val, "://")) {
			return optfail("-relay wants the relay's UDP address (HOST:PORT), not the URL %s: "
				"ws:// and wss:// are for the browser build", val);
		}
		if (strlen(val) >= sizeof(optrelay)) return optfail("-relay value too long");
		strcpy(optrelay, val);
		sawrelay = 1;
	} else if (isoptname(argv[i], "room")) {
		if (strlen(val) != 6) {
			return optfail("a room code is 6 letters and digits, not \"%s\"", val);
		}
		for (n = 0; n < 6; n++) {
			char c = val[n];
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '2' && c <= '9'))) {
				return optfail("\"%s\" is not a room code (6 letters and digits, no 0, 1, I or O)", val);
			}
			optroom[n] = (char)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
		}
		optroom[6] = 0;
		sawroom = 1;
	} else {
		char *p;
		long l = strtol(val, &p, 10);
		if (*p || l < 2 || l > MAXMULTIPLAYERS) {
			return optfail("-host-room needs a number of players from 2 to %d, not \"%s\"",
				MAXMULTIPLAYERS, val);
		}
		opthost = (int)l;
		sawhost = 1;
	}
	return 0;
}

int mmulti_vnet_checkopts(void)
{
	if (!sawrelay && !sawroom && !sawhost) { wantedflag = 0; return 0; }
	if (!sawrelay) {
		return optfail("-room and -host-room need -relay HOST:PORT");
	}
	if (sawroom == sawhost) {
		return optfail("-relay needs exactly one of -host-room N (create a room) or -room CODE (join one)");
	}
	wantedflag = 1;
	hostplayers = sawhost ? opthost : 0;
	return 0;
}

static void vlog(void *user, int level, const char *msg)
{
	(void)user;
	if (level <= VNET_LOG_WARN) printf("vnet: %s\n", msg);
}

static void fail(int code, const char *what)
{
	state = MMULTI_VNET_FAILED;
	errcode = code;
	snprintf(errmsg, sizeof(errmsg), "%s (%s)", what, vnet_strerror(code));
}

static const char *describe(int code)
{
	switch (code) {
		case VNET_ENOROOM:      return "there is no room with that code";
		case VNET_EROOMFULL:    return "the room is full";
		case VNET_ECONNREFUSED: return "the relay could not be reached";
		case VNET_ETIMEDOUT:    return "the relay did not answer";
		case VNET_ECLOSED:      return "the relay ended the session (room closed, timed out, or the relay restarted)";
		case VNET_ELIMIT:       return "the relay refused: a limit was reached";
		case VNET_EPROTO:       return "the relay speaks another protocol version";
		default:                return "the relay connection failed";
	}
}

static void atexit_close(void) { mmulti_vnet_close(); }

int mmulti_vnet_open(int port)
{
	vnet_config cfg;
	vnet_addr relay;
	char hostport[300], numeric[128];
	int err = 0, rc;
	const char *colon;

	if (!wantedflag) return 0;

	// The relay's address: numeric, so that vnet_create does not block.
	colon = strrchr(optrelay, ':');
	if (!colon || strchr(colon, ']'))
		snprintf(hostport, sizeof(hostport), "%s:%d", optrelay, DEFAULT_RELAY_PORT);
	else
		snprintf(hostport, sizeof(hostport), "%s", optrelay);
	rc = vnet_resolve_host(hostport, VNET_FAMILY_ANY, &relay);
	if (rc < 0 || vnet_addr_to_string(&relay, numeric, sizeof(numeric)) < 0) {
		optfail("cannot resolve the relay address %s (%s)", hostport, vnet_strerror(rc < 0 ? rc : VNET_EINVAL));
		return 0;
	}

	vnet_config_init(&cfg);
	cfg.backend = VNET_BACKEND_RELAY_AUTO;
	cfg.relay_udp = numeric;
	cfg.room = optroom[0] ? optroom : NULL;
	cfg.relay_timeout_ms = 10000;
	cfg.keepalive_ms = 1000;
	cfg.log = vlog;

	ctx = vnet_create(&cfg, &err);
	if (!ctx) {
		optfail("cannot start the relay client: %s", vnet_strerror(err));
		return 0;
	}
	sock = vnet_open(ctx, (uint16_t)port);
	if (sock < 0) {
		optfail("cannot open port %d in the room: %s", port, vnet_strerror(sock));
		vnet_destroy(ctx); ctx = NULL;
		return 0;
	}
	sockport = port;
	state = MMULTI_VNET_CONNECTING;
	roomcode[0] = 0;
	if (!atexitset) { atexit(atexit_close); atexitset = 1; }

	if (optroom[0])
		printf("mmulti: joining room %s on relay %s (vnet %s)\n", optroom, numeric, vnet_version());
	else
		printf("mmulti: creating a room on relay %s (vnet %s)\n", numeric, vnet_version());
	return 1;
}

void mmulti_vnet_close(void)
{
	if (ctx) {
		vnet_close(ctx, sock);
		vnet_destroy(ctx);
	}
	ctx = NULL;
	sock = -1;
}

void mmulti_vnet_hostaddr(struct sockaddr_storage *out, int port)
{
	struct sockaddr_in *a = (struct sockaddr_in *)out;
	memset(out, 0, sizeof(*out));
	a->sin_family = AF_INET;
	a->sin_port = htons((unsigned short)port);
	a->sin_addr.s_addr = htonl(0x0a000001u);	// 10.0.0.1, the room creator
}

// Notice a change of the relay session's state.
static void pump(void)
{
	vnet_status_info st;
	int s;

	if (!ctx) return;
	vnet_poll(ctx);
	if (state == MMULTI_VNET_FAILED) return;

	s = vnet_status(ctx, &st);
	if (s == VNET_STATUS_FAILED) {
		int was = state;
		state = MMULTI_VNET_FAILED;
		errcode = st.error;
		snprintf(errmsg, sizeof(errmsg), "%s (%s)", describe(st.error), vnet_strerror(st.error));
		if (was == MMULTI_VNET_READY)
			printf("mmulti: the relay session was lost: %s\n", errmsg);
		else
			printf("mmulti error: could not %s: %s\n", optroom[0] ? "join the room" : "create the room", errmsg);
	} else if (s == VNET_STATUS_READY && state == MMULTI_VNET_CONNECTING) {
		state = MMULTI_VNET_READY;
		readyat = vnet_time(ctx);
		strncpy(roomcode, st.room, sizeof(roomcode)-1);
		roomcode[sizeof(roomcode)-1] = 0;
		if (optroom[0])
			printf("mmulti: joined room %s as 10.0.0.%d (%s)\n", roomcode, st.self.bytes[3], st.transport);
		else
			printf("mmulti: room code %s (you are 10.0.0.%d, %s; the room takes %d players)\n",
				roomcode, st.self.bytes[3], st.transport, st.max_members);
		fflush(stdout);	// a launcher reading our output wants the code now
	}
}

// Apply the player cap (the game's and the relay room's) to a -host-room N.
static void clampplayers(void)
{
	vnet_status_info st;
	int n = hostplayers, i, limit;

	if (!hostplayers || !ctx) return;
	vnet_status(ctx, &st);
	limit = playercap;
	if (limit > 0 && n > limit) {
		printf("mmulti: this game allows at most %d players; hosting a %d-player game\n", limit, limit);
		n = limit;
	}
	if (st.max_members > 0 && n > st.max_members) {
		printf("mmulti: this relay takes at most %d players per room; hosting a %d-player game\n",
			st.max_members, st.max_members);
		n = st.max_members;
	}
	if (n != hostplayers) {
		hostplayers = n;
		numplayers = n;
		for (i = 0; i < n-1; i++) connectpoint2[i] = i+1;
		connectpoint2[n-1] = -1;
	}
}

int mmulti_vnet_cycle(int isslave, int silent_ms)
{
	int before = state;

	pump();
	if (state == MMULTI_VNET_READY && before == MMULTI_VNET_CONNECTING && !isslave)
		clampplayers();
	if (state == MMULTI_VNET_READY && isslave) {
		unsigned long long waited = vnet_time(ctx) - readyat;
		if (silent_ms < 0 && waited > JOIN_TIMEOUT_MS) {
			fail(VNET_ETIMEDOUT, "the host did not answer: the room may be full or the host has left");
			printf("mmulti error: %s\n", errmsg);
		} else if (silent_ms > HOSTLOST_TIMEOUT_MS) {
			fail(VNET_ETIMEDOUT, "the host stopped answering");
			printf("mmulti error: %s\n", errmsg);
		}
	}
	return state;
}

int mmulti_vnet_send(const struct sockaddr_storage *to, const void *buf, int len)
{
	const struct sockaddr_in *a = (const struct sockaddr_in *)to;
	vnet_addr dst;

	if (!ctx || a->sin_family != AF_INET) return 0;

	memset(&dst, 0, sizeof(dst));
	dst.family = VNET_AF_IPV4;
	memcpy(dst.bytes, &a->sin_addr, 4);
	dst.port = ntohs(a->sin_port);

	pump();
	if (state != MMULTI_VNET_READY) return 0;
	return vnet_sendto(ctx, sock, buf, (size_t)len, &dst) >= 0;
}

int mmulti_vnet_recv(void *buf, int cap, struct sockaddr_storage *from)
{
	vnet_addr src;
	struct sockaddr_in *a = (struct sockaddr_in *)from;
	int n;

	if (!ctx) return 0;
	pump();
	if (state != MMULTI_VNET_READY) return 0;

	for (;;) {
		n = vnet_recvfrom(ctx, sock, buf, (size_t)cap, &src);
		if (n == VNET_EMSGSIZE) continue;	// longer than any mmulti packet: dropped
		if (n < 0) return 0;			// VNET_EAGAIN: nothing waiting
		if (n == 0) continue;
		if (src.family != VNET_AF_IPV4 || src.bytes[0] != 10 || src.bytes[3] >= 254) continue;
		break;
	}
	memset(from, 0, sizeof(*from));
	a->sin_family = AF_INET;
	a->sin_port = htons(src.port);
	memcpy(&a->sin_addr, src.bytes, 4);
	return n;
}

#endif
