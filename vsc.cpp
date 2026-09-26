// vsc — a tiny VS Code–style terminal text editor. Single file, no dependencies
// beyond the C++ standard library and POSIX. PDF viewing shells out to poppler
// (pdftoppm/pdftotext/pdfinfo) and draws pages as sixel graphics (foot).
//
// Build: make            Run: ./vsc [file ...]
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

using std::string;
using std::vector;
template <class T> static int len(const T& c) { return (int)c.size(); }
static string num(long v) { return std::to_string(v); }
static int iround(double v) { return (int)(v < 0 ? v - 0.5 : v + 0.5); }

// ───────────────────────── terminal ─────────────────────────
static termios g_orig;
static bool g_raw = false, g_gfx = false;  // g_gfx: the terminal shows sixel images
static int W = 80, H = 24, cellW = 0, cellH = 0;
static volatile sig_atomic_t g_resized = 0;
// LaTeX build started on save of a .tex file (runs in the background)
static pid_t g_texPid = -1;
static int g_texFd = -1;  // read end of a pipe the build holds open; EOF = build finished
static string g_texPdf, g_texLog;
static long long g_texT0;
static void texDone();

static void wr(const string& s) {
  const char* p = s.data();
  size_t n = s.size();
  while (n) {
    ssize_t k = write(1, p, n);
    if (k < 0) { if (errno == EINTR) continue; return; }
    p += k; n -= (size_t)k;
  }
}
static void restore() {
  if (g_texPid > 0) kill(-g_texPid, SIGTERM);
  if (!g_raw) return;
  g_raw = false;
  const char* s = "\x1b[?2004l\x1b[?1006l\x1b[?1002l\x1b[?1000l"
                  "\x1b[0m\x1b[0 q\x1b[?25h\x1b[?1049l\x1b[23;0t";
  if (write(1, s, strlen(s)) < 0) {}
  tcsetattr(0, TCSAFLUSH, &g_orig);
}
static void onFatal(int s) { restore(); signal(s, SIG_DFL); raise(s); }
static void onWinch(int) { g_resized = 1; }
static void getSize() {
  winsize ws{};
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
    W = ws.ws_col; H = ws.ws_row;
    if (ws.ws_xpixel && ws.ws_ypixel) { cellW = ws.ws_xpixel / W; cellH = ws.ws_ypixel / H; }
  }
  if (H < 3) H = 3;
  if (W < 10) W = 10;
}
static void rawOn() {
  tcgetattr(0, &g_orig);
  termios t = g_orig;
  t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t.c_oflag &= ~OPOST;
  t.c_cflag |= CS8;
  t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
  tcsetattr(0, TCSAFLUSH, &t);
  g_raw = true;
  atexit(restore);
  struct sigaction sa{};
  sa.sa_handler = onWinch;  // no SA_RESTART: poll() returns EINTR so we redraw
  sigaction(SIGWINCH, &sa, nullptr);
  for (int s : {SIGTERM, SIGHUP, SIGINT, SIGSEGV, SIGABRT, SIGBUS}) signal(s, onFatal);
  // alt screen, mouse (click+drag, SGR), bracketed paste, bar cursor
  wr("\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?2004h\x1b[6 q");
}
static long long nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

// ───────────────────────── input ─────────────────────────
static unsigned char ib[4096];
static int ibN = 0, ibP = 0;
static int rd(int ms) {
  if (ibP < ibN) return ib[ibP++];
  // only a blocking read watches the build: short reads are mid-escape-sequence or mid-paste
  pollfd p[2] = {{0, POLLIN, 0}, {g_texFd, POLLIN, 0}};
  if (poll(p, ms < 0 && g_texFd >= 0 ? 2 : 1, ms) <= 0) return -1;
  if (!p[0].revents) { texDone(); return -1; }
  ssize_t n = read(0, ib, sizeof ib);
  if (n == 0) exit(0);  // terminal gone
  if (n < 0) return -1;
  ibN = (int)n; ibP = 1;
  return ib[0];
}

enum { K_NONE = -1, K_ESC = 27,
       K_UP = 0x110000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
       K_DEL, K_INS, K_F3, K_BTAB, K_MOUSE, K_PASTE, K_F2 };
enum { M_SHIFT = 1 << 24, M_ALT = 1 << 25, M_CTRL = 1 << 26, M_ALL = M_SHIFT | M_ALT | M_CTRL };
static int mB, mX, mY;
static bool mDown;
static string pasteBuf;

static void readPaste() {
  pasteBuf.clear();
  for (;;) {
    int c = rd(1000);
    if (c < 0) break;
    pasteBuf += (char)c;
    if (pasteBuf.size() >= 6 && !pasteBuf.compare(pasteBuf.size() - 6, 6, "\x1b[201~")) {
      pasteBuf.resize(pasteBuf.size() - 6);
      break;
    }
  }
  string o;  // normalise CRLF / CR to LF
  o.reserve(pasteBuf.size());
  for (size_t i = 0; i < pasteBuf.size(); i++) {
    if (pasteBuf[i] == '\r') { o += '\n'; if (i + 1 < pasteBuf.size() && pasteBuf[i + 1] == '\n') i++; }
    else o += pasteBuf[i];
  }
  pasteBuf.swap(o);
}

static int readKey() {
  int c = rd(-1);
  if (c < 0) return K_NONE;
  if (c != 27) return c;
  int c1 = rd(30);
  if (c1 < 0) return K_ESC;
  if (c1 == 'O') {
    switch (rd(30)) {
      case 'A': return K_UP; case 'B': return K_DOWN; case 'C': return K_RIGHT; case 'D': return K_LEFT;
      case 'H': return K_HOME; case 'F': return K_END; case 'R': return K_F3; case 'Q': return K_F2;
    }
    return K_NONE;
  }
  if (c1 == '_' || c1 == ']' || c1 == 'P') {  // terminal replies (APC/OSC/DCS): swallow
    for (int prev = 0;;) {
      int x = rd(100);
      if (x < 0 || x == 7 || (prev == 27 && x == '\\')) break;
      prev = x;
    }
    return K_NONE;
  }
  if (c1 != '[') return c1 == 27 ? K_ESC : (M_ALT | c1);
  string p;
  int f;
  for (;;) {
    f = rd(30);
    if (f < 0) return K_NONE;
    if (f >= 0x40 && f <= 0x7e) break;
    p += (char)f;
  }
  if (!p.empty() && p[0] == '<') {  // SGR mouse
    int b = 0, x = 1, y = 1;
    sscanf(p.c_str() + 1, "%d;%d;%d", &b, &x, &y);
    mB = b; mX = x - 1; mY = y - 1; mDown = f == 'M';
    return K_MOUSE;
  }
  int n1 = 0, n2 = 1;
  sscanf(p.c_str(), "%d;%d", &n1, &n2);
  int m = n2 - 1, mod = 0;
  if (m & 1) mod |= M_SHIFT;
  if (m & 2) mod |= M_ALT;
  if (m & 4) mod |= M_CTRL;
  int k = K_NONE;
  switch (f) {
    case 'A': k = K_UP; break;   case 'B': k = K_DOWN; break;
    case 'C': k = K_RIGHT; break; case 'D': k = K_LEFT; break;
    case 'H': k = K_HOME; break; case 'F': k = K_END; break;
    case 'R': k = K_F3; break;   case 'Q': k = K_F2; break;
    case 'Z': return K_BTAB;
    case '~':
      switch (n1) {
        case 1: case 7: k = K_HOME; break;
        case 4: case 8: k = K_END; break;
        case 2: k = K_INS; break;  case 3: k = K_DEL; break;
        case 5: k = K_PGUP; break; case 6: k = K_PGDN; break;
        case 12: k = K_F2; break;  case 13: k = K_F3; break;
        case 200: readPaste(); return K_PASTE;
      }
  }
  return k == K_NONE ? k : (k | mod);
}
static void utf8Tail(string& s, int c) {
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  while (n--) { int x = rd(30); if (x < 0) break; s += (char)x; }
}

// ───────────────────────── UTF-8 ─────────────────────────
static bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }
static int nx(const string& s, int i) {
  if (i >= len(s)) return i;
  for (i++; i < len(s) && cont(s[i]); i++) {}
  return i;
}
static int pv(const string& s, int i) {
  if (i <= 0) return 0;
  for (i--; i > 0 && cont(s[i]); i--) {}
  return i;
}
static unsigned decode(const string& s, int i) {
  unsigned char c = s[i];
  if (c < 0x80) return c;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  unsigned cp = c & (0x3F >> n);
  for (int k = 1; k <= n && i + k < len(s); k++) cp = (cp << 6) | (s[i + k] & 0x3F);
  return cp;
}
static int cpw(unsigned cp) {
  if (cp >= 0x300 && cp < 0x370) return 0;
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
      (cp >= 0x20000 && cp <= 0x3FFFD))
    return 2;
  return 1;
}
static int chw(const string& s, int i, int rc) { return s[i] == '\t' ? 4 - rc % 4 : cpw(decode(s, i)); }
static int rcol(const string& s, int x) {  // byte column -> screen column
  int r = 0;
  for (int i = 0; i < x && i < len(s); i = nx(s, i)) r += chw(s, i, r);
  return r;
}
static int xfromr(const string& s, int rc) {  // screen column -> byte column
  int r = 0, i = 0;
  while (i < len(s)) {
    int w = chw(s, i, r);
    if (r + w > rc) break;
    r += w; i = nx(s, i);
  }
  return i;
}
static int dispW(const string& s) { return rcol(s, len(s)); }
static string fitW(const string& s, int w) { return s.substr(0, xfromr(s, w)); }

// ───────────────────────── languages ─────────────────────────
enum { C_N, C_KW, C_CTL, C_TY, C_STR, C_CMT, C_NUM, C_PRE, C_FN, C_ID, C_B0, C_B1, C_B2 };
// VS Code Dark+ token colours
static const char* FG[] = {"212;212;212", "86;156;214",  "197;134;192", "78;201;176", "206;145;120",
                           "106;153;85",  "181;206;168", "197;134;192", "220;220;170", "156;220;254",
                           "255;215;0",   "218;112;214", "23;159;255"};
struct Lang { const char *lc; bool blk, pre; const char *q, *kw, *ctl, *ty; int id; };
static const Lang LANGS[] = {
  {"", false, false, "", "", "", "", C_N},
  {"//", true, true, "\"'",
   " auto class struct union enum namespace template typename using public private protected virtual override final"
   " static const constexpr consteval constinit volatile extern inline friend operator sizeof new delete this nullptr"
   " true false typedef mutable explicit noexcept decltype alignof alignas static_assert thread_local register unsigned"
   " signed let fn impl pub mod trait func package import type var val fun interface extends implements super self Self"
   " mut dyn ref where as crate use NULL ",
   " if else for while do switch case default break continue return goto try catch throw finally co_return co_await"
   " co_yield match loop in defer go select range ",
   " void int char short long float double bool size_t ssize_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t"
   " uint16_t uint32_t uint64_t uintptr_t wchar_t char8_t char16_t char32_t string i8 i16 i32 i64 i128 u8 u16 u32"
   " u64 u128 f32 f64 usize isize str byte rune std ",
   C_ID},
  {"#", false, false, "\"'",
   " def class lambda None True False and or not is in global nonlocal self cls pass async ",
   " if elif else for while break continue return try except finally raise with as yield import from await del"
   " assert match case ",
   " int float str bool list dict set tuple bytes object type complex frozenset ", C_ID},
  {"//", true, false, "\"'`",
   " var let const function class extends new delete typeof instanceof in of this super true false null undefined"
   " void async static get set interface type enum implements public private protected readonly declare namespace"
   " abstract keyof as ",
   " if else for while do switch case default break continue return throw try catch finally await yield import"
   " export from ",
   " number string boolean any unknown never object symbol bigint Array Promise Map Set Record ", C_ID},
  {"#", false, false, "\"'",
   " function local export readonly declare echo cd set unset source alias printf read eval exec shift test ",
   " if then else elif fi for while until do done case esac in return exit break continue ", "", C_N},
  {"", false, false, "\"", " true false null ", "", "", C_N},
};
static bool inList(const char* list, const char* w, int n) {
  if (n > 40 || !*list) return false;
  char k[48];
  k[0] = ' '; memcpy(k + 1, w, n); k[n + 1] = ' '; k[n + 2] = 0;
  return strstr(list, k) != nullptr;
}
static bool isw(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

// Highlights one line. `st` = (bracket depth << 1) | inside-block-comment, carried
// between lines. Writes a token class per byte to `out` (if given); returns next state.
static int hl(const Lang& lg, const string& s, int st, unsigned char* out) {
  bool cm = st & 1;
  int d = st >> 1, n = len(s), i = 0, first = 0;
  auto set = [&](int a, int b, int c) { if (out && b > a) memset(out + a, c, b - a); };
  while (first < n && (s[first] == ' ' || s[first] == '\t')) first++;
  int lcl = (int)strlen(lg.lc);
  while (i < n) {
    if (cm) {
      size_t e = s.find("*/", i);
      int j = e == string::npos ? n : (int)e + 2;
      set(i, j, C_CMT); i = j;
      if (e != string::npos) cm = false;
      continue;
    }
    unsigned char c = s[i];
    if (lg.blk && c == '/' && i + 1 < n && s[i + 1] == '*') { cm = true; set(i, i + 2, C_CMT); i += 2; continue; }
    if (lcl && !s.compare(i, lcl, lg.lc)) { set(i, n, C_CMT); break; }
    if (c && strchr(lg.q, c)) {
      int j = i + 1;
      while (j < n && (unsigned char)s[j] != c) j += s[j] == '\\' ? 2 : 1;
      j = std::min(j + 1, n);
      set(i, j, C_STR); i = j; continue;
    }
    if (lg.pre && c == '#' && i == first) {
      int j = i + 1;
      while (j < n && isalpha((unsigned char)s[j])) j++;
      set(i, j, C_PRE);
      bool inc = !s.compare(i, 8, "#include");
      i = j;
      if (inc) {
        while (i < n && s[i] == ' ') i++;
        if (i < n && s[i] == '<') {
          size_t k = s.find('>', i);
          int e = k == string::npos ? n : (int)k + 1;
          set(i, e, C_STR); i = e;
        }
      }
      continue;
    }
    if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
      int j = i;
      while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == '_')) j++;
      set(i, j, C_NUM); i = j; continue;
    }
    if (isalpha(c) || c == '_' || c == '$' || c == '@' || c >= 0x80) {
      int j = i + 1;
      while (j < n && isw(s[j])) j++;
      const char* w = s.data() + i;
      int wl = j - i, k = j, cls = lg.id;
      while (k < n && s[k] == ' ') k++;
      if (c == '@') cls = C_FN;
      else if (c == '$') cls = C_ID;
      else if (inList(lg.ctl, w, wl)) cls = C_CTL;
      else if (inList(lg.kw, w, wl)) cls = C_KW;
      else if (inList(lg.ty, w, wl)) cls = C_TY;
      else if (lg.id == C_ID && k < n && s[k] == '(') cls = C_FN;
      else if (lg.id == C_ID && isupper(c) && std::any_of(w, w + wl, [](char x) { return islower((unsigned char)x); })) cls = C_TY;
      set(i, j, cls); i = j; continue;
    }
    if (lg.id == C_ID && (c == '(' || c == '[' || c == '{')) { set(i, i + 1, C_B0 + d % 3); d++; i++; continue; }
    if (lg.id == C_ID && (c == ')' || c == ']' || c == '}')) { if (d > 0) d--; set(i, i + 1, C_B0 + d % 3); i++; continue; }
    set(i, i + 1, C_N); i++;
  }
  return (d << 1) | (cm ? 1 : 0);
}

// ───────────────────────── buffers ─────────────────────────
struct Op { bool ins; int y, x; string s; int by, bx, ey, ex; unsigned g; };
struct Buf {
  int id = 0;
  string path, name, langName = "Plain Text";
  vector<string> L{""};
  bool crlf = false, ro = false;
  int cy = 0, cx = 0, want = -1, top = 0, left = 0;
  bool follow = true, sel = false;
  int ay = 0, ax = 0;
  vector<Op> U, R;
  unsigned saved = 0;
  int lang = 0, stOK = 1;
  vector<int> st{0};
  // PDF
  bool pdf = false, pdfText = false;
  int page = 1, pages = 1, zoom = 100, panX = 0;  // page: the one in the middle of the view
  double pos = 0;  // scroll position as page index + fraction of that page, so it survives relayouts
  struct PSz { float w, h; bool turned; };  // points, as shown (after the page's /Rotate)
  vector<PSz> psz;
  string ver;  // size and mtime of the file psz was read from
  vector<int> ps;  // first line of each page in text mode
  bool dirty() const { return (U.empty() ? 0 : U.back().g) != saved; }
};
static vector<Buf> B;
static int cur = 0, g_ids = 0, g_untitled = 0;
static unsigned g_grp = 1, g_lastType = 0, g_prevType = 0;
static string g_msg, g_clip, g_findQ, g_lastFind;
static bool g_clipLine = false, g_run = true;
static unsigned g_msgGen = 0;
static int g_armed = 0;

static bool msg(const string& m) { g_msg = m; g_msgGen++; return false; }

static int stAt(Buf& b, int y) {
  if (b.st.size() < b.L.size() + 1) b.st.resize(b.L.size() + 1);
  for (; b.stOK <= y; b.stOK++) b.st[b.stOK] = hl(LANGS[b.lang], b.L[b.stOK - 1], b.st[b.stOK - 1], nullptr);
  return b.st[y];
}
static void touch(Buf& b, int y) { if (b.stOK > y + 1) b.stOK = y + 1; }

static void insRaw(Buf& b, int y, int x, const string& s, int& ey, int& ex) {
  touch(b, y);
  size_t q = s.find('\n');
  if (q == string::npos) { b.L[y].insert(x, s); ey = y; ex = x + len(s); return; }
  vector<string> parts;
  for (size_t p = 0;;) {
    q = s.find('\n', p);
    if (q == string::npos) { parts.push_back(s.substr(p)); break; }
    parts.push_back(s.substr(p, q - p));
    p = q + 1;
  }
  string tail = b.L[y].substr(x);
  b.L[y].erase(x);
  b.L[y] += parts[0];
  ex = len(parts.back());
  parts.back() += tail;
  b.L.insert(b.L.begin() + y + 1, std::make_move_iterator(parts.begin() + 1), std::make_move_iterator(parts.end()));
  ey = y + (int)parts.size() - 1;
}
static string getText(const Buf& b, int y0, int x0, int y1, int x1) {
  if (y0 == y1) return b.L[y0].substr(x0, x1 - x0);
  string r = b.L[y0].substr(x0);
  for (int i = y0 + 1; i < y1; i++) { r += '\n'; r += b.L[i]; }
  r += '\n';
  r += b.L[y1].substr(0, x1);
  return r;
}
static string delRaw(Buf& b, int y0, int x0, int y1, int x1) {
  touch(b, y0);
  string r = getText(b, y0, x0, y1, x1);
  if (y0 == y1) { b.L[y0].erase(x0, x1 - x0); return r; }
  b.L[y0].erase(x0);
  b.L[y0] += b.L[y1].substr(x1);
  b.L.erase(b.L.begin() + y0 + 1, b.L.begin() + y1 + 1);
  return r;
}
static void endOf(int y, int x, const string& s, int& ey, int& ex) {
  size_t q = s.rfind('\n');
  ey = y + (int)std::count(s.begin(), s.end(), '\n');
  ex = q == string::npos ? x + len(s) : len(s) - (int)q - 1;
}
static bool guard(Buf& b) { return b.ro ? msg("Read-only") : true; }

// Recorded edits (undoable). The cursor ends at the end of the change.
static void ins(Buf& b, int y, int x, const string& s) {
  if (s.empty() || b.ro) return;
  Op o{true, y, x, s, b.cy, b.cx, 0, 0, g_grp};
  insRaw(b, y, x, s, b.cy, b.cx);
  o.ey = b.cy; o.ex = b.cx;
  b.U.push_back(std::move(o));
  b.R.clear();
}
static void del(Buf& b, int y0, int x0, int y1, int x1) {
  if ((y0 == y1 && x0 == x1) || b.ro) return;
  Op o{false, y0, x0, "", b.cy, b.cx, y0, x0, g_grp};
  o.s = delRaw(b, y0, x0, y1, x1);
  b.cy = y0; b.cx = x0;
  b.U.push_back(std::move(o));
  b.R.clear();
}
static void undo(Buf& b) {
  if (b.U.empty()) return;
  unsigned g = b.U.back().g;
  while (!b.U.empty() && b.U.back().g == g) {
    Op o = std::move(b.U.back());
    b.U.pop_back();
    int ey, ex;
    if (o.ins) { endOf(o.y, o.x, o.s, ey, ex); delRaw(b, o.y, o.x, ey, ex); }
    else insRaw(b, o.y, o.x, o.s, ey, ex);
    b.cy = o.by; b.cx = o.bx;
    b.R.push_back(std::move(o));
  }
  b.sel = false;
}
static void redo(Buf& b) {
  if (b.R.empty()) return;
  unsigned g = b.R.back().g;
  while (!b.R.empty() && b.R.back().g == g) {
    Op o = std::move(b.R.back());
    b.R.pop_back();
    int ey, ex;
    if (o.ins) insRaw(b, o.y, o.x, o.s, ey, ex);
    else { endOf(o.y, o.x, o.s, ey, ex); delRaw(b, o.y, o.x, ey, ex); }
    b.cy = o.ey; b.cx = o.ex;
    b.U.push_back(std::move(o));
  }
  b.sel = false;
}
static void clampCur(Buf& b) {
  int n = len(b.L);
  b.cy = std::max(0, std::min(b.cy, n - 1));
  b.cx = std::max(0, std::min(b.cx, len(b.L[b.cy])));
  b.ay = std::max(0, std::min(b.ay, n - 1));
  b.ax = std::max(0, std::min(b.ax, len(b.L[b.ay])));
}

// ───────────────────────── selection helpers ─────────────────────────
static bool selRange(const Buf& b, int& y0, int& x0, int& y1, int& x1) {
  if (!b.sel) return false;
  y0 = b.ay; x0 = b.ax; y1 = b.cy; x1 = b.cx;
  if (y0 > y1 || (y0 == y1 && x0 > x1)) { std::swap(y0, y1); std::swap(x0, x1); }
  return !(y0 == y1 && x0 == x1);
}
static bool delSel(Buf& b) {
  int y0, x0, y1, x1;
  if (!selRange(b, y0, x0, y1, x1)) { b.sel = false; return false; }
  del(b, y0, x0, y1, x1);
  b.sel = false;
  return true;
}
static void lineRange(const Buf& b, int& y0, int& y1) {
  int x0, x1;
  if (selRange(b, y0, x0, y1, x1)) { if (y1 > y0 && x1 == 0) y1--; }
  else y0 = y1 = b.cy;
}
static string joinLines(const Buf& b, int y0, int y1) {
  string r;
  for (int y = y0; y <= y1; y++) { if (y > y0) r += '\n'; r += b.L[y]; }
  return r;
}
static void adj(int& py, int& px, int y, int x, int d) {
  if (py != y || px < x) return;
  px = d < 0 ? std::max(x, px + d) : px + d;
}
static int firstNonWs(const string& l) {
  int i = 0;
  while (i < len(l) && (l[i] == ' ' || l[i] == '\t')) i++;
  return i;
}
static int wc(unsigned char c) { return isw(c) ? 2 : (c == ' ' || c == '\t') ? 0 : 1; }
static void wordRight(Buf& b) {
  const string& l = b.L[b.cy];
  int n = len(l), x = b.cx;
  if (x >= n) { if (b.cy + 1 < len(b.L)) { b.cy++; b.cx = 0; } return; }
  while (x < n && wc(l[x]) == 0) x++;
  if (x < n) { int k = wc(l[x]); while (x < n && wc(l[x]) == k) x++; }
  b.cx = x;
}
static void wordLeft(Buf& b) {
  const string& l = b.L[b.cy];
  int x = b.cx;
  if (x == 0) { if (b.cy > 0) { b.cy--; b.cx = len(b.L[b.cy]); } return; }
  while (x > 0 && wc(l[x - 1]) == 0) x--;
  if (x > 0) { int k = wc(l[x - 1]); while (x > 0 && wc(l[x - 1]) == k) x--; }
  b.cx = x;
}

// ───────────────────────── helpers: processes, base64, files ─────────────────────────
// Runs a program and hands its stdout to fn(data, n) as it arrives; returns its exit status.
template <class F>
static int runEach(const vector<string>& args, F fn) {
  int pfd[2];
  if (pipe(pfd)) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_RDWR);
    dup2(dn, 0); dup2(dn, 2); dup2(pfd[1], 1);
    close(pfd[0]); close(pfd[1]);
    vector<char*> av;
    for (auto& a : args) av.push_back((char*)a.c_str());
    av.push_back(nullptr);
    execvp(av[0], av.data());
    _exit(127);
  }
  close(pfd[1]);
  int st = -1;
  if (pid > 0) {
    char buf[65536];
    for (;;) {
      ssize_t n = read(pfd[0], buf, sizeof buf);
      if (n > 0) fn(buf, (size_t)n);
      else if (n < 0 && errno == EINTR) continue;
      else break;
    }
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
  }
  close(pfd[0]);
  return pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static string run(const vector<string>& args, int* status = nullptr) {  // run a program, capture stdout
  string out;
  int st = runEach(args, [&](const char* d, size_t n) { out.append(d, n); });
  if (status) *status = st;
  return out;
}
static void b64(string& o, const unsigned char* p, size_t n) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i = 0;
  for (; i + 2 < n; i += 3) {
    unsigned v = p[i] << 16 | p[i + 1] << 8 | p[i + 2];
    o += T[v >> 18]; o += T[v >> 12 & 63]; o += T[v >> 6 & 63]; o += T[v & 63];
  }
  if (i < n) {
    unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0);
    o += T[v >> 18]; o += T[v >> 12 & 63]; o += i + 1 < n ? T[v >> 6 & 63] : '='; o += '=';
  }
}
static void osc52(const string& s) {  // copy to system clipboard via the terminal
  if (s.size() > 100000) return;
  string o = "\x1b]52;c;";
  b64(o, (const unsigned char*)s.data(), s.size());
  o += "\x07";
  wr(o);
}
static string baseName(const string& p) { size_t k = p.rfind('/'); return k == string::npos ? p : p.substr(k + 1); }
static string expandHome(const string& p) {
  const char* h = getenv("HOME");
  if (h && p.size() >= 1 && p[0] == '~' && (p.size() == 1 || p[1] == '/')) return h + p.substr(1);
  return p;
}
static string absPath(const string& p) {  // absolute, symlink-free path (file need not exist)
  char buf[PATH_MAX];
  if (realpath(p.c_str(), buf)) return buf;
  size_t k = p.rfind('/');
  string dir = k == string::npos ? "." : p.substr(0, k ? k : 1), nm = k == string::npos ? p : p.substr(k + 1);
  if (realpath(dir.c_str(), buf)) return string(buf) + (strcmp(buf, "/") ? "/" : "") + nm;
  return p;
}
static string extOf(const string& name) {
  size_t k = name.rfind('.');
  string e = k == string::npos ? "" : name.substr(k + 1);
  for (auto& c : e) c = (char)tolower((unsigned char)c);
  return e;
}
static void setLang(Buf& b) {
  static const struct { const char* e; int l; const char* n; } M[] = {
    {"c", 1, "C"}, {"h", 1, "C"}, {"cpp", 1, "C++"}, {"cc", 1, "C++"}, {"cxx", 1, "C++"}, {"hpp", 1, "C++"},
    {"hh", 1, "C++"}, {"hxx", 1, "C++"}, {"ino", 1, "C++"}, {"cu", 1, "CUDA C++"}, {"java", 1, "Java"},
    {"cs", 1, "C#"}, {"rs", 1, "Rust"}, {"go", 1, "Go"}, {"swift", 1, "Swift"}, {"kt", 1, "Kotlin"},
    {"py", 2, "Python"}, {"pyw", 2, "Python"}, {"js", 3, "JavaScript"}, {"mjs", 3, "JavaScript"},
    {"cjs", 3, "JavaScript"}, {"jsx", 3, "JavaScript React"}, {"ts", 3, "TypeScript"},
    {"tsx", 3, "TypeScript React"}, {"sh", 4, "Shell Script"}, {"bash", 4, "Shell Script"},
    {"zsh", 4, "Shell Script"}, {"json", 5, "JSON"}, {"jsonc", 5, "JSON with Comments"}};
  string e = extOf(b.name);
  b.lang = 0; b.langName = "Plain Text";
  if (b.name == "Makefile" || b.name == "makefile") { b.lang = 4; b.langName = "Makefile"; }
  for (auto& m : M)
    if (e == m.e) { b.lang = m.l; b.langName = m.n; }
  b.st.assign(1, 0); b.stOK = 1;
}
static void newTab() {
  Buf nb;
  nb.id = ++g_ids;
  nb.name = "Untitled-" + num(++g_untitled);
  B.push_back(std::move(nb));
  cur = (int)B.size() - 1;
}

// ───────────────────────── PDF ─────────────────────────
static string fileVer(const string& path) {
  struct stat st;
  if (stat(path.c_str(), &st)) return "";
  return num((long)st.st_size) + "/" + num((long)st.st_mtim.tv_sec) + "/" + num(st.st_mtim.tv_nsec);
}
static void pdfInfo(Buf& b) {  // page count and every page's size, in one pdfinfo run
  b.ver = fileVer(b.path);
  string r = run({"pdfinfo", "-f", "1", "-l", "1000000", b.path});
  size_t k = r.find("Pages:");
  b.pages = k == string::npos ? 0 : atoi(r.c_str() + k + 6);
  b.psz.assign(std::max(1, b.pages), Buf::PSz{612, 792, false});
  for (size_t p = 0; (p = r.find("\nPage ", p)) != string::npos; p++) {
    const char* l = r.c_str() + p + 6;
    int n, rot;
    double w, h;
    if (sscanf(l, "%d size: %lf x %lf", &n, &w, &h) == 3 && n >= 1 && n <= b.pages && w > 0 && h > 0) {
      b.psz[n - 1].w = (float)w; b.psz[n - 1].h = (float)h;
    } else if (sscanf(l, "%d rot: %d", &n, &rot) == 2 && n >= 1 && n <= b.pages) b.psz[n - 1].turned = rot % 180;
  }
  for (auto& z : b.psz)
    if (z.turned) std::swap(z.w, z.h);
}
static void pdfText(Buf& b, bool on) {
  b.sel = false;
  if (on) {
    string t = run({"pdftotext", "-layout", b.path, "-"});
    if (t.empty()) t = g_gfx ? "(no text layer in this PDF - press Ctrl+T to see the page image)" : "(no text layer in this PDF)";
    b.L.clear(); b.ps.assign(1, 0);
    for (size_t p = 0;;) {
      size_t q = t.find('\n', p);
      string l = t.substr(p, q == string::npos ? string::npos : q - p);
      if (!l.empty() && l[0] == '\f') {
        l.erase(0, 1);
        if (q != string::npos || !l.empty()) b.ps.push_back(len(b.L));
      }
      b.L.push_back(std::move(l));
      if (q == string::npos) break;
      p = q + 1;
    }
    if (b.L.empty()) b.L.push_back("");
    int pg = std::min(b.page, (int)b.ps.size()) - 1;
    b.cy = b.top = pg >= 0 ? b.ps[pg] : 0;
    b.cx = 0; b.pdfText = true;
  } else {
    if (b.page != (int)b.pos + 1) b.pos = b.page - 1;  // moved to another page in the text view: go there
    vector<string>{""}.swap(b.L);
    vector<int>().swap(b.ps);
    b.cy = b.cx = b.top = 0;
    b.pdfText = false;
    malloc_trim(0);
  }
  b.st.assign(1, 0); b.stOK = 1;
}
// ───────────────────────── LaTeX build on save ─────────────────────────
static string g_imgKey, g_geoKey;  // what is on screen / the layout the cache was rendered for
// The document is laid out as one column of pages (100% zoom = the widest page fits the width) and
// scrolled by the pixel. Only the rows in view plus 20% of the view height are kept, that 20% ahead
// in the direction of scrolling (split above and below at rest), and only the columns in view (a
// sideways pan when zoomed in starts the cache over). They are rendered by pdftoppm as raw PPM and stored as
// 6-row bands (one sixel row each) of palette indices, PackBits-coded per row: a byte n < 128 is
// followed by n+1 literal indices, n >= 128 by one index repeated n-125 times.
// Palette: 0-15 greys, 16-231 a 6x6x6 colour cube, 232 the editor background around the pages.
enum { PAL_BG = 232, PAL_N = 233 };
static vector<int> g_ptop, g_pw, g_ph;  // each page's top (document pixels) and size in pixels
static int g_docW = 0, g_docH = 0, g_gap = 0;
static vector<string> g_win;  // cached bands, from band g_wb0; "" = not rendered yet
static int g_wb0 = 0, g_wx0 = 0, g_wxw = 0;  // ... covering document columns [g_wx0, g_wx0 + g_wxw)
static bool g_palUsed[PAL_N];
static int g_holdA = -1, g_holdB = -1;  // rows also kept while a scroll animation runs
static int g_lastY = -1;  // view top at the last frame, for the direction of scrolling
static void pdfDrop() {
  vector<string>().swap(g_win);
  g_wb0 = g_wxw = 0;
  g_lastY = -1;
  memset(g_palUsed, 0, sizeof g_palUsed);
  g_geoKey.clear(); g_imgKey.clear();
}
static bool texMagic(const string& l, const char* key, string& v) {  // % !TEX key = value
  size_t k = l.find("!TEX");
  if (k == string::npos || l.find('%') > k || (k = l.find(key, k)) == string::npos || (k = l.find('=', k)) == string::npos)
    return false;
  size_t a = l.find_first_not_of(" \t", k + 1), e = l.find_last_not_of(" \t\r");
  if (a == string::npos || e < a) return false;
  v = l.substr(a, e - a + 1);
  return true;
}
static string g_texRoot, g_texRootProg;  // last main file built, used when a fragment is saved
static bool mentions(const string& path, const char* s) {  // does a (small) file contain s?
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  char line[512];
  bool hit = false;
  while (!hit && fgets(line, sizeof line, f)) hit = strstr(line, s);
  fclose(f);
  return hit;
}
static void texBuild(const Buf& b) {
  string file, prog, v;
  for (int i = 0; i < std::min(len(b.L), 20); i++) {
    if (texMagic(b.L[i], "root", v)) file = absPath(v[0] == '/' ? v : b.path.substr(0, b.path.rfind('/') + 1) + v);
    if (texMagic(b.L[i], "program", v)) {
      prog = v;
      for (auto& c : prog) c = (char)tolower((unsigned char)c);
    }
  }
  if (file.empty()) {
    bool main = false;  // a file with \documentclass is a main file; anything else is a fragment
    for (auto& l : b.L) {
      size_t k = l.find_first_not_of(" \t");
      if (k != string::npos && !l.compare(k, 14, "\\documentclass")) { main = true; break; }
    }
    if (main) file = b.path;
    else if (g_texRoot.empty()) { msg("Saved - not a main .tex file; add % !TEX root = main.tex to build"); return; }
    else { file = g_texRoot; if (prog.empty()) prog = g_texRootProg; }
  }
  g_texRoot = file; g_texRootProg = prog;
  if (g_texPid > 0) {  // a newer save wins
    kill(-g_texPid, SIGTERM);
    while (waitpid(g_texPid, nullptr, 0) < 0 && errno == EINTR) {}
    close(g_texFd); g_texFd = -1; g_texPid = -1;
  }
  string dir = file.substr(0, file.rfind('/') + 1), base = baseName(file), job = base.substr(0, base.rfind('.'));
  const char* eng = prog.find("xelatex") != string::npos ? "xelatex" : prog.find("lualatex") != string::npos ? "lualatex" : "pdflatex";
  const char* mode = eng[0] == 'x' ? "-pdfxe" : eng[0] == 'l' ? "-pdflua" : "-pdf";
  if (prog.empty()) {  // a latexmkrc that picks the engine wins over our default -pdf
    const char* h = getenv("HOME");
    string home = h ? h : "";
    for (const string& rc : {dir + "latexmkrc", dir + ".latexmkrc", home + "/.latexmkrc", home + "/.config/latexmk/latexmkrc"})
      if (mentions(rc, "pdf_mode")) mode = nullptr;
  }
  int pfd[2];
  if (pipe2(pfd, O_CLOEXEC)) { msg("LaTeX build: " + string(strerror(errno))); return; }
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);
    int dn = open("/dev/null", O_RDWR);  // keep latex output off the screen; errors are read from the .log
    dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
    fcntl(pfd[1], F_SETFD, 0);  // inherited by the whole build, so EOF only comes once it all exits
    if (chdir(dir.c_str())) _exit(126);
    const char* av[] = {"latexmk", "-interaction=nonstopmode", "-halt-on-error", "-file-line-error", base.c_str(), mode, nullptr};
    execvp(av[0], (char* const*)av);
    execlp(eng, eng, "-interaction=nonstopmode", "-halt-on-error", "-file-line-error", base.c_str(), (char*)nullptr);
    _exit(127);
  }
  close(pfd[1]);
  if (pid < 0) { close(pfd[0]); msg("LaTeX build: " + string(strerror(errno))); return; }
  setpgid(pid, pid);
  g_texPid = pid; g_texFd = pfd[0]; g_texT0 = nowMs();
  g_texPdf = dir + job + ".pdf"; g_texLog = dir + job + ".log";
  msg("Saved - building " + base + "...");
}
static string texError() {  // first error in the log (-file-line-error style, or "! message")
  string r;
  if (FILE* f = fopen(g_texLog.c_str(), "r")) {
    char line[1024];
    while (r.empty() && fgets(line, sizeof line, f)) {
      string s = line;
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      size_t c = s.find(':'), d = c == string::npos ? c : s.find_first_not_of("0123456789", c + 1);
      if (!s.compare(0, 2, "! ")) r = s.substr(2);
      else if (d != string::npos && d > c + 1 && !s.compare(d, 2, ": ")) r = baseName(s.substr(0, c)) + s.substr(c);
    }
    fclose(f);
  }
  return r.empty() ? "see " + baseName(g_texLog) : r;
}
static void texDone() {
  int st = -1;
  close(g_texFd); g_texFd = -1;
  while (waitpid(g_texPid, &st, 0) < 0 && errno == EINTR) {}
  g_texPid = -1;
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  long long ms = nowMs() - g_texT0;
  if (code == 127) msg("LaTeX build: neither latexmk nor pdflatex found");
  else if (code) msg("LaTeX build failed: " + texError());
  else msg("Built " + baseName(g_texPdf) + " (" + num(ms / 1000) + "." + num(ms / 100 % 10) + "s)");
  for (auto& b : B)  // reload the output if it is open in a tab; the scroll position is kept by page
    if (b.pdf && b.path == g_texPdf) {
      pdfInfo(b);
      b.page = std::max(1, std::min(b.page, b.pages));
      if (b.pdfText) pdfText(b, true);
    }
  pdfDrop();
}

// ───────────────────────── rendering ─────────────────────────
static const char *BG = "30;30;30", *CURL = "40;40;40", *SELB = "38;79;120", *FINDB = "98;51;21",
                  *GUT = "133;133;133", *GUTA = "198;198;198", *TABBAR = "37;37;38", *TABIN = "45;45;45",
                  *TABFG = "150;150;150", *WHITE = "255;255;255", *STAT = "0;122;204", *FGN = "212;212;212",
                  *SBBG = "37;37;38", *SBFG = "204;204;204", *SBHEAD = "187;187;187", *SELF = "4;57;94",
                  *SELU = "55;55;61", *CHEV = "197;197;197", *CRUMB = "169;169;169", *KEYBG = "51;51;51";
static const char *g_cf, *g_cb;
static void col(string& o, const char* f, const char* b) {
  if (f != g_cf) { o += "\x1b[38;2;"; o += f; o += 'm'; g_cf = f; }
  if (b != g_cb) { o += "\x1b[48;2;"; o += b; o += 'm'; g_cb = b; }
}
static void at(string& o, int row, int c) { o += "\x1b["; o += num(row); o += ';'; o += num(c); o += 'H'; }

struct TabHit { int x0, x1, cx; };
static vector<TabHit> g_tabs;
static int g_tabOff = 0, g_lnX0 = -1, g_lnX1 = -1, g_prevX = -1, g_nextX = -1;
static const char* g_pLabel = nullptr;
static string* g_pText = nullptr;
// layout: sidebar is columns [0, g_sbW); the editor starts at column EX and is EW wide
static int g_sbW = 0, g_sbWant = 30, EX = 0, EW = 80;
static bool g_sbShow = true, g_focusSb = false;

static void layout() {
  g_sbW = (g_sbShow && W >= 40) ? std::max(15, std::min(g_sbWant, W / 2)) : 0;
  EX = g_sbW; EW = W - EX;
}
static int gutterW(const Buf& b) {
  int d = 1;
  for (int n = len(b.L); n >= 10; n /= 10) d++;
  return std::max(d, 3) + 3;
}
static int ifind(const string& s, const string& q, int from) {
  int n = len(s), m = len(q);
  for (int i = std::max(0, from); i + m <= n; i++)
    if (!strncasecmp(s.data() + i, q.data(), m)) return i;
  return -1;
}

// ───────────────────────── explorer model ─────────────────────────
struct Node { string name; bool dir = false, open = false, loaded = false; vector<Node> kids; };
struct Row { Node* n; string path; int depth, parent; };
static Node g_root;
static string g_rootPath, g_branch, g_sbSelPath, g_lastActive, g_title;
static vector<Row> g_rows;
static int g_sbTop = 0, g_btnX[4] = {-9, -9, -9, -9};

static string join(const string& d, const string& n) { return d == "/" ? "/" + n : d + "/" + n; }
static string rootPrefix() { return g_rootPath == "/" ? "/" : g_rootPath + "/"; }
static bool isDirPath(const string& p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static void loadDir(Node& n, const string& path) {  // (re)reads a folder, keeping expanded sub-folders expanded
  vector<Node> old;
  old.swap(n.kids);
  if (DIR* d = opendir(path.c_str())) {
    while (dirent* e = readdir(d)) {
      string nm = e->d_name;
      if (nm == "." || nm == ".." || nm == ".git") continue;
      Node k;
      k.name = nm;
      k.dir = e->d_type == DT_DIR || ((e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) && isDirPath(join(path, nm)));
      n.kids.push_back(std::move(k));
    }
    closedir(d);
  }
  std::sort(n.kids.begin(), n.kids.end(), [](const Node& a, const Node& b) {
    if (a.dir != b.dir) return a.dir;
    int c = strcasecmp(a.name.c_str(), b.name.c_str());
    return c ? c < 0 : a.name < b.name;
  });
  for (auto& o : old)
    if (o.open)
      for (auto& k : n.kids)
        if (k.dir && k.name == o.name) { k.open = true; k.kids.swap(o.kids); loadDir(k, join(path, k.name)); break; }
  n.loaded = true;
}
static void flatten(Node& n, const string& path, int depth, int parent) {
  for (auto& k : n.kids) {
    int me = (int)g_rows.size();
    string p = join(path, k.name);
    g_rows.push_back({&k, p, depth, parent});
    if (k.dir && k.open) flatten(k, p, depth + 1, me);
  }
}
static void buildRows() { g_rows.clear(); if (g_root.open) flatten(g_root, g_rootPath, 0, -1); }
static int selIndex() {
  for (int i = 0; i < len(g_rows); i++)
    if (g_rows[i].path == g_sbSelPath) return i;
  return -1;
}
static void sbShow(int i) {  // scroll the tree so row i is visible
  int tv = H - 3;
  if (i < 0) return;
  if (i < g_sbTop) g_sbTop = i;
  if (i >= g_sbTop + tv) g_sbTop = i - tv + 1;
}
static void toggleNode(Node& n, const string& p) {
  if (!n.open) loadDir(n, p);  // re-read on expand so new files show up
  n.open = !n.open;
}
static bool reveal(const string& path) {  // expand the folders leading to path
  string pre = rootPrefix();
  if (g_rootPath.empty() || path.compare(0, pre.size(), pre)) return false;
  Node* n = &g_root;
  string p = g_rootPath, rel = path.substr(pre.size());
  g_root.open = true;
  for (size_t a = 0, b; (b = rel.find('/', a)) != string::npos; a = b + 1) {
    string c = rel.substr(a, b - a);
    if (!n->loaded) loadDir(*n, p);
    p = join(p, c);
    Node* next = nullptr;
    for (auto& k : n->kids)
      if (k.dir && k.name == c) next = &k;
    if (!next) return false;
    if (!next->loaded) loadDir(*next, p);
    next->open = true;
    n = next;
  }
  return true;
}
static void readBranch() {
  g_branch.clear();
  FILE* f = fopen((g_rootPath + "/.git/HEAD").c_str(), "r");
  if (!f) return;
  char buf[256] = {0};
  if (fgets(buf, sizeof buf, f)) {
    string s = buf;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    g_branch = !s.compare(0, 16, "ref: refs/heads/") ? s.substr(16) : s.substr(0, 7);
  }
  fclose(f);
}
static void refresh() { loadDir(g_root, g_rootPath); readBranch(); }

struct Icon { const char *g, *c; };
static Icon iconFor(const string& name) {  // Seti-style file icons
  static const struct { const char *e, *g, *c; } T[] = {
    {"c", "C ", "81;154;186"},  {"h", "h ", "160;116;196"}, {"cpp", "C+", "81;154;186"}, {"cc", "C+", "81;154;186"},
    {"cxx", "C+", "81;154;186"}, {"hpp", "h+", "160;116;196"}, {"hh", "h+", "160;116;196"}, {"py", "py", "81;154;186"},
    {"js", "JS", "203;203;65"}, {"mjs", "JS", "203;203;65"}, {"jsx", "JS", "81;154;186"}, {"ts", "TS", "81;154;186"},
    {"tsx", "TS", "81;154;186"}, {"json", "{}", "203;203;65"}, {"md", "M↓", "81;154;186"}, {"sh", "$_", "141;193;73"},
    {"bash", "$_", "141;193;73"}, {"zsh", "$_", "141;193;73"}, {"txt", "≡ ", "204;204;204"}, {"pdf", "PD", "204;62;68"},
    {"html", "<>", "227;121;51"}, {"css", "# ", "81;154;186"}, {"rs", "rs", "227;121;51"}, {"go", "go", "81;154;186"},
    {"java", "J ", "204;62;68"}, {"yml", "! ", "160;116;196"}, {"yaml", "! ", "160;116;196"}, {"toml", "≡ ", "109;128;134"},
    {"png", "▣ ", "160;116;196"}, {"jpg", "▣ ", "160;116;196"}, {"svg", "▣ ", "203;203;65"}, {"xml", "<>", "227;121;51"},
    {"cs", "C#", "141;193;73"}, {"lua", "lu", "81;154;186"}, {"cmake", "M ", "227;121;51"}};
  if (name == "Makefile" || name == "makefile" || name == "CMakeLists.txt") return {"M ", "227;121;51"};
  if (name[0] == '.') return {"◆ ", "109;128;134"};
  string e = extOf(name);
  for (auto& t : T)
    if (e == t.e) return {t.g, t.c};
  return {"· ", "109;128;134"};
}
static string relPath(const string& p) {
  string pre = rootPrefix();
  if (!g_rootPath.empty() && !p.compare(0, pre.size(), pre)) return p.substr(pre.size());
  return p;
}

static unsigned char palQuant(const unsigned char* p, int x, int y) {
  int r = p[0], g = p[1], b = p[2];
  if (std::max({r, g, b}) - std::min({r, g, b}) < 24) return (unsigned char)(((r + g + b) * 15 + 382) / 765);
  static const signed char bayer[16] = {-24, 0, -18, 6, 12, -12, 18, -6, -15, 9, -21, 3, 21, -3, 15, -9};
  int d = bayer[(y & 3) * 4 + (x & 3)];  // ordered dither, so colour gradients don't band on the 6-level cube
  auto q = [d](int v) { return std::max(0, std::min(5, (v + d) * 5 / 255 + ((v + d) * 5 % 255 > 127))); };
  return (unsigned char)(16 + q(r) * 36 + q(g) * 6 + q(b));
}
static int palRGB(int c, int k) {  // channel k (0 r, 1 g, 2 b) of palette entry c, 0-255
  if (c == PAL_BG) return 30;
  if (c < 16) return c * 17;
  c -= 16;
  return (k == 0 ? c / 36 : k == 1 ? c / 6 % 6 : c % 6) * 51;
}
static void packRow(string& out, const unsigned char* q, int n) {
  for (int i = 0; i < n;) {
    int r = 1;
    while (i + r < n && r < 130 && q[i + r] == q[i]) r++;
    if (r >= 3) { out += (char)(r + 125); out += (char)q[i]; i += r; continue; }
    int j = i;  // literals up to the next run of 3
    while (j < n && j - i < 128 && !(j + 2 < n && q[j] == q[j + 1] && q[j] == q[j + 2])) j++;
    out += (char)(j - i - 1);
    out.append((const char*)q + i, j - i);
    i = j;
  }
}
// Decodes document row y from the cache into dst[0, w), starting at document column x0.
static void unpackRow(int y, int x0, int w, unsigned char* dst) {
  const string& band = g_win[y / 6 - g_wb0];
  if (band.empty()) { memset(dst, PAL_BG, w); return; }  // not rendered (never expected)
  const unsigned char* p = (const unsigned char*)band.data();
  x0 -= g_wx0;
  for (int r = 0; r <= y % 6; r++)
    for (int x = 0; x < g_wxw;) {
      int n = *p++, k = n >= 128 ? n - 125 : n + 1, a = std::max(x, x0), e = std::min(x + k, x0 + w);
      if (r == y % 6)
        for (int i = a; i < e; i++) dst[i - x0] = n >= 128 ? *p : p[i - x];
      p += n >= 128 ? 1 : k;
      x += k;
    }
}

// ── continuous layout ──
static int pdfPage(int y) {  // index of the page at document row y (or of the gap below it)
  return std::max(0, (int)(std::upper_bound(g_ptop.begin(), g_ptop.end(), y) - g_ptop.begin()) - 1);
}
static int pdfViewH() { return std::min(g_docH, (H - 3) * cellH / 6 * 6); }  // whole sixel rows only
static int pdfY(const Buf& b) {
  int i = std::max(0, std::min((int)b.pos, len(g_ptop) - 1));
  return g_ptop[i] + iround((b.pos - i) * (g_ph[i] + g_gap));
}
static void pdfSetY(Buf& b, int y) {
  int dh = pdfViewH();
  y = std::max(0, std::min(y, g_docH - dh));
  int i = pdfPage(y);
  b.pos = i + (double)(y - g_ptop[i]) / (g_ph[i] + g_gap);
  b.page = pdfPage(y + dh / 2) + 1;
}
// Lays the pages out for the current editor width and zoom; returns the view height in pixels.
static int pdfGeo(Buf& b) {
  if (!cellW || !cellH) { cellW = 8; cellH = 16; }
  int aw = EW * cellW;
  string key = num(b.id) + "/" + b.ver + "/" + num(b.zoom) + "/" + num(aw) + "/" + num(cellH);
  if (key != g_geoKey) {
    string ik = g_imgKey;
    pdfDrop();
    g_imgKey = ik;  // still on screen
    g_geoKey = key;
    float mw = 1;
    for (auto& z : b.psz) mw = std::max(mw, z.w);
    double s = aw / mw * b.zoom / 100.0;
    int n = len(b.psz), y = 0;
    g_ptop.resize(n); g_pw.resize(n); g_ph.resize(n);
    g_gap = std::max(2, cellH / 2);
    g_docW = 1;
    for (int i = 0; i < n; i++) {
      g_pw[i] = std::max(1, (int)(b.psz[i].w * s + 1e-3)); g_ph[i] = std::max(1, (int)(b.psz[i].h * s + 1e-3));
      g_ptop[i] = y;
      y += g_ph[i] + g_gap;
      g_docW = std::max(g_docW, g_pw[i]);
    }
    g_docH = y - g_gap;
  }
  return pdfViewH();
}

// Renders document rows [y0, y1) (y0 a multiple of 6) into the cache: one pdftoppm run per page.
static bool pdfRender(Buf& b, int y0, int y1) {
  vector<unsigned char> band((size_t)g_wxw * 6), q(g_wxw), row;
  int y = y0;
  auto put = [&] {  // q is document row y
    memcpy(band.data() + (size_t)(y % 6) * g_wxw, q.data(), g_wxw);
    if (++y % 6 && y != g_docH) return;
    string& d = g_win[(y - 1) / 6 - g_wb0];
    d.clear();
    for (int r = 0; r <= (y - 1) % 6; r++) packRow(d, band.data() + (size_t)r * g_wxw, g_wxw);
  };
  while (y < y1) {
    int i = pdfPage(y), top = g_ptop[i], bot = top + g_ph[i];
    int px = (g_docW - g_pw[i]) / 2, a = std::max(g_wx0, px), z = std::min(g_wx0 + g_wxw, px + g_pw[i]);
    std::fill(q.begin(), q.end(), (unsigned char)PAL_BG);
    if (y >= bot || a >= z || a > g_wx0 || z < g_wx0 + g_wxw) g_palUsed[PAL_BG] = true;
    if (y >= bot || a >= z) {  // gap between pages, or the page is off to the side
      int e = std::min(y1, y < bot ? bot : i + 1 < len(g_ptop) ? g_ptop[i + 1] : g_docH);
      while (y < e) put();
      continue;
    }
    int e = std::min(y1, bot), cw = z - a, w = -1;
    string pg = num(i + 1), hdr;
    row.resize((size_t)cw * 3);
    size_t fill = 0;
    bool t = b.psz[i].turned;  // -scale-to-x/y name the sides before the page's rotation
    runEach({"pdftoppm", "-f", pg, "-l", pg, "-scale-to-x", num(t ? g_ph[i] : g_pw[i]), "-scale-to-y", num(t ? g_pw[i] : g_ph[i]),
             "-x", num(a - px), "-y", num(y - top), "-W", num(cw), "-H", num(e - y), b.path},
            [&](const char* d, size_t n) {
              while (n && w < 0) {  // "P6\nW H\n255\n"
                hdr += *d++; n--;
                if (std::count(hdr.begin(), hdr.end(), '\n') == 3) {
                  int h;
                  w = sscanf(hdr.c_str(), "P6 %d %d", &w, &h) == 2 && w == cw ? w : 0;
                }
              }
              while (n && w > 0 && y < e) {
                size_t k = std::min(n, row.size() - fill);
                memcpy(row.data() + fill, d, k);
                d += k; n -= k; fill += k;
                if (fill < row.size()) break;
                fill = 0;
                for (int x = 0; x < cw; x++) g_palUsed[q[a - g_wx0 + x] = palQuant(row.data() + 3 * x, a + x, y)] = true;
                put();
              }
            });
    if (y < e) return false;
  }
  return true;
}
static void flushBig(string& o) {  // keep the frame buffer small while streaming a sixel
  if (o.size() > 65536) { wr(o); o.clear(); }
}
static void sixelImage(string& o, int x0, int y0, int w, int h) {
  o += "\x1bPq\"1;1;" + num(w) + ";" + num(h);
  for (int c = 0; c < PAL_N; c++)
    if (g_palUsed[c]) {
      o += "#" + num(c) + ";2";
      for (int k = 0; k < 3; k++) o += ";" + num((palRGB(c, k) * 100 + 127) / 255);
    }
  vector<unsigned char> q((size_t)w * 6);
  for (int y = y0; y < y0 + h; y += 6) {
    int rows = std::min(6, y0 + h - y);
    bool has[PAL_N] = {};
    for (int r = 0; r < rows; r++) {
      unpackRow(y + r, x0, w, q.data() + (size_t)r * w);
      for (int x = 0; x < w; x++) has[q[(size_t)r * w + x]] = true;
    }
    for (int c = 0; c < PAL_N; c++) {
      if (!has[c]) continue;
      o += "#" + num(c);
      int run = 0, gap = 0;  // pending run of `last`; empty columns not yet sent (trailing ones never are)
      char last = 0;
      auto rle = [&](int n, char ch) {
        if (n > 3) o += "!" + num(n) + ch;
        else o.append(n, ch);
      };
      for (int x = 0; x < w; x++) {
        int bits = 0;
        for (int r = 0; r < rows; r++) bits |= (q[(size_t)r * w + x] == c) << r;
        if (!bits) {
          if (run) { rle(run, last); run = 0; }
          gap++;
          continue;
        }
        char ch = (char)(63 + bits);
        if (run && ch == last) { run++; continue; }
        if (run) rle(run, last);
        rle(gap, '?'); gap = 0;
        last = ch; run = 1;
      }
      if (run) rle(run, last);
      o += '$';
    }
    o += '-';
    flushBig(o);
  }
  o += "\x1b\\";
}

// Draws the part of the document in view as a sixel. Returns false if it fell back to text.
static bool drawPdf(string& o, Buf& b, int th, bool fresh = false) {
  bool building = g_texPid > 0 && b.path == g_texPdf;  // the file is being rewritten; wait for texDone
  auto blank = [&] {
    col(o, FGN, BG);
    for (int r = 0; r < th; r++) { at(o, r + 3, EX + 1); o.append(EW, ' '); }
  };
  int dh = pdfGeo(b), aw = EW * cellW, dw = std::min(g_docW, aw);
  dh = std::min(dh, th * cellH / 6 * 6);
  if (dw <= 0 || dh <= 0) return true;
  b.panX = std::max(0, std::min(b.panX, g_docW - dw));
  int y = std::max(0, std::min(pdfY(b), g_docH - dh));
  pdfSetY(b, y);
  string pk = g_geoKey + "/" + num(th) + "/" + num(EX), ik = pk + "/" + num(b.panX) + "/" + num(y);
  if (ik == g_imgKey) return true;  // a sixel stays on screen as long as nothing is drawn over it
  bool moved = g_imgKey.compare(0, pk.size() + 1, pk + "/") != 0;  // new size or place, not just a scroll

  if (b.panX < g_wx0 || b.panX + dw > g_wx0 + g_wxw) {  // panned sideways out of the cached columns
    vector<string>().swap(g_win);
    g_wx0 = b.panX;
    g_wxw = dw;
  }
  // The cache window: the view plus 20% of its height, placed ahead in the direction of scrolling. It
  // only moves once a row in view is missing, so a render (~50-70 ms of pdftoppm) is needed about once
  // per 20% of a screen scrolled rather than on every step.
  int m = dh / 5, dir = g_lastY < 0 ? 0 : y > g_lastY ? 1 : y < g_lastY ? -1 : 0, nw = len(g_win);
  bool need = y / 6 < g_wb0 || (y + dh + 5) / 6 > g_wb0 + nw;
  for (int i = y / 6; !need && i < (y + dh + 5) / 6; i++) need = g_win[i - g_wb0].empty();
  int ya = g_wb0 * 6, yb = std::min(g_docH, (g_wb0 + nw) * 6);
  if (need || nw * 6 > dh + m + 12 || g_holdA >= 0) {
    ya = dir > 0 ? y : dir < 0 ? y - m : y - m / 2;
    yb = ya + dh + m;
    if (ya < 0) { yb -= ya; ya = 0; }
    if (yb > g_docH) { ya = std::max(0, ya - (yb - g_docH)); yb = g_docH; }
    if (g_holdA >= 0) { ya = std::max(0, std::min(ya, g_holdA - m)); yb = std::min(g_docH, std::max(yb, g_holdB + m)); }
  }
  int t0 = ya / 6, t1 = (yb + 5) / 6;
  vector<string> win(t1 - t0);  // slide the window: keep the overlap, drop the rest
  for (int i = std::max(t0, g_wb0); i < std::min(t1, g_wb0 + len(g_win)); i++) win[i - t0].swap(g_win[i - g_wb0]);
  g_win.swap(win);
  g_wb0 = t0;
  vector<string>().swap(win);
  int r0 = t0, r1 = t1;
  while (r0 < r1 && !g_win[r0 - t0].empty()) r0++;
  while (r1 > r0 && !g_win[r1 - 1 - t0].empty()) r1--;
  if (r0 < r1) {
    bool seen = true;  // is everything in view cached?
    for (int i = y / 6; i < (y + dh + 5) / 6; i++) seen &= !g_win[i - t0].empty();
    if (building) {
      if (!seen) {
        if (g_imgKey.empty()) blank();
        return true;
      }
    } else if (!fresh && fileVer(b.path) != b.ver && !fileVer(b.path).empty()) {  // rewritten on disk
      pdfInfo(b);
      return drawPdf(o, b, th, true);
    } else if (!pdfRender(b, r0 * 6, std::min(g_docH, r1 * 6))) {
      pdfDrop();
      msg("Could not render PDF page (is poppler-utils installed?) - showing text");
      pdfText(b, true);
      return false;
    }
  }
  if (moved) blank();  // clears the old image; a scroll is drawn over it in place
  at(o, 3, EX + 1 + (aw - dw) / 2 / cellW);
  sixelImage(o, b.panX, y, dw, dh);
  g_imgKey = ik;
  g_lastY = y;
  return true;
}

static void drawSidebar(string& o) {
  int sw = g_sbW, tv = H - 3;
  g_btnX[0] = g_btnX[1] = g_btnX[2] = g_btnX[3] = -9;
  if (!sw) return;
  buildRows();
  int n = len(g_rows), si = selIndex();
  g_sbTop = std::max(0, std::min(g_sbTop, std::max(0, n - tv)));
  // header: title + New File / New Folder / Refresh / Collapse buttons
  string left = " EXPLORER", right = sw >= 24 ? " +  ⊞  ↻  ⊟ " : "";
  int rw = dispW(right);
  at(o, 1, 1);
  col(o, SBHEAD, SBBG);
  left = fitW(left, sw - rw);
  o += left;
  o.append(sw - rw - dispW(left), ' ');
  if (rw) {
    col(o, CHEV, SBBG);
    o += right;
    for (int i = 0; i < 4; i++) g_btnX[i] = sw - rw + 1 + 3 * i;
  }
  // folder section header
  string sec = string(g_root.open ? " ▾ " : " ▸ ") + g_root.name;
  for (size_t i = 3; i < sec.size(); i++) sec[i] = (char)toupper((unsigned char)sec[i]);
  sec = fitW(sec, sw);
  at(o, 2, 1);
  col(o, SBFG, SBBG);
  o += "\x1b[1m" + sec + "\x1b[22m";
  o.append(sw - dispW(sec), ' ');
  for (int r = 0; r < tv; r++) {
    int j = g_sbTop + r;
    at(o, r + 3, 1);
    if (j >= n) { col(o, SBFG, SBBG); o.append(sw, ' '); continue; }
    const Row& R = g_rows[j];
    const char* bg = j == si ? (g_focusSb ? SELF : SELU) : SBBG;
    int used = std::min(1 + 2 * R.depth, std::max(0, sw - 6));
    col(o, SBFG, bg);
    o.append(used, ' ');
    if (R.n->dir) { col(o, CHEV, bg); o += R.n->open ? "▾ " : "▸ "; used += 2; }
    else { Icon ic = iconFor(R.n->name); col(o, ic.c, bg); o += ic.g; o += ' '; used += 3; }
    string nm = fitW(R.n->name, std::max(0, sw - used - 1));
    col(o, SBFG, bg);
    o += nm;
    used += dispW(nm);
    o.append(std::max(0, sw - used), ' ');
  }
}

static void drawWatermark(string& o, int th) {  // empty editor group, like VS Code
  static const char* T[][2] = {{"Show Explorer", "Ctrl+E"},   {"Open File", "Ctrl+O"},     {"New Untitled File", "Ctrl+N"},
                               {"Toggle Sidebar", "Ctrl+B"},  {"Find in File", "Ctrl+F"}, {"Quit", "Ctrl+Q"}};
  int n = 6, top = std::max(0, (th - n * 2) / 2), w = 30, pad = std::max(0, (EW - w) / 2);
  for (int r = 0; r < th; r++) {
    at(o, r + 3, EX + 1);
    col(o, FGN, BG);
    int li = r - top;
    if (li < 0 || li % 2 || li / 2 >= n || EW < w) { o.append(EW, ' '); continue; }
    string a = T[li / 2][0], k = T[li / 2][1];
    o.append(pad, ' ');
    col(o, GUT, BG);
    o += a;
    o.append(22 - len(a), ' ');
    col(o, SBFG, KEYBG);
    o += " " + k + " ";
    col(o, FGN, BG);
    o.append(EW - pad - 22 - len(k) - 2, ' ');
  }
}

static void draw() {
  if (g_resized) { g_resized = 0; getSize(); }
  layout();
  bool has = !B.empty();
  int th = H - 3;
  if (has && B[cur].path != g_lastActive) {  // explorer follows the active editor
    g_lastActive = B[cur].path;
    if (reveal(g_lastActive)) { g_sbSelPath = g_lastActive; buildRows(); sbShow(selIndex()); }
  }
  string o;
  o.reserve((size_t)W * H * 8);
  g_cf = g_cb = nullptr;
  o += "\x1b[?25l\x1b[0m";
  string title = (has ? (B[cur].dirty() ? "● " : "") + B[cur].name + " - " : string()) + g_root.name + " - vsc";
  if (title != g_title) { g_title = title; o += "\x1b]2;" + title + "\x07"; }
  drawSidebar(o);

  // ── tab bar (row 1, right of the sidebar) ──
  g_tabs.assign(B.size(), TabHit{-1, -1, -1});
  vector<string> lab(B.size());
  vector<int> tw(B.size());
  for (size_t i = 0; i < B.size(); i++) {
    Icon ic = iconFor(B[i].name);
    lab[i] = string("  ") + ic.g + " " + fitW(B[i].name, 30) + (B[i].dirty() ? "  ● " : "  × ");
    tw[i] = dispW(lab[i]);
  }
  if (cur < g_tabOff) g_tabOff = cur;
  for (;;) {
    int sum = 0;
    for (int j = g_tabOff; j <= cur && has; j++) sum += tw[j] + 1;
    if (sum <= EW || g_tabOff >= cur) break;
    g_tabOff++;
  }
  at(o, 1, EX + 1);
  int x = 0;
  for (int i = g_tabOff; i < (int)B.size(); i++) {
    if (x + tw[i] > EW) break;
    bool a = i == cur;
    const char* bg = a ? BG : TABIN;
    Icon ic = iconFor(B[i].name);
    col(o, a ? WHITE : TABFG, bg);
    o += "  ";
    col(o, ic.c, bg);
    o += ic.g;
    col(o, a ? WHITE : TABFG, bg);
    o += lab[i].substr(2 + strlen(ic.g));
    g_tabs[i] = {EX + x, EX + x + tw[i], EX + x + tw[i] - 2};
    x += tw[i];
    if (x < EW) { col(o, TABFG, TABBAR); o += ' '; x++; }
  }
  col(o, TABFG, TABBAR);
  o.append(EW - x, ' ');

  // ── breadcrumbs (row 2) ──
  at(o, 2, EX + 1);
  {
    string bc;
    if (has) {
      string rp = B[cur].path.empty() ? B[cur].name : relPath(B[cur].path);
      for (size_t a = 0, b; (b = rp.find('/', a)) != string::npos; a = b + 1) bc += rp.substr(a, b - a) + " › ";
      bc = fitW(bc, std::max(0, EW - 2));
    }
    col(o, CRUMB, BG);
    o += "  " + bc;
    int used = 2 + dispW(bc);
    if (has && used + 4 < EW) {
      Icon ic = iconFor(B[cur].name);
      string nm = fitW(B[cur].name, EW - used - 3);
      col(o, ic.c, BG); o += ic.g;
      col(o, SBFG, BG); o += " " + nm;
      used += 3 + dispW(nm);
    }
    o.append(std::max(0, EW - used), ' ');
  }

  // ── editor body (rows 3 .. H-1) ──
  bool img = has && B[cur].pdf && !B[cur].pdfText;
  if (!img && !g_imgKey.empty()) pdfDrop();  // the text drawn over the sixel erases it
  if (img && !drawPdf(o, B[cur], th)) img = false;
  int crow = -1, ccol = 0;
  if (!has) drawWatermark(o, th);
  else if (!img) {
    Buf& b = B[cur];
    int n = len(b.L), gw = gutterW(b), txw = EW - gw;
    const string& cl = b.L[std::min(b.cy, n - 1)];
    if (b.follow) {
      if (b.cy < b.top) b.top = b.cy;
      if (b.cy >= b.top + th) b.top = b.cy - th + 1;
      int rc = rcol(cl, b.cx);
      if (rc < b.left) b.left = rc;
      if (rc >= b.left + txw) b.left = rc - txw + 1;
    }
    b.top = std::max(0, std::min(b.top, n - 1));
    int sy0 = 0, sx0 = 0, sy1 = 0, sx1 = 0;
    bool hs = selRange(b, sy0, sx0, sy1, sx1);
    static vector<unsigned char> cls;
    static vector<char> fm;
    const Lang& lg = LANGS[b.lang];
    for (int r = 0; r < th; r++) {
      int y = b.top + r;
      at(o, r + 3, EX + 1);
      if (y >= n) { col(o, FGN, BG); o.append(EW, ' '); continue; }
      bool isCur = y == b.cy;
      const char* rowbg = isCur && !hs ? CURL : BG;
      string ln = num(y + 1);
      col(o, isCur ? GUTA : GUT, BG);
      o.append(std::max(0, gw - 2 - len(ln)), ' ');
      o += ln;
      o += "  ";
      const string& s = b.L[y];
      cls.assign(s.size() + 1, C_N);
      if (b.lang) hl(lg, s, stAt(b, y), cls.data());
      fm.assign(s.size() + 1, 0);
      if (!g_findQ.empty())
        for (int p = ifind(s, g_findQ, 0); p >= 0; p = ifind(s, g_findQ, p + len(g_findQ)))
          memset(fm.data() + p, 1, g_findQ.size());
      int a = -1, e = -1;
      bool eol = false;
      if (hs && y >= sy0 && y <= sy1) { a = y == sy0 ? sx0 : 0; e = y == sy1 ? sx1 : len(s); eol = y < sy1; }
      int rc = 0, used = 0;
      for (int i = 0; i < len(s) && used < txw; i = nx(s, i)) {
        int w = chw(s, i, rc);
        if (rc + w <= b.left) { rc += w; continue; }
        const char* bg = (i >= a && i < e) ? SELB : fm[i] ? FINDB : rowbg;
        col(o, FG[cls[i]], bg);
        unsigned char c = s[i];
        int vis = std::min(rc + w, b.left + txw) - std::max(rc, b.left);
        if (c == '\t' || rc < b.left || rc + w > b.left + txw) o.append(vis, ' ');
        else if (c < 32 || c == 127) o += '?';
        else o.append(s, i, nx(s, i) - i);
        used += vis;
        rc += w;
      }
      if (eol && used < txw && rc >= b.left) { col(o, FGN, SELB); o += ' '; used++; }
      col(o, FGN, rowbg);
      o.append(std::max(0, txw - used), ' ');
      if (isCur) {
        int cr = rcol(s, b.cx) - b.left;
        if (cr >= 0 && cr < txw) { crow = r + 3; ccol = EX + gw + cr + 1; }
      }
    }
  }

  // ── status bar ──
  string left = g_branch.empty() ? "" : " ⎇ " + g_branch + " ", right;
  g_prevX = g_nextX = g_lnX0 = g_lnX1 = -1;
  if (has) {
    Buf& b = B[cur];
    int sel = 0, y0, x0, y1, x1;
    if (!img && selRange(b, y0, x0, y1, x1))
      for (int y = y0; y <= y1; y++) {
        const string& s = b.L[y];
        int a = y == y0 ? x0 : 0, e = y == y1 ? x1 : len(s);
        for (int i = a; i < e; i++) sel += !cont(s[i]);
        sel += y < y1;
      }
    if (img) {
      right = " ◀  Page " + num(b.page) + " / " + num(b.pages) + "  ▶    Zoom " + num(b.zoom) + "%    Ctrl+T text    PDF ";
    } else {
      string lc = "Ln " + num(b.cy + 1) + ", Col " + num(rcol(b.L[b.cy], b.cx) + 1);
      if (sel) lc += " (" + num(sel) + " selected)";
      right = lc + "    Spaces: 4    UTF-8    " + (b.crlf ? "CRLF" : "LF") + "    " + b.langName + " ";
      if (b.pdf) {
        int pg = (int)(std::upper_bound(b.ps.begin(), b.ps.end(), b.cy) - b.ps.begin());
        b.page = std::max(1, pg);
        right = "Page " + num(b.page) + " / " + num(b.pages) + "    " + lc + "    PDF (text) ";
      }
    }
    int rw = std::min(dispW(right), W);
    right = fitW(right, rw);
    int rx = W - rw;
    if (img) {
      g_prevX = rx + 1;
      g_nextX = rx + dispW(" ◀  Page " + num(b.page) + " / " + num(b.pages) + "  ");
      g_lnX0 = g_prevX + 3; g_lnX1 = g_nextX - 1;
    } else {
      size_t k = right.find("Ln ");
      if (k != string::npos) {
        g_lnX0 = rx + dispW(right.substr(0, k));
        g_lnX1 = g_lnX0 + dispW(right.substr(k, right.find("    ", k) - k));
      }
    }
    if (b.ro && !g_pText && g_msg.empty()) left += " [Read-only]";
  }
  if (g_texPid > 0) left += " ⟳ LaTeX";
  int rw = dispW(right), pcol = 0;
  if (g_pText) {
    left += string(" ") + g_pLabel + *g_pText;
    pcol = dispW(left) + 1;
    if (!g_msg.empty()) left += "    " + g_msg;
  } else if (!g_msg.empty()) left += " " + g_msg;
  left = fitW(left, std::max(0, W - rw - 1));
  at(o, H, 1);
  col(o, WHITE, STAT);
  o += left;
  o.append(W - rw - dispW(left), ' ');
  o += right;
  o += "\x1b[0m";
  if (g_pText) { at(o, H, std::min(pcol, W)); o += "\x1b[?25h"; }
  else if (crow > 0 && !g_focusSb) { at(o, crow, ccol); o += "\x1b[?25h"; }
  wr(o);
}


// ───────────────────────── PDF scrolling ─────────────────────────
// Scrolls to document row `to`, easing over `ms` (a frame is drawn whenever the last one is done). For
// moves past the 20% kept ahead, the rows passed through are rendered up front in one go, as each
// pdftoppm run costs ~50-80 ms however few rows it makes (rendering per frame would give ~8 fps); the
// cache shrinks back to the view + 20% afterwards. A key press ends the animation at its target.
static void pdfScrollTo(Buf& b, int to, int ms = 150) {
  int dh = pdfGeo(b), from = pdfY(b);
  to = std::max(0, std::min(to, g_docH - dh));
  if (to == from) return;
  if (std::abs(to - from) > 2 * dh || !g_gfx || b.pdfText) { pdfSetY(b, to); return; }
  if (std::abs(to - from) > dh / 5) { g_holdA = std::min(from, to); g_holdB = std::max(from, to) + dh; }
  long long t0 = nowMs();
  for (;;) {
    double t = (nowMs() - t0) / (double)ms;
    int y = t >= 1 ? to : from + iround((to - from) * (1 - (1 - t) * (1 - t) * (1 - t)));
    pdfSetY(b, y);
    draw();
    if (y == to) break;
    pollfd p{0, POLLIN, 0};
    if (ibP < ibN || poll(&p, 1, 4) > 0) { pdfSetY(b, to); break; }
  }
  g_holdA = g_holdB = -1;
}
static void pdfScrollBy(Buf& b, int d) { pdfGeo(b); pdfScrollTo(b, pdfY(b) + d, 80); }  // wheel, arrows
static void pdfGo(Buf& b, int p) {  // to the top of page p (1-based)
  pdfGeo(b);
  p = std::max(1, std::min(p, len(g_ptop)));
  pdfScrollTo(b, g_ptop[p - 1]);
}

// ───────────────────────── prompts ─────────────────────────
// cb(ev): ev 0 = text changed, 1 = Enter, 2 = previous (Up / Shift+F3), 3 = next (Down / F3).
// Returns true from cb to close the prompt as accepted.
template <class F>
static bool prompt(const char* label, string& s, F cb) {
  g_pLabel = label; g_pText = &s;
  bool ok = false;
  for (;;) {
    if (ibP >= ibN) draw();
    int k = readKey();
    if (k == K_NONE || k == K_MOUSE) continue;
    unsigned gen = g_msgGen;
    if (k == K_ESC || k == 3 || k == 17) break;
    int ev = -1;
    if (k == 13) ev = 1;
    else if (k == K_UP || k == (K_F3 | M_SHIFT)) ev = 2;
    else if (k == K_DOWN || k == K_F3) ev = 3;
    else if (k == 127 || k == 8) { if (!s.empty()) s.erase(pv(s, len(s))); ev = 0; }
    else if (k == K_PASTE) { s += pasteBuf.substr(0, pasteBuf.find('\n')); ev = 0; }
    else if (k >= 32 && k < 256) { s += (char)k; if (k >= 0xC0) utf8Tail(s, k); ev = 0; }
    if (ev >= 0 && cb(ev)) { ok = true; break; }
    if (g_msgGen == gen) g_msg.clear();
  }
  g_pText = nullptr;
  return ok;
}
static bool findFrom(Buf& b, const string& q, int y, int x, int dir) {
  int n = len(b.L);
  for (int k = 0; k <= n; k++) {
    int yy = ((y + dir * k) % n + n) % n, pos = -1;
    const string& s = b.L[yy];
    if (dir > 0) {
      pos = ifind(s, q, k == 0 ? x : 0);
      if (k == n && pos >= x) pos = -1;
    } else {
      int lim = k == 0 ? x : INT_MAX;
      for (int p = ifind(s, q, 0); p >= 0 && p < lim; p = ifind(s, q, p + 1)) pos = p;
    }
    if (pos >= 0) {
      b.sel = true; b.ay = yy; b.ax = pos; b.cy = yy; b.cx = pos + len(q); b.follow = true;
      return true;
    }
  }
  return msg("No results");
}
static void findPrevNext(Buf& b, const string& q, bool prev) {
  int y0 = b.cy, x0 = b.cx, y1, x1;
  if (prev && !selRange(b, y0, x0, y1, x1)) { y0 = b.cy; x0 = b.cx; }
  if (prev) findFrom(b, q, y0, x0, -1);
  else findFrom(b, q, b.cy, b.cx, 1);
}
static void doFind(Buf& b) {
  string q = g_lastFind;
  int y0 = b.cy, x0 = b.cx, y1, x1;
  if (selRange(b, y0, x0, y1, x1) && y0 == y1) q = b.L[y0].substr(x0, x1 - x0);
  else { y0 = b.cy; x0 = b.cx; }
  g_findQ = q;
  prompt("Find: ", q, [&](int ev) {
    g_findQ = g_lastFind = q;
    if (q.empty()) return false;
    if (ev == 0) findFrom(b, q, y0, x0, 1);
    else findPrevNext(b, q, ev == 2);
    return false;
  });
  g_findQ.clear();
}
static void doGoto(Buf& b) {
  string s;
  bool page = b.pdf && !b.pdfText;
  prompt(page ? "Go to page: " : "Go to line[:col]: ", s, [&](int ev) {
    if (ev != 1) return false;
    int l = 0, c = 0;
    sscanf(s.c_str(), "%d:%d", &l, &c);
    if (page) { pdfGo(b, l); return true; }
    if (l < 0) l = len(b.L) + l + 1;
    b.cy = std::max(0, std::min(l - 1, len(b.L) - 1));
    b.cx = c > 0 ? xfromr(b.L[b.cy], c - 1) : firstNonWs(b.L[b.cy]);
    b.sel = false; b.follow = true;
    int th = H - 2;  // centre the target like VS Code
    b.top = std::max(0, b.cy - th / 2);
    return true;
  });
}

// ───────────────────────── files & tabs ─────────────────────────
static void openFile(const string& path) {
  for (size_t i = 0; i < B.size(); i++)
    if (B[i].path == path) { cur = (int)i; return; }
  struct stat stt;
  if (stat(path.c_str(), &stt) == 0 && S_ISDIR(stt.st_mode)) { msg(path + " is a directory"); return; }
  Buf nb;
  nb.id = ++g_ids;
  nb.path = path;
  nb.name = baseName(path);
  setLang(nb);
  FILE* f = fopen(path.c_str(), "rb");
  char magic[5] = {0};
  if (f && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "%PDF", 4)) {
    fclose(f);
    nb.pdf = nb.ro = true;
    nb.langName = "PDF";
    nb.pages = 0;
    pdfInfo(nb);
    if (nb.pages <= 0) { msg("Cannot read PDF (install poppler-utils)"); nb.pages = 1; }
    B.push_back(std::move(nb));
    cur = (int)B.size() - 1;
    if (!g_gfx) pdfText(B[cur], true);
    else if (!g_msg.size()) msg("Page blank? Press Ctrl+T for text view, or run with VSC_GFX=0");
    return;
  }
  if (f) {
    rewind(f);
    string d;
    char buf[65536];
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) d.append(buf, k);
    fclose(f);
    if (d.find('\0') < std::min<size_t>(d.size(), 8000)) { nb.ro = true; msg("Binary file - opened read-only"); }
    nb.L.clear();
    nb.L.reserve(std::count(d.begin(), d.end(), '\n') + 1);
    for (size_t p = 0;;) {
      size_t q = d.find('\n', p);
      if (q == string::npos) { nb.L.push_back(d.substr(p)); break; }
      nb.L.push_back(d.substr(p, q - p));
      p = q + 1;
    }
    if (!nb.L[0].empty() && nb.L[0].back() == '\r') nb.crlf = true;
    if (nb.crlf)
      for (auto& l : nb.L)
        if (!l.empty() && l.back() == '\r') l.pop_back();
  } else if (errno != ENOENT) {
    msg("Cannot open " + path + ": " + strerror(errno));
    return;
  }
  // replace a lone, untouched Untitled tab
  if (B.size() == 1 && B[0].path.empty() && !B[0].dirty() && B[0].L.size() == 1 && B[0].L[0].empty()) B.clear();
  B.push_back(std::move(nb));
  cur = (int)B.size() - 1;
}
static bool save(Buf& b) {
  if (b.pdf || b.ro) return msg("Read-only - not saved");
  string path = b.path;
  if (path.empty()) {  // Save as: only adopt the new name once the write succeeds
    string p;
    if (!prompt("Save as: ", p, [](int ev) { return ev == 1; }) || p.empty()) return false;
    path = absPath(expandHome(p));
    for (auto& o : B)
      if (&o != &b && o.path == path) return msg("\"" + o.name + "\" is open in another tab - close it first");
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) return msg(path + " is a folder");
      string a;
      if (!prompt((baseName(path) + " already exists. Overwrite? (y/N): ").c_str(), a, [](int ev) { return ev == 1; }) ||
          (a != "y" && a != "Y"))
        return false;
    }
  }
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) return msg("Save failed: " + string(strerror(errno)));
  const char* nl = b.crlf ? "\r\n" : "\n";
  bool ok = true;
  for (size_t i = 0; i < b.L.size(); i++) {
    if (i) ok &= fputs(nl, f) >= 0;
    ok &= fwrite(b.L[i].data(), 1, b.L[i].size(), f) == b.L[i].size();
  }
  ok &= fclose(f) == 0;
  if (!ok) return msg("Save failed: " + string(strerror(errno)));
  if (b.path.empty()) { b.path = path; b.name = baseName(path); setLang(b); refresh(); }
  b.saved = b.U.empty() ? 0 : b.U.back().g;
  msg("Saved " + b.path);
  if (extOf(b.name) == "tex") texBuild(b);
  return true;
}
static void closeTab(int i, int armed) {
  if (B[i].dirty() && armed != 2 + i) {
    g_armed = 2 + i; cur = i;
    msg("\"" + B[i].name + "\" has unsaved changes - Ctrl+W (or click ×) again to discard, Ctrl+S to save");
    return;
  }
  B.erase(B.begin() + i);
  malloc_trim(0);  // hand the closed buffer's pages back to the OS
  if (B.empty()) { cur = 0; g_focusSb = true; return; }
  if (cur > i || cur >= (int)B.size()) cur = std::max(0, cur - 1);
}

// ───────────────────────── editing commands ─────────────────────────
static void typeText(Buf& b, const string& s) {
  if (!guard(b)) return;
  static const char *OP = "([{\"'`", *CL = ")]}\"'`";
  char c = s.size() == 1 ? s[0] : 0;
  const char* po = c ? strchr(OP, c) : nullptr;
  bool code = b.lang != 0;
  int y0, x0, y1, x1;
  if (po && code && selRange(b, y0, x0, y1, x1)) {  // wrap the selection
    b.sel = false;
    ins(b, y1, x1, string(1, CL[po - OP]));
    ins(b, y0, x0, string(1, c));
    b.sel = true; b.ay = y0; b.ax = x0 + 1; b.cy = y1; b.cx = x1 + (y0 == y1);
    return;
  }
  delSel(b);
  const string& l = b.L[b.cy];
  char nc = b.cx < len(l) ? l[b.cx] : 0, pc = b.cx > 0 ? l[b.cx - 1] : 0;
  if (c && code && strchr(CL, c) && nc == c) { b.cx++; return; }  // type over closer
  if (po && code) {
    bool quote = c == '"' || c == '\'' || c == '`';
    bool okNext = !nc || strchr(" \t)]};,:", nc);
    bool okPrev = !quote || !(isw(pc) || pc == c);
    if (okNext && okPrev && !(c == '`' && b.lang != 3)) {
      ins(b, b.cy, b.cx, string(1, c) + CL[po - OP]);
      b.cx--;
      return;
    }
  }
  if (!b.U.empty()) {  // merge a run of typing into one undo step
    Op& o = b.U.back();
    if (o.ins && o.g == g_prevType && o.g != b.saved && o.y == b.cy && o.x + len(o.s) == b.cx &&
        o.s.find('\n') == string::npos && !(c == ' ' && o.s.back() != ' ')) {
      touch(b, b.cy);
      b.L[b.cy].insert(b.cx, s);
      o.s += s;
      b.cx += len(s);
      o.ey = b.cy; o.ex = b.cx;
      b.R.clear();
      g_lastType = o.g;
      return;
    }
  }
  ins(b, b.cy, b.cx, s);
  g_lastType = g_grp;
}
static void enter(Buf& b) {
  if (!guard(b)) return;
  delSel(b);
  const string& l = b.L[b.cy];
  int x = b.cx, i = 0;
  while (i < x && (l[i] == ' ' || l[i] == '\t')) i++;
  string ind = l.substr(0, i);
  char pc = x > 0 ? l[x - 1] : 0, nc = x < len(l) ? l[x] : 0;
  bool open = b.lang && (pc == '{' || pc == '(' || pc == '[' || (pc == ':' && b.lang == 2));
  if (open && ((pc == '{' && nc == '}') || (pc == '(' && nc == ')') || (pc == '[' && nc == ']'))) {
    ins(b, b.cy, x, "\n" + ind + "    \n" + ind);
    b.cy--; b.cx = len(ind) + 4;
    return;
  }
  ins(b, b.cy, x, "\n" + ind + (open ? "    " : ""));
}
static void backspace(Buf& b) {
  if (!guard(b) || delSel(b)) return;
  const string& l = b.L[b.cy];
  int x = b.cx;
  if (x == 0) { if (b.cy > 0) del(b, b.cy - 1, len(b.L[b.cy - 1]), b.cy, 0); return; }
  if (firstNonWs(l) >= x && l.find('\t') == string::npos) { del(b, b.cy, (x - 1) / 4 * 4, b.cy, x); return; }
  char p = l[x - 1], c = x < len(l) ? l[x] : 0;
  if (b.lang && c && ((p == '(' && c == ')') || (p == '[' && c == ']') || (p == '{' && c == '}') ||
                      ((p == '"' || p == '\'' || p == '`') && c == p))) {
    del(b, b.cy, x - 1, b.cy, x + 1);
    return;
  }
  del(b, b.cy, pv(l, x), b.cy, x);
}
static void delForward(Buf& b, bool word) {
  if (!guard(b) || delSel(b)) return;
  const string& l = b.L[b.cy];
  if (word) { int y = b.cy, x = b.cx; wordRight(b); del(b, y, x, b.cy, b.cx); }
  else if (b.cx < len(l)) del(b, b.cy, b.cx, b.cy, nx(l, b.cx));
  else if (b.cy + 1 < len(b.L)) del(b, b.cy, b.cx, b.cy + 1, 0);
}
static void copy(Buf& b) {
  int y0, x0, y1, x1;
  if (selRange(b, y0, x0, y1, x1)) { g_clip = getText(b, y0, x0, y1, x1); g_clipLine = false; }
  else { g_clip = b.L[b.cy] + "\n"; g_clipLine = true; }
  osc52(g_clip);
}
static void cut(Buf& b) {
  if (!guard(b)) return;
  copy(b);
  if (delSel(b)) return;
  int y = b.cy, x = b.cx, n = len(b.L);
  if (y + 1 < n) del(b, y, 0, y + 1, 0);
  else if (y > 0) del(b, y - 1, len(b.L[y - 1]), y, len(b.L[y]));
  else del(b, 0, 0, 0, len(b.L[0]));
  b.cx = std::min(x, len(b.L[b.cy]));
}
static void paste(Buf& b, const string& t, bool lineMode) {
  if (!guard(b) || t.empty()) return;
  if (lineMode && !b.sel) {
    int x = b.cx;
    ins(b, b.cy, 0, t);
    b.cx = std::min(x, len(b.L[b.cy]));
    return;
  }
  delSel(b);
  ins(b, b.cy, b.cx, t);
}
struct SavedCur { int cy, cx, ay, ax; bool sel; };
static SavedCur saveCur(const Buf& b) { return {b.cy, b.cx, b.ay, b.ax, b.sel}; }
static void loadCur(Buf& b, const SavedCur& s) { b.cy = s.cy; b.cx = s.cx; b.ay = s.ay; b.ax = s.ax; b.sel = s.sel; }
static void adjBoth(SavedCur& s, int y, int x, int d) { adj(s.cy, s.cx, y, x, d); adj(s.ay, s.ax, y, x, d); }

static void toggleComment(Buf& b) {
  const char* p = LANGS[b.lang].lc;
  if (!*p || !guard(b)) return;
  int pl = (int)strlen(p), y0, y1;
  lineRange(b, y0, y1);
  bool all = true, any = false;
  int mi = INT_MAX;
  for (int y = y0; y <= y1; y++) {
    const string& l = b.L[y];
    int i = firstNonWs(l);
    if (i == len(l)) continue;
    any = true; mi = std::min(mi, i);
    if (l.compare(i, pl, p)) all = false;
  }
  if (!any) return;
  SavedCur sc = saveCur(b);
  for (int y = y0; y <= y1; y++) {
    int i = firstNonWs(b.L[y]);
    if (i == len(b.L[y])) continue;
    if (all) {
      int m = pl + (i + pl < len(b.L[y]) && b.L[y][i + pl] == ' ');
      del(b, y, i, y, i + m);
      adjBoth(sc, y, i, -m);
    } else {
      ins(b, y, mi, string(p) + " ");
      adjBoth(sc, y, mi, pl + 1);
    }
  }
  loadCur(b, sc);
}
static void indent(Buf& b, bool in) {
  if (!guard(b)) return;
  int y0, x0, y1, x1;
  bool hs = selRange(b, y0, x0, y1, x1);
  if (in && (!hs || y0 == y1)) {
    delSel(b);
    ins(b, b.cy, b.cx, string(4 - rcol(b.L[b.cy], b.cx) % 4, ' '));
    return;
  }
  lineRange(b, y0, y1);
  SavedCur sc = saveCur(b);
  for (int y = y0; y <= y1; y++) {
    const string& l = b.L[y];
    if (in) {
      if (l.empty()) continue;
      ins(b, y, 0, "    ");
      adjBoth(sc, y, 0, 4);
    } else {
      int m = 0;
      while (m < 4 && m < len(l) && l[m] == ' ') m++;
      if (!m && !l.empty() && l[0] == '\t') m = 1;
      if (m) { del(b, y, 0, y, m); adjBoth(sc, y, 0, -m); }
    }
  }
  loadCur(b, sc);
}
static void moveLines(Buf& b, bool up) {
  if (!guard(b)) return;
  int y0, y1;
  lineRange(b, y0, y1);
  if (up ? y0 == 0 : y1 == len(b.L) - 1) return;
  SavedCur sc = saveCur(b);
  string blk = joinLines(b, y0, y1);
  if (up) {
    string a = b.L[y0 - 1];
    del(b, y0 - 1, 0, y1, len(b.L[y1]));
    ins(b, y0 - 1, 0, blk + "\n" + a);
    sc.cy--; sc.ay--;
  } else {
    string a = b.L[y1 + 1];
    del(b, y0, 0, y1 + 1, len(b.L[y1 + 1]));
    ins(b, y0, 0, a + "\n" + blk);
    sc.cy++; sc.ay++;
  }
  loadCur(b, sc);
}
static void dupLines(Buf& b, bool down) {
  if (!guard(b)) return;
  int y0, y1;
  lineRange(b, y0, y1);
  SavedCur sc = saveCur(b);
  string blk = joinLines(b, y0, y1);
  if (down) {
    ins(b, y1, len(b.L[y1]), "\n" + blk);
    sc.cy += y1 - y0 + 1; sc.ay += y1 - y0 + 1;
  } else ins(b, y0, 0, blk + "\n");
  loadCur(b, sc);
}
static void selectWord(Buf& b, int y, int x) {
  const string& l = b.L[y];
  int a = std::min(x, len(l)), e = a;
  if (a < len(l)) {
    int k = wc(l[a]);
    while (a > 0 && wc(l[a - 1]) == k) a--;
    while (e < len(l) && wc(l[e]) == k) e++;
  }
  b.sel = true; b.ay = y; b.ax = a; b.cy = y; b.cx = e;
}
static void selectLineAt(Buf& b, int y, bool keepAnchor) {
  if (!keepAnchor) { b.sel = true; b.ay = y; b.ax = 0; }
  if (y + 1 < len(b.L)) { b.cy = y + 1; b.cx = 0; }
  else { b.cy = y; b.cx = len(b.L[y]); }
}

// ───────────────────────── key & mouse dispatch ─────────────────────────
static void pdfKey(Buf& b, int k) {
  int base = k & ~M_ALL, dh = pdfGeo(b), step = dh - dh / 10;
  bool wide = g_docW > EW * cellW;  // zoomed in past the width: left/right pan
  switch (base) {
    case K_PGDN: case ' ': pdfScrollTo(b, pdfY(b) + step); break;
    case K_PGUP: pdfScrollTo(b, pdfY(b) - step); break;
    case K_HOME: pdfScrollTo(b, 0); break;
    case K_END: pdfScrollTo(b, g_docH); break;
    case K_DOWN: pdfScrollBy(b, cellH * 3); break;
    case K_UP: pdfScrollBy(b, -cellH * 3); break;
    case K_RIGHT: if (wide) b.panX += cellW * 8; else pdfGo(b, b.page + 1); break;
    case K_LEFT: if (wide) b.panX -= cellW * 8; else pdfGo(b, b.page - 1); break;
    case '+': case '=': b.zoom = std::min(400, b.zoom + 25); break;
    case '-': b.zoom = std::max(25, b.zoom - 25); break;
    case '0': b.zoom = 100; b.panX = 0; break;
    case 7: doGoto(b); break;
    case 20: pdfText(b, true); break;
    case 6: pdfText(b, true); doFind(b); break;
    case 3: msg("Press Ctrl+T for text mode to select and copy"); break;
  }
}

// ───────────────────────── explorer actions ─────────────────────────
static string sbTargetDir() {  // folder that "New File" / "New Folder" create in
  buildRows();
  int i = selIndex();
  if (i < 0) return g_rootPath;
  if (g_rows[i].n->dir) return g_rows[i].path;
  return g_rows[i].parent >= 0 ? g_rows[g_rows[i].parent].path : g_rootPath;
}
static void sbCreate(bool dir) {
  string d = sbTargetDir(), s;
  if (!prompt(dir ? "New folder: " : "New file: ", s, [](int ev) { return ev == 1; }) || s.empty()) return;
  string p = join(d, s);
  for (size_t k = p.size() - s.size(); (k = p.find('/', k)) != string::npos; k++) mkdir(p.substr(0, k).c_str(), 0755);
  bool ok;
  if (dir) ok = mkdir(p.c_str(), 0755) == 0;
  else {
    int fd = open(p.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
    ok = fd >= 0;
    if (ok) close(fd);
  }
  if (!ok) { msg("Cannot create " + s + ": " + strerror(errno)); return; }
  refresh();
  reveal(p);
  g_sbSelPath = p;
  buildRows();
  sbShow(selIndex());
  if (!dir) { openFile(p); g_focusSb = false; }
}
static void sbRename() {
  buildRows();
  int i = selIndex();
  if (i < 0) return;
  string old = g_rows[i].path, name = g_rows[i].n->name, s = name;
  if (!prompt("Rename to: ", s, [](int ev) { return ev == 1; }) || s.empty() || s == name) return;
  string np = old.substr(0, old.rfind('/') + 1) + s;
  if (access(np.c_str(), F_OK) == 0) { msg("\"" + s + "\" already exists"); return; }
  if (rename(old.c_str(), np.c_str())) { msg("Rename failed: " + string(strerror(errno))); return; }
  for (auto& b : B)
    if (b.path == old || !b.path.compare(0, old.size() + 1, old + "/")) {
      b.path = np + b.path.substr(old.size());
      b.name = baseName(b.path);
      if (!b.pdf) setLang(b);
    }
  refresh();
  g_sbSelPath = np;
  g_lastActive.clear();
}
static void sbDelete() {
  buildRows();
  int i = selIndex();
  if (i < 0) return;
  string p = g_rows[i].path, nm = g_rows[i].n->name, s;
  bool dir = g_rows[i].n->dir;
  string label = "Move '" + nm + "' to Trash? (y/n) ";
  if (!prompt(label.c_str(), s, [&](int ev) { return ev == 1 || (ev == 0 && !s.empty()); }) || (s != "y" && s != "Y")) return;
  int st = -1;
  run({"gio", "trash", "--", p}, &st);  // recoverable, like VS Code
  if (st != 0) {
    s.clear();
    if (!prompt("Can't move to Trash here. Delete PERMANENTLY? (y/n) ", s,
                [&](int ev) { return ev == 1 || (ev == 0 && !s.empty()); }) || (s != "y" && s != "Y"))
      return;
    if ((dir ? rmdir(p.c_str()) : unlink(p.c_str())) != 0) {
      msg(errno == ENOTEMPTY ? "Folder is not empty - not deleted" : "Delete failed: " + string(strerror(errno)));
      return;
    }
  }
  int j = i + 1;  // select the next row that isn't inside the deleted item
  while (j < len(g_rows) && !g_rows[j].path.compare(0, p.size() + 1, p + "/")) j++;
  g_sbSelPath = j < len(g_rows) ? g_rows[j].path : i > 0 ? g_rows[i - 1].path : "";
  refresh();
  msg(st == 0 ? "Moved " + nm + " to Trash" : "Deleted " + nm);
}
static void collapseAll(Node& n) { for (auto& k : n.kids) { k.open = false; collapseAll(k); } }

static void sbKey(int k) {
  buildRows();
  int n = len(g_rows), i = selIndex(), tv = H - 3;
  auto sel = [&](int j) {
    if (!n) return;
    j = std::max(0, std::min(n - 1, j));
    g_sbSelPath = g_rows[j].path;
    sbShow(j);
  };
  switch (k) {
    case K_UP: sel(i < 0 ? 0 : i - 1); return;
    case K_DOWN: sel(i + 1); return;
    case K_PGUP: sel(i - tv); return;
    case K_PGDN: sel(i + tv); return;
    case K_HOME: sel(0); return;
    case K_END: sel(n - 1); return;
    case K_ESC: if (!B.empty()) g_focusSb = false; return;
    case K_F2: sbRename(); return;
    case K_DEL: sbDelete(); return;
    case M_ALT | 'n': sbCreate(true); return;
  }
  if (i < 0) { sel(0); return; }
  Row r = g_rows[i];
  switch (k) {
    case K_RIGHT:
      if (r.n->dir) { if (!r.n->open) toggleNode(*r.n, r.path); else sel(i + 1); }
      return;
    case K_LEFT:
      if (r.n->dir && r.n->open) r.n->open = false;
      else if (r.parent >= 0) sel(r.parent);
      return;
    case 13: case ' ':
      if (r.n->dir) toggleNode(*r.n, r.path);
      else { openFile(r.path); if (k == 13 && !B.empty()) g_focusSb = false; }
      return;
  }
  if (k > 32 && k < 127)  // type to jump
    for (int j = 1; j <= n; j++) {
      int t = (i + j) % n;
      if (tolower((unsigned char)g_rows[t].n->name[0]) == tolower(k)) { sel(t); break; }
    }
}

static long long g_lastClick = 0;
static int g_lcx = -1, g_lcy = -1, g_clicks = 0;
static int g_dragY = 0;  // line where a line-select drag started
static bool g_drag = false, g_dragLine = false, g_sbResizing = false;

static void sbMouse() {
  if (mB & 64) { g_sbTop = std::max(0, g_sbTop + ((mB & 1) ? 3 : -3)); return; }
  if (!mDown || (mB & 32) || (mB & 3) != 0) return;
  g_focusSb = true;
  if (mY == 0) {
    for (int b = 0; b < 4; b++)
      if (mX == g_btnX[b]) {
        if (b == 0) sbCreate(false);
        else if (b == 1) sbCreate(true);
        else if (b == 2) { refresh(); msg("Explorer refreshed"); }
        else collapseAll(g_root);
      }
    return;
  }
  if (mY == 1) { g_root.open = !g_root.open; return; }
  buildRows();
  int j = g_sbTop + mY - 2;
  if (j < 0 || j >= len(g_rows)) return;
  Row r = g_rows[j];
  g_sbSelPath = r.path;
  if (r.n->dir) toggleNode(*r.n, r.path);
  else { openFile(r.path); if (!B.empty()) g_focusSb = false; }
}

static void mouse(int armed) {
  layout();
  if (g_sbResizing) {
    if (!mDown) g_sbResizing = false;
    else if (mB & 32) g_sbWant = std::max(15, std::min(W - 20, mX + 1));
    return;
  }
  bool motion = mB & 32;
  if (mDown && !motion && !(mB & 64) && g_sbW && mX == g_sbW - 1 && mY > 0 && mY < H - 1) { g_sbResizing = true; return; }
  if (g_sbW && mX < g_sbW && mY < H - 1 && !(g_drag && motion)) { sbMouse(); return; }
  if (B.empty()) return;
  Buf& b = B[cur];
  bool img = b.pdf && !b.pdfText;
  int n = len(b.L);
  if (mB & 64) {  // wheel
    int d = (mB & 1) ? 1 : -1;
    if (mY == 0) { cur = (cur + d + (int)B.size()) % (int)B.size(); return; }
    if (img) { pdfScrollBy(b, d * cellH * 3); return; }
    b.top = std::max(0, std::min(n - 1, b.top + 3 * d));
    b.follow = false;
    return;
  }
  int btn = mB & 3;
  if (!mDown) { g_drag = false; return; }
  if (!motion && mY == 0) {
    for (size_t i = 0; i < g_tabs.size() && i < B.size(); i++)
      if (g_tabs[i].x0 >= 0 && mX >= g_tabs[i].x0 && mX < g_tabs[i].x1) {
        if (btn == 1 || (btn == 0 && std::abs(mX - g_tabs[i].cx) <= 1)) closeTab((int)i, armed);
        else if (btn == 0) { cur = (int)i; g_focusSb = false; }
        return;
      }
    if (btn == 0 && nowMs() - g_lastClick < 400) newTab();
    g_lastClick = nowMs();
    return;
  }
  if (!motion && mY == H - 1) {
    if (btn != 0) return;
    if (img && std::abs(mX - g_prevX) <= 1) pdfGo(b, b.page - 1);
    else if (img && std::abs(mX - g_nextX) <= 1) pdfGo(b, b.page + 1);
    else if (mX >= g_lnX0 && mX < g_lnX1) doGoto(b);
    return;
  }
  if (!motion && mY == 1) { g_focusSb = false; return; }  // breadcrumbs
  if (img || btn != 0) return;
  g_focusSb = false;
  int y = std::max(0, std::min(n - 1, b.top + mY - 2));
  int gw = gutterW(b);
  bool gutter = mX - EX < gw;
  int x = gutter ? 0 : xfromr(b.L[y], mX - EX - gw + b.left);
  b.follow = true; b.want = -1;
  if (motion) {
    if (!g_drag) return;
    b.sel = true;
    if (g_dragLine) {
      if (y >= g_dragY) { b.ay = g_dragY; b.ax = 0; selectLineAt(b, y, true); }
      else {  // dragging up: anchor at the end of the clicked line
        if (g_dragY + 1 < n) { b.ay = g_dragY + 1; b.ax = 0; } else { b.ay = g_dragY; b.ax = len(b.L[g_dragY]); }
        b.cy = y; b.cx = 0;
      }
    }
    else { b.cy = y; b.cx = x; }
    return;
  }
  long long t = nowMs();
  g_clicks = (t - g_lastClick < 400 && y == g_lcy && std::abs(mX - g_lcx) <= 1) ? g_clicks + 1 : 1;
  g_lastClick = t; g_lcx = mX; g_lcy = y;
  g_drag = true; g_dragLine = false;
  if (gutter || g_clicks >= 3) { selectLineAt(b, y, false); g_dragLine = true; g_dragY = y; return; }
  if (g_clicks == 2) { selectWord(b, y, x); g_drag = false; return; }
  if (mB & 4) {
    if (!b.sel) { b.sel = true; b.ay = b.cy; b.ax = b.cx; }
    b.cy = y; b.cx = x;
    return;
  }
  b.sel = false; b.ay = b.cy = y; b.ax = b.cx = x;
}


static void editKey(Buf& b, int k) {
  int base = k & ~M_ALL, n = len(b.L);
  bool sh = k & M_SHIFT, ct = k & M_CTRL, al = k & M_ALT;
  bool vert = base == K_UP || base == K_DOWN || base == K_PGUP || base == K_PGDN;
  if (!vert || al || ct) b.want = -1;
  if (al && (base == K_UP || base == K_DOWN)) {
    if (sh) dupLines(b, base == K_DOWN); else moveLines(b, base == K_UP);
    return;
  }
  if (base >= K_UP && base <= K_PGDN) {
    if (ct && (base == K_UP || base == K_DOWN)) {
      b.top = std::max(0, std::min(n - 1, b.top + (base == K_UP ? -1 : 1)));
      b.follow = false;
      return;
    }
    int y0, x0, y1, x1;
    if (!sh && !ct && (base == K_LEFT || base == K_RIGHT) && selRange(b, y0, x0, y1, x1)) {
      b.sel = false;
      if (base == K_LEFT) { b.cy = y0; b.cx = x0; } else { b.cy = y1; b.cx = x1; }
      return;
    }
    if (sh) { if (!b.sel) { b.sel = true; b.ay = b.cy; b.ax = b.cx; } }
    else b.sel = false;
    const string& l = b.L[b.cy];
    switch (base) {
      case K_LEFT:
        if (ct) wordLeft(b);
        else if (b.cx > 0) b.cx = pv(l, b.cx);
        else if (b.cy > 0) { b.cy--; b.cx = len(b.L[b.cy]); }
        break;
      case K_RIGHT:
        if (ct) wordRight(b);
        else if (b.cx < len(l)) b.cx = nx(l, b.cx);
        else if (b.cy < n - 1) { b.cy++; b.cx = 0; }
        break;
      case K_HOME:
        if (ct) { b.cy = 0; b.cx = 0; }
        else { int f = firstNonWs(l); b.cx = b.cx == f ? 0 : f; }
        break;
      case K_END:
        if (ct) b.cy = n - 1;
        b.cx = len(b.L[b.cy]);
        break;
      default: {
        if (b.want < 0) b.want = rcol(l, b.cx);
        int page = H - 3;
        int d = base == K_UP ? -1 : base == K_DOWN ? 1 : base == K_PGUP ? -page : page;
        int ny = b.cy + d;
        if (ny < 0) { b.cy = 0; b.cx = 0; break; }
        if (ny >= n) { b.cy = n - 1; b.cx = len(b.L[n - 1]); break; }
        if (base == K_PGUP || base == K_PGDN) b.top = std::max(0, b.top + d);
        b.cy = ny;
        b.cx = xfromr(b.L[ny], b.want);
      }
    }
    return;
  }
  switch (k) {
    case 26: undo(b); return;                   // Ctrl+Z
    case 25: redo(b); return;                   // Ctrl+Y
    case 1:                                     // Ctrl+A
      b.sel = true; b.ay = 0; b.ax = 0; b.cy = n - 1; b.cx = len(b.L[n - 1]); b.follow = false;
      return;
    case 3: copy(b); return;                    // Ctrl+C
    case 24: cut(b); return;                    // Ctrl+X
    case 22: paste(b, g_clip, g_clipLine); return;  // Ctrl+V
    case K_PASTE: paste(b, pasteBuf, false); return;
    case 31: toggleComment(b); return;          // Ctrl+/
    case 4: dupLines(b, true); return;          // Ctrl+D
    case 12: selectLineAt(b, b.cy, b.sel); if (!b.sel) b.sel = true; return;  // Ctrl+L
    case 6: doFind(b); return;                  // Ctrl+F
    case 7: doGoto(b); return;                  // Ctrl+G
    case K_F3: case K_F3 | M_SHIFT:
      if (!g_lastFind.empty()) findPrevNext(b, g_lastFind, k & M_SHIFT);
      return;
    case K_ESC: b.sel = false; return;
    case 9: indent(b, true); return;
    case K_BTAB: indent(b, false); return;
    case 13: enter(b); return;
    case 127: backspace(b); return;
    case 8: case 127 | M_ALT:                   // Ctrl+Backspace / Alt+Backspace
      if (guard(b) && !delSel(b)) { int y = b.cy, x = b.cx; wordLeft(b); del(b, b.cy, b.cx, y, x); }
      return;
    case K_DEL: delForward(b, false); return;
    case K_DEL | M_CTRL: delForward(b, true); return;
    case K_INS: return;
  }
  if (k >= 32 && k < 256 && k != 127) {
    string s(1, (char)k);
    if (k >= 0xC0) utf8Tail(s, k);
    typeText(b, s);
  }
}

static void handle(int k) {
  int armed = g_armed;
  g_armed = 0;
  if (k == K_MOUSE) {
    if (!mDown || (mB & 32)) g_armed = armed;  // a release or drag is not a new action
    mouse(armed);
    return;
  }
  bool has = !B.empty();
  switch (k) {
    case 17: {  // Ctrl+Q
      bool d = false;
      for (auto& x : B) d |= x.dirty();
      if (d && armed != 1) { g_armed = 1; msg("Unsaved changes - press Ctrl+Q again to quit without saving"); }
      else g_run = false;
      return;
    }
    case 2:  // Ctrl+B: toggle sidebar
      g_sbShow = !g_sbShow;
      if (!g_sbShow) g_focusSb = false;
      return;
    case 5:  // Ctrl+E / Ctrl+Shift+E: focus explorer <-> editor
      if (g_focusSb && g_sbShow && has) g_focusSb = false;
      else { g_sbShow = true; g_focusSb = true; refresh(); }
      return;
    case 14:  // Ctrl+N
      if (g_focusSb) sbCreate(false);
      else newTab();
      return;
    case 15: {  // Ctrl+O
      string p;
      if (prompt("Open file: ", p, [](int ev) { return ev == 1; }) && !p.empty()) {
        string a = absPath(expandHome(p));
        if (isDirPath(a)) { g_rootPath = a; g_root = Node(); g_root.name = baseName(a); g_root.open = true; refresh(); g_focusSb = true; }
        else { openFile(a); g_focusSb = B.empty(); }
      }
      return;
    }
  }
  if (has) switch (k) {
    case 19: save(B[cur]); return;              // Ctrl+S
    case 23: closeTab(cur, armed); return;      // Ctrl+W
    case K_PGUP | M_CTRL: cur = (cur + (int)B.size() - 1) % (int)B.size(); return;
    case K_PGDN | M_CTRL: cur = (cur + 1) % (int)B.size(); return;
  }
  if (k >= (M_ALT | '1') && k <= (M_ALT | '9')) {
    int i = (k & 0xff) - '1';
    if (i < (int)B.size()) { cur = i; g_focusSb = false; }
    return;
  }
  if (g_focusSb || !has) { if (g_sbShow) { g_focusSb = true; sbKey(k); } return; }
  Buf& b = B[cur];
  if (b.pdf && !b.pdfText) { pdfKey(b, k); return; }
  if (b.pdf && k == 20) {  // Ctrl+T back to page view
    if (g_gfx) pdfText(b, false);
    else msg("This terminal can't show images - staying in text view");
    return;
  }
  editKey(b, k);
}

// Ask the terminal whether it can show sixel images (DA1) and how big a cell is.
static void detectGfx() {
  wr("\x1b[16t\x1b[c");
  string r;
  long long t0 = nowMs();
  while (nowMs() - t0 < 400) {
    int c = rd(50);
    if (c < 0) continue;
    r += (char)c;
    size_t da = r.rfind("\x1b[?");
    if (c == 'c' && da != string::npos && r.find_first_not_of("0123456789;", da + 3) == r.size() - 1) break;
  }
  size_t da = r.rfind("\x1b[?");  // \e[?62;4;...c: parameter 4 = sixel graphics
  if (da != string::npos)
    for (const char* q = r.c_str() + da + 3; *q >= '0' && *q <= '9';) {
      char* e;
      if (strtol(q, &e, 10) == 4) g_gfx = true;
      q = *e == ';' ? e + 1 : e;
    }
  if (const char* e = getenv("VSC_GFX")) g_gfx = *e == '1' || !strcmp(e, "sixel");
  size_t p = r.find("\x1b[6;");
  int h, w;
  if (!cellW && p != string::npos && sscanf(r.c_str() + p + 4, "%d;%dt", &h, &w) == 2 && h > 0 && w > 0) {
    cellW = w; cellH = h;
  }
}

int main(int argc, char** argv) {
  string root;
  vector<string> files;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      puts("usage: vsc [folder] [file ...]     (default folder: current directory)\n"
           "  Ctrl+E explorer  Ctrl+B sidebar  Ctrl+O open  Ctrl+N new  Ctrl+S save  Ctrl+W close  Ctrl+Q quit\n"
           "  Ctrl+F find  Ctrl+G go to line  Ctrl+Z/Y undo/redo  Ctrl+/ comment  Alt+Up/Down move line\n"
           "  Explorer: Enter open, F2 rename, Del trash, Ctrl+N new file, Alt+N new folder\n"
           "  PDF: wheel/arrows/PgUp/PgDn scroll, +/- zoom, Ctrl+T text mode   (VSC_GFX=0/1 forces sixel graphics)");
      return 0;
    }
    if (isDirPath(argv[i])) { if (root.empty()) root = argv[i]; }
    else files.push_back(argv[i]);
  }
  if (!isatty(0) || !isatty(1)) { fputs("vsc: needs a terminal\n", stderr); return 1; }
  g_rootPath = absPath(root.empty() ? "." : root);
  g_root.name = g_rootPath == "/" ? "/" : baseName(g_rootPath);
  g_root.open = true;
  refresh();
  rawOn();
  wr("\x1b[22;0t");  // save the terminal title
  getSize();
  detectGfx();
  for (auto& f : files) openFile(absPath(f));
  cur = 0;
  g_focusSb = B.empty();
  while (g_run) {
    if (ibP >= ibN) draw();
    int k = readKey();
    if (k == K_NONE) continue;
    unsigned gen = g_msgGen;
    g_prevType = g_lastType;
    g_lastType = 0;
    ++g_grp;
    if (k != K_MOUSE && !B.empty()) B[cur].follow = true;
    handle(k);
    if (!g_run) break;
    if (!B.empty()) {
      cur = std::min(cur, (int)B.size() - 1);
      Buf& b = B[cur];
      clampCur(b);
      if (!b.U.empty() && b.U.back().g == g_grp) { b.U.back().ey = b.cy; b.U.back().ex = b.cx; }
    }
    if (g_msgGen == gen) g_msg.clear();
  }
  return 0;
}
