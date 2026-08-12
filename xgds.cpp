// xgds - X gmenu desktop switcher
// Dependencies: X11, XShm, XRandR, gmenu
//
// Screenshots are handled as raw P6 PPMs end-to-end (capture, crop,
// composite) with plain fread/fwrite/memcpy — see savePpm,
// cropWindowScreenshot, and compositeDesktopScreenshot. There is
// intentionally no image-library dependency: every file this program reads
// was written by this program, so there's no format to negotiate.

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <unistd.h>
#include <spawn.h>
#include <fcntl.h>
#include <signal.h>

#include <climits>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <sstream>

#define NAME       "xgds"
#define NAME_UPPER "XGDS"
#define GMENU      "gmenu"

static std::string runDir;
static std::string sockPath;

// ============================================================
// Helpers
// ============================================================

static const std::string runtimeDir() {
    // XDG_RUNTIME_DIR is guaranteed by systemd/PAM on any modern Linux
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return std::string(xdg);
    // Fallback for environments without systemd
    return "/run/user/" + std::to_string(getuid());
}

static void initPaths() {
    const auto p = runtimeDir();
    runDir   = p + "/" NAME;
    sockPath = p + "/" NAME "/daemon.sock";
}

static std::string desktopScreenshotPath(long desk) {
    return runDir + "/" + std::to_string(desk) + ".ppm";
}

// Temp file for a single monitor's raw capture, used only as input to the
// background crop queue (see ScreenshotRef) and deleted once every window
// crop has consumed it. This must NOT share a name with
// desktopScreenshotPath(): that path is also written by the picker
// (compositeDesktopScreenshot) as the persistent "current desktop icon"
// file, and if the two collided, the daemon's worker thread could delete
// or overwrite the picker's freshly-composited icon out from under it —
// which is exactly what caused the current desktop to intermittently
// vanish from `xgds switch`. A monotonic sequence number keeps every
// capture's filename unique even across rapid back-to-back requests.
static std::atomic<unsigned long> g_captureSeq{0};

static std::string rawCapturePath(size_t monitorIndex) {
    return runDir + "/cap-" + std::to_string(monitorIndex) + "-" +
           std::to_string(g_captureSeq.fetch_add(1)) + ".ppm";
}

static std::string windowScreenshotPath(Window w) {
    return runDir + "/win-" + std::to_string((unsigned long)w) + ".ppm";
}

static void ensureDir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, NAME ": mkdir(%s): %s\n", path, strerror(errno));
}

static void clearDir(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    char full[PATH_MAX];
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (ent->d_type == DT_DIR) continue;
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        unlink(full);
    }
    closedir(dir);
}

// Strip trailing newline
static void chomp(std::string &s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

// Read all of fd until EOF into a string.
static std::string readFd(int fd) {
    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, n);
    return out;
}

// Read an env var like "cmd arg1 arg2" and split on spaces into a vector.
// Returns empty vector if the var is unset or empty.
static std::vector<std::string> envCmd(const char* var) {
    const char* val = getenv(var);
    if (!val || !*val) return {};
    std::vector<std::string> parts;
    std::istringstream ss(val);
    std::string tok;
    while (ss >> tok)
        parts.push_back(std::move(tok));
    return parts;
}

// Build a null-terminated argv from a vector of strings, optionally appending
// one extra argument (e.g. the desktop id). The strings must outlive the array.
static std::vector<char*> makeArgv(std::vector<std::string>& parts,
                                    const char* extra = nullptr) {
    std::vector<char*> argv;
    argv.reserve(parts.size() + 2);
    for (auto& p : parts)
        argv.push_back(p.data());
    if (extra)
        argv.push_back(const_cast<char*>(extra));
    argv.push_back(nullptr);
    return argv;
}

// Spawn a command from a pre-split vector and optionally an extra arg.
// Returns false if the vector is empty or the spawn fails.
static bool spawnCmd(std::vector<std::string>& parts,
                      const char* extra = nullptr) {
    if (parts.empty()) return false;
    auto argv = makeArgv(parts, extra);
    pid_t pid;
    if (posix_spawnp(&pid, parts[0].c_str(), nullptr, nullptr,
                     argv.data(), environ) != 0) {
        perror((NAME ": posix_spawnp: " + parts[0]).c_str());
        return false;
    }
    waitpid(pid, nullptr, 0);
    return true;
}

// ============================================================
// X11 error handling
// ============================================================

// Xlib's default error handler calls exit() on any protocol error (e.g.
// BadWindow from XGetWindowProperty on a window that has since been
// destroyed). Since we routinely query windows we discovered a moment
// earlier (_NET_CLIENT_LIST, _NET_ACTIVE_WINDOW), those windows can
// legitimately disappear out from under us — that's not a bug, it's a
// race against the rest of the desktop. Log and continue instead of
// crashing the whole picker over one stale window.
static int xErrorHandler(Display *dpy, XErrorEvent *ev) {
    static char buf[128];
    XGetErrorText(dpy, ev->error_code, buf, sizeof(buf));
    // fprintf(stderr, NAME ": X error ignored: %s (request %d.%d, resource 0x%lx)\n",
    //         buf, ev->request_code, ev->minor_code, ev->resourceid);
    return 0; // return value is ignored by Xlib
}

// ============================================================
// X11 atoms — cached once per Display
// ============================================================

struct Atoms {
    Atom netNumberOfDesktops;
    Atom netCurrentDesktop;
    Atom netDesktopViewport;
    Atom netClientList;
    Atom netWmDesktop;
    Atom netWmName;
    Atom netActiveWindow;
    Atom utf8String;
};

static Atoms initAtoms(Display* dpy) {
    Atoms a;
    a.netNumberOfDesktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    a.netCurrentDesktop   = XInternAtom(dpy, "_NET_CURRENT_DESKTOP",    False);
    a.netDesktopViewport  = XInternAtom(dpy, "_NET_DESKTOP_VIEWPORT",   False);
    a.netClientList       = XInternAtom(dpy, "_NET_CLIENT_LIST",        False);
    a.netWmDesktop        = XInternAtom(dpy, "_NET_WM_DESKTOP",         False);
    a.netWmName           = XInternAtom(dpy, "_NET_WM_NAME",            False);
    a.netActiveWindow     = XInternAtom(dpy, "_NET_ACTIVE_WINDOW",      False);
    a.utf8String          = XInternAtom(dpy, "UTF8_STRING",             False);
    return a;
}

// ============================================================
// X11 helpers
// ============================================================

static int getCurrentDesktopIndex(Display* dpy, const Atoms& atoms) {
    if (!dpy) return -1;

    Window root = DefaultRootWindow(dpy);

    Atom type;
    int format;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, root, atoms.netCurrentDesktop,
                           0, 1, False, XA_CARDINAL,
                           &type, &format, &nitems,
                           &bytesAfter, &data) != Success || !data)
        return -1;

    int index = *reinterpret_cast<int*>(data);
    XFree(data);
    return index;
}

static std::string getWindowTitle(Display* dpy, Window w, const Atoms& atoms) {
    Atom actualType;
    int actualFormat;
    unsigned long nItems, bytesAfter;
    unsigned char* prop = nullptr;

    // _NET_WM_NAME (UTF-8)
    if (XGetWindowProperty(dpy, w, atoms.netWmName,
                           0, (~0L), False, atoms.utf8String,
                           &actualType, &actualFormat,
                           &nItems, &bytesAfter,
                           &prop) == Success && prop) {
        std::string title(reinterpret_cast<char*>(prop));
        XFree(prop);
        return title;
    }

    if (prop) XFree(prop);

    // fallback WM_NAME
    XTextProperty tp;
    if (XGetWMName(dpy, w, &tp) && tp.value) {
        std::string title(reinterpret_cast<char*>(tp.value));
        XFree(tp.value);
        return title;
    }

    return {};
}

static std::map<long, std::string> getDesktopsWindowTitles(Display* dpy, const Atoms& atoms) {
    std::map<long, std::string> result;

    Window root = DefaultRootWindow(dpy);

    Atom actualType;
    int actualFormat;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy,
                           root,
                           atoms.netClientList,
                           0,
                           (~0L),
                           False,
                           XA_WINDOW,
                           &actualType,
                           &actualFormat,
                           &nitems,
                           &bytesAfter,
                           &data) != Success || !data)
        {
            return result;
        }

    std::map<unsigned long, std::vector<std::string>> desktops;

    Window* windows = reinterpret_cast<Window*>(data);
    unsigned long nwindows = nitems;

    for (unsigned long i = 0; i < nwindows; ++i) {
        Window w = windows[i];

        // Desktop index
        unsigned char* deskData = nullptr;
        unsigned long desktopItems;

        if (XGetWindowProperty(dpy,
                               w,
                               atoms.netWmDesktop,
                               0,
                               1,
                               False,
                               XA_CARDINAL,
                               &actualType,
                               &actualFormat,
                               &desktopItems,
                               &bytesAfter,
                               &deskData) != Success || !deskData)
            {
                continue;
            }

        unsigned long desktop = *reinterpret_cast<unsigned long*>(deskData);
        XFree(deskData);

        // Window title
        std::string title = getWindowTitle(dpy, w, atoms);
        if (!title.empty())
            desktops[desktop].push_back(std::move(title));
    }

    XFree(data);

    int displayDesktop = 1;
    for (const auto& [desktop, titles] : desktops) {
        std::string line = std::to_string(displayDesktop++) + ": ";

        for (size_t i = 0; i < titles.size(); ++i) {
            if (i)
                line += ", ";
            line += titles[i];
        }

        result[static_cast<long>(desktop)] = std::move(line);
    }

    return result;
}

// Enumerate individual windows (in _NET_CLIENT_LIST order) with their titles.
// Unlike getDesktopsWindowTitles(), this keeps each window separate so a
// specific one can be targeted for window switching.
static std::vector<std::pair<Window, std::string>> getWindowList(Display* dpy, const Atoms& atoms) {
    std::vector<std::pair<Window, std::string>> result;

    Window root = DefaultRootWindow(dpy);

    Atom actualType;
    int actualFormat;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, root, atoms.netClientList, 0, (~0L), False,
                           XA_WINDOW, &actualType, &actualFormat,
                           &nitems, &bytesAfter, &data) != Success || !data)
        return result;

    Window* windows = reinterpret_cast<Window*>(data);
    for (unsigned long i = 0; i < nitems; ++i) {
        std::string title = getWindowTitle(dpy, windows[i], atoms);
        if (!title.empty())
            result.emplace_back(windows[i], std::move(title));
    }

    XFree(data);
    return result;
}

// ============================================================
// Window geometry metadata (for cropping/compositing window images)
// ============================================================

// A window's on-screen geometry, relative to the top-left of whichever
// monitor screenshot it was captured from. Persisted per window (keyed by
// its immutable window id), not per desktop: desktop *numbers* are
// renumbered by the WM whenever desktops are inserted/removed, but a
// window's id and its rough on-screen position don't change just because
// some other desktop got renamed around it.
struct WinMeta {
    int x, y, w, h;
};

// Same, but still in root-window (absolute) coordinates and tagged with the
// desktop the window currently lives on. Used only while capturing.
struct WinGeom {
    Window id;
    long   desktop;
    int    x, y, w, h;
};

// Enumerate every mapped client window with its desktop and absolute
// on-screen geometry. This is cheap (a handful of property/attribute
// queries per window) compared to actually rasterizing window contents.
static std::vector<WinGeom> getAllWindowGeometries(Display* dpy, const Atoms& atoms) {
    std::vector<WinGeom> result;

    Window root = DefaultRootWindow(dpy);

    Atom actualType;
    int actualFormat;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, root, atoms.netClientList, 0, (~0L), False,
                           XA_WINDOW, &actualType, &actualFormat,
                           &nitems, &bytesAfter, &data) != Success || !data)
        return result;

    Window* windows = reinterpret_cast<Window*>(data);
    for (unsigned long i = 0; i < nitems; ++i) {
        Window w = windows[i];

        unsigned char* deskData = nullptr;
        unsigned long deskItems;
        long desktop = -1;

        if (XGetWindowProperty(dpy, w, atoms.netWmDesktop, 0, 1, False,
                               XA_CARDINAL, &actualType, &actualFormat,
                               &deskItems, &bytesAfter, &deskData) == Success
            && deskData) {
            desktop = (long)*reinterpret_cast<unsigned long*>(deskData);
            XFree(deskData);
        }
        if (desktop < 0) continue;

        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable)
            continue;

        Window child;
        int rx = wa.x, ry = wa.y;
        XTranslateCoordinates(dpy, w, root, 0, 0, &rx, &ry, &child);

        result.push_back({ w, desktop, rx, ry, wa.width, wa.height });
    }

    XFree(data);
    return result;
}

// Live (uncached) grouping of windows by their current desktop. Always
// queried fresh at the moment it's needed, so it's never stale relative to
// desktop renumbering — unlike anything we might have cached from a
// previous capture.
static std::map<long, std::vector<Window>> getDesktopWindowIds(Display* dpy, const Atoms& atoms) {
    std::map<long, std::vector<Window>> result;

    Window root = DefaultRootWindow(dpy);

    Atom actualType;
    int actualFormat;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, root, atoms.netClientList, 0, (~0L), False,
                           XA_WINDOW, &actualType, &actualFormat,
                           &nitems, &bytesAfter, &data) != Success || !data)
        return result;

    Window* windows = reinterpret_cast<Window*>(data);
    for (unsigned long i = 0; i < nitems; ++i) {
        Window w = windows[i];

        unsigned char* deskData = nullptr;
        unsigned long deskItems;

        if (XGetWindowProperty(dpy, w, atoms.netWmDesktop, 0, 1, False,
                               XA_CARDINAL, &actualType, &actualFormat,
                               &deskItems, &bytesAfter, &deskData) != Success
            || !deskData)
            continue;

        long desktop = (long)*reinterpret_cast<unsigned long*>(deskData);
        XFree(deskData);

        result[desktop].push_back(w);
    }

    XFree(data);
    return result;
}

static std::string windowMetaPath(Window w) {
    return runDir + "/win-" + std::to_string((unsigned long)w) + ".meta";
}

static void writeWindowMeta(Window w, int x, int y, int width, int height) {
    const std::string path = windowMetaPath(w);
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) {
        fprintf(stderr, NAME ": fopen(%s): %s\n", path.c_str(), strerror(errno));
        return;
    }
    fprintf(fp, "%d %d %d %d\n", x, y, width, height);
    fclose(fp);
}

static bool readWindowMeta(Window w, WinMeta &out) {
    const std::string path = windowMetaPath(w);
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return false;
    bool ok = fscanf(fp, "%d %d %d %d", &out.x, &out.y, &out.w, &out.h) == 4;
    fclose(fp);
    return ok;
}

static bool switchDesktop(Display *dpy, long desktop, const Atoms& atoms) {
    Window root = DefaultRootWindow(dpy);

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = atoms.netCurrentDesktop;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = desktop;
    ev.xclient.data.l[1] = CurrentTime;

    XSendEvent(dpy,
               root,
               False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               &ev);

    XFlush(dpy);
    return true;
}

static bool moveFocusedWindowToDesktop(Display *dpy, long desktop, const Atoms& atoms) {
    Window root = DefaultRootWindow(dpy);

    Atom actual;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = nullptr;

    if (XGetWindowProperty(dpy,
                           root,
                           atoms.netActiveWindow,
                           0,
                           1,
                           False,
                           XA_WINDOW,
                           &actual,
                           &format,
                           &nitems,
                           &bytes_after,
                           &data) != Success ||
        !data)
        return false;

    Window win = *(Window *)data;
    XFree(data);

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms.netWmDesktop;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = desktop;
    ev.xclient.data.l[1] = CurrentTime;

    XSendEvent(dpy,
               root,
               False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               &ev);

    XFlush(dpy);
    return true;
}

static bool moveFocusedWindowAndSwitch(Display *dpy, long desktop, const Atoms& atoms) {
    if (!moveFocusedWindowToDesktop(dpy, desktop, atoms))
        return false;
    return switchDesktop(dpy, desktop, atoms);
}

// Activate a specific window (switch to its desktop and raise/focus it),
// per the EWMH _NET_ACTIVE_WINDOW client message convention.
static bool switchWindow(Display *dpy, Window win, const Atoms& atoms) {
    Window root = DefaultRootWindow(dpy);

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = atoms.netActiveWindow;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2; // source indication: pager/task-switcher
    ev.xclient.data.l[1] = CurrentTime;

    XSendEvent(dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask,
               &ev);

    XFlush(dpy);
    return true;
}

// ============================================================
// X11 monitor enumeration
// ============================================================

struct Monitor {
    int x, y, w, h;
    std::string name;
};

static std::vector<Monitor> getMonitors(Display *dpy) {
    std::vector<Monitor> mons;
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    int rr_event, rr_error;
    if (!XRRQueryExtension(dpy, &rr_event, &rr_error)) {
        mons.push_back({ 0, 0, DisplayWidth(dpy, screen), DisplayHeight(dpy, screen), "screen" });
        fprintf(stderr, NAME ": RandR unavailable, using single monitor\n");
        return mons;
    }

    XRRScreenResources *res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) { fprintf(stderr, NAME ": XRRGetScreenResourcesCurrent failed\n"); return mons; }

    for (int i = 0; i < res->noutput; ++i) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;
        if (oi->connection != RR_Connected || oi->crtc == None) { XRRFreeOutputInfo(oi); continue; }

        XRRCrtcInfo *ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
        if (!ci) { XRRFreeOutputInfo(oi); continue; }

        Monitor m;
        m.x    = ci->x;
        m.y    = ci->y;
        m.w    = (int)ci->width;
        m.h    = (int)ci->height;
        m.name = oi->name ? oi->name : ("mon" + std::to_string(mons.size()));
        mons.push_back(m);

        XRRFreeCrtcInfo(ci);
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return mons;
}

// ============================================================
// XShm framebuffer
// ============================================================

struct ShmImage {
    XImage         *img   = nullptr;
    XShmSegmentInfo info  = {};
    bool            valid = false;
};

static bool shmAlloc(Display *dpy, ShmImage &s, int w, int h) {
    int screen = DefaultScreen(dpy);
    s.img = XShmCreateImage(dpy, DefaultVisual(dpy, screen),
                            DefaultDepth(dpy, screen),
                            ZPixmap, nullptr, &s.info, w, h);
    if (!s.img) return false;

    s.info.shmid = shmget(IPC_PRIVATE,
                          (size_t)s.img->bytes_per_line * s.img->height,
                          IPC_CREAT | 0600);
    if (s.info.shmid < 0) { perror(NAME ": shmget"); return false; }

    s.info.shmaddr = s.img->data = (char *)shmat(s.info.shmid, nullptr, 0);
    s.info.readOnly = False;

    if (!XShmAttach(dpy, &s.info)) {
        fprintf(stderr, NAME ": XShmAttach failed\n");
        return false;
    }
    XSync(dpy, False);
    s.valid = true;
    return true;
}

static void shmFree(Display *dpy, ShmImage &s) {
    if (!s.valid) return;
    XShmDetach(dpy, &s.info);
    shmdt(s.info.shmaddr);
    shmctl(s.info.shmid, IPC_RMID, nullptr);
    XDestroyImage(s.img);
    s.valid = false;
}

// ============================================================
// Screenshot + PPM writer
// ============================================================

static void grabMonitor(Display *dpy, const Monitor &mon, const ShmImage &s) {
    XShmGetImage(dpy, DefaultRootWindow(dpy), s.img, mon.x, mon.y, AllPlanes);
    XFlush(dpy);
}

static bool savePpm(Display *dpy, XImage *img, const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, NAME ": fopen(%s): %s\n", path, strerror(errno)); return false; }

    fprintf(fp, "P6\n%d %d\n255\n", img->width, img->height);

    Visual       *vis   = DefaultVisual(dpy, DefaultScreen(dpy));
    unsigned long rmask = vis->red_mask;
    unsigned long gmask = vis->green_mask;
    unsigned long bmask = vis->blue_mask;
    int rs = 0, gs = 0, bs = 0;
    while (!((rmask >> rs) & 1)) ++rs;
    while (!((gmask >> gs) & 1)) ++gs;
    while (!((bmask >> bs) & 1)) ++bs;

    int w = img->width, h = img->height;
    unsigned char *row = (unsigned char *)malloc((size_t)w * 3);
    if (!row) { fclose(fp); return false; }

    bool ok = true;
    for (int y = 0; y < h && ok; ++y) {
        unsigned char *p = row;
        for (int x = 0; x < w; ++x) {
            unsigned long px = XGetPixel(img, x, y);
            *p++ = (unsigned char)((px & rmask) >> rs);
            *p++ = (unsigned char)((px & gmask) >> gs);
            *p++ = (unsigned char)((px & bmask) >> bs);
        }
        if (fwrite(row, 1, (size_t)w * 3, fp) != (size_t)w * 3) ok = false;
    }

    free(row);
    fclose(fp);
    return ok;
}

// Crop a window's rectangle out of an already-captured desktop screenshot
// and write it to its own file. This replaces per-window XGetImage() calls:
// no extra round-trip to the X server, just a read+crop+write on an image
// already sitting on disk. Coordinates are clamped to the source image
// bounds in case a window is partially off-screen.
//
// Both files are always our own P6 PPMs (see savePpm), so — same idea as
// compositeDesktopScreenshot below — we parse and blit them directly
// instead of going through ImageMagick's decode/encode pipeline. We also
// fseek() past the rows above the crop region instead of reading and
// discarding them, since a crop only ever needs a horizontal band of the
// source.
static bool cropWindowScreenshot(const std::string &desktop_ppm,
                                   int x, int y, int w, int h,
                                   const std::string &out_path) {
    if (w <= 0 || h <= 0) return false;

    FILE *f = fopen(desktop_ppm.c_str(), "rb");
    if (!f) return false;

    char magic[3] = {};
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fclose(f);
        return false;
    }

    auto skip_comments = [&]() {
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (isspace(c)) continue;
            if (c == '#') {
                while ((c = fgetc(f)) != EOF && c != '\n') ;
                continue;
            }
            ungetc(c, f);
            break;
        }
    };

    int iw = 0, ih = 0, maxval = 0;
    skip_comments();
    if (fscanf(f, "%d", &iw) != 1) { fclose(f); return false; }
    skip_comments();
    if (fscanf(f, "%d", &ih) != 1) { fclose(f); return false; }
    skip_comments();
    if (fscanf(f, "%d", &maxval) != 1) { fclose(f); return false; }
    if (iw <= 0 || ih <= 0 || maxval != 255) { fclose(f); return false; }
    fgetc(f);  // consume the single whitespace byte after maxval

    // Clamp the crop rect to the source image bounds.
    long cx = x, cy = y, cw = w, ch = h;
    if (cx < 0) { cw += cx; cx = 0; }
    if (cy < 0) { ch += cy; cy = 0; }
    if (cx + cw > iw) cw = iw - cx;
    if (cy + ch > ih) ch = ih - cy;
    if (cw <= 0 || ch <= 0) { fclose(f); return false; }

    const long header_end = ftell(f);
    const size_t src_row_bytes = (size_t)iw * 3;
    const size_t dst_row_bytes = (size_t)cw * 3;

    if (header_end < 0 ||
        fseek(f, header_end + cy * (long)src_row_bytes, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    std::vector<unsigned char> out_buf((size_t)ch * dst_row_bytes);
    std::vector<unsigned char> row(src_row_bytes);

    bool ok = true;
    for (long ry = 0; ry < ch; ++ry) {
        if (fread(row.data(), 1, src_row_bytes, f) != src_row_bytes) {
            ok = false;
            break;
        }
        memcpy(out_buf.data() + (size_t)ry * dst_row_bytes,
               row.data() + (size_t)cx * 3, dst_row_bytes);
    }
    fclose(f);
    if (!ok) return false;

    FILE *out = fopen(out_path.c_str(), "wb");
    if (!out) return false;
    fprintf(out, "P6\n%ld %ld\n255\n", cw, ch);
    const bool write_ok =
        fwrite(out_buf.data(), 1, out_buf.size(), out) == out_buf.size();
    fclose(out);
    return write_ok;
}

// Recreate a desktop's screenshot from its windows' persisted crops. Desktop
// *numbers* get renumbered by the WM whenever a desktop is inserted or
// removed, so nothing here is keyed by desktop index — the caller passes in
// the window ids currently on that desktop (queried live), and each
// window's own image/geometry is looked up by its immutable id. The result
// is always consistent with the current desktop layout, never stale.
//
// Both this function and cropWindowScreenshot() above work on our own P6
// PPMs directly (parse header, blit rows, write header+bytes) rather than
// going through ImageMagick — avoids the library's decode/encode overhead
// and per-call setup cost for a format we fully control on both ends.
static bool compositeDesktopScreenshot(const std::vector<Window> &ids,
                                         const std::string &out_path) {
    struct Placed {
        Window id;
        WinMeta m;
        std::string path;
    };

    std::vector<Placed> placed;
    placed.reserve(ids.size());

    int canvas_w = 0;
    int canvas_h = 0;

    // First collect valid windows.
    for (Window id : ids) {
        WinMeta m;
        if (!readWindowMeta(id, m))
            continue;

        const std::string path = windowScreenshotPath(id);

        struct stat st;
        if (stat(path.c_str(), &st) != 0 || st.st_size <= 0)
            continue;

        placed.push_back({id, m, path});

        canvas_w = std::max(canvas_w, m.x + m.w);
        canvas_h = std::max(canvas_h, m.y + m.h);
    }

    if (placed.empty() || canvas_w <= 0 || canvas_h <= 0)
        return false;

    // RGB canvas; background color
    constexpr unsigned char BG_R = 0;
    constexpr unsigned char BG_G = 0;
    constexpr unsigned char BG_B = 0;

    const size_t canvas_stride = static_cast<size_t>(canvas_w) * 3;
    const size_t canvas_size =
        static_cast<size_t>(canvas_h) * canvas_stride;

    std::vector<unsigned char> canvas(canvas_size);

    // Fill background.
    for (int y = 0; y < canvas_h; ++y) {
        unsigned char* row = canvas.data() +
                             static_cast<size_t>(y) * canvas_stride;

        for (int x = 0; x < canvas_w; ++x) {
            row[x * 3 + 0] = BG_R;
            row[x * 3 + 1] = BG_G;
            row[x * 3 + 2] = BG_B;
        }
    }

    bool placed_any = false;

    for (const auto& p : placed) {
        FILE* f = fopen(p.path.c_str(), "rb");
        if (!f)
            continue;

        /*
         * PPM header:
         *
         * P6
         * width height
         * 255
         */
        char magic[3] = {};
        if (fscanf(f, "%2s", magic) != 1 ||
            strcmp(magic, "P6") != 0) {
            fclose(f);
            continue;
        }

        auto skip_comments = [&]() {
            int c;

            while ((c = fgetc(f)) != EOF) {
                if (isspace(c))
                    continue;

                if (c == '#') {
                    while ((c = fgetc(f)) != EOF && c != '\n')
                        ;
                    continue;
                }

                ungetc(c, f);
                break;
            }
        };

        int w = 0;
        int h = 0;
        int maxval = 0;

        skip_comments();
        if (fscanf(f, "%d", &w) != 1) {
            fclose(f);
            continue;
        }

        skip_comments();
        if (fscanf(f, "%d", &h) != 1) {
            fclose(f);
            continue;
        }

        skip_comments();
        if (fscanf(f, "%d", &maxval) != 1) {
            fclose(f);
            continue;
        }

        if (w <= 0 || h <= 0 || maxval != 255) {
            fclose(f);
            continue;
        }

        // Consume the whitespace after maxval.
        fgetc(f);

        const size_t row_bytes = static_cast<size_t>(w) * 3;

        std::vector<unsigned char> row(row_bytes);

        for (int y = 0; y < h; ++y) {
            if (fread(row.data(), 1, row_bytes, f) != row_bytes)
                break;

            const int dst_y = p.m.y + y;

            if (dst_y < 0 || dst_y >= canvas_h)
                continue;

            const int src_x = std::max(0, -p.m.x);
            const int dst_x = std::max(0, p.m.x);

            const int copy_width =
                std::min(w - src_x, canvas_w - dst_x);

            if (copy_width <= 0)
                continue;

            unsigned char* dst =
                canvas.data() +
                static_cast<size_t>(dst_y) * canvas_stride +
                static_cast<size_t>(dst_x) * 3;

            const unsigned char* src =
                row.data() +
                static_cast<size_t>(src_x) * 3;

            memcpy(dst, src, static_cast<size_t>(copy_width) * 3);
        }

        fclose(f);
        placed_any = true;
    }

    if (!placed_any)
        return false;

    /*
     * Write the final image.
     *
     * If out_path is .ppm, this is extremely fast.
     */
    FILE* out = fopen(out_path.c_str(), "wb");
    if (!out)
        return false;

    fprintf(out, "P6\n%d %d\n255\n", canvas_w, canvas_h);

    const bool write_ok =
        fwrite(canvas.data(), 1, canvas.size(), out) == canvas.size();

    fclose(out);

    return write_ok;
}


// ============================================================
// Async window-crop queue
// ============================================================
//
// Screenshot capture must stay fast, since it happens on every desktop
// switch. Cropping the individual windows out of it is comparatively slow
// (one decode+crop+encode per window) and nothing waiting on the client
// side actually needs it to finish before the switch proceeds. So capture
// only writes the full per-monitor screenshot and hands off, and a
// background thread does the cropping afterwards.
//
// A monitor's temporary screenshot file is shared by every window that was
// on that desktop when it was captured. It's wrapped in a ScreenshotRef and
// handed out via shared_ptr; once the last CropJob referencing it has been
// popped and destroyed, the shared_ptr refcount hits zero and the
// ScreenshotRef destructor deletes the temporary file — i.e. the file goes
// away automatically exactly when "all windows from a given desktop" have
// been cropped, with no separate bookkeeping required.

struct ScreenshotRef {
    std::string path;
    explicit ScreenshotRef(std::string p) : path(std::move(p)) {}
    ~ScreenshotRef() { unlink(path.c_str()); }
};

struct CropJob {
    std::shared_ptr<ScreenshotRef> screenshot;
    Window win;
    int x, y, w, h;
};

static std::mutex              g_cropMutex;
static std::condition_variable g_cropCv;
static std::queue<CropJob>     g_cropQueue;
static std::atomic<bool>       g_cropStop{false};

static void enqueueCropJobs(std::vector<CropJob> jobs) {
    if (jobs.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_cropMutex);
        for (auto &j : jobs) g_cropQueue.push(std::move(j));
    }
    g_cropCv.notify_all();
}

static void cropWorker() {
    for (;;) {
        CropJob job;
        {
            std::unique_lock<std::mutex> lock(g_cropMutex);
            g_cropCv.wait(lock, [] {
                return !g_cropQueue.empty() || g_cropStop.load();
            });
            if (g_cropQueue.empty()) {
                if (g_cropStop.load()) return;
                continue;
            }
            job = std::move(g_cropQueue.front());
            g_cropQueue.pop();
        }

        if (cropWindowScreenshot(job.screenshot->path, job.x, job.y,
                                   job.w, job.h,
                                   windowScreenshotPath(job.win))) {
            writeWindowMeta(job.win, job.x, job.y, job.w, job.h);
        }
        // job (and its shared_ptr<ScreenshotRef>) is destroyed here; once
        // every job sharing this screenshot has gone through this path,
        // ScreenshotRef's destructor removes the temp file.
    }
}



// Send the ack reply back through the already-connected client socket.
static void daemonSendAck(int client_fd, bool ok) {
    const char *msg = ok ? "ok\n" : "err\n";
    // Best-effort write; if the client disconnected early, ignore the error.
    (void)write(client_fd, msg, strlen(msg));
}

// Free all SHM buffers and rebuild monitors + SHM from the current RandR state.
// Returns false if no monitors are found after the refresh.
static bool rebuildMonitors(Display *dpy,
                             std::vector<Monitor>  &monitors,
                             std::vector<ShmImage> &shms) {
    // Release old SHM buffers.
    for (auto &s : shms) shmFree(dpy, s);
    shms.clear();
    monitors.clear();

    monitors = getMonitors(dpy);
    if (monitors.empty()) {
        fprintf(stderr, NAME ": no connected monitors after hotplug\n");
        return false;
    }

    printf(NAME " daemon: %zu monitor(s):\n", monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i)
        printf("  [%zu] %-12s  %dx%d+%d+%d\n",
               i, monitors[i].name.c_str(),
               monitors[i].w, monitors[i].h,
               monitors[i].x, monitors[i].y);
    fflush(stdout);

    shms.resize(monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (!shmAlloc(dpy, shms[i], monitors[i].w, monitors[i].h)) {
            fprintf(stderr, NAME ": shmAlloc failed for monitor %zu\n", i);
            // Free anything we already allocated before returning.
            for (size_t j = 0; j < i; ++j) shmFree(dpy, shms[j]);
            shms.clear();
            monitors.clear();
            return false;
        }
    }
    return true;
}

static bool captureMonitorDesktops(Display *dpy,
                                     const std::vector<Monitor> &monitors,
                                     const std::vector<ShmImage> &shms,
                                     const Atoms& atoms) {
    if (monitors.empty()) {
        fprintf(stderr, NAME ": screenshot skipped — no monitors available\n");
        return false;
    }

    Window root = DefaultRootWindow(dpy);

    Atom atype;
    int afmt;
    unsigned long nitems;
    unsigned long after;
    unsigned char *data = nullptr;

    auto read_cardinal = [&](Atom atom, long fallback) -> long {
        if (XGetWindowProperty(dpy, root, atom, 0, 1, False,
                               XA_CARDINAL, &atype, &afmt,
                               &nitems, &after, &data) == Success
            && data) {
            long value = (long)*(unsigned long *)data;
            XFree(data);
            data = nullptr;
            return value;
        }

        return fallback;
    };

    // Total number of desktops.
    long num_desktops = read_cardinal(atoms.netNumberOfDesktops, 1);

    // For each monitor, find which desktop is currently visible on
    // it by matching the desktop's viewport origin to the monitor
    // geometry.  A desktop is "on" a monitor when its viewport
    // origin falls inside (or equals the top-left of) that monitor.
    //
    // Fallback: if no desktop maps to a monitor (e.g. the WM uses a
    // single shared viewport), use _NET_CURRENT_DESKTOP for every
    // monitor so we at least capture something useful.
    long current_desk = read_cardinal(atoms.netCurrentDesktop, -1);
    long current_mon = -1;
    if (current_desk < 0) {
        fprintf(stderr, NAME ": screenshot skipped - current desktop not found\n");
        return false;
    }

    // _NET_DESKTOP_VIEWPORT: pairs of (x,y) per desktop that tell
    // which viewport origin each desktop is mapped to.  On WMs that
    // assign one desktop per monitor (e.g. bspwm, Openbox with
    // per-monitor workspaces) the viewport origin equals the
    // monitor's top-left corner.
    std::vector<std::pair<long, long>> viewport(num_desktops, {-1, -1});
    if (XGetWindowProperty(dpy, root, atoms.netDesktopViewport, 0,
                           num_desktops * 2, False, XA_CARDINAL,
                           &atype, &afmt, &nitems, &after, &data) == Success
        && data) {
        auto *vals = (unsigned long *)data;
        for (long d = 0;
             d < num_desktops &&
                 (unsigned long)(d * 2 + 1) < nitems;
             ++d) {
            viewport[d] = {
                (long)vals[d * 2],
                (long)vals[d * 2 + 1]
            };
        }
        XFree(data);
        data = nullptr;
    }

    auto monitor_for_point = [&](long x, long y) -> long {
        for (size_t mi = 0; mi < monitors.size(); ++mi) {
            const Monitor &m = monitors[mi];
            if (x >= m.x && x < m.x + m.w &&
                y >= m.y && y < m.y + m.h) {
                return (long)mi;
            }
        }
        return -1;
    };

    // Build monitor→desktop mapping.
    std::vector<long> mon_desk(monitors.size(), -1);
    for (long d = 0; d < num_desktops; ++d) {
        const auto &[vx, vy] = viewport[d];
        if (vx < 0 || vy < 0)
            continue;
        long mi = monitor_for_point(vx, vy);
        if (mi < 0)
            continue;
        mon_desk[mi] = d;
        if (d == current_desk)
            current_mon = mi;
    }

    // Gather every window's absolute geometry once; cheap (attribute/property
    // queries only, no pixel data) compared to rasterizing window contents.
    const std::vector<WinGeom> all_wins = getAllWindowGeometries(dpy, atoms);

    // Capture every monitor and save its PPM — this is the only part that
    // has to be fast, since it happens on every desktop switch. Cropping
    // individual windows out of it is handed off to the background queue;
    // this function returns as soon as the raw screenshots are on disk.
    int saved_count = 0;
    for (size_t mi = 0; mi < monitors.size(); ++mi) {
        long desk = ((long)mi == current_mon) ?
            current_desk : mon_desk[mi];
        grabMonitor(dpy, monitors[mi], shms[mi]);
        std::string path = rawCapturePath(mi);
        if (!savePpm(dpy, shms[mi].img, path.c_str()))
            continue;
        ++saved_count;

        // Reference-counted handle to the temp screenshot: it's deleted
        // automatically once every job below has consumed it (see
        // ScreenshotRef). If this desktop has no windows, the shared_ptr
        // simply goes out of scope at the end of this iteration and the
        // file is removed immediately.
        auto screenshot = std::make_shared<ScreenshotRef>(path);

        std::vector<CropJob> jobs;
        for (const auto& g : all_wins) {
            if (g.desktop != desk) continue;
            jobs.push_back({ screenshot, g.id,
                             g.x - monitors[mi].x, g.y - monitors[mi].y,
                             g.w, g.h });
        }
        enqueueCropJobs(std::move(jobs));
    }

    return saved_count > 0;
}

static int runDaemon() {
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, NAME ": cannot open display\n");
        return 1;
    }

    if (!XShmQueryExtension(dpy)) {
        fprintf(stderr, NAME ": XShm extension required\n"); return 1;
    }

    // Subscribe to RandR events so we learn about hotplug changes.
    int rr_event_base, rr_error_base;
    bool have_randr = XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
    if (have_randr) {
        Window root = DefaultRootWindow(dpy);
        XRRSelectInput(dpy, root, RROutputChangeNotifyMask | RRScreenChangeNotifyMask);
    }

    clearDir(runDir.c_str());
    ensureDir(runDir.c_str());

    // Remove any stale socket from a previous run.
    unlink(sockPath.c_str());

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror(NAME ": socket"); return 1; }

    // Ignore SIGPIPE so that a write() to a client that has already
    // disconnected returns EPIPE instead of killing the daemon.
    signal(SIGPIPE, SIG_IGN);

    // CLOEXEC so child processes (e.g. spawned commands) don't inherit it.
    fcntl(srv, F_SETFD, FD_CLOEXEC);

    // Non-blocking so we can interleave accept() with X11 event processing.
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL) | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror(NAME ": bind"); close(srv); return 1;
    }
    chmod(sockPath.c_str(), 0600);

    if (listen(srv, /*backlog=*/8) != 0) {
        perror(NAME ": listen"); close(srv); return 1;
    }

    std::vector<Monitor> monitors;
    std::vector<ShmImage> shms;

    if (!rebuildMonitors(dpy, monitors, shms)) {
        fprintf(stderr, NAME ": no connected monitors\n");
        close(srv); return 1;
    }

    // Cache all needed atoms once — XInternAtom round-trips are cheap
    // but there is no reason to repeat them on every screenshot request.
    const Atoms atoms = initAtoms(dpy);

    // Background thread that crops individual windows out of the desktop
    // screenshots asynchronously (see enqueueCropJobs/cropWorker). It
    // only touches plain files (raw PPM read/write), never X, so it's safe
    // to run alongside the X11 event loop below without any Xlib locking.
    std::thread worker(cropWorker);

    printf(NAME " daemon: ready — listening on %s\n", sockPath.c_str());
    fflush(stdout);

    int x11_fd = ConnectionNumber(dpy);

    for (;;) {
        // Drain all pending X11 events (RandR hotplug notifications).
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (!have_randr) continue;

            // RRScreenChangeNotify: screen geometry changed (resolution/rotation).
            if (ev.type == rr_event_base + RRScreenChangeNotify) {
                // Update RandR's internal state before re-querying.
                XRRUpdateConfiguration(&ev);
                printf(NAME ": RandR screen change — rebuilding monitors\n");
                fflush(stdout);
                rebuildMonitors(dpy, monitors, shms);
                continue;
            }

            // RRNotify sub-events (output connect/disconnect).
            if (ev.type == rr_event_base + RRNotify) {
                XRRNotifyEvent *rrev = reinterpret_cast<XRRNotifyEvent *>(&ev);
                if (rrev->subtype == RRNotify_OutputChange) {
                    printf(NAME ": RandR output change — rebuilding monitors\n");
                    fflush(stdout);
                    rebuildMonitors(dpy, monitors, shms);
                }
                continue;
            }
        }

        // Wait for activity on either the Unix socket or the X11 connection.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv,    &rfds);
        FD_SET(x11_fd, &rfds);
        int nfds = std::max(srv, x11_fd) + 1;

        // No timeout — we wake up on socket activity OR X11 events.
        int sel = select(nfds, &rfds, nullptr, nullptr, nullptr);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror(NAME ": select");
            continue;
        }

        // Handle incoming screenshot requests.
        if (FD_ISSET(srv, &rfds)) {
            int client_fd = accept(srv, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    perror(NAME ": accept");
                // else: spurious wake-up, loop back
            } else {
                fcntl(client_fd, F_SETFD, FD_CLOEXEC);

                // Drain the single-byte request token (content is ignored).
                char token;
                if ((read(client_fd, &token, 1)) < 1) {
                    close(client_fd);
                    continue;
                }

                bool saved = false;

                if (token == 's') {
                    saved = captureMonitorDesktops(dpy, monitors, shms, atoms);
                }

                // Reply goes back through the same connection — no race possible.
                daemonSendAck(client_fd, saved);
                close(client_fd);
            }
        }
        // X11 activity is handled at the top of the loop via XPending().
    }

    g_cropStop.store(true);
    g_cropCv.notify_all();
    worker.join();

    for (auto &s : shms) shmFree(dpy, s);
    close(srv);
    unlink(sockPath.c_str());
    XCloseDisplay(dpy);
    return 0;
}

// ============================================================
// Client helpers
// ============================================================

// Check whether the daemon is up by attempting a real connection to its
// Unix socket.  Using connect() rather than stat() catches stale socket
// files left by a crashed daemon.
static bool daemonRunning() {
    // Probe by actually connecting — avoids stale socket files.
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    bool ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
}

// Require the daemon; print a helpful message and return false if absent.
static bool requireDaemon() {
    if (daemonRunning()) return true;
    fprintf(stderr, NAME ": daemon is not running\n");
    return false;
}

// Send a screenshot request to the daemon over the Unix socket and wait for
// the "ok" / "err" reply.  Each call is a fully independent connection, so
// there is no open-order race and concurrent callers never steal each other's
// acknowledgement.
static bool clientScreenshot() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror(NAME ": socket"); return false; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror(NAME ": connect");
        close(fd);
        return false;
    }

    // Send the single-byte request token.
    if (write(fd, "s", 1) != 1) {
        perror(NAME ": write");
        close(fd);
        return false;
    }

    // Block until the daemon replies "ok\n" or "err\n".
    char buf[4] = {};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_WAITALL);
    close(fd);
    return (n >= 2 && strncmp(buf, "ok", 2) == 0);
}

// public subcommand wrapping clientScreenshot()
static int runScreenshot() {
    if (!requireDaemon()) return 1;
    bool ok = clientScreenshot();
    if (!ok) { fprintf(stderr, NAME ": screenshot failed\n"); return 1; }
    return 0;
}

// ============================================================
// Client: workspace picker via gmenu
// ============================================================

enum PickerMode { PICK_SWITCH, PICK_MOVE, PICK_WINDOW };

static int runPicker(PickerMode mode) {
    if (!requireDaemon()) return 1;

    Display* dpy = XOpenDisplay(nullptr);

    if (!clientScreenshot())
        fprintf(stderr, NAME
                ": screenshot failed — menu may show stale thumbnails\n");

    bool move_mode = (mode == PICK_MOVE);

    auto cmd_change     = envCmd(NAME_UPPER "_CHANGE_CMD");
    auto cmd_move       = envCmd(NAME_UPPER "_MOVE_CMD");
    auto cmd_change_new = envCmd(NAME_UPPER "_CHANGE_NEW_CMD");
    auto cmd_move_new   = envCmd(NAME_UPPER "_MOVE_NEW_CMD");

    const Atoms atoms = initAtoms(dpy);

    int focused_idx = getCurrentDesktopIndex(dpy, atoms);
    auto desks = getDesktopsWindowTitles(dpy, atoms);

    // Only populated for PICK_WINDOW — pairs of (window id, title).
    auto winlist = (mode == PICK_WINDOW) ? getWindowList(dpy, atoms)
                                          : std::vector<std::pair<Window, std::string>>{};

    // Build the gmenu item list in memory (same format as before)
    std::ostringstream oss;
    if (mode == PICK_WINDOW) {
        // The daemon crops these asynchronously in the background; we just
        // use whatever's on disk already (falling back to the generic
        // "window" icon if a crop hasn't landed yet, e.g. right after the
        // very first screenshot before the worker catches up).
        for (const auto &[win, title] : winlist) {
            const std::string ppm = windowScreenshotPath(win);
            struct stat st;
            const std::string icon =
                (stat(ppm.c_str(), &st) == 0) ? ppm : "window";

            // Encode the window id as a prefix ("id:title"); the generic
            // colon-split below hands us the id back untouched, so
            // selection never depends on matching title text.
            oss << ">>j {\"name\":\"" << (unsigned long)win << ": " << title
                << "\",\"icon\":\"" << icon << "\"}\n";
        }
    } else {
        // Desktop numbers can be renumbered by the WM (e.g. inserting a
        // desktop in the middle shifts every later one), so we never trust
        // any previously-captured desktop screenshot here. Instead, query
        // which windows are on which desktop right now and rebuild each
        // desktop's screenshot from the windows' own (id-keyed, therefore
        // renumbering-proof) persisted crops.
        auto desk_windows = getDesktopWindowIds(dpy, atoms);
        for (const auto &[idx, line] : desks) {
            if (line.empty()) continue;

            std::string icon = "desktop";
            const auto wit = desk_windows.find(idx);
            if (wit != desk_windows.end() && !wit->second.empty()) {
                const std::string out = desktopScreenshotPath(idx);
                if (compositeDesktopScreenshot(wit->second, out))
                    icon = out;
            }

            oss << ">>j {\"name\":\"" << line
                << "\",\"icon\":\"" << icon << "\"}\n";
        }
        if (!cmd_change_new.empty() || !cmd_move_new.empty()) {
            oss << ">>j {\"name\":\"New\","
                "\"icon\":\"window-new-symbolic\",\"icon-size\":64}\n";
        }
    }
    std::string items = oss.str();

    // Two pipes: parent->child (stdin of gmenu) and child->parent (stdout of gmenu)
    int in_pipe[2];   // [0] child reads,  [1] parent writes
    int out_pipe[2];  // [0] parent reads, [1] child writes
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        perror(NAME ": pipe");
        XCloseDisplay(dpy);
        return 1;
    }

    const char* prompt = move_mode ? "Move window to workspace…" :
                         mode == PICK_WINDOW ? "Switch window…" : "Workspaces";

    // Build argv for gmenu — no shell, no quoting worries
    char n_str[32];
    snprintf(n_str, sizeof(n_str), "%d", focused_idx);
    const char* argv[] = {
        GMENU,
        "-i",  "384",
        "--maxlbl", "35",
        "--full",
        "-n",  n_str,
        "-p",  prompt,
        nullptr
    };

    pid_t pid = fork();
    if (pid < 0) {
        perror(NAME ": fork");
        XCloseDisplay(dpy);
        return 1;
    }

    if (pid == 0) {
        // ---- child ----
        // Rewire stdin/stdout to our pipes
        dup2(in_pipe[0],  STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        // Close all four pipe ends (child doesn't need the parent-side ends)
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        execvp(GMENU, const_cast<char* const*>(argv));
        perror(NAME ": execvp gmenu");
        _exit(127);
    }

    // ---- parent ----
    // Close ends we don't use
    close(in_pipe[0]);
    close(out_pipe[1]);

    // Feed all items into gmenu's stdin, then signal EOF
    const char* ptr = items.c_str();
    size_t      rem = items.size();
    while (rem > 0) {
        ssize_t w = write(in_pipe[1], ptr, rem);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror(NAME ": write to gmenu");
            break;
        }
        ptr += w;
        rem -= w;
    }
    close(in_pipe[1]);   // EOF -> gmenu stops reading and opens its window

    // Read gmenu's selection
    std::string choice = readFd(out_pipe[0]);
    close(out_pipe[0]);

    // Reap the child
    waitpid(pid, nullptr, 0);

    // Trim trailing newline
    chomp(choice);

    if (choice.empty()) return 0;

    std::size_t pos = choice.find(':');
    std::string choice_desk =
        (pos == std::string::npos) ? choice : choice.substr(0, pos);

    if (mode == PICK_WINDOW) {
        errno = 0;
        char *end = nullptr;
        Window target = (Window)strtoul(choice_desk.c_str(), &end, 10);
        if (target == None || errno != 0 || end == choice_desk.c_str()) {
            fprintf(stderr, NAME ": failed finding window id\n");
            return 1;
        }
        switchWindow(dpy, target, atoms);
        XCloseDisplay(dpy);
        return 0;
    }

    bool is_new = (choice == "New" || choice.rfind("New", 0) == 0);

    if (!cmd_change.empty() || !cmd_move.empty() ||
        !cmd_change_new.empty() || !cmd_move_new.empty()) {
        if (move_mode) {
            if (is_new) {
                if (!spawnCmd(cmd_move_new)) {
                    fprintf(stderr, NAME ": "
                            NAME "_MOVE_NEW_CMD not set or failed\n");
                    return 1;
                }
            } else {
                if (!spawnCmd(cmd_move, choice_desk.c_str())) {
                    fprintf(stderr, NAME ": " NAME
                            "_MOVE_CMD not set or failed\n");
                    return 1;
                }
            }

        } else {
            if (is_new) {
                if (!spawnCmd(cmd_change_new)) {
                    fprintf(stderr, NAME ": " NAME
                            "_CHANGE_NEW_CMD not set or failed\n");
                    return 1;
                }
            } else {
                if (!spawnCmd(cmd_change, choice_desk.c_str())) {
                    fprintf(stderr, NAME ": " NAME
                            "_CHANGE_CMD not set or failed\n");
                    return 1;
                }
            }
        }

    } else {
        long choice_idx = -1;
        for (const auto &[idx, line] : desks) {
            if (choice_desk == line) {
                choice_idx = idx;
                break;
            }
        }
        if (choice_idx < 0) {
            fprintf(stderr, NAME ": failed finding workspace id\n");
            return 1;
        }

        if (move_mode) {
            moveFocusedWindowAndSwitch(dpy, choice_idx, atoms);
        } else {
            switchDesktop(dpy, choice_idx, atoms);
        }
    }

    XCloseDisplay(dpy);

    return 0;
}

// ============================================================
// Entry point
// ============================================================

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s daemon          run the screenshot daemon\n"
            "  %s screenshot      capture the current desktop now\n"
            "  %s switch          open workspace picker and switch to selection\n"
            "  %s move            open workspace picker and move focused window\n"
            "  %s switch-windows  open window picker and switch to selection\n"
            "  %s ls              list current desktops and windows\n",
            argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    XSetErrorHandler(xErrorHandler);
    initPaths();

    if (!strcmp(cmd, "daemon")) {
        return runDaemon();
    } else if (!strcmp(cmd, "screenshot")) {
        return runScreenshot();
    } else if (!strcmp(cmd, "switch")) {
        return runPicker(PICK_SWITCH);
    } else if (!strcmp(cmd, "move")) {
        return runPicker(PICK_MOVE);
    } else if (!strcmp(cmd, "switch-windows")) {
        return runPicker(PICK_WINDOW);
    } else if (!strcmp(cmd, "ls")) {
        Display* dpy = XOpenDisplay(nullptr);
        const Atoms atoms = initAtoms(dpy);
        auto desks = getDesktopsWindowTitles(dpy, atoms);
        for (const auto &[_, line] : desks) {
            printf("%s\n", line.c_str());
        }
        XCloseDisplay(dpy);
        return 0;
    } else {
        fprintf(stderr, NAME ": unknown command '%s'\n", cmd);
        usage(argv[0]);
        return 1;
    }
}
