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
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dirent.h>
#include <unistd.h>
#include <spawn.h>
#include <fcntl.h>
#include <signal.h>

#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <format>
#include <vector>
#include <map>
#include <sstream>
#include <iostream>

#define NAME       "xgds"
#define NAME_UPPER "XGDS"
#define GMENU      "gmenu"

static std::string run_dir;
static std::string sock_path;

// ============================================================
// Helpers
// ============================================================

[[maybe_unused]]
unsigned long millis() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const std::string runtime_dir() {
    // XDG_RUNTIME_DIR is guaranteed by systemd/PAM on any modern Linux
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg) return std::string(xdg);
    // Fallback for environments without systemd
    return "/run/user/" + std::to_string(getuid());
}

static void init_paths() {
    const auto p = runtime_dir();
    run_dir   = p + "/" NAME;
    sock_path = p + "/" NAME "/daemon.sock";
}

static std::string desktop_screenshot_path(long desk) {
    return run_dir + "/" + std::to_string(desk) + ".ppm";
}

static std::string window_screenshot_path(Window w) {
    return run_dir + "/win-" + std::to_string((unsigned long)w) + ".ppm";
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, NAME ": mkdir(%s): %s\n", path, strerror(errno));
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
// X11 error handling
// ============================================================

// Xlib's default error handler calls exit() on any protocol error (e.g.
// BadWindow from XGetWindowProperty on a window that has since been
// destroyed). Since we routinely query windows we discovered a moment
// earlier (_NET_CLIENT_LIST, _NET_ACTIVE_WINDOW), those windows can
// legitimately disappear out from under us — that's not a bug, it's a
// race against the rest of the desktop. Log and continue instead of
// crashing the whole picker over one stale window.
static int xerror_handler(Display *dpy, XErrorEvent *ev) {
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
    Atom net_number_of_desktops;
    Atom net_current_desktop;
    Atom net_desktop_names;
    Atom net_desktop_viewport;
    Atom net_client_list;
    Atom net_wm_desktop;
    Atom net_wm_name;
    Atom net_active_window;
    Atom utf8_string;
};

static Atoms init_atoms(Display* dpy) {
    Atoms a;
    a.net_number_of_desktops = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    a.net_current_desktop    = XInternAtom(dpy, "_NET_CURRENT_DESKTOP",    False);
    a.net_desktop_names      = XInternAtom(dpy, "_NET_DESKTOP_NAMES",      False);
    a.net_desktop_viewport   = XInternAtom(dpy, "_NET_DESKTOP_VIEWPORT",   False);
    a.net_client_list        = XInternAtom(dpy, "_NET_CLIENT_LIST",        False);
    a.net_wm_desktop         = XInternAtom(dpy, "_NET_WM_DESKTOP",         False);
    a.net_wm_name            = XInternAtom(dpy, "_NET_WM_NAME",            False);
    a.net_active_window      = XInternAtom(dpy, "_NET_ACTIVE_WINDOW",      False);
    a.utf8_string            = XInternAtom(dpy, "UTF8_STRING",             False);
    return a;
}

// ============================================================
// X11 helpers
// ============================================================

static std::map<long, std::string> getDesktopNames(Display* dpy, const Atoms& atoms) {
    std::map<long, std::string> out;
    if (!dpy) return out;

    Window root = DefaultRootWindow(dpy);

    // Get number of desktops
    long num_desktops = 0;

    {
        Atom actualType;
        int actualFormat;
        unsigned long nItems;
        unsigned long bytesAfter;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(dpy, root, atoms.net_number_of_desktops,
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

    if (XGetWindowProperty(dpy, root, atoms.net_desktop_names,
                           0, (~0L),
                           False,
                           atoms.utf8_string,
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

static int getCurrentDesktopIndex(Display* dpy, const Atoms& atoms) {
    if (!dpy) return -1;

    Window root = DefaultRootWindow(dpy);

    Atom type;
    int format;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, root, atoms.net_current_desktop,
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
    if (XGetWindowProperty(dpy, w, atoms.net_wm_name,
                           0, (~0L), False, atoms.utf8_string,
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

std::map<long, std::string> getDesktopWindowTitles(Display* dpy, const Atoms& atoms) {
    std::map<long, std::string> result;

    Window root = DefaultRootWindow(dpy);

    Atom actualType;
    int actualFormat;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy,
                           root,
                           atoms.net_client_list,
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
                               atoms.net_wm_desktop,
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
// Unlike getDesktopWindowTitles(), this keeps each window separate so a
// specific one can be targeted for window switching.
static std::vector<std::pair<Window, std::string>> getWindowList(Display* dpy, const Atoms& atoms) {
    std::vector<std::pair<Window, std::string>> result;

    Window root = DefaultRootWindow(dpy);

    Atom actualType;
    int actualFormat;
    unsigned long nitems, bytesAfter;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, root, atoms.net_client_list, 0, (~0L), False,
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

static bool switchDesktop(Display *dpy, long desktop, const Atoms& atoms) {
    Window root = DefaultRootWindow(dpy);

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = root;
    ev.xclient.message_type = atoms.net_current_desktop;
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
                           atoms.net_active_window,
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
    ev.xclient.message_type = atoms.net_wm_desktop;
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
    ev.xclient.message_type = atoms.net_active_window;
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

[[maybe_unused]]
static int focused_monitor(Display *dpy, const std::vector<Monitor> &mons, const Atoms& atoms) {
    if (mons.size() == 1) return 0;

    Window root = DefaultRootWindow(dpy);
    if (atoms.net_active_window == None) return 0;

    Atom atype; int afmt; unsigned long nitems, after;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, root, atoms.net_active_window, 0, 1, False, XA_WINDOW,
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

static void grab_monitor(Display *dpy, const Monitor &mon, const ShmImage &s) {
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
// Daemon: trigger screenshot via Unix socket, save ppm
// ============================================================

// Send the ack reply back through the already-connected client socket.
static void daemon_send_ack(int client_fd, bool ok) {
    const char *msg = ok ? "ok\n" : "err\n";
    // Best-effort write; if the client disconnected early, ignore the error.
    (void)write(client_fd, msg, strlen(msg));
}

// Free all SHM buffers and rebuild monitors + SHM from the current RandR state.
// Returns false if no monitors are found after the refresh.
static bool rebuild_monitors(Display *dpy,
                             std::vector<Monitor>  &monitors,
                             std::vector<ShmImage> &shms) {
    // Release old SHM buffers.
    for (auto &s : shms) shm_free(dpy, s);
    shms.clear();
    monitors.clear();

    monitors = get_monitors(dpy);
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
        if (!shm_alloc(dpy, shms[i], monitors[i].w, monitors[i].h)) {
            fprintf(stderr, NAME ": shm_alloc failed for monitor %zu\n", i);
            // Free anything we already allocated before returning.
            for (size_t j = 0; j < i; ++j) shm_free(dpy, shms[j]);
            shms.clear();
            monitors.clear();
            return false;
        }
    }
    return true;
}

static bool capture_monitor_desktops(Display *dpy,
                                     const std::vector<Monitor> &monitors,
                                     const std::vector<ShmImage> &shms,
                                     const Atoms& atoms,
                                     bool &saved) {
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
    long num_desktops = read_cardinal(atoms.net_number_of_desktops, 1);

    // For each monitor, find which desktop is currently visible on
    // it by matching the desktop's viewport origin to the monitor
    // geometry.  A desktop is "on" a monitor when its viewport
    // origin falls inside (or equals the top-left of) that monitor.
    //
    // Fallback: if no desktop maps to a monitor (e.g. the WM uses a
    // single shared viewport), use _NET_CURRENT_DESKTOP for every
    // monitor so we at least capture something useful.
    long current_desk = read_cardinal(atoms.net_current_desktop, -1);
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
    if (XGetWindowProperty(dpy, root, atoms.net_desktop_viewport, 0,
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

    // Capture every monitor and save its PPM.
    int saved_count = 0;
    for (size_t mi = 0; mi < monitors.size(); ++mi) {
        long desk = ((long)mi == current_mon) ?
            current_desk : mon_desk[mi];
        grab_monitor(dpy, monitors[mi], shms[mi]);
        std::string path = desktop_screenshot_path(desk);
        if (save_ppm(dpy, shms[mi].img, path.c_str()))
            ++saved_count;
    }

    saved = (saved_count > 0);
    return saved;
}

// Capture a single window's own contents (not a screen region) and save it.
static bool save_window_screenshot(Display *dpy, Window win) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, win, &wa) || wa.map_state != IsViewable)
        return false;

    XImage *img = XGetImage(dpy, win, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);
    if (!img) return false;

    bool ok = save_ppm(dpy, img, window_screenshot_path(win).c_str());
    XDestroyImage(img);
    return ok;
}

// Screenshot every window individually, beside the per-desktop screenshots.
static void capture_window_screenshots(Display *dpy, const Atoms& atoms) {
    for (const auto &[win, title] : getWindowList(dpy, atoms))
        save_window_screenshot(dpy, win);
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

    // Subscribe to RandR events so we learn about hotplug changes.
    int rr_event_base, rr_error_base;
    bool have_randr = XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
    if (have_randr) {
        Window root = DefaultRootWindow(dpy);
        XRRSelectInput(dpy, root, RROutputChangeNotifyMask | RRScreenChangeNotifyMask);
    }

    clear_dir(run_dir.c_str());
    ensure_dir(run_dir.c_str());

    // Remove any stale socket from a previous run.
    unlink(sock_path.c_str());

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
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror(NAME ": bind"); close(srv); return 1;
    }
    chmod(sock_path.c_str(), 0600);

    if (listen(srv, /*backlog=*/8) != 0) {
        perror(NAME ": listen"); close(srv); return 1;
    }

    std::vector<Monitor> monitors;
    std::vector<ShmImage> shms;

    if (!rebuild_monitors(dpy, monitors, shms)) {
        fprintf(stderr, NAME ": no connected monitors\n");
        close(srv); return 1;
    }

    // Cache all needed atoms once — XInternAtom round-trips are cheap
    // but there is no reason to repeat them on every screenshot request.
    const Atoms atoms = init_atoms(dpy);

    printf(NAME " daemon: ready — listening on %s\n", sock_path.c_str());
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
                rebuild_monitors(dpy, monitors, shms);
                continue;
            }

            // RRNotify sub-events (output connect/disconnect).
            if (ev.type == rr_event_base + RRNotify) {
                XRRNotifyEvent *rrev = reinterpret_cast<XRRNotifyEvent *>(&ev);
                if (rrev->subtype == RRNotify_OutputChange) {
                    printf(NAME ": RandR output change — rebuilding monitors\n");
                    fflush(stdout);
                    rebuild_monitors(dpy, monitors, shms);
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
                    saved = capture_monitor_desktops(dpy, monitors, shms, atoms, saved);
                    capture_window_screenshots(dpy, atoms);
                }

                // Reply goes back through the same connection — no race possible.
                daemon_send_ack(client_fd, saved);
                close(client_fd);
            }
        }
        // X11 activity is handled at the top of the loop via XPending().
    }

    for (auto &s : shms) shm_free(dpy, s);
    close(srv);
    unlink(sock_path.c_str());
    XCloseDisplay(dpy);
    return 0;
}

// ============================================================
// Client helpers
// ============================================================

// Check whether the daemon is up by attempting a real connection to its
// Unix socket.  Using connect() rather than stat() catches stale socket
// files left by a crashed daemon.
static bool daemon_running() {
    // Probe by actually connecting — avoids stale socket files.
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    bool ok = (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(fd);
    return ok;
}

// Require the daemon; print a helpful message and return false if absent.
static bool require_daemon() {
    if (daemon_running()) return true;
    fprintf(stderr, NAME ": daemon is not running\n");
    return false;
}

// Send a screenshot request to the daemon over the Unix socket and wait for
// the "ok" / "err" reply.  Each call is a fully independent connection, so
// there is no open-order race and concurrent callers never steal each other's
// acknowledgement.
static bool client_screenshot() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror(NAME ": socket"); return false; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

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

enum PickerMode { PICK_SWITCH, PICK_MOVE, PICK_WINDOW };

int run_picker(PickerMode mode) {
    if (!require_daemon()) return 1;

    Display* dpy = XOpenDisplay(nullptr);

    if (!client_screenshot())
        fprintf(stderr, NAME
                ": screenshot failed — menu may show stale thumbnails\n");

    bool move_mode = (mode == PICK_MOVE);

    auto cmd_change     = env_cmd(NAME_UPPER "_CHANGE_CMD");
    auto cmd_move       = env_cmd(NAME_UPPER "_MOVE_CMD");
    auto cmd_change_new = env_cmd(NAME_UPPER "_CHANGE_NEW_CMD");
    auto cmd_move_new   = env_cmd(NAME_UPPER "_MOVE_NEW_CMD");

    const Atoms atoms = init_atoms(dpy);

    int focused_idx = getCurrentDesktopIndex(dpy, atoms);
    auto desks = getDesktopNames(dpy, atoms);
    auto wins  = getDesktopWindowTitles(dpy, atoms);

    // Only populated for PICK_WINDOW — pairs of (window id, title).
    auto winlist = (mode == PICK_WINDOW) ? getWindowList(dpy, atoms)
                                          : std::vector<std::pair<Window, std::string>>{};

    // Build the gmenu item list in memory (same format as before)
    std::ostringstream oss;
    if (mode == PICK_WINDOW) {
        for (const auto &[win, title] : winlist) {
            const std::string ppm = window_screenshot_path(win);
            struct stat st;
            const std::string icon =
                (stat(ppm.c_str(), &st) == 0) ? ppm : "window";
            oss << ">>j {\"name\":\"" << title
                << "\",\"icon\":\"" << icon << "\"}\n";
        }
    } else {
        for (const auto &[idx, d] : desks) {
            if (d.empty()) continue;
            const std::string ppm = desktop_screenshot_path(idx);
            struct stat st;
            const std::string icon =
                (stat(ppm.c_str(), &st) == 0) ? ppm : "desktop";
            const auto it = wins.find(idx);
            const std::string &label =
                (it != wins.end() && !it->second.empty()) ? it->second : d;
            oss << ">>j {\"name\":\"" << label
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
    std::string choice = read_fd(out_pipe[0]);
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
        Window target = None;
        for (const auto &[win, title] : winlist) {
            if (choice_desk == title) { target = win; break; }
        }
        if (target == None) {
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
            "  %s daemon       run the screenshot daemon\n"
            "  %s screenshot   capture the current desktop now\n"
            "  %s switch       open workspace picker and switch to selection\n"
            "  %s move         open workspace picker and move focused window\n"
            "  %s switch-windows  open window picker and switch to selection\n"
            "  %s ls           list current desktops and windows\n",
            argv0, argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    const char *cmd = argv[1];

    XSetErrorHandler(xerror_handler);
    init_paths();

    if (!strcmp(cmd, "daemon")) {
        return run_daemon();
    } else if (!strcmp(cmd, "screenshot")) {
        return run_screenshot();
    } else if (!strcmp(cmd, "switch")) {
        return run_picker(PICK_SWITCH);
    } else if (!strcmp(cmd, "move")) {
        return run_picker(PICK_MOVE);
    } else if (!strcmp(cmd, "switch-windows")) {
        return run_picker(PICK_WINDOW);
    } else if (!strcmp(cmd, "ls")) {
        Display* dpy = XOpenDisplay(nullptr);
        const Atoms atoms = init_atoms(dpy);
        auto desks = getDesktopNames(dpy, atoms);
        auto wins = getDesktopWindowTitles(dpy, atoms);
        for (const auto &[idx, d] : desks) {
            const auto it = wins.find(idx);
            if (it != wins.end() && !it->second.empty())
                printf("%s: %s\n", d.c_str(), it->second.c_str());
            else
                printf("%s\n", d.c_str());
        }
        XCloseDisplay(dpy);
        return 0;
    } else {
        fprintf(stderr, NAME ": unknown command '%s'\n", cmd);
        usage(argv[0]);
        return 1;
    }
}
