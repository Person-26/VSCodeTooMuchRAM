// vsc — a tiny VS Code–style terminal text editor. Single file, no dependencies
// beyond the C++ standard library and POSIX. PDFs are opened in Okular.
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

// ───────────────────────── terminal ─────────────────────────
static termios g_orig;
static bool g_raw = false;
static int W = 80, H = 24;
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
  if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col) { W = ws.ws_col; H = ws.ws_row; }
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
  {"%", false, false, "", "", "", "", C_N},  // LaTeX: highlighted by hlTex
};
enum { L_TEX = 6 };
static bool inList(const char* list, const char* w, int n) {
  if (n > 40 || !*list) return false;
  char k[48];
  k[0] = ' '; memcpy(k + 1, w, n); k[n + 1] = ' '; k[n + 2] = 0;
  return strstr(list, k) != nullptr;
}
static bool isw(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

// LaTeX: \commands blue, structure (\begin, \section, \newcommand...) purple, references (\label,
// \cite...) yellow with their {key} orange, environment names teal, math green, % comments, and
// brackets coloured by depth; a comment environment is a comment. `st` = (bracket depth << 2) |
// in-comment-environment << 1 | in-math, carried between lines.
static int hlTex(const string& s, int st, unsigned char* out, int lim) {
  static const char* CTL = " begin end documentclass usepackage RequirePackage input include includeonly part chapter"
                           " section subsection subsubsection paragraph subparagraph newcommand renewcommand"
                           " providecommand newenvironment renewenvironment def let DeclareMathOperator item if else fi ";
  static const char* REF = " label ref eqref cref Cref autoref pageref nameref cite citep citet citeauthor nocite"
                           " includegraphics bibliography bibliographystyle addbibresource url href ";
  static const char* MATH = " equation align gather multline eqnarray displaymath math flalign alignat ";
  bool m = st & 1, cmt = st & 2;
  int d = st >> 2, n = len(s), i = 0;
  auto set = [&](int a, int b, int c) { b = std::min(b, lim); if (out && b > a) memset(out + a, c, b - a); };
  if (cmt) {  // inside \begin{comment}: everything up to \end{comment}
    size_t e = s.find("\\end{comment}");
    if (e == string::npos) { set(0, n, C_CMT); return st; }
    set(0, (int)e, C_CMT);
    i = (int)e;
    cmt = false;
  }
  auto arg = [&](int cls) {  // a {name} right after the command: returns the name
    int j = i;
    while (j < n && s[j] == ' ') j++;
    if (j >= n || s[j] != '{') return string();
    size_t e = s.find('}', j);
    int k = e == string::npos ? n : (int)e;
    set(i, j, C_N);
    set(j, j + 1, C_B0 + d % 3); set(j + 1, k, cls); set(k, k + 1, C_B0 + d % 3);
    string name = s.substr(j + 1, k - j - 1);
    i = std::min(n, k + 1);
    return name;
  };
  while (i < n) {
    unsigned char c = s[i];
    if (c == '%') { set(i, n, C_CMT); break; }
    if (c == '\\') {
      int j = i + 1;
      if (j < n && isalpha((unsigned char)s[j])) {
        while (j < n && isalpha((unsigned char)s[j])) j++;
        if (j < n && s[j] == '*') j++;
      } else if (j < n) j = nx(s, j);
      const char* w = s.data() + i + 1;
      int wl = j - i - 1 - (s[j - 1] == '*');
      if (wl == 1 && (*w == '[' || *w == '(')) { set(i, j, C_CTL); m = true; i = j; continue; }
      if (wl == 1 && (*w == ']' || *w == ')')) { set(i, j, C_CTL); m = false; i = j; continue; }
      bool be = (wl == 5 && !memcmp(w, "begin", 5)) || (wl == 3 && !memcmp(w, "end", 3)), ref = inList(REF, w, wl);
      set(i, j, ref ? C_FN : inList(CTL, w, wl) ? C_CTL : C_KW);
      i = j;
      if (be) {
        string e = arg(C_TY);
        if (!e.empty() && e.back() == '*') e.pop_back();
        if (inList(MATH, e.data(), len(e))) m = *w == 'b';
        if (*w == 'b' && e == "comment") { set(i, n, C_CMT); cmt = true; break; }
      } else if (ref) arg(C_STR);
      continue;
    }
    if (c == '$') {
      int j = i + 1 < n && s[i + 1] == '$' ? i + 2 : i + 1;
      set(i, j, C_CTL); m = !m; i = j; continue;
    }
    if (c == '(' || c == '[' || c == '{') { set(i, i + 1, C_B0 + d % 3); d++; i++; continue; }
    if (c == ')' || c == ']' || c == '}') { if (d > 0) d--; set(i, i + 1, C_B0 + d % 3); i++; continue; }
    set(i, i + 1, m ? C_NUM : C_N); i++;
  }
  return (d << 2) | (cmt ? 2 : 0) | (m ? 1 : 0);
}

// Highlights one line. `st` = (bracket depth << 3) | (open multi-line string << 1) | inside-block-comment,
// carried between lines; the string is 1 = """, 2 = ''' (Python), 3 = `...` (JavaScript). Writes a
// token class per byte to `out` (if given); returns next state.
// Only bytes [0, lim) of `out` are written, so a caller drawing part of a huge line needs only that much.
static int hl(const Lang& lg, const string& s, int st, unsigned char* out, int lim = INT_MAX) {
  if (&lg == &LANGS[L_TEX]) return hlTex(s, st, out, lim);
  static const char* QS[] = {"", "\"\"\"", "\'\'\'", "`"};
  bool cm = st & 1;
  int q = (st >> 1) & 3, d = st >> 3, n = len(s), i = 0, first = 0;
  auto set = [&](int a, int b, int c) { b = std::min(b, lim); if (out && b > a) memset(out + a, c, b - a); };
  while (first < n && (s[first] == ' ' || s[first] == '\t')) first++;
  int lcl = (int)strlen(lg.lc);
  auto closeStr = [&](int j) {  // string q runs from i; ends at its closing quote or runs on to the next line
    int ql = (int)strlen(QS[q]);
    while (j < n && s.compare(j, ql, QS[q])) j += s[j] == '\\' ? 2 : 1;
    if (j < n) { j += ql; q = 0; }
    j = std::min(j, n);
    set(i, j, C_STR); i = j;
  };
  while (i < n) {
    if (q) { closeStr(i); continue; }
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
    if (&lg == &LANGS[2] && (c == '"' || c == '\'') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
      q = c == '"' ? 1 : 2;  // Python triple-quoted string
      closeStr(i + 3); continue;
    }
    if (c == '`' && strchr(lg.q, c)) { q = 3; closeStr(i + 1); continue; }
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
  return (d << 3) | (q << 1) | (cm ? 1 : 0);
}

// ───────────────────────── buffers ─────────────────────────
struct Op { bool ins; int y, x; string s; int by, bx, ey, ex; unsigned g; };
// Undo history: a circular buffer of edits, so the oldest can be dropped from the front in O(1) as new
// ones arrive at the back. It only grows (doubling, unwrapping the ring) when it is full.
struct Ring {
  vector<Op> a;
  size_t h = 0, n = 0;  // first element, count
  bool empty() const { return !n; }
  Op& front() { return a[h]; }
  Op& back() { return a[(h + n - 1) % a.size()]; }
  const Op& back() const { return a[(h + n - 1) % a.size()]; }
  void push_back(Op&& o) {
    if (n == a.size()) {
      vector<Op> b(std::max<size_t>(8, a.size() * 2));
      for (size_t i = 0; i < n; i++) b[i] = std::move(a[(h + i) % a.size()]);
      a.swap(b);
      h = 0;
    }
    a[(h + n++) % a.size()] = std::move(o);
  }
  void pop_back() { back() = Op(); n--; }  // Op() frees the text
  void pop_front() { a[h] = Op(); h = (h + 1) % a.size(); n--; }
  void clear() { vector<Op>().swap(a); h = n = 0; }
};
struct Buf {
  int id = 0;
  string path, name, langName = "Plain Text";
  vector<string> L{""};
  bool crlf = false, ro = false;
  int cy = 0, cx = 0, want = -1, top = 0, left = 0;
  bool follow = true, sel = false;
  int ay = 0, ax = 0;
  Ring U;
  vector<Op> R;
  size_t ub = 0;  // bytes held by U, kept under UNDO_CAP by dropping the oldest steps
  unsigned saved = 0, base = 0;  // base: the step the oldest dropped one left the text at

  int lang = 0, stOK = 1;
  vector<int> st{0};
  unsigned head() const { return U.empty() ? base : U.back().g; }  // the step the text is at
  bool dirty() const { return head() != saved; }
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
  if ((int)b.st.size() < y + 1) b.st.resize(y + 1);  // only as far as has been looked at
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
// The undo history holds at most UNDO_CAP bytes: each time it goes over, just enough whole steps are
// dropped from the oldest end of the ring to get back under. A single change bigger than the cap can't
// be undone at all.
enum : size_t { UNDO_CAP = 1 << 20 };
static size_t opBytes(const Op& o) { return sizeof(Op) + (o.s.capacity() > 15 ? o.s.capacity() + 1 : 0); }
static void trimU(Buf& b) {
  bool all = false;
  while (b.ub > UNDO_CAP && !b.U.empty()) {
    unsigned g = b.U.front().g;
    while (!b.U.empty() && b.U.front().g == g) { b.ub -= opBytes(b.U.front()); b.U.pop_front(); }
    b.base = g;
    all = b.U.empty();
  }
  if (all) {  // the newest step went too: more of it may follow, so its number can't name this text
    static unsigned uniq = ~0u;
    b.base = uniq--;
    b.U.clear();
    b.ub = 0;
    msg("Change too large to undo (over 1 MB)");
  }
}
static void pushU(Buf& b, Op&& o) {
  b.ub += opBytes(o);
  b.U.push_back(std::move(o));
  trimU(b);
}
static void clearR(Buf& b) { if (!b.R.empty()) vector<Op>().swap(b.R); }
static void ins(Buf& b, int y, int x, const string& s) {
  if (s.empty() || b.ro) return;
  Op o{true, y, x, s, b.cy, b.cx, 0, 0, g_grp};
  insRaw(b, y, x, s, b.cy, b.cx);
  o.ey = b.cy; o.ex = b.cx;
  pushU(b, std::move(o));
  clearR(b);
}
static void del(Buf& b, int y0, int x0, int y1, int x1) {
  if ((y0 == y1 && x0 == x1) || b.ro) return;
  Op o{false, y0, x0, "", b.cy, b.cx, y0, x0, g_grp};
  o.s = delRaw(b, y0, x0, y1, x1);
  b.cy = y0; b.cx = x0;
  pushU(b, std::move(o));
  clearR(b);
}
static void undo(Buf& b) {
  if (b.U.empty()) return;
  unsigned g = b.U.back().g;
  while (!b.U.empty() && b.U.back().g == g) {
    Op o = std::move(b.U.back());
    b.U.pop_back();
    b.ub -= opBytes(o);
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
    pushU(b, std::move(o));
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
    {"zsh", 4, "Shell Script"}, {"json", 5, "JSON"}, {"jsonc", 5, "JSON with Comments"},
    {"tex", L_TEX, "LaTeX"}, {"ltx", L_TEX, "LaTeX"}, {"sty", L_TEX, "LaTeX"}, {"cls", L_TEX, "LaTeX"}};
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

// ───────────────────────── LaTeX build on save ─────────────────────────
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
}

// ───────────────────────── rendering ─────────────────────────
// All-black theme: every pane is black and the panes are told apart by thin boundary lines: a │ between
// the sidebar and the editor (btop's cpu-box green), an underline under the tab row (grey) and one above
// the status bar (btop's net-box purple). Text and token colours are VS Code Dark+'s.
static const char *BG = "0;0;0", *CURL = "40;40;40", *SELB = "38;79;120", *FINDB = "98;51;21",
                  *GUT = "133;133;133", *GUTA = "198;198;198", *TABBAR = "0;0;0", *TABIN = "0;0;0",
                  *TABFG = "150;150;150", *WHITE = "255;255;255", *STAT = "0;0;0", *FGN = "212;212;212",
                  *SBBG = "0;0;0", *SBFG = "204;204;204", *SBHEAD = "187;187;187", *SELF = "0;122;204",
                  *SELT = "117;190;255", *CHEV = "197;197;197", *CRUMB = "169;169;169", *KEYBG = "51;51;51",
                  *GUIDE = "64;64;64", *BORDER = "85;109;89";
// a boundary drawn as a coloured underline of a whole row (foot supports SGR 58 underline colours)
static const char *UL_ON = "\x1b[4m\x1b[58;2;90;90;90m", *UL_STAT = "\x1b[4m\x1b[58;2;92;88;141m", *UL_ACC = "\x1b[58;2;0;122;204m", *UL_OFF = "\x1b[24m";
static const char *g_cf, *g_cb;
static void col(string& o, const char* f, const char* b) {
  if (f != g_cf) { o += "\x1b[38;2;"; o += f; o += 'm'; g_cf = f; }
  if (b != g_cb) { o += "\x1b[48;2;"; o += b; o += 'm'; g_cb = b; }
}
static void at(string& o, int row, int c) { o += "\x1b["; o += num(row); o += ';'; o += num(c); o += 'H'; }

struct TabHit { int x0, x1, cx; };
static vector<TabHit> g_tabs;
static int g_tabOff = 0, g_lnX0 = -1, g_lnX1 = -1;
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
  o += UL_ON;
  col(o, SBHEAD, SBBG);
  left = fitW(left, sw - rw);
  o += left;
  o.append(sw - rw - dispW(left), ' ');
  if (rw) {
    col(o, CHEV, SBBG);
    o += right;
    for (int i = 0; i < 4; i++) g_btnX[i] = sw - rw + 1 + 3 * i;
  }
  o += UL_OFF;
  // folder section header
  string sec = string(g_root.open ? " ▾ " : " ▸ ") + g_root.name;
  for (size_t i = 3; i < sec.size(); i++) sec[i] = (char)toupper((unsigned char)sec[i]);
  sec = fitW(sec, sw);
  // The selected row is outlined in VS Code blue rather than filled, with its name in a lighter blue: its
  // sides are ▏ ▕ and its top and bottom edges are underlines of the row above and of itself.
  const char* sc = SELF;
  string selUL = string("\x1b[4m\x1b[58;2;") + sc + "m";
  at(o, 2, 1);
  if (si >= 0 && si == g_sbTop) o += selUL;  // top edge of a selection in the first row
  col(o, SBFG, SBBG);
  o += "\x1b[1m" + sec + "\x1b[22m";
  o.append(sw - dispW(sec), ' ');
  for (int r = 0; r < tv; r++) {
    int j = g_sbTop + r;
    at(o, r + 3, 1);
    o += UL_OFF;
    bool sel = j == si;
    if (sel || (si >= 0 && j == si - 1)) o += selUL;
    else if (r == tv - 1) o += UL_STAT;
    if (j >= n) { col(o, SBFG, SBBG); o.append(sw, ' '); continue; }
    const Row& R = g_rows[j];
    const char* bg = SBBG;
    int used = std::min(1 + 2 * R.depth, std::max(0, sw - 6));
    if (sel && used) { col(o, sc, bg); o += "▏"; col(o, SBFG, bg); o.append(used - 1, ' '); }
    else { col(o, SBFG, bg); o.append(used, ' '); }
    if (R.n->dir) { col(o, sel ? SELT : CHEV, bg); o += R.n->open ? "▾ " : "▸ "; used += 2; }
    else { Icon ic = iconFor(R.n->name); col(o, ic.c, bg); o += ic.g; o += ' '; used += 3; }
    string nm = fitW(R.n->name, std::max(0, sw - used - 1 - sel));  // (column sw is the pane border)
    col(o, sel ? SELT : SBFG, bg);
    o += nm;
    used += dispW(nm);
    o.append(std::max(0, sw - 1 - sel - used), ' ');
    if (sel) { col(o, sc, bg); o += "▕"; }
    o += ' ';
  }
  o += UL_OFF;
  col(o, BORDER, SBBG);  // the boundary with the editor, in the sidebar's last column
  for (int r = 1; r < H; r++) { at(o, r, sw); o += "│"; }
}

static void drawWatermark(string& o, int th) {  // empty editor group, like VS Code
  static const char* T[][2] = {{"Show Explorer", "Ctrl+E"},   {"Open File", "Ctrl+O"},     {"New Untitled File", "Ctrl+N"},
                               {"Toggle Sidebar", "Ctrl+B"},  {"Find in File", "Ctrl+F"}, {"Quit", "Ctrl+Q"}};
  int n = 6, top = std::max(0, (th - n * 2) / 2), w = 30, pad = std::max(0, (EW - w) / 2);
  for (int r = 0; r < th; r++) {
    at(o, r + 3, EX + 1);
    o += r == th - 1 ? UL_STAT : UL_OFF;
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
  o += UL_ON;
  int x = 0;
  for (int i = g_tabOff; i < (int)B.size(); i++) {
    if (x + tw[i] > EW) break;
    bool a = i == cur;
    const char* bg = a ? BG : TABIN;
    Icon ic = iconFor(B[i].name);
    if (a) o += UL_ACC;  // the active tab's underline is the accent colour
    col(o, a ? WHITE : TABFG, bg);
    o += "  ";
    col(o, ic.c, bg);
    o += ic.g;
    col(o, a ? WHITE : TABFG, bg);
    o += lab[i].substr(2 + strlen(ic.g));
    g_tabs[i] = {EX + x, EX + x + tw[i], EX + x + tw[i] - 2};
    if (a) o += UL_ON;
    x += tw[i];
    if (x < EW) { col(o, TABFG, TABBAR); o += ' '; x++; }
  }
  col(o, TABFG, TABBAR);
  o.append(EW - x, ' ');
  o += UL_OFF;

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
  int crow = -1, ccol = 0;
  if (!has) drawWatermark(o, th);
  else {
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
    // indent guides (│ at each whole 4-column level of leading whitespace); a blank line takes the deeper of the nearest
    // non-blank lines above and below, so guides run through gaps inside a block
    auto indentOf = [&](int y) { const string& l = b.L[y]; int f = firstNonWs(l); return f < len(l) ? rcol(l, f) : -1; };
    auto guideTo = [&](int y) {
      int g = indentOf(y);
      if (g >= 0) return g;
      int up = 0, dn = 0;
      for (int k = y - 1; k >= 0 && k >= y - 100; k--) if ((up = indentOf(k)) >= 0) break;
      for (int k = y + 1; k < n && k <= y + 100; k++) if ((dn = indentOf(k)) >= 0) break;
      return std::max({up, dn, 0});
    };
    for (int r = 0; r < th; r++) {
      int y = b.top + r;
      at(o, r + 3, EX + 1);
      if (r == th - 1) o += UL_STAT;  // the boundary with the status bar
      if (y >= n) { col(o, FGN, BG); o.append(EW, ' '); continue; }
      bool isCur = y == b.cy;
      const char* rowbg = isCur && !hs ? CURL : BG;
      string ln = num(y + 1);
      col(o, isCur ? GUTA : GUT, BG);
      o.append(std::max(0, gw - 2 - len(ln)), ' ');
      o += ln;
      o += "  ";
      const string& s = b.L[y];
      int lim = std::min(len(s), xfromr(s, b.left + txw) + 4);  // bytes that can reach the screen
      cls.assign(lim + 1, C_N);
      if (b.lang) hl(lg, s, stAt(b, y), cls.data(), lim);
      fm.assign(lim + 1, 0);
      if (!g_findQ.empty())
        for (int p = ifind(s, g_findQ, 0); p >= 0 && p < lim; p = ifind(s, g_findQ, p + len(g_findQ)))
          memset(fm.data() + p, 1, std::min(len(g_findQ), lim - p));
      int a = -1, e = -1, gi = guideTo(y), lead = firstNonWs(s);
      auto pad = [&](int c0, int c1) {  // screen-visible columns [c0, c1) of whitespace, with guides
        for (int cc = c0; cc < c1; cc++)
          if (cc + 4 <= gi && cc % 4 == 0) { col(o, GUIDE, g_cb); o += "│"; }
          else o += ' ';
      };
      bool eol = false;
      if (hs && y >= sy0 && y <= sy1) { a = y == sy0 ? sx0 : 0; e = y == sy1 ? sx1 : len(s); eol = y < sy1; }
      int rc = 0, used = 0;
      for (int i = 0; i < len(s) && used < txw; i = nx(s, i)) {
        int w = chw(s, i, rc);
        if (rc + w <= b.left) { rc += w; continue; }
        if (i >= lim) break;
        const char* bg = (i >= a && i < e) ? SELB : fm[i] ? FINDB : rowbg;
        col(o, FG[cls[i]], bg);
        unsigned char c = s[i];
        int vis = std::min(rc + w, b.left + txw) - std::max(rc, b.left);
        if (i < lead && rc >= b.left) pad(rc, rc + vis);
        else if (c == '\t' || rc < b.left || rc + w > b.left + txw) o.append(vis, ' ');
        else if (c < 32 || c == 127) o += '?';
        else o.append(s, i, nx(s, i) - i);
        used += vis;
        rc += w;
      }
      if (eol && used < txw && rc >= b.left) { col(o, FGN, SELB); o += ' '; used++; }
      col(o, FGN, rowbg);
      pad(b.left + used, b.left + txw);
      if (isCur) {
        int cr = rcol(s, b.cx) - b.left;
        if (cr >= 0 && cr < txw) { crow = r + 3; ccol = EX + gw + cr + 1; }
      }
    }
    if (cls.capacity() > 65536) { vector<unsigned char>().swap(cls); vector<char>().swap(fm); }  // after a huge line
  }

  // ── status bar ──
  string left = g_branch.empty() ? "" : " ⎇ " + g_branch + " ", right;
  g_lnX0 = g_lnX1 = -1;
  if (has) {
    Buf& b = B[cur];
    int sel = 0, y0, x0, y1, x1;
    if (selRange(b, y0, x0, y1, x1))
      for (int y = y0; y <= y1; y++) {
        const string& s = b.L[y];
        int a = y == y0 ? x0 : 0, e = y == y1 ? x1 : len(s);
        for (int i = a; i < e; i++) sel += !cont(s[i]);
        sel += y < y1;
      }
    string lc = "Ln " + num(b.cy + 1) + ", Col " + num(rcol(b.L[b.cy], b.cx) + 1);
    if (sel) lc += " (" + num(sel) + " selected)";
    right = lc + "    Spaces: 4    UTF-8    " + (b.crlf ? "CRLF" : "LF") + "    " + b.langName + " ";
    int rw = std::min(dispW(right), W);
    right = fitW(right, rw);
    int rx = W - rw;
    size_t k = right.find("Ln ");
    if (k != string::npos) {
      g_lnX0 = rx + dispW(right.substr(0, k));
      g_lnX1 = g_lnX0 + dispW(right.substr(k, right.find("    ", k) - k));
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
  o += UL_OFF;
  col(o, WHITE, STAT);
  o += left;
  o.append(W - rw - dispW(left), ' ');
  o += right;
  o += "\x1b[0m";
  if (g_pText) { at(o, H, std::min(pcol, W)); o += "\x1b[?25h"; }
  else if (crow > 0 && !g_focusSb) { at(o, crow, ccol); o += "\x1b[?25h"; }
  wr(o);
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
  prompt("Go to line[:col]: ", s, [&](int ev) {
    if (ev != 1) return false;
    int l = 0, c = 0;
    sscanf(s.c_str(), "%d:%d", &l, &c);
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
// Opens a PDF in Okular, detached: its own session (so it outlives vsc and a closed terminal) and no
// stdio (so its warnings never land on our screen). Double fork, so there is no child left to reap.
static void openOkular(const string& path) {
  static string last;
  static long long lastT = 0;
  if (path == last && nowMs() - lastT < 1000) return;  // a double click opens it once
  last = path; lastT = nowMs();
  int ep[2];  // the grandchild reports a failed exec here; a successful one just closes it (O_CLOEXEC)
  if (pipe2(ep, O_CLOEXEC)) { msg("Cannot launch Okular: " + string(strerror(errno))); return; }
  const char* a = path.c_str();
  pid_t pid = fork();
  auto fail = [&] { int e = errno; if (write(ep[1], &e, sizeof e) < 0) {} _exit(1); };
  if (pid == 0) {
    setsid();
    pid_t gc = fork();
    if (gc < 0) fail();
    if (gc == 0) {
      int dn = open("/dev/null", O_RDWR);
      if (dn < 0 || dup2(dn, 0) < 0 || dup2(dn, 1) < 0 || dup2(dn, 2) < 0) fail();  // never onto our screen
      execlp("okular", "okular", a, (char*)nullptr);
      fail();
    }
    _exit(0);
  }
  close(ep[1]);
  int e = 0;
  ssize_t n = 0;
  if (pid > 0) {
    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    while ((n = read(ep[0], &e, sizeof e)) < 0 && errno == EINTR) {}
  } else e = errno, n = sizeof e;
  close(ep[0]);
  if (n == (ssize_t)sizeof e) msg(e == ENOENT ? "okular not found - install it to open PDFs" : "Cannot launch Okular: " + string(strerror(e)));
  else msg("Opened " + baseName(path) + " in Okular");
}
// Opens path in a tab (true), or a PDF in Okular (false: no tab).
static bool openFile(const string& path) {
  for (size_t i = 0; i < B.size(); i++)
    if (B[i].path == path) { cur = (int)i; return true; }
  struct stat stt;
  if (stat(path.c_str(), &stt) == 0 && S_ISDIR(stt.st_mode)) { msg(path + " is a directory"); return false; }
  Buf nb;
  nb.id = ++g_ids;
  nb.path = path;
  nb.name = baseName(path);
  setLang(nb);
  FILE* f = fopen(path.c_str(), "rb");
  int openErr = errno;
  char magic[5] = {0};
  bool pdfMagic = f && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "%PDF", 4);
  if (pdfMagic || extOf(nb.name) == "pdf") {  // Okular's, never a text tab
    if (f) fclose(f);
    if (!f) msg(openErr == ENOENT ? nb.name + " does not exist" : "Cannot open " + path + ": " + strerror(openErr));
    else if (!pdfMagic && stt.st_size == 0) msg(nb.name + " is empty");
    else openOkular(path);
    return false;
  }
  if (f) {
    // Read twice in 64 KB chunks - once to count the lines, once to fill them - so the only copy of the
    // text is the lines themselves (no whole-file buffer) and the line array is allocated exactly once.
    // A line that spans chunks is reserved at its exact length (measured in the first pass) and moved in.
    static char buf[65536];
    size_t lines = 1, total = 0, cl = 0;  // cl: length of the line being counted
    vector<size_t> span;  // lengths of the lines that cross a chunk boundary, in order
    bool bin = false, open = false;  // open: the current line started in an earlier chunk
    rewind(f);
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0; total += k) {
      if (total < 8000 && memchr(buf, 0, std::min(k, 8000 - total))) bin = true;
      for (const char *p = buf, *e = buf + k;;) {
        const char* q = (const char*)memchr(p, '\n', e - p);
        if (!q) { cl += e - p; open = true; break; }
        lines++;
        if (open) span.push_back(cl + (q - p));
        cl = 0; open = false;
        p = q + 1;
      }
    }
    if (open) span.push_back(cl);
    if (bin) { nb.ro = true; msg("Binary file - opened read-only"); }
    nb.L.clear();
    nb.L.reserve(lines);
    rewind(f);
    string part;  // a line that runs across chunks
    size_t si = 0;
    open = false;
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;)
      for (const char *p = buf, *e = buf + k;;) {
        const char* q = (const char*)memchr(p, '\n', e - p);
        if (!q) {
          if (!open && si < span.size()) part.reserve(span[si++]);
          part.append(p, e); open = true;
          break;
        }
        if (!open) nb.L.emplace_back(p, q);
        else { part.append(p, q); nb.L.push_back(std::move(part)); part = string(); }
        open = false;
        p = q + 1;
      }
    nb.L.push_back(std::move(part));
    fclose(f);
    if (!nb.L[0].empty() && nb.L[0].back() == '\r') nb.crlf = true;
    if (nb.crlf)
      for (auto& l : nb.L)
        if (!l.empty() && l.back() == '\r') l.pop_back();
  } else if (openErr != ENOENT) {
    msg("Cannot open " + path + ": " + strerror(openErr));
    return false;
  }
  // replace a lone, untouched Untitled tab
  if (B.size() == 1 && B[0].path.empty() && !B[0].dirty() && B[0].L.size() == 1 && B[0].L[0].empty()) B.clear();
  B.push_back(std::move(nb));
  cur = (int)B.size() - 1;
  return true;
}
static bool save(Buf& b) {
  if (b.ro) return msg("Read-only - not saved");
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
  b.saved = b.head();
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
      b.ub -= opBytes(o);
      o.s += s;
      b.ub += opBytes(o);
      b.cx += len(s);
      o.ey = b.cy; o.ex = b.cx;
      clearR(b);
      g_lastType = o.g;
      trimU(b);  // (may drop o)
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
  if (!dir && openFile(p)) g_focusSb = false;
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
      setLang(b);
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
      else if (openFile(r.path) && k == 13) g_focusSb = false;
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
  else if (openFile(r.path)) g_focusSb = false;
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
  int n = len(b.L);
  if (mB & 64) {  // wheel
    int d = (mB & 1) ? 1 : -1;
    if (mY == 0) { cur = (cur + d + (int)B.size()) % (int)B.size(); return; }
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
    if (mX >= g_lnX0 && mX < g_lnX1) doGoto(b);
    return;
  }
  if (!motion && mY == 1) { g_focusSb = false; return; }  // breadcrumbs
  if (btn != 0) return;
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
        else if (openFile(a)) g_focusSb = false;
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
  editKey(b, k);
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
           "  PDFs open in Okular");
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
