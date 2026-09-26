// sixtest — checks vsc's sixel output without a real terminal.
//
//   sixtest run [-s COLSxROWS] [-p PXWxPXH] [-k MS] -o OUT PROGRAM [ARGS...] -- [STEP...]
//       runs PROGRAM in a pseudo-terminal of that size, plays the steps, saves everything it wrote to OUT
//       and prints the byte offset reached after each step plus the program's peak memory (VmHWM).
//       -a REPLY answers the program's first terminal-ID query (\e[c) with REPLY (C escapes).
//       -t prints, for each key, the time until its output stopped and how many bytes it caused.
//       STEP: keys with C escapes (\e \r \n \t \\ \xNN), each followed by a pause of -k ms (default 800);
//             wait:MS   until:TEXT (up to 30 s)   mark (print the current offset)   sh:COMMAND
//   sixtest list OUT                 every sixel in OUT: index, offset, size, bytes (and any kitty APCs)
//   sixtest decode OUT N IMG.ppm     decode sixel N (negative counts from the end) to a PPM
//   sixtest psnr A.ppm B.ppm [AX AY BX BY W H]   PSNR of B against A, over a region if given
//   sixtest count IMG.ppm X Y W H R G B          pixels in the region that are not exactly R;G;B
//
// Build: make sixtest
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

using std::string;
using std::vector;

static string slurp(const char* path) {
  string s;
  if (FILE* f = fopen(path, "rb")) {
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
  } else {
    perror(path);
    exit(1);
  }
  return s;
}
static long long nowMs() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

// ── image ──
struct Img {
  int w = 0, h = 0;
  vector<unsigned char> px;  // RGB
  vector<unsigned char> hits;  // sixel: how many times each pixel was painted
};
static bool savePpm(const Img& im, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
  fwrite(im.px.data(), 1, im.px.size(), f);
  return fclose(f) == 0;
}
static Img loadPpm(const char* path) {
  string s = slurp(path);
  Img im;
  int maxv, off = 0;
  if (sscanf(s.c_str(), "P6 %d %d %d%n", &im.w, &im.h, &maxv, &off) != 3 || maxv != 255) {
    fprintf(stderr, "%s: not a binary PPM\n", path);
    exit(1);
  }
  off++;  // the single whitespace after maxval
  im.px.assign(s.begin() + off, s.begin() + off + (size_t)im.w * im.h * 3);
  return im;
}

// ── sixel decoder ──
static vector<std::pair<size_t, size_t>> findSixels(const string& d) {  // [start, end) of each DCS ... ST
  vector<std::pair<size_t, size_t>> r;
  for (size_t p = 0; (p = d.find("\x1bP", p)) != string::npos;) {
    size_t q = p + 2;
    while (q < d.size() && (isdigit((unsigned char)d[q]) || d[q] == ';')) q++;
    size_t e = d.find("\x1b\\", q);
    if (q >= d.size() || d[q] != 'q' || e == string::npos) { p += 2; continue; }
    r.push_back({p, e + 2});
    p = e + 2;
  }
  return r;
}
static Img decodeSixel(const string& s) {
  Img im;
  size_t i = s.find('q') + 1, end = s.size() - 2;
  int pw = 0, ph = 0;
  if (s[i] == '"') {
    int a, b;
    if (sscanf(s.c_str() + i + 1, "%d;%d;%d;%d", &a, &b, &pw, &ph) != 4) { pw = ph = 0; }
    while (i < end && (s[i] == '"' || isdigit((unsigned char)s[i]) || s[i] == ';')) i++;
  }
  // first pass would be needed for images without raster attributes; vsc always sends them
  if (pw <= 0 || ph <= 0) { fprintf(stderr, "sixel without raster attributes\n"); exit(1); }
  im.w = pw; im.h = ph;
  im.px.assign((size_t)pw * ph * 3, 0);
  im.hits.assign((size_t)pw * ph, 0);
  int pal[256][3] = {};
  int c = 0, x = 0, y = 0;
  auto readNum = [&](int& v) {
    v = 0;
    bool any = false;
    while (i < end && isdigit((unsigned char)s[i])) { v = v * 10 + (s[i++] - '0'); any = true; }
    return any;
  };
  while (i < end) {
    char ch = s[i];
    if (ch == '#') {
      i++;
      int n, t, a, b2, cc;
      readNum(n);
      if (i < end && s[i] == ';') {
        i++; readNum(t); i++; readNum(a); i++; readNum(b2); i++; readNum(cc);
        if (t == 2 && n < 256) {
          pal[n][0] = (a * 255 + 50) / 100; pal[n][1] = (b2 * 255 + 50) / 100; pal[n][2] = (cc * 255 + 50) / 100;
        }
      } else c = n & 255;
      continue;
    }
    int rep = 1;
    if (ch == '!') { i++; readNum(rep); ch = s[i]; }
    i++;
    if (ch == '$') x = 0;
    else if (ch == '-') { x = 0; y += 6; }
    else if (ch >= 63 && ch <= 126) {
      int bits = ch - 63;
      for (int k = 0; k < rep; k++, x++)
        for (int r = 0; r < 6; r++)
          if ((bits >> r & 1) && x < pw && y + r < ph) {
            size_t p = (size_t)(y + r) * pw + x;
            im.px[p * 3] = (unsigned char)pal[c][0]; im.px[p * 3 + 1] = (unsigned char)pal[c][1];
            im.px[p * 3 + 2] = (unsigned char)pal[c][2];
            if (im.hits[p] < 255) im.hits[p]++;
          }
    }
  }
  return im;
}

// ── pty driver ──
static string unescape(const string& s) {
  string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '\\' || i + 1 == s.size()) { r += s[i]; continue; }
    char c = s[++i];
    if (c == 'e') r += '\x1b';
    else if (c == 'r') r += '\r';
    else if (c == 'n') r += '\n';
    else if (c == 't') r += '\t';
    else if (c == 'x' && i + 2 < s.size() + 1) { r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
    else r += c;
  }
  return r;
}
static int cmdRun(int argc, char** argv) {
  int cols = 160, rows = 50, pxw = 1600, pxh = 1050, keyMs = 800, a = 2;
  bool timing = false;
  string da1;
  const char* out = nullptr;
  for (; a < argc && argv[a][0] == '-'; a++) {
    if (!strcmp(argv[a], "-s")) sscanf(argv[++a], "%dx%d", &cols, &rows);
    else if (!strcmp(argv[a], "-p")) sscanf(argv[++a], "%dx%d", &pxw, &pxh);
    else if (!strcmp(argv[a], "-k")) keyMs = atoi(argv[++a]);
    else if (!strcmp(argv[a], "-o")) out = argv[++a];
    else if (!strcmp(argv[a], "-t")) timing = true;
    else if (!strcmp(argv[a], "-a")) da1 = unescape(argv[++a]);
  }
  if (!out || a >= argc) { fputs("sixtest run: need -o OUT and a program\n", stderr); return 2; }
  vector<char*> prog;
  for (; a < argc && strcmp(argv[a], "--"); a++) prog.push_back(argv[a]);
  prog.push_back(nullptr);
  vector<string> steps;
  for (a++; a < argc; a++) steps.push_back(argv[a]);

  winsize ws{(unsigned short)rows, (unsigned short)cols, (unsigned short)pxw, (unsigned short)pxh};
  int fd;
  pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
  if (pid < 0) { perror("forkpty"); return 1; }
  if (pid == 0) {
    execvp(prog[0], prog.data());
    _exit(127);
  }
  string cap;
  long hwm = 0;
  bool alive = true;
  auto sample = [&] {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (FILE* f = fopen(path, "r")) {
      char l[256];
      while (fgets(l, sizeof l, f))
        if (!strncmp(l, "VmHWM:", 6)) hwm = std::max(hwm, atol(l + 6));
      fclose(f);
    }
  };
  long long lastByte = 0;
  auto pump = [&](int ms, const char* until) {
    size_t from = cap.size();
    long long end = nowMs() + ms;
    while (alive && nowMs() < end) {
      pollfd p{fd, POLLIN, 0};
      if (poll(&p, 1, 20) > 0) {
        char buf[65536];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) { alive = false; break; }
        cap.append(buf, n);
        lastByte = nowMs();
        if (!da1.empty() && cap.find("\x1b[c") != string::npos) {
          if (write(fd, da1.data(), da1.size()) < 0) break;
          da1.clear();
        }
      }
      sample();
      if (until && cap.find(until, from) != string::npos) { until = nullptr; end = nowMs() + 600; }
    }
    return !until;
  };
  pump(2500, nullptr);
  for (auto& st : steps) {
    if (!st.compare(0, 5, "wait:")) pump(atoi(st.c_str() + 5), nullptr);
    else if (!st.compare(0, 6, "until:")) printf("until %s: %s\n", st.c_str() + 6, pump(30000, st.c_str() + 6) ? "seen" : "TIMED OUT");
    else if (st == "mark") printf("mark %zu\n", cap.size());
    else if (!st.compare(0, 3, "sh:")) { if (system(st.c_str() + 3)) printf("sh failed: %s\n", st.c_str() + 3); }
    else {
      string k = unescape(st);
      size_t before = cap.size();
      long long t = nowMs();
      lastByte = 0;
      if (write(fd, k.data(), k.size()) < 0) break;
      pump(keyMs, nullptr);
      if (timing)
        printf("key %-8s %4lld ms  %7zu bytes\n", st.c_str(), lastByte ? lastByte - t : 0, cap.size() - before);
    }
  }
  printf("peak VmHWM %ld kB\n", hwm);
  if (alive && write(fd, "\x11", 1) == 1) pump(800, nullptr);  // Ctrl+Q
  kill(pid, SIGTERM);
  waitpid(pid, nullptr, 0);
  FILE* f = fopen(out, "wb");
  if (!f || fwrite(cap.data(), 1, cap.size(), f) != cap.size() || fclose(f)) { perror(out); return 1; }
  printf("captured %zu bytes\n", cap.size());
  return 0;
}

int main(int argc, char** argv) {
  string cmd = argc > 1 ? argv[1] : "";
  if (cmd == "run") return cmdRun(argc, argv);
  if (cmd == "list" && argc == 3) {
    string d = slurp(argv[2]);
    auto six = findSixels(d);
    int i = 0;
    for (auto& [a, e] : six) {
      int w = 0, h = 0, x, y;
      size_t q = d.find('"', a);
      if (q < e) sscanf(d.c_str() + q + 1, "%d;%d;%d;%d", &x, &y, &w, &h);
      printf("%d at %zu: %dx%d, %zu bytes\n", i++, a, w, h, e - a);
    }
    size_t k = 0, n = 0;
    while ((k = d.find("\x1b_G", k)) != string::npos) { n++; k += 3; }
    printf("%zu sixel(s), %zu kitty graphics sequence(s)\n", six.size(), n);
    return 0;
  }
  if (cmd == "decode" && argc == 5) {
    string d = slurp(argv[2]);
    auto six = findSixels(d);
    int n = atoi(argv[3]), k = n < 0 ? (int)six.size() + n : n;
    if (k < 0 || k >= (int)six.size()) { fprintf(stderr, "no sixel %d (have %zu)\n", n, six.size()); return 1; }
    Img im = decodeSixel(d.substr(six[k].first, six[k].second - six[k].first));
    size_t unset = 0, twice = 0;
    for (auto h : im.hits) { unset += h == 0; twice += h > 1; }
    printf("sixel %d: %dx%d, %zu pixels never painted, %zu painted twice\n", k, im.w, im.h, unset, twice);
    return savePpm(im, argv[4]) ? 0 : 1;
  }
  if (cmd == "psnr" && (argc == 4 || argc == 10)) {
    Img A = loadPpm(argv[2]), Bm = loadPpm(argv[3]);
    int ax = 0, ay = 0, bx = 0, by = 0, w = std::min(A.w, Bm.w), h = std::min(A.h, Bm.h);
    if (argc == 10) { ax = atoi(argv[4]); ay = atoi(argv[5]); bx = atoi(argv[6]); by = atoi(argv[7]); w = atoi(argv[8]); h = atoi(argv[9]); }
    if (ax + w > A.w || ay + h > A.h || bx + w > Bm.w || by + h > Bm.h || w <= 0 || h <= 0) { fputs("region out of range\n", stderr); return 1; }
    double se = 0;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w * 3; x++) {
        double d = A.px[((size_t)(ay + y) * A.w + ax) * 3 + x] - (double)Bm.px[((size_t)(by + y) * Bm.w + bx) * 3 + x];
        se += d * d;
      }
    double mse = se / ((double)w * h * 3);
    if (mse == 0) printf("PSNR inf (identical)\n");
    else printf("PSNR %.2f dB\n", 10 * log10(255.0 * 255.0 / mse));
    return 0;
  }
  if (cmd == "count" && argc == 10) {
    Img A = loadPpm(argv[2]);
    int x0 = atoi(argv[3]), y0 = atoi(argv[4]), w = atoi(argv[5]), h = atoi(argv[6]);
    unsigned char c[3] = {(unsigned char)atoi(argv[7]), (unsigned char)atoi(argv[8]), (unsigned char)atoi(argv[9])};
    size_t n = 0;
    for (int y = y0; y < y0 + h && y < A.h; y++)
      for (int x = x0; x < x0 + w && x < A.w; x++) n += memcmp(&A.px[((size_t)y * A.w + x) * 3], c, 3) != 0;
    printf("%zu pixel(s) differ from %d;%d;%d\n", n, c[0], c[1], c[2]);
    return 0;
  }
  fputs("usage: sixtest run|list|decode|psnr|count ... (see the top of sixtest.cpp)\n", stderr);
  return 2;
}
