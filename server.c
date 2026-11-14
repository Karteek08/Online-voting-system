/*
 * NIT Silchar Gymkhana Online Voting System - C web backend (OOP style)
 *
 * Serves the frontend files from the same folder as the server executable
 * and exposes a small JSON API.
 * No external libraries: plain sockets (works on Windows, Linux, macOS).
 *
 * Build (Linux / macOS):  gcc server.c -o server
 * Build (Windows MinGW):  gcc server.c -o server.exe -lws2_32
 * Run from THIS folder :  ./server      then open  http://localhost:8080
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

#ifdef _WIN32
  #define _WIN32_WINNT 0x0601
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define CLOSESOCK closesocket
  #define BAD_SOCK INVALID_SOCKET
  #define MSG_NOSIGNAL 0
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <signal.h>
  typedef int sock_t;
  #define CLOSESOCK close
  #define BAD_SOCK (-1)
#endif

/* ================================================================ */
/* Constants                                                        */
/* ================================================================ */
#define PORT            8080
#define MAX_REQ         65536
#define MAX_VOTERS      500
#define MAX_CANDIDATES  50
#define MAX_BALLOTS     5000
#define MAX_SESSIONS    300
#define MAX_PENDING     100
#define OTP_TTL_SECONDS 300
#define ADMIN_PASSWORD  "admin123"

typedef enum { POST_GS, POST_VP, POST_GSC, POST_GST, POST_GSS, NUM_POSTS } Post;
static const char *POST_CODE[NUM_POSTS] = { "GS", "VP", "GSC", "GST", "GSS" };
static const char *POST_NAME[NUM_POSTS] = {
    "Gymkhana Secretary (GS)", "Vice President (VP)",
    "Gymkhana Secretary of Culture (GSC)",
    "Gymkhana Secretary of Technical (GST)",
    "Gymkhana Secretary of Sports (GSS)"
};
typedef enum { ELECTION_NOT_STARTED, ELECTION_ACTIVE, ELECTION_CLOSED } ElectionStatus;
static const char *STATUS_NAME[] = { "NOT_STARTED", "ACTIVE", "CLOSED" };

/* ================================================================ */
/* Growable string buffer (for building JSON)                       */
/* ================================================================ */
typedef struct { char *d; size_t len, cap; } Buf;

static void buf_init(Buf *b) { b->cap = 1024; b->len = 0; b->d = malloc(b->cap); b->d[0] = '\0'; }
static void buf_free(Buf *b) { free(b->d); b->d = NULL; }
static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        while (b->len + extra + 1 > b->cap) b->cap *= 2;
        b->d = realloc(b->d, b->cap);
    }
}
static void buf_addf(Buf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    buf_reserve(b, (size_t)n);
    vsnprintf(b->d + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}
static void buf_add_json_str(Buf *b, const char *s) {
    buf_addf(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"')       buf_addf(b, "\\\"");
        else if (c == '\\') buf_addf(b, "\\\\");
        else if (c == '\n') buf_addf(b, "\\n");
        else if (c == '\r') buf_addf(b, "\\r");
        else if (c == '\t') buf_addf(b, "\\t");
        else if (c < 0x20)  buf_addf(b, "\\u%04x", c);
        else                buf_addf(b, "%c", c);
    }
    buf_addf(b, "\"");
}

/* ================================================================ */
/* Scholar ID + email rules                                         */
/*   ID = YY U B SSS  (e.g. 2412058)                                */
/*   U=1 (UG), B: 1 civil, 2 cse, 3 ee, 4 ece, 5 eie, 6 mech        */
/* ================================================================ */
typedef struct { int year; int serial; char branch[8]; } ScholarInfo;

static int parse_scholar_id(const char *id, ScholarInfo *out) {
    static const char *BRANCH[7] = { "", "civil", "cse", "ee", "ece", "eie", "mech" };
    if (strlen(id) != 7) return 0;
    for (int i = 0; i < 7; i++) if (!isdigit((unsigned char)id[i])) return 0;
    int year = (id[0] - '0') * 10 + (id[1] - '0');
    int ug = id[2] - '0', br = id[3] - '0';
    int ser = (id[4] - '0') * 100 + (id[5] - '0') * 10 + (id[6] - '0');
    if (ug != 1 || br < 1 || br > 6 || ser < 1) return 0;
    out->year = year; out->serial = ser;
    strcpy(out->branch, BRANCH[br]);
    return 1;
}

static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* Email: firstname_ug_YY@branch.nits.ac.in */
static int validate_email(const char *email, const char *first, const ScholarInfo *si) {
    char fn[64], given[128], expected[160];
    snprintf(fn, sizeof fn, "%s", first); lower(fn);
    snprintf(given, sizeof given, "%s", email); lower(given);
    snprintf(expected, sizeof expected, "%s_ug_%02d@%s.nits.ac.in", fn, si->year, si->branch);
    return strcmp(expected, given) == 0;
}

/* ================================================================ */
/* Class: User (base)                                               */
/* ================================================================ */
typedef struct User User;
struct User {
    int  user_id;
    char name[64];
    char email[128];
    char password_[32];              /* private: only via user_check_password */
    int  logged_in;
    void (*login)(User *self);
    void (*logout)(User *self);
};
static void user_login(User *u)  { u->logged_in = 1; }
static void user_logout(User *u) { u->logged_in = 0; }
static int  user_check_password(const User *u, const char *pw) { return strcmp(u->password_, pw) == 0; }
static void user_init(User *u, int id, const char *name, const char *email, const char *pw) {
    u->user_id = id;
    snprintf(u->name, sizeof u->name, "%s", name);
    snprintf(u->email, sizeof u->email, "%s", email);
    snprintf(u->password_, sizeof u->password_, "%s", pw);
    u->logged_in = 0; u->login = user_login; u->logout = user_logout;
}

/* ================================================================ */
/* Class: Candidate (inherits User)                                 */
/* ================================================================ */
typedef struct Candidate Candidate;
struct Candidate {
    User base;
    int  candidate_id;
    Post running_post;
    char manifesto[160];
    int  vote_count_;                      /* private */
    const ElectionStatus *status;
    void (*increment_vote)(Candidate *self);
    int  (*get_vote_count)(const Candidate *self);   /* only after election closed */
};
static void candidate_increment_vote(Candidate *c) { c->vote_count_++; }
static int  candidate_get_vote_count(const Candidate *c) {
    return (*c->status == ELECTION_CLOSED) ? c->vote_count_ : -1;
}
static Candidate *candidate_new(int id, const char *name, Post post, const char *manifesto,
                                const ElectionStatus *st) {
    Candidate *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    user_init(&c->base, 9000 + id, name, "", "");
    c->candidate_id = id; c->running_post = post; c->status = st;
    snprintf(c->manifesto, sizeof c->manifesto, "%s", manifesto);
    c->increment_vote = candidate_increment_vote;
    c->get_vote_count = candidate_get_vote_count;
    return c;
}

/* ================================================================ */
/* Class: Ballot (anonymous: no voter identity stored)              */
/* ================================================================ */
typedef struct { int vote_id; Post post; int candidate_id; time_t timestamp; } Ballot;

/* ================================================================ */
/* Class: Voter (inherits User)                                     */
/* ================================================================ */
typedef struct VotingSystemManager VotingSystemManager;
typedef struct Voter Voter;
struct Voter {
    User base;
    char scholar_id[8];
    char department[8];
    int  has_voted[NUM_POSTS];
    VotingSystemManager *mgr;
    int (*cast_vote)(Voter *self, int candidate_id, Post post, char *msg, size_t n);
};

/* OTP waiting to be verified */
typedef struct {
    int used; char scholar_id[8]; char first[32], second[32], email[128];
    int otp; int attempts; time_t expires;
} PendingLogin;

/* ================================================================ */
/* Class: VotingSystemManager                                       */
/* ================================================================ */
struct VotingSystemManager {
    Voter     *voters_list[MAX_VOTERS];   int voter_count;
    Candidate *candidates_list[MAX_CANDIDATES]; int candidate_count;
    Ballot     ballots[MAX_BALLOTS];      int ballot_count;
    PendingLogin pending[MAX_PENDING];
    ElectionStatus election_status;

    int    (*authenticate_student)(VotingSystemManager *m, const char *first, const char *second,
                                   const char *sid, const char *email, int *otp_out,
                                   char *msg, size_t n);
    Voter *(*verify_otp)(VotingSystemManager *m, const char *sid, int otp, char *msg, size_t n);
    int    (*process_vote)(VotingSystemManager *m, Voter *v, int cid, Post post, char *msg, size_t n);
    int    (*generate_results)(VotingSystemManager *m, Buf *out, int live);
};

static Candidate *find_candidate(VotingSystemManager *m, int cid, Post post) {
    for (int i = 0; i < m->candidate_count; i++) {
        Candidate *c = m->candidates_list[i];
        if (c->candidate_id == cid && c->running_post == post) return c;
    }
    return NULL;
}
static Voter *find_voter(VotingSystemManager *m, const char *sid) {
    for (int i = 0; i < m->voter_count; i++)
        if (strcmp(m->voters_list[i]->scholar_id, sid) == 0) return m->voters_list[i];
    return NULL;
}

static int voter_cast_vote(Voter *self, int cid, Post post, char *msg, size_t n) {
    return self->mgr->process_vote(self->mgr, self, cid, post, msg, n);
}
static Voter *voter_new(VotingSystemManager *m, const char *first, const char *second,
                        const char *sid, const char *email, const ScholarInfo *si) {
    Voter *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    char full[96];
    snprintf(full, sizeof full, "%s %s", first, second);
    user_init(&v->base, atoi(sid), full, email, "");
    snprintf(v->scholar_id, sizeof v->scholar_id, "%s", sid);
    snprintf(v->department, sizeof v->department, "%s", si->branch);
    v->mgr = m; v->cast_vote = voter_cast_vote;
    return v;
}

static int manager_authenticate_student(VotingSystemManager *m, const char *first,
        const char *second, const char *sid, const char *email, int *otp_out,
        char *msg, size_t n) {
    ScholarInfo si;
    if (!first[0] || !second[0]) { snprintf(msg, n, "First name and second name are required."); return 0; }
    if (!parse_scholar_id(sid, &si)) {
        snprintf(msg, n, "Invalid scholar ID. Use YY + 1 (UG) + branch digit + 3-digit serial, e.g. 2412058.");
        return 0;
    }
    if (!validate_email(email, first, &si)) {
        snprintf(msg, n, "Email must be %s_ug_%02d@%s.nits.ac.in", first, si.year, si.branch);
        return 0;
    }
    PendingLogin *slot = NULL;
    for (int i = 0; i < MAX_PENDING; i++)
        if (m->pending[i].used && strcmp(m->pending[i].scholar_id, sid) == 0) { slot = &m->pending[i]; break; }
    if (!slot)
        for (int i = 0; i < MAX_PENDING; i++)
            if (!m->pending[i].used || m->pending[i].expires < time(NULL)) { slot = &m->pending[i]; break; }
    if (!slot) { snprintf(msg, n, "Server busy, try again shortly."); return 0; }

    memset(slot, 0, sizeof *slot);
    slot->used = 1;
    snprintf(slot->scholar_id, sizeof slot->scholar_id, "%s", sid);
    snprintf(slot->first, sizeof slot->first, "%s", first);
    snprintf(slot->second, sizeof slot->second, "%s", second);
    snprintf(slot->email, sizeof slot->email, "%s", email);
    slot->otp = 100000 + rand() % 900000;
    slot->expires = time(NULL) + OTP_TTL_SECONDS;
    *otp_out = slot->otp;
    snprintf(msg, n, "OTP sent to %s", email);
    return 1;
}

static Voter *manager_verify_otp(VotingSystemManager *m, const char *sid, int otp,
                                 char *msg, size_t n) {
    PendingLogin *p = NULL;
    for (int i = 0; i < MAX_PENDING; i++)
        if (m->pending[i].used && strcmp(m->pending[i].scholar_id, sid) == 0) { p = &m->pending[i]; break; }
    if (!p) { snprintf(msg, n, "Request an OTP first."); return NULL; }
    if (time(NULL) > p->expires) { p->used = 0; snprintf(msg, n, "OTP expired. Request a new one."); return NULL; }
    if (p->otp != otp) {
        if (++p->attempts >= 3) { p->used = 0; snprintf(msg, n, "Too many wrong attempts. Request a new OTP."); }
        else snprintf(msg, n, "Wrong OTP (%d/3).", p->attempts);
        return NULL;
    }
    Voter *v = find_voter(m, sid);
    if (!v) {
        ScholarInfo si; parse_scholar_id(sid, &si);
        if (m->voter_count >= MAX_VOTERS) { snprintf(msg, n, "Voter limit reached."); return NULL; }
        v = voter_new(m, p->first, p->second, sid, p->email, &si);
        if (!v) { snprintf(msg, n, "Out of memory."); return NULL; }
        m->voters_list[m->voter_count++] = v;
    }
    p->used = 0;
    v->base.login(&v->base);
    snprintf(msg, n, "Login successful.");
    return v;
}

static int manager_process_vote(VotingSystemManager *m, Voter *v, int cid, Post post,
                                char *msg, size_t n) {
    if (m->election_status != ELECTION_ACTIVE) { snprintf(msg, n, "Election is not active."); return 0; }
    if (!v->base.logged_in) { snprintf(msg, n, "Please login first."); return 0; }
    if (v->has_voted[post]) { snprintf(msg, n, "You have already voted for %s.", POST_CODE[post]); return 0; }
    Candidate *c = find_candidate(m, cid, post);
    if (!c) { snprintf(msg, n, "Invalid candidate for this post."); return 0; }
    if (m->ballot_count >= MAX_BALLOTS) { snprintf(msg, n, "Ballot box full."); return 0; }

    Ballot *b = &m->ballots[m->ballot_count];
    b->vote_id = ++m->ballot_count;
    b->post = post; b->candidate_id = cid; b->timestamp = time(NULL);
    c->increment_vote(c);
    v->has_voted[post] = 1;
    snprintf(msg, n, "Vote recorded for %s.", POST_CODE[post]);
    return 1;
}

/* Tallies computed programmatically. live=1: from ballots (admin view);
 * live=0: final results via Candidate.get_vote_count() once election is closed. */
static int manager_generate_results(VotingSystemManager *m, Buf *out, int live) {
    if (!live && m->election_status != ELECTION_CLOSED) return 0;
    buf_addf(out, "{\"ok\":true,\"status\":\"%s\",\"live\":%s,\"posts\":[",
             STATUS_NAME[m->election_status], live ? "true" : "false");
    for (int p = 0; p < NUM_POSTS; p++) {
        int best = -1, tie = 0, total = 0;
        for (int i = 0; i < m->candidate_count; i++) {
            Candidate *c = m->candidates_list[i];
            if (c->running_post != (Post)p) continue;
            int votes = 0;
            for (int k = 0; k < m->ballot_count; k++)
                if (m->ballots[k].post == (Post)p && m->ballots[k].candidate_id == c->candidate_id) votes++;
            if (!live) votes = c->get_vote_count(c);
            total += votes;
            if (votes > best) { best = votes; tie = 0; } else if (votes == best) tie = 1;
        }
        buf_addf(out, "%s{\"code\":\"%s\",\"name\":", p ? "," : "", POST_CODE[p]);
        buf_add_json_str(out, POST_NAME[p]);
        buf_addf(out, ",\"total\":%d,\"tie\":%s,\"candidates\":[", total, (tie && total > 0) ? "true" : "false");
        int first = 1;
        for (int i = 0; i < m->candidate_count; i++) {
            Candidate *c = m->candidates_list[i];
            if (c->running_post != (Post)p) continue;
            int votes = 0;
            for (int k = 0; k < m->ballot_count; k++)
                if (m->ballots[k].post == (Post)p && m->ballots[k].candidate_id == c->candidate_id) votes++;
            if (!live) votes = c->get_vote_count(c);
            buf_addf(out, "%s{\"id\":%d,\"name\":", first ? "" : ",", c->candidate_id);
            buf_add_json_str(out, c->base.name);
            buf_addf(out, ",\"votes\":%d,\"winner\":%s}", votes,
                     (!tie && total > 0 && votes == best) ? "true" : "false");
            first = 0;
        }
        buf_addf(out, "]}");
    }
    buf_addf(out, "]}");
    return 1;
}

/* ================================================================ */
/* Class: Admin / ElectionCommission (inherits User)                */
/* ================================================================ */
typedef struct Admin Admin;
struct Admin {
    User base;
    int  admin_id;
    int (*add_candidate)(Admin *self, VotingSystemManager *m, int post, const char *name,
                         const char *manifesto, char *msg, size_t n);
    int (*start_election)(Admin *self, VotingSystemManager *m, char *msg, size_t n);
    int (*stop_election)(Admin *self, VotingSystemManager *m, char *msg, size_t n);
    int (*publish_results)(Admin *self, VotingSystemManager *m, Buf *out);
};
static int admin_add_candidate(Admin *self, VotingSystemManager *m, int post, const char *name,
                               const char *manifesto, char *msg, size_t n) {
    (void)self;
    if (m->election_status != ELECTION_NOT_STARTED) { snprintf(msg, n, "Candidates can only be added before the election starts."); return 0; }
    if (post < 0 || post >= NUM_POSTS) { snprintf(msg, n, "Invalid post."); return 0; }
    if (!name[0]) { snprintf(msg, n, "Candidate name is required."); return 0; }
    if (m->candidate_count >= MAX_CANDIDATES) { snprintf(msg, n, "Candidate list is full."); return 0; }
    Candidate *c = candidate_new(m->candidate_count + 1, name, (Post)post, manifesto, &m->election_status);
    if (!c) { snprintf(msg, n, "Out of memory."); return 0; }
    m->candidates_list[m->candidate_count++] = c;
    snprintf(msg, n, "Added candidate #%d for %s.", c->candidate_id, POST_CODE[post]);
    return 1;
}
static int admin_start_election(Admin *self, VotingSystemManager *m, char *msg, size_t n) {
    (void)self;
    if (m->candidate_count == 0) { snprintf(msg, n, "Add candidates first."); return 0; }
    if (m->election_status != ELECTION_NOT_STARTED) { snprintf(msg, n, "Election already started or closed."); return 0; }
    m->election_status = ELECTION_ACTIVE;
    snprintf(msg, n, "Election is now ACTIVE.");
    return 1;
}
static int admin_stop_election(Admin *self, VotingSystemManager *m, char *msg, size_t n) {
    (void)self;
    if (m->election_status != ELECTION_ACTIVE) { snprintf(msg, n, "Election is not active."); return 0; }
    m->election_status = ELECTION_CLOSED;
    snprintf(msg, n, "Election CLOSED. Final results are now available.");
    return 1;
}
static int admin_publish_results(Admin *self, VotingSystemManager *m, Buf *out) {
    (void)self;
    return m->generate_results(m, out, 0);
}
static void admin_init(Admin *a) {
    user_init(&a->base, 1, "Election Commission", "ec@nits.ac.in", ADMIN_PASSWORD);
    a->admin_id = 1;
    a->add_candidate = admin_add_candidate;
    a->start_election = admin_start_election;
    a->stop_election = admin_stop_election;
    a->publish_results = admin_publish_results;
}

/* ================================================================ */
/* Sessions                                                         */
/* ================================================================ */
typedef struct { int used; int is_admin; char token[33]; Voter *voter; } Session;

static VotingSystemManager mgr;
static Admin admin;
static Session sessions[MAX_SESSIONS];

static Session *session_create(int is_admin, Voter *v) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].used) continue;
        Session *s = &sessions[i];
        s->used = 1; s->is_admin = is_admin; s->voter = v;
        for (int k = 0; k < 32; k++) s->token[k] = "0123456789abcdef"[rand() % 16];
        s->token[32] = '\0';
        return s;
    }
    return NULL;
}
static Session *session_find(const char *token, int want_admin) {
    if (!token || strlen(token) != 32) return NULL;
    for (int i = 0; i < MAX_SESSIONS; i++)
        if (sessions[i].used && strcmp(sessions[i].token, token) == 0 && sessions[i].is_admin == want_admin)
            return &sessions[i];
    return NULL;
}

/* ================================================================ */
/* HTTP helpers                                                     */
/* ================================================================ */
static void url_decode(const char *src, char *dst, size_t n) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < n; i++) {
        if (src[i] == '+') dst[j++] = ' ';
        else if (src[i] == '%' && isxdigit((unsigned char)src[i+1]) && isxdigit((unsigned char)src[i+2])) {
            char hx[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(hx, NULL, 16); i += 2;
        } else dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* read key from a form-encoded / query string */
static int get_param(const char *src, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    const char *p = src;
    out[0] = '\0';
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t len = strcspn(v, "&");
            char *raw = malloc(len + 1);
            memcpy(raw, v, len); raw[len] = '\0';
            url_decode(raw, out, n);
            free(raw);
            /* trim */
            size_t l = strlen(out);
            while (l && isspace((unsigned char)out[l-1])) out[--l] = '\0';
            char *s = out; while (*s && isspace((unsigned char)*s)) s++;
            if (s != out) memmove(out, s, strlen(s) + 1);
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static void send_all(sock_t s, const char *d, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = (int)send(s, d + sent, (int)(len - sent), MSG_NOSIGNAL);
        if (r <= 0) return;
        sent += (size_t)r;
    }
}
static void send_response(sock_t s, int code, const char *ctype, const char *body, size_t len) {
    const char *txt = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                    : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Error";
    char head[400];
    int hl = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        code, txt, ctype, len);
    send_all(s, head, (size_t)hl);
    send_all(s, body, len);
}
static void send_json(sock_t s, int code, Buf *b) {
    send_response(s, code, "application/json; charset=utf-8", b->d, b->len);
}
static void send_msg(sock_t s, int code, int ok, const char *msg) {
    Buf b; buf_init(&b);
    buf_addf(&b, "{\"ok\":%s,\"error\":", ok ? "true" : "false");
    if (ok) buf_addf(&b, "null"); else buf_add_json_str(&b, msg);
    buf_addf(&b, ",\"message\":"); buf_add_json_str(&b, msg);
    buf_addf(&b, "}");
    send_json(s, code, &b);
    buf_free(&b);
}

static const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    return "application/octet-stream";
}

static void serve_static(sock_t s, const char *urlpath) {
    char path[300];
    if (strstr(urlpath, "..") || strchr(urlpath, '\\')) { send_msg(s, 403, 0, "Forbidden"); return; }
    snprintf(path, sizeof path, "%s", strcmp(urlpath, "/") == 0 ? "index.html" : urlpath + 1);
    FILE *f = fopen(path, "rb");
    if (!f) { send_msg(s, 404, 0, "Not found"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *data = malloc((size_t)sz + 1);
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    send_response(s, 200, mime_for(path), data, rd);
    free(data);
}

/* ================================================================ */
/* API routes                                                       */
/* ================================================================ */
static int parse_post_code(const char *s, Post *out) {
    for (int i = 0; i < NUM_POSTS; i++)
        if (strcmp(s, POST_CODE[i]) == 0) { *out = (Post)i; return 1; }
    return 0;
}

static void api_candidates(sock_t s) {
    Buf b; buf_init(&b);
    buf_addf(&b, "{\"ok\":true,\"status\":\"%s\",\"posts\":[", STATUS_NAME[mgr.election_status]);
    for (int p = 0; p < NUM_POSTS; p++) {
        buf_addf(&b, "%s{\"code\":\"%s\",\"name\":", p ? "," : "", POST_CODE[p]);
        buf_add_json_str(&b, POST_NAME[p]);
        buf_addf(&b, ",\"candidates\":[");
        int first = 1;
        for (int i = 0; i < mgr.candidate_count; i++) {
            Candidate *c = mgr.candidates_list[i];
            if (c->running_post != (Post)p) continue;
            buf_addf(&b, "%s{\"id\":%d,\"name\":", first ? "" : ",", c->candidate_id);
            buf_add_json_str(&b, c->base.name);
            buf_addf(&b, ",\"manifesto\":"); buf_add_json_str(&b, c->manifesto);
            buf_addf(&b, "}");
            first = 0;
        }
        buf_addf(&b, "]}");
    }
    buf_addf(&b, "]}");
    send_json(s, 200, &b); buf_free(&b);
}

static void handle_api(sock_t s, const char *method, const char *path, const char *query, const char *body) {
    char msg[256], tok[64];
    int is_post = strcmp(method, "POST") == 0;

    /* ---------- public ---------- */
    if (!is_post && !strcmp(path, "/api/candidates")) { api_candidates(s); return; }

    if (!is_post && !strcmp(path, "/api/results")) {
        Buf b; buf_init(&b);
        if (!admin.publish_results(&admin, &mgr, &b)) {
            buf_free(&b); send_msg(s, 403, 0, "Results are available only after the election is closed.");
            return;
        }
        send_json(s, 200, &b); buf_free(&b); return;
    }

    /* ---------- student ---------- */
    if (is_post && !strcmp(path, "/api/login/request")) {
        char first[32], second[32], sid[16], email[128]; int otp = 0;
        get_param(body, "first", first, sizeof first);
        get_param(body, "second", second, sizeof second);
        get_param(body, "scholar_id", sid, sizeof sid);
        get_param(body, "email", email, sizeof email);
        if (!mgr.authenticate_student(&mgr, first, second, sid, email, &otp, msg, sizeof msg)) {
            send_msg(s, 400, 0, msg); return;
        }
        /* DEMO MODE: no mail server, so the OTP is returned to the page. */
        Buf b; buf_init(&b);
        buf_addf(&b, "{\"ok\":true,\"message\":"); buf_add_json_str(&b, msg);
        buf_addf(&b, ",\"demo_otp\":%d}", otp);
        send_json(s, 200, &b); buf_free(&b); return;
    }
    if (is_post && !strcmp(path, "/api/login/verify")) {
        char sid[16], otp[16];
        get_param(body, "scholar_id", sid, sizeof sid);
        get_param(body, "otp", otp, sizeof otp);
        Voter *v = mgr.verify_otp(&mgr, sid, atoi(otp), msg, sizeof msg);
        if (!v) { send_msg(s, 400, 0, msg); return; }
        Session *se = session_create(0, v);
        if (!se) { send_msg(s, 400, 0, "Too many active sessions."); return; }
        Buf b; buf_init(&b);
        buf_addf(&b, "{\"ok\":true,\"token\":\"%s\",\"name\":", se->token); buf_add_json_str(&b, v->base.name);
        buf_addf(&b, ",\"scholar_id\":\"%s\",\"department\":\"%s\"}", v->scholar_id, v->department);
        send_json(s, 200, &b); buf_free(&b); return;
    }
    if (!is_post && !strcmp(path, "/api/me")) {
        get_param(query, "token", tok, sizeof tok);
        Session *se = session_find(tok, 0);
        if (!se) { send_msg(s, 401, 0, "Not logged in."); return; }
        Voter *v = se->voter;
        Buf b; buf_init(&b);
        buf_addf(&b, "{\"ok\":true,\"status\":\"%s\",\"name\":", STATUS_NAME[mgr.election_status]);
        buf_add_json_str(&b, v->base.name);
        buf_addf(&b, ",\"voted\":{");
        for (int p = 0; p < NUM_POSTS; p++) buf_addf(&b, "%s\"%s\":%s", p ? "," : "", POST_CODE[p], v->has_voted[p] ? "true" : "false");
        buf_addf(&b, "}}");
        send_json(s, 200, &b); buf_free(&b); return;
    }
    if (is_post && !strcmp(path, "/api/vote")) {
        char pc[16], cid[16]; Post post;
        get_param(body, "token", tok, sizeof tok);
        Session *se = session_find(tok, 0);
        if (!se) { send_msg(s, 401, 0, "Not logged in."); return; }
        get_param(body, "post", pc, sizeof pc);
        get_param(body, "candidate_id", cid, sizeof cid);
        if (!parse_post_code(pc, &post)) { send_msg(s, 400, 0, "Invalid post."); return; }
        int ok = se->voter->cast_vote(se->voter, atoi(cid), post, msg, sizeof msg);
        send_msg(s, ok ? 200 : 400, ok, msg); return;
    }
    if (is_post && !strcmp(path, "/api/logout")) {
        get_param(body, "token", tok, sizeof tok);
        Session *se = session_find(tok, 0);
        if (se) { se->voter->base.logout(&se->voter->base); se->used = 0; }
        send_msg(s, 200, 1, "Logged out."); return;
    }

    /* ---------- admin ---------- */
    if (is_post && !strcmp(path, "/api/admin/login")) {
        char pw[64];
        get_param(body, "password", pw, sizeof pw);
        if (!user_check_password(&admin.base, pw)) { send_msg(s, 401, 0, "Wrong admin password."); return; }
        Session *se = session_create(1, NULL);
        if (!se) { send_msg(s, 400, 0, "Too many active sessions."); return; }
        admin.base.login(&admin.base);
        Buf b; buf_init(&b);
        buf_addf(&b, "{\"ok\":true,\"token\":\"%s\"}", se->token);
        send_json(s, 200, &b); buf_free(&b); return;
    }

    if (!strncmp(path, "/api/admin/", 11)) {
        get_param(is_post ? body : query, "token", tok, sizeof tok);
        Session *se = session_find(tok, 1);
        if (!se) { send_msg(s, 401, 0, "Admin login required."); return; }

        if (is_post && !strcmp(path, "/api/admin/add_candidate")) {
            char pc[16], name[64], man[160]; Post post;
            get_param(body, "post", pc, sizeof pc);
            get_param(body, "name", name, sizeof name);
            get_param(body, "manifesto", man, sizeof man);
            if (!parse_post_code(pc, &post)) { send_msg(s, 400, 0, "Invalid post."); return; }
            int ok = admin.add_candidate(&admin, &mgr, (int)post, name, man, msg, sizeof msg);
            send_msg(s, ok ? 200 : 400, ok, msg); return;
        }
        if (is_post && !strcmp(path, "/api/admin/start")) {
            int ok = admin.start_election(&admin, &mgr, msg, sizeof msg);
            send_msg(s, ok ? 200 : 400, ok, msg); return;
        }
        if (is_post && !strcmp(path, "/api/admin/stop")) {
            int ok = admin.stop_election(&admin, &mgr, msg, sizeof msg);
            send_msg(s, ok ? 200 : 400, ok, msg); return;
        }
        if (!is_post && !strcmp(path, "/api/admin/tally")) {
            Buf b; buf_init(&b);
            mgr.generate_results(&mgr, &b, 1);
            send_json(s, 200, &b); buf_free(&b); return;
        }
    }
    send_msg(s, 404, 0, "Unknown API endpoint.");
}

/* ================================================================ */
/* Connection handling                                              */
/* ================================================================ */
static void handle_connection(sock_t cs) {
    char *buf = malloc(MAX_REQ + 1);
    if (!buf) { CLOSESOCK(cs); return; }
    int total = 0, header_end = -1, content_len = 0;

    while (total < MAX_REQ) {
        int n = (int)recv(cs, buf + total, MAX_REQ - total, 0);
        if (n <= 0) break;
        total += n; buf[total] = '\0';
        if (header_end < 0) {
            char *p = strstr(buf, "\r\n\r\n");
            if (p) {
                header_end = (int)(p - buf) + 4;
                char *hdr = malloc((size_t)header_end + 1);
                memcpy(hdr, buf, (size_t)header_end); hdr[header_end] = '\0';
                lower(hdr);
                char *cl = strstr(hdr, "content-length:");
                if (cl) content_len = atoi(cl + 15);
                free(hdr);
            }
        }
        if (header_end >= 0 && total >= header_end + content_len) break;
    }
    if (header_end < 0) { free(buf); CLOSESOCK(cs); return; }

    char method[8] = "", target[512] = "";
    sscanf(buf, "%7s %511s", method, target);
    char *q = strchr(target, '?');
    const char *query = "";
    if (q) { *q = '\0'; query = q + 1; }
    const char *body = buf + header_end;

    if (!strncmp(target, "/api/", 5)) handle_api(cs, method, target, query, body);
    else if (!strcmp(method, "GET")) serve_static(cs, target);
    else send_msg(cs, 400, 0, "Bad request.");

    free(buf);
    CLOSESOCK(cs);
}

int main(void) {
    srand((unsigned)time(NULL) ^ (unsigned)clock());

    mgr.election_status = ELECTION_NOT_STARTED;
    mgr.authenticate_student = manager_authenticate_student;
    mgr.verify_otp = manager_verify_otp;
    mgr.process_vote = manager_process_vote;
    mgr.generate_results = manager_generate_results;
    admin_init(&admin);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { fprintf(stderr, "WSAStartup failed\n"); return 1; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == BAD_SOCK) { perror("socket"); return 1; }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* localhost only */
    addr.sin_port = htons(PORT);

    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind (is port 8080 in use?)"); return 1; }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    printf("NIT Silchar Gymkhana Voting System running.\n");
    printf("Open http://localhost:%d in your browser.  (Admin password: %s)\n", PORT, ADMIN_PASSWORD);
    printf("Press Ctrl+C to stop.\n");

    for (;;) {
        sock_t cs = accept(srv, NULL, NULL);
        if (cs == BAD_SOCK) continue;
        handle_connection(cs);
    }
    return 0;
}
