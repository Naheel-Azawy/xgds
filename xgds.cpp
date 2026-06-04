// xgds - X gmenu desktop switcher
// Dependencies: X11, XShm, XRandR, gmenu

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <unistd.h>
#include <spawn.h>

#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>

#define NAME       "xgds"
#define NAME_UPPER "XGDS"

static const char *RUNDIR  = "/tmp/" NAME;
static const char *REQFIFO = "/tmp/" NAME "/req";
static const char *ACKFIFO = "/tmp/" NAME "/ack";
static const char *GMENU   = "gmenu";

// ============================================================
// Helpers
// ============================================================

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, NAME ": mkdir(%s): %s\n", path, strerror(errno));
}

static void ensure_fifo(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISFIFO(st.st_mode)) return;
        remove(path);
    }
    if (mkfifo(path, 0600) != 0)
        fprintf(stderr, NAME ": mkfifo(%s): %s\n", path, strerror(errno));
}

static void clear_dir(const char *path) {
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
static std::string read_fd(int fd) {
    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, n);
    return out;
}

// Read an env var like "cmd arg1 arg2" and split on spaces into a vector.
// Returns empty vector if the var is unset or empty.
static std::vector<std::string> env_cmd(const char* var) {
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
static std::vector<char*> make_argv(std::vector<std::string>& parts,
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
static bool spawn_cmd(std::vector<std::string>& parts,
                      const char* extra = nullptr) {
    if (parts.empty()) return false;
    auto argv = make_argv(parts, extra);
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
// X11 helpers
// ============================================================

static std::map<long, std::string> getDesktopNames(Display* dpy) {
    std::map<long, std::string> out;
    if (!dpy) return out;

    Window root = DefaultRootWindow(dpy);

    Atom net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    Atom net_desktop_names      = XInternAtom(dpy, "_NET_DESKTOP_NAMES",      False);
    Atom utf8_string            = XInternAtom(dpy, "UTF8_STRING",             False);

    // Get number of desktops
    long num_desktops = 0;

    {
        Atom actualType;
        int actualFormat;
        unsigned long nItems;
        unsigned long bytesAfter;
        unsigned char* data = nullptr;

        Atom prop = net_number_of_desktops;

        if (XGetWindowProperty(dpy, root, prop,
                               0, 1,
                               False,
                               XA_CARDINAL,
                               &actualType,
                               &actualFormat,
                               &nItems,
                               &bytesAfter,
                               &data) == Success && data) {
            if (nItems > 0)
                num_desktops = *reinterpret_cast<unsigned long*>(data);
            XFree(data);
        }
    }

    if (num_desktops <= 0)
        return out;

    // Get desktop names (null-separated UTF-8 string)
    Atom actualType;
    int actualFormat;
    unsigned long nItems;
    unsigned long bytesAfter;
    unsigned char* data = nullptr;

    std::string names_blob;

    if (XGetWindowProperty(dpy, root, net_desktop_names,
                           0, (~0L),
                           False,
                           utf8_string,
                           &actualType,
                           &actualFormat,
                           &nItems,
                           &bytesAfter,
                           &data) == Success && data)
        {
            names_blob.assign(reinterpret_cast<char*>(data), nItems);
            XFree(data);
        }

    std::vector<std::string> names;
    std::string current;

    for (char c : names_blob) {
        if (c == '\0') {
            names.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }

    if (!current.empty())
        names.push_back(current);

    // Map indices
    for (long i = 0; i < num_desktops; ++i) {
        if (i < (long)names.size() && !names[i].empty())
            out[i] = names[i];
        else
            out[i] = std::to_string(i);
    }

    return out;
}

static int getCurrentDesktopIndex(Display* dpy) {
    if (!dpy) return -1;

    Window root = DefaultRootWindow(dpy);

    Atom prop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    Atom type;
    int format;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, root, prop,
                           0, 1, False, XA_CARDINAL,
                           &type, &format, &nitems,
                           &bytesAfter, &data) != Success || !data)
        return -1;

    int index = *reinterpret_cast<int*>(data);
    XFree(data);
    return index;
}

static std::string getWindowTitle(Display* dpy, Window w, Atom netWmName, Atom utf8) {
    Atom actualType;
    int actualFormat;
    unsigned long nItems, bytesAfter;
    unsigned char* prop = nullptr;

    // _NET_WM_NAME (UTF-8)
    if (XGetWindowProperty(dpy, w, netWmName,
                           0, (~0L), False, utf8,
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

/* TODO: something is wrong here

21:05 3> sh foo.sh                      <----------- (CORRECT OLD SCRIPT)
1: btop - tmux 0:1878
2: Inbox - info@iecon2026.org - Mozilla Thunderbird
3: c++ print map - Google Search - Brave
4: chwsg.sh - emacs - tmux 80:2049132, zsh - tmux 126:2445806, wsp.cpp - emacs - tmux 87:2098353, foo.sh - emacs - tmux 123:2442046, chwsg.sh - emacs - tmux 124:2443368
5: item.vala - emacs - tmux 97:2208949, zsh - tmux 111:2358834
6: wm-msg - emacs - tmux 113:2384343, zsh - tmux 116:2392472
7: EWMH Desktop Names Mapping - Brave
8: 2025-10-19-tii - tmux 3:12335
21:05 3>
21:05 3>
21:05 3> ./foo                      <----------- (THIS)
1: btop - tmux 0:1878
2: Inbox - info@iecon2026.org - Mozilla Thunderbird
3: c++ print map - Google Search - Brave
4: chwsg.sh - emacs - tmux 80:2049132, zsh - tmux 126:2445806, wsp.cpp - emacs - tmux 87:2098353, foo.sh - emacs - tmux 123:2442046, chwsg.sh - emacs - tmux 124:2443368
5:
6: item.vala - emacs - tmux 97:2208949, zsh - tmux 111:2358834
7: wm-msg - emacs - tmux 113:2384343, zsh - tmux 116:2392472
8: EWMH Desktop Names Mapping - Brave
*/
std::vector<std::string> getDesktopWindowTitles(Display* dpy) {
    std::vector<std::string> result;
    if (!dpy) return result;

    Window root = DefaultRootWindow(dpy);

    Atom netNumberOfDesktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    Atom netClientList       = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom netWmDesktop        = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    Atom netWmName           = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8                = XInternAtom(dpy, "UTF8_STRING", False);

    Atom actualType;
    int actualFormat;
    unsigned long nItems, bytesAfter;

    // -------- desktop count (fixed authoritative size) --------
    long numDesktops = 0;
    {
        unsigned char* prop = nullptr;

        if (XGetWindowProperty(dpy, root, netNumberOfDesktops,
                               0, 1, False, XA_CARDINAL,
                               &actualType, &actualFormat,
                               &nItems, &bytesAfter,
                               &prop) == Success && prop) {
            numDesktops = *(unsigned long*)prop;
            XFree(prop);
        }
    }

    if (numDesktops <= 0) numDesktops = 1;

    std::vector<std::vector<std::string>> desktops(numDesktops);

    // -------- client list --------
    Window* clients = nullptr;
    unsigned long clientCount = 0;

    if (XGetWindowProperty(dpy, root, netClientList,
                           0, (~0L), False, XA_WINDOW,
                           &actualType, &actualFormat,
                           &clientCount, &bytesAfter,
                           (unsigned char**)&clients) != Success || !clients) {
        return result;
    }

    // -------- fill desktops --------
    for (unsigned long i = 0; i < clientCount; ++i) {
        Window w = clients[i];

        unsigned char* deskProp = nullptr;
        unsigned long desk = 0xFFFFFFFF;

        if (XGetWindowProperty(dpy, w, netWmDesktop,
                               0, 1, False, XA_CARDINAL,
                               &actualType, &actualFormat,
                               &nItems, &bytesAfter,
                               &deskProp) == Success && deskProp) {
            desk = *(unsigned long*)deskProp;
            XFree(deskProp);
        }

        if (desk == 0xFFFFFFFF) {
            continue; // sticky or unassigned
        }

        if (desk >= (unsigned long)numDesktops) {
            continue; // invalid/out-of-range
        }

        std::string title = getWindowTitle(dpy, w, netWmName, utf8);
        if (title.empty()) continue;

        desktops[desk].push_back(title);
    }

    if (clients) XFree(clients);

    // -------- format output --------
    for (long d = 0; d < numDesktops; ++d) {
        std::ostringstream oss;

        auto& v = desktops[d];
        for (size_t i = 0; i < v.size(); ++i) {
            oss << v[i];
            if (i + 1 < v.size()) oss << ", ";
        }

        result.push_back(oss.str());
    }

    return result;
}

static bool switchDesktop(Display *dpy, long desktop) {
    Window root = DefaultRootWindow(dpy);

    Atom current =
        XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = current;
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

static bool moveFocusedWindowToDesktop(Display *dpy, long desktop) {
    Window root = DefaultRootWindow(dpy);

    Atom activeAtom =
        XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);

    Atom actual;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char *data = nullptr;

    if (XGetWindowProperty(dpy,
                           root,
                           activeAtom,
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

    Atom wmDesktop =
        XInternAtom(dpy, "_NET_WM_DESKTOP", False);

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = wmDesktop;
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

static bool moveFocusedWindowAndSwitch(Display *dpy, long desktop) {
    if (!moveFocusedWindowToDesktop(dpy, desktop))
        return false;
    return switchDesktop(dpy, desktop);
}

// ============================================================
// X11 monitor enumeration
// ============================================================

struct Monitor {
    int x, y, w, h;
    std::string name;
};

static std::vector<Monitor> get_monitors(Display *dpy) {
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
// Focused-monitor detection
// ============================================================

static int focused_monitor(Display *dpy, const std::vector<Monitor> &mons) {
    if (mons.size() == 1) return 0;

    Window root = DefaultRootWindow(dpy);
    Atom net_active = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (net_active == None) return 0;

    Atom atype; int afmt; unsigned long nitems, after;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, root, net_active, 0, 1, False, XA_WINDOW,
                           &atype, &afmt, &nitems, &after, &prop) != Success || !prop)
        return 0;

    Window active = *(Window *)prop;
    XFree(prop);
    if (active == None || active == root) return 0;

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, active, &wa)) return 0;

    Window child;
    int wx = wa.x, wy = wa.y;
    XTranslateCoordinates(dpy, active, root, 0, 0, &wx, &wy, &child);

    int cx = wx + (int)wa.width  / 2;
    int cy = wy + (int)wa.height / 2;

    for (int i = 0; i < (int)mons.size(); ++i) {
        const Monitor &m = mons[i];
        if (cx >= m.x && cx < m.x + m.w && cy >= m.y && cy < m.y + m.h)
            return i;
    }

    // Fallback: largest overlap
    int  best = 0;
    long best_area = -1;
    for (int i = 0; i < (int)mons.size(); ++i) {
        const Monitor &m = mons[i];
        int ow = std::min(wx + (int)wa.width,  m.x + m.w) - std::max(wx, m.x);
        int oh = std::min(wy + (int)wa.height, m.y + m.h) - std::max(wy, m.y);
        long area = (ow > 0 && oh > 0) ? (long)ow * oh : 0;
        if (area > best_area) { best_area = area; best = i; }
    }
    return best;
}

// ============================================================
// XShm framebuffer
// ============================================================

struct ShmImage {
    XImage         *img   = nullptr;
    XShmSegmentInfo info  = {};
    bool            valid = false;
};

static bool shm_alloc(Display *dpy, ShmImage &s, int w, int h) {
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

static void shm_free(Display *dpy, ShmImage &s) {
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

static void grab_monitor(Display *dpy, const Monitor &mon, ShmImage &s) {
    XShmGetImage(dpy, DefaultRootWindow(dpy), s.img, mon.x, mon.y, AllPlanes);
    XFlush(dpy);
}

static bool save_ppm(Display *dpy, XImage *img, const char *path) {
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

// ============================================================
// Daemon: trigger screenshot via FIFO, save ppm
// ============================================================

static void daemon_send_ack(bool ok) {
    FILE *ack = fopen(ACKFIFO, "w");
    if (ack) { fprintf(ack, ok ? "ok\n" : "err\n"); fclose(ack); }
    else perror(NAME ": fopen ack");
}

static int run_daemon() {
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        fprintf(stderr, NAME ": cannot open display\n");
        return 1;
    }

    if (!XShmQueryExtension(dpy)) {
        fprintf(stderr, NAME ": XShm extension required\n"); return 1;
    }

    clear_dir(RUNDIR);
    ensure_dir(RUNDIR);
    ensure_fifo(REQFIFO);
    ensure_fifo(ACKFIFO);

    std::vector<Monitor> monitors = get_monitors(dpy);
    if (monitors.empty()) {
        fprintf(stderr, NAME ": no connected monitors\n");
        return 1;
    }

    printf(NAME " daemon: %zu monitor(s):\n", monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i)
        printf("  [%zu] %-12s  %dx%d+%d+%d\n",
               i, monitors[i].name.c_str(),
               monitors[i].w, monitors[i].h,
               monitors[i].x, monitors[i].y);

    std::vector<ShmImage> shms(monitors.size());
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (!shm_alloc(dpy, shms[i], monitors[i].w, monitors[i].h)) {
            fprintf(stderr, NAME ": shm_alloc failed for monitor %zu\n", i);
            return 1;
        }
    }

    printf(NAME " daemon: ready — listening on %s\n", REQFIFO);
    fflush(stdout);

    for (;;) {
        // Blocks until client opens the write end
        FILE *req = fopen(REQFIFO, "r");
        if (!req) { perror(NAME ": fopen req"); continue; }
        fclose(req);   // contents irrelevant — trigger only

        // Determine which monitor and desktop are currently focused
        Atom net_current_desktop = XInternAtom(dpy, "_NET_CURRENT_DESKTOP",
                                               False);
        Atom atype; int afmt; unsigned long nitems, after;
        unsigned char *data = nullptr;
        long desk = 0;
        if (XGetWindowProperty(dpy, DefaultRootWindow(dpy),
                               net_current_desktop, 0, 1, False, XA_CARDINAL,
                               &atype, &afmt, &nitems,
                               &after, &data) == Success
            && data && nitems == 1) {
            desk = (long)*(unsigned long *)data;
            XFree(data);
        }

        int mi = focused_monitor(dpy, monitors);
        const Monitor &mon = monitors[mi];
        grab_monitor(dpy, mon, shms[mi]);

        char path[256];
        snprintf(path, sizeof(path), "%s/%ld.ppm", RUNDIR, desk);
        bool saved = save_ppm(dpy, shms[mi].img, path);

        // printf(NAME " daemon: desk=%ld mon=[%d]%s -> %s (%s)\n",
        //        desk, mi, mon.name.c_str(), path, saved ? "ok" : "err");
        // fflush(stdout);

        daemon_send_ack(saved);
    }

    for (size_t i = 0; i < monitors.size(); ++i)
        shm_free(dpy, shms[i]);
    XCloseDisplay(dpy);
    return 0;
}

// ============================================================
// Client helpers
// ============================================================

// Check whether the daemon is up by seeing if the FIFOs exist and are
// actually FIFOs.  This does NOT open them, so it never blocks.
static bool daemon_running() {
    struct stat st;
    return stat(REQFIFO, &st) == 0 && S_ISFIFO(st.st_mode) &&
           stat(ACKFIFO, &st) == 0 && S_ISFIFO(st.st_mode);
}

// Require the daemon; print a helpful message and return false if absent.
static bool require_daemon() {
    if (daemon_running()) return true;
    fprintf(stderr, NAME ": daemon is not running\n");
    return false;
}

// Trigger a screenshot and block until the daemon acknowledges it.
static bool client_screenshot() {
    FILE *req = fopen(REQFIFO, "w");
    if (!req) { perror(NAME ": fopen req"); return false; }
    fclose(req);   // opening the write end is the trigger; content is irrelevant

    FILE *ack = fopen(ACKFIFO, "r");
    if (!ack) { perror(NAME ": fopen ack"); return false; }
    char line[16] = {};
    bool ok = (fgets(line, sizeof(line), ack) != nullptr)
              && (strncmp(line, "ok", 2) == 0);
    fclose(ack);
    return ok;
}

// public subcommand wrapping client_screenshot()
static int run_screenshot() {
    if (!require_daemon()) return 1;
    bool ok = client_screenshot();
    if (!ok) { fprintf(stderr, NAME ": screenshot failed\n"); return 1; }
    return 0;
}

// ============================================================
// Client: workspace picker via gmenu
// ============================================================

int run_picker(bool move_mode) {
    if (!require_daemon()) return 1;

    Display* dpy = XOpenDisplay(nullptr);

    if (!client_screenshot())
        fprintf(stderr, NAME
                ": screenshot failed — menu may show stale thumbnails\n");

    auto cmd_change     = env_cmd(NAME_UPPER "_CHANGE_CMD");     // e.g. "bspdd"
    auto cmd_move       = env_cmd(NAME_UPPER "_MOVE_CMD");       // e.g. "bspdd node-move-go"
    auto cmd_change_new = env_cmd(NAME_UPPER "_CHANGE_NEW_CMD"); // e.g. "bspdd new"
    auto cmd_move_new   = env_cmd(NAME_UPPER "_MOVE_NEW_CMD");   // e.g. "bspdd new-move"

    int focused_idx = getCurrentDesktopIndex(dpy);
    auto desks = getDesktopNames(dpy);
    auto wins  = getDesktopWindowTitles(dpy);

    // Build the gmenu item list in memory (same format as before)
    std::string items;
    char entry[1024];
    for (const auto &[idx, d] : desks) {
        if (d.empty()) continue;
        char ppm[256];
        snprintf(ppm, sizeof(ppm), "%s/%ld.ppm", RUNDIR, idx);
        struct stat st;
        const char* icon = (stat(ppm, &st) == 0) ? ppm : "desktop";
        snprintf(entry, sizeof(entry),
                 ">>j {\"name\":\"%s: %s\",\"icon\":\"%s\"}\n",
                 d.c_str(), wins[idx].c_str(), icon);
        items += entry;
    }
    if (!cmd_change_new.empty() || !cmd_move_new.empty()) {
        items += ">>j {\"name\":\"New\","
            "\"icon\":\"window-new-symbolic\",\"icon-size\":64}\n";
    }

    // Two pipes: parent->child (stdin of gmenu) and child->parent (stdout of gmenu)
    int in_pipe[2];   // [0] child reads,  [1] parent writes
    int out_pipe[2];  // [0] parent reads, [1] child writes
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        perror(NAME ": pipe");
        XCloseDisplay(dpy);
        return 1;
    }

    const char* prompt = move_mode ? "Move window to workspace…" : "Workspaces";

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
    std::string choice = read_fd(out_pipe[0]);
    close(out_pipe[0]);

    // Reap the child
    waitpid(pid, nullptr, 0);

    // Trim trailing newline
    chomp(choice);

    if (choice.empty()) return 0;

    bool is_new = (choice == "New" || choice.rfind("New", 0) == 0);
    std::size_t pos = choice.find(':');
    std::string choice_desk =
        (pos == std::string::npos) ? choice : choice.substr(0, pos);

    if (!cmd_change.empty() || !cmd_move.empty() ||
        !cmd_change_new.empty() || !cmd_move_new.empty()) {
        if (move_mode) {
            if (is_new) {
                if (!spawn_cmd(cmd_move_new)) {
                    fprintf(stderr, NAME ": "
                            NAME "_MOVE_NEW_CMD not set or failed\n");
                    return 1;
                }
            } else {
                if (!spawn_cmd(cmd_move, choice_desk.c_str())) {
                    fprintf(stderr, NAME ": " NAME
                            "_MOVE_CMD not set or failed\n");
                    return 1;
                }
            }

        } else {
            if (is_new) {
                if (!spawn_cmd(cmd_change_new)) {
                    fprintf(stderr, NAME ": " NAME
                            "_CHANGE_NEW_CMD not set or failed\n");
                    return 1;
                }
            } else {
                if (!spawn_cmd(cmd_change, choice_desk.c_str())) {
                    fprintf(stderr, NAME ": " NAME
                            "_CHANGE_CMD not set or failed\n");
                    return 1;
                }
            }
        }

    } else {
        long choice_idx = -1;
        for (const auto &[idx, d] : desks) {
            if (choice_desk == d) {
                choice_idx = idx;
                break;
            }
        }
        if (choice_idx < 0) {
            fprintf(stderr, NAME ": failed finding workspace id\n");
            return 1;
        }

        if (move_mode) {
            moveFocusedWindowAndSwitch(dpy, choice_idx);
        } else {
            switchDesktop(dpy, choice_idx);
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
            "  %s daemon       run the screenshot daemon\n"
            "  %s screenshot   capture the current desktop now\n"
            "  %s switch       open workspace picker and switch to selection\n"
            "  %s move         open workspace picker and move focused window\n"
            "  %s ls           list current desktops and windows\n",
            argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "daemon")) {
        return run_daemon();
    } else if (!strcmp(cmd, "screenshot")) {
        return run_screenshot();
    } else if (!strcmp(cmd, "switch")) {
        return run_picker(false);
    } else if (!strcmp(cmd, "move")) {
        return run_picker(true);
    } else if (!strcmp(cmd, "ls")) {
        Display* dpy = XOpenDisplay(nullptr);
        auto desks = getDesktopNames(dpy);
        auto wins = getDesktopWindowTitles(dpy);
        for (const auto &[idx, d] : desks) {
            printf("%s: %s\n", d.c_str(), wins[idx].c_str());
        }
        XCloseDisplay(dpy);
        return 0;
    } else {
        fprintf(stderr, NAME ": unknown command '%s'\n", cmd);
        usage(argv[0]);
        return 1;
    }
}
