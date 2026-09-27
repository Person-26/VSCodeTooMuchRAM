// vsc-pdf — the page renderer behind vsc's PDF tiles. It runs Okular's own document engine (libOkular6Core,
// the same generators, fonts and settings as the Okular app) with no window, and hands finished pages to vsc as
// raw RGB in POSIX shared memory, whose names vsc passes on to the terminal (kitty graphics, t=s). vsc is the only
// process that writes to the terminal; this one only talks to vsc, over its stdin/stdout:
//
//   in   o <doc> <path>             open a document
//        w <doc> <page> <w> <h> ... the pages wanted now, at w x h pixels each: any others still queued are
//                                   dropped (like Okular's view, which only ever asks for what it will show)
//        c <doc>                    close a document
//   out  n <doc> <pages> <w0> <h0> <w1> <h1> ...   opened: each page's size in points
//        e <doc> <message>                         could not open
//        p <doc> <page> <w> <h> <shm name>         a rendered page, w*h*3 bytes of RGB
//
// Build: make vsc-pdf (needs okular-devel)
#include <QApplication>
#include <QImage>
#include <QMimeDatabase>
#include <QPixmap>
#include <QSocketNotifier>
#include <okular/core/area.h>
#include <okular/core/document.h>
#include <okular/core/generator.h>
#include <okular/core/observer.h>
#include <okular/core/page.h>
#include <settings_core.h>

#include <cstdio>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <map>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

// Page::_o_nearestPixmap is exported but private (Okular's own views reach it as friends). This is the
// standard-conforming way to take a private member's address: explicit instantiation ignores access.
using NearestFn = const QPixmap *(Okular::Page::*)(Okular::DocumentObserver *, int, int) const;
template <NearestFn F> struct Steal { friend NearestFn nearest() { return F; } };
NearestFn nearest();
template struct Steal<&Okular::Page::_o_nearestPixmap>;

static void out(const std::string &s) {
    for (size_t n = 0; n < s.size();) {
        ssize_t k = write(1, s.data() + n, s.size() - n);
        if (k < 0) {
            if (errno == EINTR) continue;
            _exit(0);  // vsc is gone
        }
        n += size_t(k);
    }
}

struct Doc : Okular::DocumentObserver {
    int id;
    Okular::Document doc{nullptr};
    std::map<int, std::pair<int, int>> want;  // page -> the size vsc asked for, until it is sent
    explicit Doc(int i) : id(i) { doc.addObserver(this); }
    ~Doc() override { doc.removeObserver(this); doc.closeDocument(); }
    void notifyPageChanged(int page, int flags) override {
        if (flags & Pixmap) send(page);
    }
    void send(int page) {
        auto it = want.find(page);
        if (it == want.end()) return;
        auto [w, h] = it->second;
        const QPixmap *px = (doc.page(page)->*nearest())(this, w, h);
        if (!px || px->width() != w || px->height() != h) return;  // an older size: the new one is still coming
        want.erase(it);
        QImage im = px->toImage().convertToFormat(QImage::Format_RGB888);
        static unsigned seq = 0;
        std::string name = "/vsc-pdf." + std::to_string(getpid()) + "." + std::to_string(++seq);
        int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) return;
        size_t row = size_t(w) * 3, n = row * size_t(h);
        void *m = ftruncate(fd, off_t(n)) == 0 ? mmap(nullptr, n, PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
        close(fd);
        if (m == MAP_FAILED) { shm_unlink(name.c_str()); return; }
        for (int y = 0; y < h; y++) memcpy((char *)m + row * y, im.constScanLine(y), row);  // drop row padding
        munmap(m, n);
        out("p " + std::to_string(id) + ' ' + std::to_string(page) + ' ' + std::to_string(w) + ' ' + std::to_string(h) + ' ' + name + '\n');
    }
};

static std::map<int, std::unique_ptr<Doc>> docs;

static void command(const char *l) {
    int id = 0, page = 0, w = 0, h = 0, off = 0;
    if (l[0] == 'o' && sscanf(l, "o %d %n", &id, &off) == 1 && off) {
        auto d = std::make_unique<Doc>(id);
        QString f = QString::fromLocal8Bit(l + off);
        auto r = d->doc.openDocument(f, QUrl::fromLocalFile(f), QMimeDatabase().mimeTypeForFile(f));
        if (r != Okular::Document::OpenSuccess) {
            out("e " + std::to_string(id) + (r == Okular::Document::OpenNeedsPassword ? " is password-protected\n" : " could not be opened\n"));
            return;
        }
        std::string s = "n " + std::to_string(id) + ' ' + std::to_string(d->doc.pages());
        char b[64];
        for (uint i = 0; i < d->doc.pages(); i++) {
            const Okular::Page *p = d->doc.page(i);
            snprintf(b, sizeof b, " %.3f %.3f", p->width(), p->height());
            s += b;
        }
        out(s + '\n');
        docs[id] = std::move(d);
    } else if (l[0] == 'c' && sscanf(l, "c %d", &id) == 1) {
        docs.erase(id);
    }
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");  // Okular's core needs a QApplication, never a window
    QApplication app(argc, argv);
    Okular::SettingsCore::instance(QStringLiteral("okularpartrc"));  // Okular's own settings (rendering, ...)
    // but keep few pixmaps: each page goes to the terminal once, which keeps it (in memory only, never saved)
    Okular::SettingsCore::self()->setMemoryLevel(Okular::SettingsCore::EnumMemoryLevel::Low);
    signal(SIGPIPE, SIG_IGN);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    std::string in;
    QSocketNotifier sn(0, QSocketNotifier::Read);
    QObject::connect(&sn, &QSocketNotifier::activated, [&] {
        char b[65536];
        ssize_t k;
        bool eof = false;
        while ((k = read(0, b, sizeof b)) > 0) in.append(b, size_t(k));
        if (k == 0) eof = true;
        // the last want list per document wins; one requestPixmaps call each, replacing all earlier requests
        std::map<int, QList<Okular::PixmapRequest *>> batch;
        size_t s = 0;
        for (size_t e; (e = in.find('\n', s)) != std::string::npos; s = e + 1) {
            in[e] = 0;
            const char *l = in.c_str() + s;
            int id, page, w, h, off;
            if (l[0] == 'w' && sscanf(l, "w %d%n", &id, &off) == 1) {
                auto it = docs.find(id);
                if (it == docs.end()) continue;
                Doc *d = it->second.get();
                auto &reqs = batch[id];
                qDeleteAll(reqs);
                reqs.clear();
                d->want.clear();
                for (const char *q = l + off; sscanf(q, " %d %d %d%n", &page, &w, &h, &off) == 3; q += off) {
                    if (page < 0 || uint(page) >= d->doc.pages() || w <= 0 || h <= 0) continue;
                    d->want[page] = {w, h};
                    if (d->doc.page(page)->hasPixmap(d, w, h)) d->send(page);
                    else {
                        auto *r = new Okular::PixmapRequest(d, page, w, h, 1.0, 1, Okular::PixmapRequest::Asynchronous);
                        // the whole page, said explicitly: Okular counts an unset rect as 0% of the page, and above
                        // 4x the screen's area (our offscreen screen is 800x600) it would switch such a request to
                        // tiles and then drop it for having no rect
                        r->setNormalizedRect(Okular::NormalizedRect(0, 0, 1, 1));
                        reqs.append(r);
                    }
                }
            } else command(l);
        }
        in.erase(0, s);
        for (auto &[id, reqs] : batch)
            if (docs.count(id)) docs[id]->doc.requestPixmaps(reqs, Okular::Document::RemoveAllPrevious);
            else qDeleteAll(reqs);
        if (eof) app.quit();
    });
    int r = app.exec();
    docs.clear();
    return r;
}
