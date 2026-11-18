// midve-hud: the overview HUD for niri (see ../PLAN.md).
//
// One transparent layer surface per output, placed in the overview backdrop
// (layer-rule: place-within-backdrop, namespace "midve-hud"). The surface stays
// mapped; the patched niri fades it in and out with the overview's progress, so
// the fade runs at the output's refresh rate and in step with the cards. With
// the overview closed niri draws nothing of it, and the HUD neither draws nor
// polls.
//
// Panels live only where workspace cards never go; their sizes come from the
// same grid formula as the niri patch. Panels are drawn flat, each into a
// cached image when its data changes. niri turns the two flanks towards the
// middle in perspective, slides them in from the sides as the overview opens
// and maps the pointer back through that, all on the GPU, so nothing here
// animates with the overview. Everything is drawn as pictures; nothing is
// selectable text.
//
// Data: clock and calendar; cpu and ram from /proc, gpu through NVML; the
// MPRIS player (with its capabilities: no seek bar for a stream, disabled
// buttons it can't honour); volume through wpctl; Bluetooth through BlueZ on
// D-Bus; mako's history on D-Bus. System mode (graphs and processes) asks niri
// to hide the workspace cards (SetOverviewWorkspacesHidden).

#include <dirent.h>
#include <dlfcn.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk-layer-shell/gtk-layer-shell.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ── Palette and type ───────────────────────────────────────────────────────

#define C_TEXT 0.925, 0.878, 0.933
#define C_MUTED 0.604, 0.541, 0.596
#define C_FAINT 0.435, 0.376, 0.427
#define C_LILAC 0.863, 0.765, 0.894
#define C_WINE 0.769, 0.416, 0.502
#define C_VIOLET 0.706, 0.541, 0.776
#define FONT "Onest"

// Overview geometry, as in the niri patch and layout.kdl.
#define ZOOM 0.26
#define TILT_DEG 24.0

// ── Small helpers ──────────────────────────────────────────────────────────

static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static double ease_out(double t) { t = clamp01(t); return 1 - pow(1 - t, 3); }
static double now_s(void) { return g_get_monotonic_time() / 1e6; }
// MIDVE_HUD_PROF=1: print what each piece of work costs.
static int prof = -1;
#define PROF_BEGIN double prof_t0 = prof ? now_s() : 0
#define PROF_END(what) if (prof > 0) g_printerr("prof %-14s %6.2f ms\n", what, (now_s() - prof_t0) * 1e3)

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r) {
  r = fmin(r, fmin(w, h) / 2);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

// Draws text; returns its width. align: 0 left, 1 right, 0.5 centre (x is the anchor).
static double text(cairo_t *cr, const char *s, double x, double y, double size, int weight,
                   double r, double g, double b, double a, double align, double maxw) {
  // a partial re-render (a hover animation) clips to one button: skip text rows outside it
  double cx1, cy1, cx2, cy2;
  cairo_clip_extents(cr, &cx1, &cy1, &cx2, &cy2);
  if (y > cy2 || y + size * 1.6 < cy1) return 0;
  PangoLayout *l = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, FONT);
  pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
  pango_font_description_set_weight(fd, weight);
  pango_layout_set_font_description(l, fd);
  pango_layout_set_text(l, s, -1);
  if (maxw > 0) {
    pango_layout_set_width(l, (int)(maxw * PANGO_SCALE));
    pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
  }
  int w, h;
  pango_layout_get_pixel_size(l, &w, &h);
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_move_to(cr, x - w * align, y);
  pango_cairo_show_layout(cr, l);
  pango_font_description_free(fd);
  g_object_unref(l);
  return w;
}

// ── 3×3 matrices (row-major) ───────────────────────────────────────────────

typedef struct { double m[9]; } M3;

static M3 m3_inv(M3 a) {
  double *m = a.m, r[9];
  r[0] = m[4] * m[8] - m[5] * m[7]; r[1] = m[2] * m[7] - m[1] * m[8]; r[2] = m[1] * m[5] - m[2] * m[4];
  r[3] = m[5] * m[6] - m[3] * m[8]; r[4] = m[0] * m[8] - m[2] * m[6]; r[5] = m[2] * m[3] - m[0] * m[5];
  r[6] = m[3] * m[7] - m[4] * m[6]; r[7] = m[1] * m[6] - m[0] * m[7]; r[8] = m[0] * m[4] - m[1] * m[3];
  double det = m[0] * r[0] + m[1] * r[3] + m[2] * r[6];
  M3 out;
  for (int i = 0; i < 9; i++) out.m[i] = fabs(det) > 1e-12 ? r[i] / det : 0;
  return out;
}

static void m3_apply(M3 a, double x, double y, double *ox, double *oy) {
  double X = a.m[0] * x + a.m[1] * y + a.m[2];
  double Y = a.m[3] * x + a.m[4] * y + a.m[5];
  double Z = a.m[6] * x + a.m[7] * y + a.m[8];
  if (fabs(Z) < 1e-9) Z = 1e-9;
  *ox = X / Z;
  *oy = Y / Z;
}

// ── niri IPC ───────────────────────────────────────────────────────────────

static GSocketConnection *niri_connect(void) {
  const char *path = g_getenv("NIRI_SOCKET");
  if (!path) return NULL;
  GSocketClient *c = g_socket_client_new();
  GSocketAddress *addr = g_unix_socket_address_new(path);
  GSocketConnection *conn = g_socket_client_connect(c, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
  g_object_unref(addr);
  g_object_unref(c);
  return conn;
}

static void niri_request(const char *json) {
  GSocketConnection *conn = niri_connect();
  if (!conn) return;
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
  g_output_stream_write_all(out, json, strlen(json), NULL, NULL, NULL);
  g_output_stream_write_all(out, "\n", 1, NULL, NULL, NULL);
  g_object_unref(conn);
}

// Whether this niri has the overview HUD patch: it knows SetOverviewWorkspacesHidden.
static gboolean niri_has_patch(void) {
  GSocketConnection *c = niri_connect();
  if (!c) return FALSE;
  const char *req = "{\"Action\":{\"SetOverviewWorkspacesHidden\":{\"hidden\":false}}}\n";
  g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(c)), req, strlen(req), NULL, NULL, NULL);
  GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(c)));
  char *line = g_data_input_stream_read_line(in, NULL, NULL, NULL);
  gboolean ok = line && g_str_has_prefix(line, "{\"Ok\"");
  g_free(line);
  g_object_unref(in);
  g_object_unref(c);
  return ok;
}

// ── Data ───────────────────────────────────────────────────────────────────

#define HIST 61
typedef struct { double v[HIST]; } Series;
static void series_push(Series *s, double x) {
  memmove(s->v, s->v + 1, sizeof(double) * (HIST - 1));
  s->v[HIST - 1] = x;
}

static struct {
  double cpu, gpu, vram, ram_used, ram_total;
  unsigned long long cpu_idle, cpu_total;
  unsigned long long net_rx, net_tx;
  double net_down, net_up;  // bytes per second, physical interfaces only
  Series s_cpu, s_gpu, s_ram, s_net, s_up;
  double last_sample;
} sys;

// NVML, loaded at runtime (no build dependency on the CUDA headers).
typedef struct { unsigned int gpu, memory; } nvmlUtil;
typedef struct { unsigned long long total, free, used; } nvmlMem;
static struct {
  void *lib, *dev;
  int (*init)(void);
  int (*handle)(unsigned int, void **);
  int (*util)(void *, nvmlUtil *);
  int (*mem)(void *, nvmlMem *);
} nv;

static void nvml_load(void) {
  nv.lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
  if (!nv.lib) return;
  nv.init = dlsym(nv.lib, "nvmlInit_v2");
  nv.handle = dlsym(nv.lib, "nvmlDeviceGetHandleByIndex_v2");
  nv.util = dlsym(nv.lib, "nvmlDeviceGetUtilizationRates");
  nv.mem = dlsym(nv.lib, "nvmlDeviceGetMemoryInfo");
  if (!nv.init || !nv.handle || !nv.util || nv.init() != 0 || nv.handle(0, &nv.dev) != 0)
    nv.dev = NULL;
}

static gboolean any_shown(void);
static void sys_sample(void) {
  FILE *f = fopen("/proc/stat", "r");
  if (f) {
    unsigned long long u, n, s, i, io, irq, sirq, st;
    if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &io, &irq, &sirq,
               &st) == 8) {
      unsigned long long idle = i + io, total = u + n + s + i + io + irq + sirq + st;
      if (sys.cpu_total && total > sys.cpu_total)
        sys.cpu = 100.0 * (1.0 - (double)(idle - sys.cpu_idle) / (total - sys.cpu_total));
      sys.cpu_idle = idle;
      sys.cpu_total = total;
    }
    fclose(f);
  }
  f = fopen("/proc/meminfo", "r");
  if (f) {
    char k[64];
    unsigned long long v, tot = 0, avail = 0;
    while (fscanf(f, "%63s %llu kB\n", k, &v) == 2) {
      if (!strcmp(k, "MemTotal:")) tot = v;
      if (!strcmp(k, "MemAvailable:")) avail = v;
    }
    fclose(f);
    sys.ram_total = tot / 1048576.0;
    sys.ram_used = (tot - avail) / 1048576.0;
  }
  // Network: download and upload apart, over physical interfaces only (those with a device in
  // /sys). Bridges, veth, tun and wireguard carry the same bytes again and would count twice.
  f = fopen("/proc/net/dev", "r");
  if (f) {
    char line[512];
    unsigned long long rx_sum = 0, tx_sum = 0;
    while (fgets(line, sizeof line, f)) {
      char *c = strchr(line, ':');
      if (!c) continue;
      *c = 0;
      char *name = line;
      while (*name == ' ') name++;
      char dev[600];
      snprintf(dev, sizeof dev, "/sys/class/net/%s/device", name);
      if (access(dev, F_OK) != 0) continue;
      unsigned long long rx, x, tx;
      if (sscanf(c + 1, "%llu %llu %llu %llu %llu %llu %llu %llu %llu", &rx, &x, &x, &x, &x, &x,
                 &x, &x, &tx) == 9) {
        rx_sum += rx;
        tx_sum += tx;
      }
    }
    fclose(f);
    double t = now_s(), dt = fmax(t - sys.last_sample, 0.2);
    if (sys.last_sample > 0 && rx_sum >= sys.net_rx && tx_sum >= sys.net_tx) {
      sys.net_down = (rx_sum - sys.net_rx) / dt;
      sys.net_up = (tx_sum - sys.net_tx) / dt;
    } else {
      sys.net_down = sys.net_up = 0;  // first sample, or an interface went away
    }
    sys.net_rx = rx_sum;
    sys.net_tx = tx_sum;
    sys.last_sample = t;
  }
  PROF_BEGIN;
  // the gpu is asked only while the overview is open (an NVML query costs ~2 ms)
  if (nv.dev && any_shown()) {
    nvmlUtil u;
    nvmlMem m;
    if (nv.util(nv.dev, &u) == 0) sys.gpu = u.gpu;
    if (nv.mem && nv.mem(nv.dev, &m) == 0) sys.vram = m.used / 1073741824.0;
  }
  PROF_END("nvml");
  series_push(&sys.s_cpu, sys.cpu);
  series_push(&sys.s_gpu, sys.gpu);
  series_push(&sys.s_ram, sys.ram_used);
  series_push(&sys.s_net, sys.net_down);
  series_push(&sys.s_up, sys.net_up);
}

// Processes, by resident memory.
#define NPROC 16
typedef struct { int pid; char name[48]; double rss_gb, cpu; unsigned long long ticks; } Proc;
static Proc procs[NPROC];
static int nprocs;
static GHashTable *proc_ticks;  // pid -> last ticks

static void procs_sample(void) {
  DIR *d = opendir("/proc");
  if (!d) return;
  static Proc all[4096];
  int n = 0;
  long pg = sysconf(_SC_PAGESIZE), hz = sysconf(_SC_CLK_TCK);
  if (!proc_ticks) proc_ticks = g_hash_table_new(g_direct_hash, g_direct_equal);
  struct dirent *e;
  uid_t me = getuid();
  while ((e = readdir(d)) && n < 4096) {
    int pid = atoi(e->d_name);
    if (pid <= 0) continue;
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    FILE *f = fopen(p, "r");
    if (!f) continue;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    char *l = strchr(buf, '('), *r = strrchr(buf, ')');
    if (!l || !r) continue;
    snprintf(p, sizeof p, "/proc/%d", pid);
    GStatBuf stb;
    if (g_stat(p, &stb) == 0 && stb.st_uid != me) continue;  // only your own processes
    Proc *pr = &all[n];
    pr->pid = pid;
    size_t nl = MIN((size_t)(r - l - 1), sizeof pr->name - 1);
    memcpy(pr->name, l + 1, nl);
    pr->name[nl] = 0;
    unsigned long ut = 0, st = 0;
    long rss = 0;
    sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
           &ut, &st, &rss);
    pr->rss_gb = rss * (double)pg / 1073741824.0;
    pr->ticks = ut + st;
    gpointer prev = g_hash_table_lookup(proc_ticks, GINT_TO_POINTER(pid));
    pr->cpu = prev ? (pr->ticks - (unsigned long long)GPOINTER_TO_SIZE(prev)) * 100.0 / hz : 0;
    g_hash_table_insert(proc_ticks, GINT_TO_POINTER(pid), GSIZE_TO_POINTER((gsize)pr->ticks));
    n++;
  }
  closedir(d);
  // top NPROC by memory
  nprocs = 0;
  for (int k = 0; k < NPROC; k++) {
    int best = -1;
    for (int i = 0; i < n; i++)
      if (all[i].pid && (best < 0 || all[i].rss_gb > all[best].rss_gb)) best = i;
    if (best < 0) break;
    procs[nprocs++] = all[best];
    all[best].pid = 0;
  }
}

// MPRIS player
static struct {
  GDBusProxy *proxy;
  char *bus;
  char title[256], artist[256], trackid[256];
  gboolean playing, can_seek, can_next, can_prev, can_play, can_pause;
  gint64 length_us, pos_us;
  double pos_at;  // when pos_us was read
  cairo_surface_t *art;
  char art_url[512];
  char name[48];   // the source shown: "spotify", "helium"
  char rname[48];  // the source at the last player_read, to see a switch
} pl;

// Every MPRIS source on the bus (playerctld, a mirror of another one, left out), in a stable
// order. The panel shows one; the dots in its header switch between them.
#define NPL 8
typedef struct { char bus[128], owner[64], name[48]; gboolean playing; } PlSrc;
static PlSrc plsrc[NPL];
static int nplsrc, plsel = -1;  // plsel: the shown one's index
static int plsel_old = -1;      // the one before the last switch, for the dots' pill (A_PSW)

// Volume
static struct { double v; gboolean muted; char sink[128]; double pending; double sent_at; } vol = {.v = -1};

// Bluetooth
#define NBT 6
typedef struct { char path[128], name[96], icon[48]; gboolean connected, busy; int battery; double err_until; } BtDev;
static struct { GDBusObjectManager *om; char adapter[96]; gboolean powered, have; BtDev d[NBT]; int n; } bt;

// Notifications (mako history)
#define NNOTE 3
typedef struct { guint id; char app[64], summary[160], body[200], ago[24]; gboolean critical; } Note;
static Note notes[NNOTE];
static int nnotes;
static GHashTable *dismissed;
// When each notification arrived (id -> wall-clock µs). mako's history has no time, so the HUD
// watches the bus for Notify calls (notes_watch). Ones from before the HUD started have none.
static GHashTable *note_time;

// "just now", "5 min ago", "2 h ago", "yesterday", "3 d ago"; empty when the time is unknown.
static void note_ago(const Note *n, char *buf, size_t len) {
  gpointer v = g_hash_table_lookup(note_time, GUINT_TO_POINTER(n->id));
  if (!v) {
    buf[0] = 0;
    return;
  }
  gint64 s = (g_get_real_time() - *(gint64 *)v) / G_USEC_PER_SEC;
  if (s < 60) snprintf(buf, len, "just now");
  else if (s < 3600) snprintf(buf, len, "%d min ago", (int)(s / 60));
  else if (s < 86400) snprintf(buf, len, "%d h ago", (int)(s / 3600));
  else if (s < 2 * 86400) snprintf(buf, len, "yesterday");
  else snprintf(buf, len, "%d d ago", (int)(s / 86400));
}

// ── Panels ─────────────────────────────────────────────────────────────────

enum { P_CLOCK, P_BT, P_PLAYER, P_NOTES, P_SWITCH, P_DASH, P_MCLOCK, P_MTRACK, NPANELS };

typedef struct { int id; double x, y, w, h; } Hit;
#define NHIT 48

typedef struct {
  int kind;
  double x, y, w, h;  // logical, unwarped
  int side;           // -1 left flank (pivot left), +1 right flank (pivot right), 0 flat
  gboolean visible;
  gboolean dirty;
  cairo_surface_t *content;  // cached content at the output scale
  // what the content is shown with this frame
  M3 hm, hinv;
  double bx, by, bw, bh;     // screen bbox
  cairo_surface_t *warped;   // perspective image of the content (for side != 0)
  double wkey[3];
  // a part to re-render and re-warp (local logical rect), for hover and press animations
  gboolean part, wpart;
  double px0, py0, px1, py1;  // to re-render
  double wx0, wy0, wx1, wy1;  // to re-warp
  Hit hits[NHIT];
  int nhit;
} Panel;

typedef struct {
  char name[64];
  GdkMonitor *mon;
  GtkWidget *win, *area;
  int W, H, scale;
  gboolean portrait, shown;
  double open_t;   // when the overview opened here
  gboolean drawn;  // has content on screen (kept while niri fades it out)
  gboolean sys_mode;
  double sys_t;  // when the system mode was toggled
  gboolean dash_up;  // the system panel is drawn (while on, and while niri fades it out)
  Panel p[NPANELS];
  guint tick;
  // interaction
  int press_panel, press_hit;
  gboolean dragging;
  double drag_frac;
  int armed_pid;
  double armed_until;
  // hover and press highlights: the hit under the pointer, and per panel and hit id an
  // animated amount 0..1, eased on the frame clock
  int hov_panel, hov_hit;
  float hv[NPANELS][64], pv[NPANELS][64];
  gint64 anim_last;
  // short content animations (anim_start): when each began (0: not running)
  double an_t0[8];
} Out;

// Short content animations. Each one repaints only its part of a panel (anim_parts), on the
// frame clock, for a fraction of a second; drawing code reads its progress with ANIM().
enum { A_MINUTE, A_TODAY, A_TRACK, A_ART, A_NOTES, A_DASH, A_PSW, NANIM };
static const double anim_dur0[NANIM] = {0.26, 0.42, 0.38, 0.3, 0.24, 0.45, 0.34};
// MIDVE_HUD_SLOW=N plays them N times slower (for checking them by eye or in screenshots)
static double anim_slow = 1;
#define anim_dur(k) (anim_dur0[k] * anim_slow)

static GPtrArray *outs;

// Hit ids
enum { H_PREV = 1, H_PLAY, H_NEXT, H_SEEK, H_MUTE, H_VOL, H_BTPOW, H_SW_WS, H_SW_SYS, H_PSRC,
       H_PDOT0 = 11 /* ..18, one per source */, H_NOTE0 = 20, H_BTDEV0 = 30, H_KILL0 = 40 };

static void hit_add(Panel *p, int id, double x, double y, double w, double h) {
  if (p->nhit < NHIT) p->hits[p->nhit++] = (Hit){id, x, y, w, h};
}

// The panel being drawn, so widgets can read their hover and press amounts.
static Out *draw_o;
static Panel *draw_p;
static double HOV(int id) { return draw_o && id > 0 && id < 64 ? draw_o->hv[draw_p->kind][id] : 0; }
static double PRS(int id) { return draw_o && id > 0 && id < 64 ? draw_o->pv[draw_p->kind][id] : 0; }
// Progress 0..1 of a content animation on the output being drawn; 1 when it isn't running.
static double ANIM(int k) {
  if (!draw_o || !draw_o->an_t0[k]) return 1;
  return clamp01((now_s() - draw_o->an_t0[k]) / anim_dur(k));
}
static double ease_io(double t) {
  t = clamp01(t);
  return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2;
}
static double ease_in(double t) { t = clamp01(t); return t * t * t; }
static void out_anim(Out *o);
static gboolean any_shown(void);
// Starts a content animation on every output where the HUD is up (delay: it begins later).
static void anim_start(int k, double delay) {
  for (guint i = 0; outs && i < outs->len; i++) {
    Out *o = g_ptr_array_index(outs, i);
    if (!o->shown) continue;
    o->an_t0[k] = now_s() + delay;
    out_anim(o);
  }
}
static gboolean anim_running(int k) {
  for (guint i = 0; outs && i < outs->len; i++) {
    Out *o = g_ptr_array_index(outs, i);
    if (o->shown && o->an_t0[k] && now_s() - o->an_t0[k] < anim_dur(k)) return TRUE;
  }
  return FALSE;
}
static double anim_progress_any(int k) {
  for (guint i = 0; outs && i < outs->len; i++) {
    Out *o = g_ptr_array_index(outs, i);
    if (o->shown && o->an_t0[k]) return clamp01((now_s() - o->an_t0[k]) / anim_dur(k));
  }
  return 1;
}
// a colour between two palette colours
static void mix_rgba(cairo_t *cr, double r0, double g0, double b0, double r1, double g1, double b1, double t,
                     double a) {
  t = clamp01(t);
  cairo_set_source_rgba(cr, r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t, a);
}
// soft lilac halo around a circle
static void halo(cairo_t *cr, double cx, double cy, double r, double spread, double a) {
  if (a <= 0.002) return;
  cairo_pattern_t *g = cairo_pattern_create_radial(cx, cy, r * 0.8, cx, cy, r + spread);
  cairo_pattern_add_color_stop_rgba(g, 0, C_LILAC, a);
  cairo_pattern_add_color_stop_rgba(g, 1, C_LILAC, 0);
  cairo_set_source(cr, g);
  cairo_arc(cr, cx, cy, r + spread, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
}

static void out_redraw(Out *o);
static void panel_queue(Out *o, int kind);
static void out_queue_all(Out *o);

// A panel's data changed: re-render its content and repaint only its area.
static void mark_dirty(int kind) {
  for (guint i = 0; outs && i < outs->len; i++) {
    Out *o = g_ptr_array_index(outs, i);
    o->p[kind].dirty = TRUE;
    if (o->shown) panel_queue(o, kind);
  }
}

// ── Panel content ──────────────────────────────────────────────────────────

static void panel_bg(cairo_t *cr, double w, double h, int side) {
  rrect(cr, 0.5, 0.5, w - 1, h - 1, 22);
  cairo_set_source_rgba(cr, 0.094, 0.067, 0.094, 0.66);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, C_TEXT, 0.09);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  // lilac hairline on top, fading to the sides
  cairo_pattern_t *g = cairo_pattern_create_linear(28, 0, w - 28, 0);
  cairo_pattern_add_color_stop_rgba(g, 0, C_LILAC, 0);
  cairo_pattern_add_color_stop_rgba(g, 0.5, C_LILAC, 0.55);
  cairo_pattern_add_color_stop_rgba(g, 1, C_LILAC, 0);
  cairo_set_source(cr, g);
  cairo_rectangle(cr, 28, 0, w - 56, 1);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  (void)side;
}

// The time before the last minute change, for the rolling digits.
static char clock_prev[8];

// A layout of the time string; xs gets each character's x (xs[n] the full width). The digits
// are drawn one by one at these places, so they look exactly like the whole string.
static PangoLayout *clock_layout(cairo_t *cr, const char *s, double size, int weight, double *xs) {
  PangoLayout *l = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = pango_font_description_new();
  pango_font_description_set_family(fd, FONT);
  pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
  pango_font_description_set_weight(fd, weight);
  pango_layout_set_font_description(l, fd);
  pango_font_description_free(fd);
  pango_layout_set_text(l, s, -1);
  int n = strlen(s);
  for (int i = 0; i < n; i++) {
    PangoRectangle r;
    pango_layout_index_to_pos(l, i, &r);
    xs[i] = (double)r.x / PANGO_SCALE;
  }
  int w, h;
  pango_layout_get_pixel_size(l, &w, &h);
  xs[n] = w;
  return l;
}

static void clock_glyph(cairo_t *cr, char c, double x, double y, double size, int weight, double a) {
  if (a <= 0.002) return;
  char s[2] = {c, 0};
  double xs[2];
  PangoLayout *l = clock_layout(cr, s, size, weight, xs);
  cairo_set_source_rgba(cr, C_TEXT, a);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, l);
  g_object_unref(l);
}

// The time. While A_MINUTE runs, each digit that changed leaves upwards and the new one comes
// up from below, inside its own cell; the others stand still (or glide, if a digit of another
// width moved them).
static void clock_text(cairo_t *cr, const char *t, double x, double y, double size, int weight) {
  double cx1, cy1, cx2, cy2;
  cairo_clip_extents(cr, &cx1, &cy1, &cx2, &cy2);
  if (y > cy2 || y + size * 1.4 < cy1) return;
  double e = ease_io(ANIM(A_MINUTE));
  int n = strlen(t);
  if (e >= 1 || (int)strlen(clock_prev) != n || n > 7) {
    double xs[8];
    PangoLayout *l = clock_layout(cr, t, size, weight, xs);
    cairo_set_source_rgba(cr, C_TEXT, 1);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return;
  }
  double xn[8], xo[8];
  g_object_unref(clock_layout(cr, t, size, weight, xn));
  g_object_unref(clock_layout(cr, clock_prev, size, weight, xo));
  for (int i = 0; i < n; i++) {
    if (clock_prev[i] == t[i]) {
      clock_glyph(cr, t[i], x + xo[i] + (xn[i] - xo[i]) * e, y, size, weight, 1);
      continue;
    }
    double hgt = size * 1.3, d = size * 0.78;
    double c0 = fmin(xo[i], xn[i]), c1 = fmax(xo[i + 1], xn[i + 1]);
    cairo_save(cr);
    cairo_rectangle(cr, x + c0 - 2, y, c1 - c0 + 4, hgt);
    cairo_clip(cr);
    clock_glyph(cr, clock_prev[i], x + xo[i], y - d * e, size, weight, 1 - e);
    clock_glyph(cr, t[i], x + xn[i], y + d * (1 - e), size, weight, e);
    cairo_restore(cr);
  }
}

// Where today's cell is in the calendar of the clock panel (its centre, local).
static gboolean clock_today(Panel *p, double *cx, double *cy) {
  GDateTime *now = g_date_time_new_now_local();
  int y = g_date_time_get_year(now), m = g_date_time_get_month(now), today = g_date_time_get_day_of_month(now);
  GDateTime *first = g_date_time_new_local(y, m, 1, 0, 0, 0);
  int off = g_date_time_get_day_of_week(first) - 1;
  g_date_time_unref(first);
  g_date_time_unref(now);
  double cw = (p->w - 56) / 7.0;
  int cell = off + today - 1;
  *cx = 28 + cw * (cell % 7 + 0.5);
  *cy = 170 + 26 + (cell / 7) * 28 + 9;
  return TRUE;
}

static void draw_clock(Panel *p, cairo_t *cr) {
  GDateTime *now = g_date_time_new_now_local();
  char *t = g_date_time_format(now, "%H:%M");
  char *d = g_date_time_format(now, "%A %-d %B");
  for (char *c = d; *c; c++) *c = g_ascii_tolower(*c);
  panel_bg(cr, p->w, p->h, p->side);
  clock_text(cr, t, 28, 18, 88, 300);
  text(cr, d, 30, 118, 19, 400, C_MUTED, 1, 0, 0);
  // month calendar
  int y = g_date_time_get_year(now), m = g_date_time_get_month(now), today = g_date_time_get_day_of_month(now);
  GDateTime *first = g_date_time_new_local(y, m, 1, 0, 0, 0);
  int off = g_date_time_get_day_of_week(first) - 1;  // monday first
  int days = g_date_get_days_in_month(m, y);
  double cw = (p->w - 56) / 7.0, cy0 = 170;
  const char *hd[] = {"mo", "tu", "we", "th", "fr", "sa", "su"};
  for (int i = 0; i < 7; i++) text(cr, hd[i], 28 + cw * (i + 0.5), cy0, 13, 400, C_FAINT, 1, 0.5, 0);
  for (int dd = 1; dd <= days; dd++) {
    int cell = off + dd - 1, col = cell % 7, row = cell / 7;
    double cx = 28 + cw * (col + 0.5), cy = cy0 + 26 + row * 28;
    char s[4];
    snprintf(s, sizeof s, "%d", dd);
    if (dd == today) {
      // on each open a ring draws itself round today, then the disc fills from the middle
      double e = ANIM(A_TODAY);
      double ring = ease_io(e / 0.62), fill = ease_out((e - 0.4) / 0.6);
      // the ring's outer edge is the disc's edge, so the end state is the plain disc
      cairo_new_path(cr);
      if (ring > 0) {
        cairo_arc(cr, cx, cy + 9, 12.2, -M_PI / 2, -M_PI / 2 + 2 * M_PI * ring);
        cairo_set_source_rgba(cr, C_LILAC, 1);
        cairo_set_line_width(cr, 1.6);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_stroke(cr);
      }
      if (fill > 0) {
        cairo_set_source_rgba(cr, C_LILAC, 1);
        cairo_arc(cr, cx, cy + 9, 13 * fill, 0, 2 * M_PI);
        cairo_fill(cr);
      }
      // the number turns dark as the disc fills under it
      double k = fill;
      text(cr, s, cx, cy, 15, 600, 0.925 + (0.10 - 0.925) * k, 0.878 + (0.07 - 0.878) * k,
           0.933 + (0.094 - 0.933) * k, 1, 0.5, 0);
    } else {
      text(cr, s, cx, cy, 15, 400, C_TEXT, 0.9, 0.5, 0);
    }
  }
  g_date_time_unref(first);
  // one quiet line of load
  double ly = p->h - 46;
  cairo_set_source_rgba(cr, C_TEXT, 0.09);
  cairo_rectangle(cr, 28, ly - 14, p->w - 56, 1);
  cairo_fill(cr);
  char line[160];
  if (nv.dev)
    snprintf(line, sizeof line, "cpu %.0f%%   gpu %.0f%%   ram %.1f / %.0f", sys.cpu, sys.gpu, sys.ram_used, sys.ram_total);
  else
    snprintf(line, sizeof line, "cpu %.0f%%   ram %.1f / %.0f", sys.cpu, sys.ram_used, sys.ram_total);
  text(cr, line, 28, ly, 15, 400, C_MUTED, 1, 0, p->w - 56);
  g_free(t);
  g_free(d);
  g_date_time_unref(now);
}

static void icon_bt(cairo_t *cr, const char *icon, double x, double y, double s) {
  // simple glyphs by device type: headset, mouse, keyboard, other
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, s / 24.0, s / 24.0);
  cairo_set_line_width(cr, 2);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  if (strstr(icon, "audio") || strstr(icon, "head")) {
    cairo_arc(cr, 12, 13, 8, M_PI, 2 * M_PI);
    cairo_stroke(cr);
    rrect(cr, 3, 13, 4, 7, 1.5);
    rrect(cr, 17, 13, 4, 7, 1.5);
    cairo_fill(cr);
  } else if (strstr(icon, "mouse")) {
    rrect(cr, 6, 3, 12, 18, 6);
    cairo_stroke(cr);
    cairo_move_to(cr, 12, 6);
    cairo_line_to(cr, 12, 10);
    cairo_stroke(cr);
  } else if (strstr(icon, "keyboard")) {
    rrect(cr, 2, 6, 20, 12, 2);
    cairo_stroke(cr);
    cairo_move_to(cr, 7, 14);
    cairo_line_to(cr, 17, 14);
    cairo_stroke(cr);
  } else {
    cairo_move_to(cr, 7, 8);
    cairo_line_to(cr, 17, 16);
    cairo_line_to(cr, 12, 20);
    cairo_line_to(cr, 12, 4);
    cairo_line_to(cr, 17, 8);
    cairo_line_to(cr, 7, 16);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}

static void draw_bt(Panel *p, cairo_t *cr) {
  panel_bg(cr, p->w, p->h, p->side);
  text(cr, "bluetooth", 28, 22, 14, 400, C_FAINT, 1, 0, 0);
  // power switch
  double sx = p->w - 28 - 46, sy = 18;
  double sh = HOV(H_BTPOW), sp = PRS(H_BTPOW);
  if (bt.powered) halo(cr, sx + 33, sy + 13, 12, 10, 0.16 + 0.14 * sh);
  rrect(cr, sx, sy, 46, 26, 13);
  if (bt.powered) cairo_set_source_rgba(cr, C_LILAC, 0.35 + 0.12 * sh);
  else cairo_set_source_rgba(cr, C_TEXT, 0.12 + 0.08 * sh);
  cairo_fill(cr);
  if (sh > 0.002) {
    rrect(cr, sx - 1.5, sy - 1.5, 49, 29, 14.5);
    cairo_set_source_rgba(cr, C_LILAC, 0.45 * sh);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
  }
  // the knob grows under the pointer and squashes when pressed
  double kr = 10 + 1.2 * sh - 1.5 * sp;
  cairo_save(cr);
  cairo_translate(cr, sx + (bt.powered ? 33 : 13), sy + 13);
  cairo_scale(cr, 1 + 0.18 * sp, 1);
  cairo_arc(cr, 0, 0, kr, 0, 2 * M_PI);
  cairo_restore(cr);
  if (bt.powered) cairo_set_source_rgba(cr, C_LILAC, 1);
  else mix_rgba(cr, C_MUTED, C_TEXT, sh, 1);
  cairo_fill(cr);
  hit_add(p, H_BTPOW, sx - 6, sy - 6, 58, 38);
  if (!bt.have) {
    text(cr, "no adapter", 28, 64, 15, 400, C_FAINT, 1, 0, 0);
    return;
  }
  double y = 60;
  double now = now_s();
  for (int i = 0; i < bt.n && y + 56 <= p->h - 10; i++) {
    BtDev *d = &bt.d[i];
    double a = bt.powered ? 1 : 0.3;
    // the row lights up under the pointer and dips when pressed
    double rh = HOV(H_BTDEV0 + i) * (bt.powered ? 1 : 0.4), rp = PRS(H_BTDEV0 + i);
    if (rh + rp > 0.002) {
      rrect(cr, 16, y + 1, p->w - 32, 50, 14);
      cairo_set_source_rgba(cr, C_TEXT, 0.045 * rh + 0.05 * rp);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, C_LILAC, 0.22 * rh);
      cairo_set_line_width(cr, 1);
      cairo_stroke(cr);
    }
    // icon tile; breathes while connecting
    double breathe = d->busy ? 0.5 + 0.5 * sin(now * 4.5) : 0;
    rrect(cr, 28, y + 6, 40, 40, 12);
    if (d->connected) cairo_set_source_rgba(cr, C_LILAC, (0.18 + 0.14 * breathe + 0.08 * rh) * a);
    else cairo_set_source_rgba(cr, C_TEXT, (0.06 + 0.12 * breathe + 0.06 * rh) * a);
    cairo_fill(cr);
    if (d->connected || d->busy) cairo_set_source_rgba(cr, C_LILAC, a);
    else mix_rgba(cr, C_MUTED, C_LILAC, rh, a);
    icon_bt(cr, d->icon, 38, y + 16, 20);
    text(cr, d->name, 82, y + 8, 16, 500, C_TEXT, a, 0, p->w - 82 - 90);
    const char *st = d->busy ? (d->connected ? "disconnecting…" : "connecting…")
                     : now < d->err_until ? "couldn't connect"
                     : d->connected ? "connected" : "not connected";
    if (d->busy) text(cr, st, 82, y + 28, 13.5, 400, C_LILAC, a, 0, 0);
    else if (now < d->err_until) text(cr, st, 82, y + 28, 13.5, 400, C_WINE, a, 0, 0);
    else text(cr, st, 82, y + 28, 13.5, 400, d->connected ? 0.604 : 0.435, d->connected ? 0.541 : 0.376,
              d->connected ? 0.596 : 0.427, a, 0, 0);
    if (d->battery >= 0) {
      char b[16];
      snprintf(b, sizeof b, "%d%%", d->battery);
      double bx = p->w - 28 - 28;
      cairo_set_source_rgba(cr, C_MUTED, a);
      cairo_set_line_width(cr, 1.5);
      rrect(cr, bx, y + 20, 26, 12, 3);
      cairo_stroke(cr);
      cairo_set_source_rgba(cr, C_LILAC, a);
      cairo_rectangle(cr, bx + 2, y + 22, 22 * d->battery / 100.0, 8);
      cairo_fill(cr);
      text(cr, b, bx - 8, y + 17, 14, 400, C_MUTED, a, 1, 0);
    }
    hit_add(p, H_BTDEV0 + i, 18, y, p->w - 36, 52);
    y += 58;
  }
}

// A round button. hov/prs (0..1): under the pointer it grows a little and lights up, a press
// pushes it in; `lit` is an active state (the main button while playing) with a quiet halo.
static void draw_btn(cairo_t *cr, double cx, double cy, double r, int kind, gboolean main, gboolean on,
                     double hov, double prs, double lit) {
  double a = on ? 1 : 0.28;
  if (!on) hov = prs = 0;
  r *= 1 + 0.07 * hov - 0.09 * prs;
  if (main) halo(cr, cx, cy, r, 12 + 4 * hov, 0.18 * lit + 0.22 * hov);
  cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
  if (main) mix_rgba(cr, C_LILAC, C_TEXT, 0.35 * hov, a);
  else cairo_set_source_rgba(cr, C_TEXT, 0.05 * a + 0.02 + 0.09 * hov + 0.06 * prs);
  cairo_fill(cr);
  if (!main && hov > 0.002) {
    cairo_arc(cr, cx, cy, r - 0.6, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, C_LILAC, 0.40 * hov);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);
  }
  if (main) cairo_set_source_rgba(cr, 0.10, 0.07, 0.094, 1);
  else mix_rgba(cr, C_TEXT, C_LILAC, hov, a);
  double s = r * 0.42;
  if (kind == 0) {  // previous
    cairo_rectangle(cr, cx - s, cy - s, s * 0.28, 2 * s);
    cairo_move_to(cr, cx + s, cy - s);
    cairo_line_to(cr, cx - s * 0.55, cy);
    cairo_line_to(cr, cx + s, cy + s);
  } else if (kind == 2) {  // next
    cairo_rectangle(cr, cx + s * 0.72, cy - s, s * 0.28, 2 * s);
    cairo_move_to(cr, cx - s, cy - s);
    cairo_line_to(cr, cx + s * 0.55, cy);
    cairo_line_to(cr, cx - s, cy + s);
  } else if (kind == 1) {  // pause
    cairo_rectangle(cr, cx - s * 0.8, cy - s, s * 0.6, 2 * s);
    cairo_rectangle(cr, cx + s * 0.2, cy - s, s * 0.6, 2 * s);
  } else {  // play
    cairo_move_to(cr, cx - s * 0.6, cy - s);
    cairo_line_to(cr, cx + s, cy);
    cairo_line_to(cr, cx - s * 0.6, cy + s);
  }
  cairo_close_path(cr);
  cairo_fill(cr);
}

static void fmt_time(char *b, size_t n, gint64 us) {
  int s = (int)(us / 1000000);
  snprintf(b, n, "%d:%02d", s / 60, s % 60);
}

static gint64 player_pos_now(void) {
  gint64 p = pl.pos_us;
  if (pl.playing) p += (gint64)((now_s() - pl.pos_at) * 1e6);
  if (pl.length_us > 0 && p > pl.length_us) p = pl.length_us;
  return p;
}

// The track before the last change, and the cover before the last one, for A_TRACK / A_ART.
static char trk_old_title[256], trk_old_artist[256], trk_old_src[48];
static cairo_surface_t *art_old;
static gboolean art_old_set;  // art_old is meaningful (NULL there means the placeholder)

// One cover (NULL: the placeholder) inside the frame, zoomed about its middle, at alpha a.
static void art_img(cairo_t *cr, cairo_surface_t *img, double x, double y, double s, double zoom, double a) {
  if (a <= 0.002) return;
  cairo_save(cr);
  rrect(cr, x, y, s, s, 14);
  cairo_clip(cr);
  cairo_translate(cr, x + s / 2, y + s / 2);
  cairo_scale(cr, zoom, zoom);
  cairo_translate(cr, -s / 2, -s / 2);
  if (img) {
    int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
    cairo_scale(cr, s / w, s / h);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint_with_alpha(cr, a);
  } else {
    cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, s, s);
    cairo_pattern_add_color_stop_rgb(g, 0, 0.353, 0.227, 0.322);
    cairo_pattern_add_color_stop_rgb(g, 0.6, C_WINE);
    cairo_pattern_add_color_stop_rgb(g, 1, C_LILAC);
    cairo_set_source(cr, g);
    cairo_paint_with_alpha(cr, a);
    cairo_pattern_destroy(g);
  }
  cairo_restore(cr);
}

// The cover. A new one fades in over the old with a slight zoom (1.06 -> 1); the old one
// recedes a little (1 -> 0.96) as it goes. Covers are pictures, so they may cross; text never.
static void draw_art(cairo_t *cr, double x, double y, double s) {
  double e = ease_io(ANIM(A_ART));
  if (e < 1 && art_old_set) {
    art_img(cr, art_old, x, y, s, 1 - 0.04 * e, 1);
    art_img(cr, pl.art, x, y, s, 1.06 - 0.06 * e, e);
  } else {
    art_img(cr, pl.art, x, y, s, 1, 1);
  }
}

// Title and artist. On a track change the old pair slides left and fades out, and only then
// does the new pair come in from the right: the two never share a frame. Each line is
// clipped to its column [x0, x1], so nothing slides over the cover or out of the panel.
// Where the track change is for text: the old text (*old) leaving, nothing, or the new one
// arriving, with its x offset and alpha. FALSE when nothing is shown this frame.
static gboolean track_phase(gboolean *old, double *off, double *a) {
  double e = ANIM(A_TRACK), dx = 12;
  *old = FALSE;
  *off = 0;
  *a = 1;
  if (e < 1) {
    if (e < 0.42) {  // the old pair leaves
      double u = ease_in(e / 0.42);
      *old = TRUE;
      *off = -dx * u;
      *a = 1 - u;
    } else if (e < 0.5) {  // a beat of nothing
      return FALSE;
    } else {  // the new pair arrives
      double v = ease_out((e - 0.5) / 0.5);
      *off = dx * (1 - v);
      *a = v;
    }
  }
  return *a > 0.002;
}

static void track_lines(cairo_t *cr, const char *title, const char *artist, double x0, double x1, double ty,
                        double ts, double ay, double as, double align) {
  gboolean old;
  double off, a;
  if (!track_phase(&old, &off, &a)) return;
  const char *t = old ? (trk_old_title[0] ? trk_old_title : "—") : title;
  const char *ar = old ? trk_old_artist : artist;
  double w = x1 - x0, ax = x0 + w * align;
  cairo_save(cr);
  cairo_rectangle(cr, x0, ty - 4, w, ay + as * 1.6 - ty + 8);
  cairo_clip(cr);
  text(cr, t, ax + off, ty, ts, 600, C_TEXT, a, align, w);
  text(cr, ar, ax + off, ay, as, 400, C_MUTED, a, align, w);
  cairo_restore(cr);
}

// The sources as dots, right-aligned at x1 around cy: the shown one is a lilac pill that
// travels like a drop (its leading edge first) on a switch; one that plays is a little
// brighter. Each dot is a hit (H_PDOT0 + i) when hits is set. Returns the dots' left edge.
#define PDOT_STEP 14
static double draw_pdots(Panel *p, cairo_t *cr, double x1, double cy, gboolean hits) {
  int n = nplsrc;
  double x0 = x1 - PDOT_STEP * n + 2;  // dot i is centred at x0 + 3 + PDOT_STEP * i
  for (int i = 0; i < n; i++) {
    double cx = x0 + 3 + PDOT_STEP * i, hv = HOV(H_PDOT0 + i);
    cairo_arc(cr, cx, cy, 3 + 0.6 * hv, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, C_TEXT, (plsrc[i].playing ? 0.42 : 0.2) + 0.3 * hv);
    cairo_fill(cr);
    if (hits) hit_add(p, H_PDOT0 + i, cx - PDOT_STEP / 2.0, cy - 12, PDOT_STEP, 24);
  }
  if (plsel >= 0 && plsel < n) {
    double e = ANIM(A_PSW);
    int from = plsel_old >= 0 && plsel_old < n ? plsel_old : plsel;
    double a = x0 + PDOT_STEP * from, b = x0 + PDOT_STEP * plsel;
    // the leading edge runs ahead, the trailing one follows
    double lead = ease_out(e), trail = ease_io(e);
    double l = b >= a ? a + (b - a) * trail : a + (b - a) * lead;
    double r = (b >= a ? a + (b - a) * lead : a + (b - a) * trail) + 6;
    double stretch = (r - l - 6) / PDOT_STEP, th = 6 - 1.2 * fmin(1, stretch);
    rrect(cr, l - 3, cy - th / 2, r - l + 6, th, th / 2);
    cairo_set_source_rgba(cr, C_LILAC, 1);
    cairo_fill(cr);
  }
  return x0;
}

// The source's name, leaving and arriving with the track text, right-aligned at x1.
static void draw_psrc_name(cairo_t *cr, double x0, double x1, double y, double size, double hv) {
  gboolean old;
  double off, a;
  if (!track_phase(&old, &off, &a)) return;
  const char *nm = old ? trk_old_src : pl.name;
  if (!nm[0]) return;
  cairo_save(cr);
  cairo_rectangle(cr, x0, y - 4, x1 - x0, size * 1.6 + 8);
  cairo_clip(cr);
  // faint, brightening to the text colour on hover
  const double f[3] = {C_FAINT}, t[3] = {C_TEXT};
  text(cr, nm, x1 + off, y, size, 400, f[0] + (t[0] - f[0]) * hv, f[1] + (t[1] - f[1]) * hv,
       f[2] + (t[2] - f[2]) * hv, a, 1, x1 - x0);
  cairo_restore(cr);
}

static void draw_player(Out *o, Panel *p, cairo_t *cr) {
  panel_bg(cr, p->w, p->h, p->side);
  text(cr, "now playing", 28, 22, 14, 400, C_FAINT, 1, 0, 0);
  // more than one source: its name and the dots, to switch (or the wheel over the track)
  if (nplsrc > 1) {
    double dx0 = draw_pdots(p, cr, p->w - 28, 31, TRUE);
    double nx1 = dx0 - 12, nx0 = 28 + 100;
    draw_psrc_name(cr, nx0, nx1, 22, 14, HOV(H_PSRC));
    hit_add(p, H_PSRC, nx0, 12, nx1 - nx0 + 6, 32);
  }
  draw_art(cr, 28, 48, 78);
  track_lines(cr, pl.title[0] ? pl.title : "—", pl.artist, 122, p->w - 28, 58, 21, 88, 16, 0);
  double y = 146;
  if (pl.can_seek && pl.length_us > 0) {
    double frac = o->dragging && o->press_hit == H_SEEK ? o->drag_frac
                  : (double)player_pos_now() / pl.length_us;
    double bw = p->w - 56;
    double sk = HOV(H_SEEK);
    double th = o->dragging && o->press_hit == H_SEEK ? 5 : 3 + 2 * sk;
    rrect(cr, 28, y + 10 - th / 2, bw, th, th / 2);
    cairo_set_source_rgba(cr, C_TEXT, 0.10);
    cairo_fill(cr);
    if (o->dragging && o->press_hit == H_SEEK) {
      double real = (double)player_pos_now() / pl.length_us;
      rrect(cr, 28, y + 10 - th / 2, bw * clamp01(real), th, th / 2);
      cairo_set_source_rgba(cr, C_LILAC, 1);
      cairo_fill(cr);
      rrect(cr, 28, y + 10 - th / 2, bw * clamp01(frac), th, th / 2);
      cairo_set_source_rgba(cr, C_LILAC, 0.35);
      cairo_fill(cr);
      cairo_arc(cr, 28 + bw * clamp01(frac), y + 10, 8.5, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, C_TEXT, 1);
      cairo_fill(cr);
    } else {
      rrect(cr, 28, y + 10 - th / 2, bw * clamp01(frac), th, th / 2);
      cairo_set_source_rgba(cr, C_LILAC, 1);
      cairo_fill(cr);
      if (sk > 0.002) {
        halo(cr, 28 + bw * clamp01(frac), y + 10, 6.5 * sk, 6, 0.25 * sk);
        cairo_arc(cr, 28 + bw * clamp01(frac), y + 10, 6.5 * sk, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, C_TEXT, sk);
        cairo_fill(cr);
      }
    }
    hit_add(p, H_SEEK, 20, y - 4, bw + 16, 28);
    char a[16], b[16];
    fmt_time(a, sizeof a, (gint64)(frac * pl.length_us));
    fmt_time(b, sizeof b, pl.length_us);
    text(cr, a, 28, y + 22, 13, 400, C_FAINT, 1, 0, 0);
    text(cr, b, p->w - 28, y + 22, 13, 400, C_FAINT, 1, 1, 0);
  } else if (pl.title[0]) {
    double pulse = 0.55 + 0.45 * sin(now_s() * 3.9);
    cairo_arc(cr, 32, y + 12, 4, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, C_WINE, pulse);
    cairo_fill(cr);
    text(cr, "live · this source can't be rewound", 46, y + 3, 14, 400, C_MUTED, 1, 0, 0);
  }
  // transport
  double cy = y + 74, cx = p->w / 2;
  draw_btn(cr, cx - 66, cy, 22, 0, FALSE, pl.can_prev, HOV(H_PREV), PRS(H_PREV), 0);
  draw_btn(cr, cx, cy, 27, pl.playing ? 1 : 3, TRUE, pl.playing ? pl.can_pause : pl.can_play, HOV(H_PLAY),
           PRS(H_PLAY), pl.playing);
  draw_btn(cr, cx + 66, cy, 22, 2, FALSE, pl.can_next, HOV(H_NEXT), PRS(H_NEXT), 0);
  if (pl.can_prev) hit_add(p, H_PREV, cx - 90, cy - 24, 48, 48);
  if (pl.playing ? pl.can_pause : pl.can_play) hit_add(p, H_PLAY, cx - 31, cy - 31, 62, 62);
  if (pl.can_next) hit_add(p, H_NEXT, cx + 42, cy - 24, 48, 48);
  // volume
  double vy = cy + 50;
  cairo_set_source_rgba(cr, C_TEXT, 0.09);
  cairo_rectangle(cr, 28, vy, p->w - 56, 1);
  cairo_fill(cr);
  vy += 22;
  double v = vol.pending >= 0 ? vol.pending : vol.v;
  double mh = HOV(H_MUTE), mp = PRS(H_MUTE);
  if (mh + mp > 0.002) {
    cairo_arc(cr, 37, vy, 15 - 1.5 * mp, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, C_TEXT, 0.08 * mh + 0.06 * mp);
    cairo_fill(cr);
  }
  mix_rgba(cr, C_TEXT, C_LILAC, mh, vol.muted ? 0.45 + 0.4 * mh : 1);
  cairo_move_to(cr, 30, vy - 5);
  cairo_line_to(cr, 35, vy - 5);
  cairo_line_to(cr, 41, vy - 10);
  cairo_line_to(cr, 41, vy + 10);
  cairo_line_to(cr, 35, vy + 5);
  cairo_line_to(cr, 30, vy + 5);
  cairo_close_path(cr);
  cairo_fill(cr);
  if (!vol.muted) {
    cairo_set_line_width(cr, 1.8);
    cairo_arc(cr, 41, vy, 6, -0.9, 0.9);
    cairo_stroke(cr);
  }
  hit_add(p, H_MUTE, 20, vy - 17, 34, 34);
  double sx = 64, sw = p->w - 64 - 28 - 58;
  double frac = clamp01(v / 1.5), base = clamp01(fmin(v, 1.0) / 1.5);
  double vh = HOV(H_VOL);
  gboolean vdrag = o->dragging && o->press_hit == H_VOL;
  double th = vdrag ? 6 : 4 + 2 * vh;
  rrect(cr, sx, vy - th / 2, sw, th, th / 2);
  cairo_set_source_rgba(cr, C_TEXT, 0.10);
  cairo_fill(cr);
  rrect(cr, sx, vy - th / 2, sw * base, th, th / 2);
  if (vol.muted) cairo_set_source_rgba(cr, C_FAINT, 1);
  else cairo_set_source_rgba(cr, C_LILAC, 1);
  cairo_fill(cr);
  if (frac > base) {
    cairo_rectangle(cr, sx + sw * base, vy - th / 2, sw * (frac - base), th);
    cairo_set_source_rgba(cr, C_WINE, 1);
    cairo_fill(cr);
  }
  cairo_set_source_rgba(cr, C_TEXT, 0.25);
  cairo_rectangle(cr, sx + sw * (1 / 1.5), vy - 6, 1, 12);
  cairo_fill(cr);
  double kr = vdrag ? 8.5 : 6.5 + 2 * vh;
  halo(cr, sx + sw * frac, vy, kr, 7, vdrag ? 0.3 : 0.22 * vh);
  cairo_arc(cr, sx + sw * frac, vy, kr, 0, 2 * M_PI);
  cairo_set_source_rgba(cr, C_TEXT, 1);
  cairo_fill(cr);
  char vs[16];
  if (vol.muted) snprintf(vs, sizeof vs, "muted");
  else snprintf(vs, sizeof vs, "%.0f%%", v * 100);
  text(cr, vs, p->w - 28, vy - 10, 15, 400, C_MUTED, 1, 1, 0);
  hit_add(p, H_VOL, sx - 10, vy - 16, sw + 20, 32);
}

// One notification row at y (its slot is 84 px). hit: its index in notes[], or -1 for a row
// that is going away; a: its opacity.
static void note_row(Panel *p, cairo_t *cr, const Note *n, double y, double a, int hit, gboolean sep) {
  if (sep) {
    cairo_set_source_rgba(cr, C_TEXT, 0.09 * a);
    cairo_rectangle(cr, 28, y - 10, p->w - 56, 1);
    cairo_fill(cr);
  }
  if (n->ago[0]) {
    char head[128];
    snprintf(head, sizeof head, "%s  ·  %s", n->app, n->ago);
    text(cr, head, 28, y, 13, 400, C_FAINT, a, 0, p->w - 100);
  } else {
    text(cr, n->app, 28, y, 13, 400, C_FAINT, a, 0, p->w - 100);
  }
  if (n->critical) text(cr, n->summary, 28, y + 18, 16, 500, C_WINE, a, 0, p->w - 90);
  else text(cr, n->summary, 28, y + 18, 16, 500, C_TEXT, a, 0, p->w - 90);
  text(cr, n->body, 28, y + 40, 15, 400, C_MUTED, a, 0, p->w - 56);
  // ×
  double xx = p->w - 28 - 13, xy = y + 12;
  double nh = hit >= 0 ? HOV(H_NOTE0 + hit) : 0, np = hit >= 0 ? PRS(H_NOTE0 + hit) : 0;
  cairo_arc(cr, xx, xy, 13 + 1.5 * nh - 2 * np, 0, 2 * M_PI);
  mix_rgba(cr, C_TEXT, C_WINE, nh, (0.07 + 0.13 * nh) * a);
  cairo_fill(cr);
  mix_rgba(cr, C_MUTED, C_TEXT, nh, a);
  cairo_set_line_width(cr, 1.6);
  cairo_move_to(cr, xx - 4.5, xy - 4.5);
  cairo_line_to(cr, xx + 4.5, xy + 4.5);
  cairo_move_to(cr, xx + 4.5, xy - 4.5);
  cairo_line_to(cr, xx - 4.5, xy + 4.5);
  cairo_stroke(cr);
  if (hit >= 0) hit_add(p, H_NOTE0 + hit, xx - 17, xy - 17, 34, 34);
}

// The list before the last change, for A_NOTES.
static Note notes_old[NNOTE];
static int nnotes_old;

// Before the list changes: what's on screen becomes the old list. (A change in the middle of
// a change starts from the newer list; rows move at most a slot.)
static Note notes_pre[NNOTE];
static int nnotes_pre;
static void notes_snapshot(void) {
  memcpy(notes_pre, notes, sizeof notes);
  nnotes_pre = nnotes;
}
// After: animate only if the rows themselves changed (not a "5 min ago"), and only while the
// HUD is up and has already shown the old list (not on the read at open).
static void notes_changed(void) {
  gboolean same = nnotes == nnotes_pre;
  for (int i = 0; same && i < nnotes; i++) same = notes[i].id == notes_pre[i].id;
  if (same) return;
  gboolean ready = FALSE;
  for (guint i = 0; outs && i < outs->len; i++) {
    Out *o = g_ptr_array_index(outs, i);
    ready |= o->shown && now_s() - o->open_t > 0.3;
  }
  if (!ready) return;
  memcpy(notes_old, notes_pre, sizeof notes_pre);
  nnotes_old = nnotes_pre;
  anim_start(A_NOTES, 0);
}

static void draw_notes(Panel *p, cairo_t *cr) {
  panel_bg(cr, p->w, p->h, p->side);
  text(cr, "notifications", 28, 22, 14, 400, C_FAINT, 1, 0, 0);
  double e = ease_io(ANIM(A_NOTES));
  // Rows in order: the current list, with the rows that went away put back where they were.
  // Each row's slot has a height factor: 1 if it stays, growing for a new row (the list
  // parts to make room), shrinking for a gone one (the list closes up).
  struct { const Note *n; int hit; double hf; } rows[2 * NNOTE];
  int nr = 0;
  for (int i = 0; i < nnotes; i++) {
    gboolean was = FALSE;
    for (int j = 0; j < nnotes_old && e < 1; j++) was |= notes_old[j].id == notes[i].id;
    rows[nr++] = (typeof(rows[0])){&notes[i], i, e < 1 && !was ? e : 1};
  }
  for (int j = 0; j < nnotes_old && e < 1; j++) {
    gboolean stays = FALSE;
    for (int i = 0; i < nnotes; i++) stays |= notes[i].id == notes_old[j].id;
    if (stays) continue;
    // after the row that came before it in the old list (or first)
    int at = 0;
    for (int k = 0; k < nr; k++)
      if (j > 0 && rows[k].n->id == notes_old[j - 1].id) at = k + 1;
    memmove(&rows[at + 1], &rows[at], sizeof rows[0] * (nr - at));
    rows[at] = (typeof(rows[0])){&notes_old[j], -1, 1 - e};
    nr++;
  }
  double sum = 0;
  for (int k = 0; k < nr; k++) sum += rows[k].hf;
  // "nothing new" shows only while the rows take (almost) no room, so the two never cross
  double na = clamp01(1 - sum / 0.25);
  if (na > 0.002) text(cr, "nothing new", 28, 56, 15, 400, C_FAINT, na, 0, 0);
  double y = 54;
  for (int k = 0; k < nr; k++) {
    double hf = rows[k].hf;
    if (hf <= 0.002) continue;
    // A row's text shows only once its slot is nearly open, and is gone before the slot
    // closes: no cut lines of text are ever seen, only the list making or closing room.
    double a = clamp01((hf - 0.75) / 0.25);
    cairo_save(cr);
    cairo_new_path(cr);
    // the slot, and never below the panel's padding
    double y1 = fmin(y - 12 + 84 * hf + 2, p->h - 8);
    if (y1 > y - 12) {
      cairo_rectangle(cr, 0, y - 12, p->w, y1 - (y - 12));
      cairo_clip(cr);
      if (a > 0.002) note_row(p, cr, rows[k].n, y, a, rows[k].hit, k > 0);
    }
    cairo_restore(cr);
    y += 84 * hf;
  }
}

static void draw_switch(Out *o, Panel *p, cairo_t *cr) {
  rrect(cr, 0.5, 0.5, p->w - 1, p->h - 1, p->h / 2);
  cairo_set_source_rgba(cr, 0.094, 0.067, 0.094, 0.72);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, C_TEXT, 0.09);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  double half = (p->w - 8) / 2;
  // The pill moves like a drop: its leading edge runs ahead, the trailing one follows later,
  // so it stretches on the way and gathers itself at the end; it thins a little while long.
  double u = clamp01((now_s() - o->sys_t) / 0.4);
  double lead = ease_out(u / 0.7), trail = ease_io(u);
  double l, r;
  if (o->sys_mode) {  // to the right
    r = 4 + half + half * lead;
    l = 4 + half * trail;
  } else {
    l = 4 + half - half * lead;
    r = 4 + 2 * half - half * trail;
  }
  double stretch = (r - l) / half - 1, ph = (p->h - 8) * (1 - 0.12 * stretch);
  rrect(cr, l, p->h / 2 - ph / 2, r - l, ph, ph / 2);
  cairo_set_source_rgba(cr, C_LILAC, 1);
  cairo_fill(cr);
  gboolean s = o->sys_mode;
  // the inactive segment under the pointer: a faint pill and brighter text
  double hw = s ? HOV(H_SW_WS) : HOV(H_SW_SYS), hp = s ? PRS(H_SW_WS) : PRS(H_SW_SYS);
  if (hw + hp > 0.002) {
    rrect(cr, 4 + (s ? 0 : half) + 2 * hp, 4 + hp, half - 4 * hp, p->h - 8 - 2 * hp, (p->h - 8) / 2);
    cairo_set_source_rgba(cr, C_TEXT, 0.07 * hw + 0.05 * hp);
    cairo_fill(cr);
  }
  // the inactive label: muted, towards the text colour under the pointer
  double ir = 0.604 + (0.925 - 0.604) * hw, ig = 0.541 + (0.878 - 0.541) * hw, ib = 0.596 + (0.933 - 0.596) * hw;
  text(cr, "workspaces", 4 + half / 2, p->h / 2 - 10, 15, 500, s ? ir : 0.10, s ? ig : 0.07, s ? ib : 0.094, 1,
       0.5, 0);
  text(cr, "system", 4 + half * 1.5, p->h / 2 - 10, 15, 500, s ? 0.10 : ir, s ? 0.07 : ig, s ? 0.094 : ib, 1,
       0.5, 0);
  hit_add(p, H_SW_WS, 0, 0, p->w / 2, p->h);
  hit_add(p, H_SW_SYS, p->w / 2, 0, p->w / 2, p->h);
}

static void draw_graph2(cairo_t *cr, double x, double y, double w, double h, const char *name,
                        const char *val, Series *s, Series *s2, double max);
static void draw_graph(cairo_t *cr, double x, double y, double w, double h, const char *name,
                       const char *val, Series *s, double max) {
  draw_graph2(cr, x, y, w, h, name, val, s, NULL, max);
}

// A rate in bytes per second, short: "0 kb/s", "340 kb/s", "1.2 mb/s", "86 mb/s".
static void fmt_rate(char *buf, size_t n, double bps) {
  if (bps < 1e6) snprintf(buf, n, "%.0f kb/s", bps / 1e3);
  else if (bps < 1e8) snprintf(buf, n, "%.1f mb/s", bps / 1e6);
  else snprintf(buf, n, "%.0f mb/s", bps / 1e6);
}

// The graph tiles' entrance when system mode comes on (A_DASH): tile k rises 18 px and fades
// in 40 ms after the one before it, its lines draw from left to right, its number counts up.
#define TILE_STEP 0.035
#define TILE_DUR 0.34
static void dash_tile(int k, double *dy, double *a, double *reveal, double *count) {
  double t = ANIM(A_DASH) * anim_dur(A_DASH);
  double lt = clamp01((t / anim_slow - k * TILE_STEP) / TILE_DUR);
  *dy = 12 * (1 - ease_out(lt));
  *a = ease_out(lt / 0.7);
  *reveal = ease_io(clamp01((lt - 0.1) / 0.9));
  *count = ease_out(lt);
}
static int graph_k;  // which tile draw_graph2 is drawing

static void draw_graph2(cairo_t *cr, double x, double y, double w, double h, const char *name,
                        const char *val, Series *s, Series *s2, double max) {
  double dy, ta, reveal, count;
  dash_tile(graph_k++, &dy, &ta, &reveal, &count);
  (void)count;
  if (ta <= 0.002) return;
  gboolean group = ta < 1;
  // the group covers this tile only (and its rise), not the whole panel
  cairo_save(cr);
  cairo_rectangle(cr, x - 2, y - 2, w + 4, h + 24);
  cairo_clip(cr);
  if (group) cairo_push_group(cr);
  cairo_save(cr);
  cairo_translate(cr, 0, dy);
  rrect(cr, x + 0.5, y + 0.5, w - 1, h - 1, 20);
  cairo_set_source_rgba(cr, 0.094, 0.067, 0.094, 0.62);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, C_TEXT, 0.09);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  text(cr, name, x + 20, y + 22, 15, 400, C_MUTED, 1, 0, 0);
  text(cr, val, x + w - 20, y + 12, 30, 300, C_TEXT, 1, 1, 0);
  double gx = x + 16, gy = y + 66, gw = w - 32, gh = h - 82;
  cairo_set_source_rgba(cr, C_TEXT, 0.06);
  for (int k = 1; k < 4; k++) {
    cairo_rectangle(cr, gx, gy + gh * k / 4, gw, 1);
    cairo_fill(cr);
  }
  double step = gw / (HIST - 1);
  if (reveal <= 0.002) goto done;
  cairo_save(cr);
  if (reveal < 1) {
    cairo_rectangle(cr, gx - 2, gy - 4, (gw + 2) * reveal + 2, gh + 8);
    cairo_clip(cr);
  }
  cairo_new_path(cr);
  for (int i = 0; i < HIST; i++) {
    double v = fmin(s->v[i] / max, 1.0);
    double px = gx + i * step, py = gy + gh - v * gh * 0.92;
    if (i) cairo_line_to(cr, px, py);
    else cairo_move_to(cr, px, py);
  }
  cairo_path_t *line = cairo_copy_path(cr);
  cairo_line_to(cr, gx + gw, gy + gh);
  cairo_line_to(cr, gx, gy + gh);
  cairo_close_path(cr);
  cairo_pattern_t *g = cairo_pattern_create_linear(0, gy, 0, gy + gh);
  cairo_pattern_add_color_stop_rgba(g, 0, C_WINE, 0.30);
  cairo_pattern_add_color_stop_rgba(g, 1, C_WINE, 0);
  cairo_set_source(cr, g);
  cairo_fill(cr);
  cairo_pattern_destroy(g);
  cairo_new_path(cr);
  cairo_append_path(cr, line);
  cairo_path_destroy(line);
  cairo_set_source_rgba(cr, C_LILAC, 1);
  cairo_set_line_width(cr, 2);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);
  if (s2) {  // a second, quieter line (upload under download)
    cairo_new_path(cr);
    for (int i = 0; i < HIST; i++) {
      double v = fmin(s2->v[i] / max, 1.0);
      double px = gx + i * step, py = gy + gh - v * gh * 0.92;
      if (i) cairo_line_to(cr, px, py);
      else cairo_move_to(cr, px, py);
    }
    cairo_set_source_rgba(cr, C_WINE, 0.9);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
done:
  cairo_restore(cr);
  if (group) {
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, ta);
  }
  cairo_restore(cr);
}

static void draw_dash(Out *o, Panel *p, cairo_t *cr) {
  // four graph tiles need room; a narrow grid gets the compact view
  gboolean compact = o->portrait || p->w < 1000;
  double gap = 18, top = 0;
  char v[64];
  // the numbers count up with each tile's entrance (A_DASH)
  double c[4], d0, d1, d2;
  for (int k = 0; k < 4; k++) dash_tile(k, &d0, &d1, &d2, &c[k]);
  graph_k = 0;
  if (compact) {
    snprintf(v, sizeof v, "%.1f / %.0f gb", sys.ram_used * c[0], sys.ram_total);
    draw_graph(cr, 0, 0, p->w, 200, "memory", v, &sys.s_ram, fmax(sys.ram_total, 1));
    top = 200 + gap;
  } else {
    double tw = (p->w - 3 * gap) / 4, th = 250;
    snprintf(v, sizeof v, "%.0f%%", sys.cpu * c[0]);
    draw_graph(cr, 0, 0, tw, th, "cpu", v, &sys.s_cpu, 100);
    if (nv.dev) snprintf(v, sizeof v, "%.0f%% · %.1f gb", sys.gpu * c[1], sys.vram * c[1]);
    else snprintf(v, sizeof v, "—");
    draw_graph(cr, (tw + gap), 0, tw, th, "gpu", v, &sys.s_gpu, 100);
    snprintf(v, sizeof v, "%.1f / %.0f gb", sys.ram_used * c[2], sys.ram_total);
    draw_graph(cr, 2 * (tw + gap), 0, tw, th, "memory", v, &sys.s_ram, fmax(sys.ram_total, 1));
    // download is the big number and the lilac line, upload the name's note and the wine line
    char up[32], name[64], down[80];
    fmt_rate(v, sizeof v, sys.net_down * c[3]);
    fmt_rate(up, sizeof up, sys.net_up);
    snprintf(name, sizeof name, "network · ↑ %s", up);
    snprintf(down, sizeof down, "↓ %s", v);
    double nmax = 64e3;  // the scale never goes below 64 kb/s, so idle chatter stays flat
    for (int i = 0; i < HIST; i++) nmax = fmax(nmax, fmax(sys.s_net.v[i], sys.s_up.v[i]) * 1.2);
    draw_graph2(cr, 3 * (tw + gap), 0, tw, th, name, down, &sys.s_net, &sys.s_up, nmax);
    top = th + gap;
  }
  // processes
  double h = p->h - top;
  rrect(cr, 0.5, top + 0.5, p->w - 1, h - 1, 20);
  cairo_set_source_rgba(cr, 0.094, 0.067, 0.094, 0.62);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, C_TEXT, 0.09);
  cairo_stroke(cr);
  double x0 = 22, y = top + 18, wn = p->w - 44;
  double cpu_x = x0 + wn * (compact ? 0.52 : 0.46), bar_x = x0 + wn * 0.58, mem_x = x0 + wn - 72;
  text(cr, "process", x0, y, 13, 400, C_FAINT, 1, 0, 0);
  text(cr, "cpu", cpu_x, y, 13, 400, C_FAINT, 1, 1, 0);
  text(cr, "memory", compact ? mem_x - 6 : bar_x, y, 13, 400, C_FAINT, 1, compact ? 1 : 0, 0);
  y += 28;
  double maxr = nprocs ? fmax(procs[0].rss_gb, 0.01) : 1;
  double now = now_s();
  for (int i = 0; i < nprocs && y + 40 < top + h; i++) {
    Proc *pr = &procs[i];
    cairo_set_source_rgba(cr, C_TEXT, 0.09);
    cairo_rectangle(cr, x0, y - 6, wn, 1);
    cairo_fill(cr);
    char pid[16], c[16], m[16];
    snprintf(pid, sizeof pid, "%d", pr->pid);
    double nw = text(cr, pr->name, x0, y + 4, 15, 500, C_TEXT, 1, 0, cpu_x - x0 - 90);
    text(cr, pid, x0 + nw + 8, y + 6, 13, 400, C_FAINT, 1, 0, 0);
    snprintf(c, sizeof c, "%.1f%%", pr->cpu);
    text(cr, c, cpu_x, y + 4, 15, 400, C_MUTED, 1, 1, 0);
    if (!compact) {
      double bw = mem_x - 74 - bar_x;
      rrect(cr, bar_x, y + 12, bw, 5, 2.5);
      cairo_set_source_rgba(cr, C_TEXT, 0.08);
      cairo_fill(cr);
      cairo_pattern_t *g = cairo_pattern_create_linear(bar_x, 0, bar_x + bw, 0);
      cairo_pattern_add_color_stop_rgb(g, 0, C_LILAC);
      cairo_pattern_add_color_stop_rgb(g, 1, C_WINE);
      rrect(cr, bar_x, y + 12, bw * pr->rss_gb / maxr, 5, 2.5);
      cairo_set_source(cr, g);
      cairo_fill(cr);
      cairo_pattern_destroy(g);
    }
    snprintf(m, sizeof m, "%.1f gb", pr->rss_gb);
    text(cr, m, mem_x - 6, y + 4, 15, 400, C_MUTED, 1, 1, 0);
    // kill: × arms it ("kill?"), a second click within 2.5 s sends SIGTERM
    gboolean armed = o->armed_pid == pr->pid && now < o->armed_until;
    double kw = armed ? 58 : 30, kx = x0 + wn - kw;
    double kh = HOV(H_KILL0 + i), kp = PRS(H_KILL0 + i);
    rrect(cr, kx + kp, y + kp, kw - 2 * kp, 30 - 2 * kp, 15 - kp);
    if (armed) mix_rgba(cr, C_WINE, C_TEXT, 0.18 * kh, 1);
    else mix_rgba(cr, C_TEXT, C_WINE, kh, 0.06 + 0.16 * kh);
    cairo_fill(cr);
    if (armed) text(cr, "kill?", kx + kw / 2, y + 6, 13, 500, 0.10, 0.07, 0.094, 1, 0.5, 0);
    else text(cr, "×", kx + kw / 2, y + 3, 17, 400, 0.604 + 0.32 * kh, 0.541 + 0.337 * kh, 0.596 + 0.337 * kh, 1,
              0.5, 0);
    hit_add(p, H_KILL0 + i, kx - 4, y - 4, kw + 8, 38);
    y += 44;
  }
}

static void draw_mclock(Panel *p, cairo_t *cr) {
  GDateTime *now = g_date_time_new_now_local();
  char *t = g_date_time_format(now, "%H:%M");
  char *d = g_date_time_format(now, "%a %-d %b");
  for (char *c = d; *c; c++) *c = g_ascii_tolower(*c);
  clock_text(cr, t, 0, 0, 38, 300);
  text(cr, d, 2, 46, 14, 400, C_MUTED, 1, 0, 0);
  g_free(t);
  g_free(d);
  g_date_time_unref(now);
  (void)p;
}

static void draw_mtrack(Panel *p, cairo_t *cr) {
  if (!pl.title[0]) return;
  draw_art(cr, p->w - 52, 0, 52);
  double th = HOV(H_PLAY), tp = PRS(H_PLAY);
  if (th + tp > 0.002) {
    rrect(cr, p->w - 52, 0, 52, 52, 14);
    cairo_set_source_rgba(cr, 0.04, 0.027, 0.04, 0.45 * th + 0.15 * tp);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, C_TEXT, th);
    double cx = p->w - 26, cy = 26, s = 8 * (1 - 0.1 * tp);
    if (pl.playing) {
      cairo_rectangle(cr, cx - s * 0.8, cy - s, s * 0.6, 2 * s);
      cairo_rectangle(cr, cx + s * 0.2, cy - s, s * 0.6, 2 * s);
    } else {
      cairo_move_to(cr, cx - s * 0.6, cy - s);
      cairo_line_to(cr, cx + s, cy);
      cairo_line_to(cr, cx - s * 0.6, cy + s);
      cairo_close_path(cr);
    }
    cairo_fill(cr);
  }
  track_lines(cr, pl.title, pl.artist, 0, p->w, 62, 15, 84, 13, 1);
  hit_add(p, H_PLAY, p->w - 56, 0, 56, 56);
  // more than one source: dots left of the cover (the wheel over the panel switches too)
  if (nplsrc > 1) draw_pdots(p, cr, p->w - 52 - 12, 26, TRUE);
}

static void panel_content(Out *o, Panel *p, cairo_t *cr) {
  p->nhit = 0;
  draw_o = o;
  draw_p = p;
  switch (p->kind) {
    case P_CLOCK: draw_clock(p, cr); break;
    case P_BT: draw_bt(p, cr); break;
    case P_PLAYER: draw_player(o, p, cr); break;
    case P_NOTES: draw_notes(p, cr); break;
    case P_SWITCH: draw_switch(o, p, cr); break;
    case P_DASH: draw_dash(o, p, cr); break;
    case P_MCLOCK: draw_mclock(p, cr); break;
    case P_MTRACK: draw_mtrack(p, cr); break;
  }
}

// ── Layout ─────────────────────────────────────────────────────────────────

static Panel mkp(int kind, double x, double y, double w, double h, int side, gboolean visible, gboolean dirty) {
  Panel p = {0};
  p.kind = kind;
  p.x = x; p.y = y; p.w = w; p.h = h;
  p.side = side;
  p.visible = visible;
  p.dirty = dirty;
  return p;
}

// Panels go only where they fit, at their natural size; a panel never squeezes its content.
// Where the full flanks don't fit, a compact clock and track take their place; where even
// those don't fit, the side stays empty. Heights are checked the same way.
#define FULL_FW 340     // the full flanks (calendar, load line, player) need this width
#define COMPACT_FW 120  // the compact clock and track
#define SWITCH_W 248
#define NIRI_FLANK_PAD 48  // niri's PAD in hud_regions
#define DASH_LINGER 0.75  // niri's return of the cards (0.7 s) fades the system panel out

static void out_layout(Out *o) {
  double W = o->W, H = o->H;
  // The grid, the same formula as the niri patch (zoom 0.26, gap 0.1·H·zoom, 2 columns).
  double cw = W * ZOOM, gap = H * 0.1 * ZOOM;
  double gw = 2 * cw + gap, x0 = (W - gw) / 2, x1 = x0 + gw;
  for (int i = 0; i < NPANELS; i++) o->p[i] = mkp(i, 0, 0, 0, 0, 0, FALSE, TRUE);
  o->portrait = H > W;
  Panel *P = o->p;
  gboolean sw = W >= SWITCH_W + 40;
  if (!o->portrait) {
    if (sw) P[P_SWITCH] = mkp(P_SWITCH, W / 2 - SWITCH_W / 2, 10, SWITCH_W, 36, 0, TRUE, TRUE);
    P[P_DASH] = mkp(P_DASH, x0, 70, x1 - x0, H - 110, 0, sw, TRUE);
    // margins grow with the room beside the grid
    double room = x0;
    double M = fmin(64, fmax(16, room * 0.1)), GAP = fmin(80, fmax(24, room * 0.14));
    // perspective pulls a flank's inner edge ~5% of its width towards the grid: keep the gap
    double fw = fmin((room - GAP - M) / 1.08, 460);
    double top = H >= 900 ? 110 : 70, bottom = H - 24;
    if (fw >= FULL_FW) {
      double lx = M, rx = W - M - fw;
      if (top + 450 <= bottom) {
        P[P_CLOCK] = mkp(P_CLOCK, lx, top, fw, 450, -1, TRUE, TRUE);
        if (top + 450 + 30 + 300 <= bottom) P[P_BT] = mkp(P_BT, lx, top + 480, fw, 300, -1, TRUE, TRUE);
      } else if (top + 70 <= bottom) {
        P[P_MCLOCK] = mkp(P_MCLOCK, lx, top, fw, 70, 0, TRUE, TRUE);
      }
      if (top + 330 <= bottom) {
        P[P_PLAYER] = mkp(P_PLAYER, rx, top, fw, 330, 1, TRUE, TRUE);
        if (top + 330 + 30 + 320 <= bottom) P[P_NOTES] = mkp(P_NOTES, rx, top + 360, fw, 320, 1, TRUE, TRUE);
      } else if (top + 100 <= bottom) {
        P[P_MTRACK] = mkp(P_MTRACK, rx, top, fw, 100, 0, TRUE, TRUE);
      }
    } else if (fw >= COMPACT_FW) {
      double w = fmin(fw, 260);
      P[P_MCLOCK] = mkp(P_MCLOCK, M, top, w, 70, 0, TRUE, TRUE);
      P[P_MTRACK] = mkp(P_MTRACK, W - M - w, top, w, 100, 0, TRUE, TRUE);
    }
  } else {
    double mw = fmin(x0 - 40, 260);
    if (sw) P[P_SWITCH] = mkp(P_SWITCH, W / 2 - 120, 14, 240, 38, 0, TRUE, TRUE);
    P[P_DASH] = mkp(P_DASH, x0 - 30, 190, x1 - x0 + 60, H - 400, 0, sw && H - 400 > 200, TRUE);
    if (mw >= COMPACT_FW) {
      P[P_MCLOCK] = mkp(P_MCLOCK, 20, 40, mw, 70, 0, TRUE, TRUE);
      P[P_MTRACK] = mkp(P_MTRACK, W - 20 - mw, H - 40 - 100, mw, 100, 0, TRUE, TRUE);
    }
  }
  // niri cuts the HUD into full-height flank strips (each side's input rects plus NIRI_FLANK_PAD)
  // and the middle; only the middle fades with system mode. The system panel must stay inside
  // the middle, or its edges stay on screen in the flank strips.
  double mid0 = 0, mid1 = W;
  for (int i = 0; i < NPANELS; i++) {
    Panel *q = &P[i];
    if (!q->visible || i == P_DASH || i == P_SWITCH || q->w <= 0) continue;
    double c = q->x + q->w / 2;
    if (c < W * 0.3) mid0 = fmax(mid0, q->x + q->w + NIRI_FLANK_PAD + 2);
    else if (c > W * 0.7) mid1 = fmin(mid1, q->x - NIRI_FLANK_PAD - 2);
  }
  if (P[P_DASH].visible) {
    double d0 = fmax(P[P_DASH].x, mid0), d1 = fmin(P[P_DASH].x + P[P_DASH].w, mid1);
    P[P_DASH].x = d0;
    P[P_DASH].w = fmax(0, d1 - d0);
  }
}

// ── Frame: poses, warping, drawing ─────────────────────────────────────────

// Pose of a panel: flank panels are turned towards the middle about their outer edge. The
// pose is fixed; niri fades the whole HUD with the overview.
static void panel_pose(Out *o, Panel *p) {
  // niri turns the flanks in perspective on the GPU (and maps the pointer back through it),
  // so every panel is drawn and hit-tested flat here.
  p->hm = (M3){{1, 0, p->x, 0, 1, p->y, 0, 0, 1}};
  p->hinv = m3_inv(p->hm);
  double xs[4], ys[4];
  m3_apply(p->hm, 0, 0, &xs[0], &ys[0]);
  m3_apply(p->hm, p->w, 0, &xs[1], &ys[1]);
  m3_apply(p->hm, 0, p->h, &xs[2], &ys[2]);
  m3_apply(p->hm, p->w, p->h, &xs[3], &ys[3]);
  double mnx = 1e9, mny = 1e9, mxx = -1e9, mxy = -1e9;
  for (int i = 0; i < 4; i++) {
    mnx = fmin(mnx, xs[i]); mny = fmin(mny, ys[i]);
    mxx = fmax(mxx, xs[i]); mxy = fmax(mxy, ys[i]);
  }
  p->bx = floor(mnx) - 2; p->by = floor(mny) - 2;
  p->bw = ceil(mxx) - p->bx + 4; p->bh = ceil(mxy) - p->by + 4;
}

static void panel_render_content(Out *o, Panel *p) {
  int s = o->scale;
  if (!p->content || p->dirty) {
    if (!p->content) {
      p->content = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)ceil(p->w * s), (int)ceil(p->h * s));
      cairo_surface_set_device_scale(p->content, s, s);
    }
    PROF_BEGIN;
    cairo_t *cr = cairo_create(p->content);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    panel_content(o, p, cr);
    cairo_destroy(cr);
    cairo_surface_flush(p->content);
    p->dirty = FALSE;
    p->part = p->wpart = FALSE;
    p->wkey[0] = NAN;  // content changed: re-warp
    PROF_END("content");
  } else if (p->part) {
    // A hover or press animation: re-render only that button's rect (whole device pixels, so
    // the clear and the redraw meet exactly), then re-warp only what it covers.
    PROF_BEGIN;
    double x0 = floor(fmax(0, p->px0)), y0 = floor(fmax(0, p->py0));
    double x1 = ceil(fmin(p->w, p->px1)), y1 = ceil(fmin(p->h, p->py1));
    p->part = FALSE;
    if (x1 <= x0 || y1 <= y0) return;
    cairo_t *cr = cairo_create(p->content);
    cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
    cairo_clip(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    panel_content(o, p, cr);
    cairo_destroy(cr);
    cairo_surface_flush(p->content);
    if (p->wpart) {
      p->wx0 = fmin(p->wx0, x0); p->wy0 = fmin(p->wy0, y0);
      p->wx1 = fmax(p->wx1, x1); p->wy1 = fmax(p->wy1, y1);
    } else {
      p->wx0 = x0; p->wy0 = y0; p->wx1 = x1; p->wy1 = y1;
    }
    p->wpart = TRUE;
    PROF_END("content-part");
  }
}

// The screen box (logical) that a local rect of a panel covers.
static void panel_rect_screen(Panel *p, double x0, double y0, double x1, double y1, double *sx0, double *sy0,
                              double *sx1, double *sy1) {
  if (!p->side) {
    *sx0 = p->x + x0; *sy0 = p->y + y0; *sx1 = p->x + x1; *sy1 = p->y + y1;
    return;
  }
  double xs[4], ys[4];
  m3_apply(p->hm, x0, y0, &xs[0], &ys[0]);
  m3_apply(p->hm, x1, y0, &xs[1], &ys[1]);
  m3_apply(p->hm, x0, y1, &xs[2], &ys[2]);
  m3_apply(p->hm, x1, y1, &xs[3], &ys[3]);
  *sx0 = *sy0 = 1e9;
  *sx1 = *sy1 = -1e9;
  for (int i = 0; i < 4; i++) {
    *sx0 = fmin(*sx0, xs[i]); *sy0 = fmin(*sy0, ys[i]);
    *sx1 = fmax(*sx1, xs[i]); *sy1 = fmax(*sy1, ys[i]);
  }
}

static void out_update_input(Out *o);

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
  Out *o = data;
  PROF_BEGIN;
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  if (!o->drawn) return FALSE;
  for (int i = 0; i < NPANELS; i++) {
    Panel *p = &o->p[i];
    if (!p->visible) continue;
    if (p->kind == P_PLAYER && !pl.proxy) continue;
    if (p->kind == P_MTRACK && !pl.title[0]) continue;
    if (p->kind == P_DASH && !o->sys_mode && !o->dash_up) continue;
    panel_render_content(o, p);
    panel_pose(o, p);
    cairo_set_source_surface(cr, p->content, p->x, p->y);
    cairo_paint(cr);
  }
  out_update_input(o);
  PROF_END("draw");
  return FALSE;
}

// Only the widgets take input; the rest of the backdrop stays with the overview.
static void out_update_input(Out *o) {
  cairo_region_t *r = cairo_region_create();
  if (o->shown) {
    for (int i = 0; i < NPANELS; i++) {
      Panel *p = &o->p[i];
      // The compact clock has no widgets, but niri finds the flanks by this region: without it
      // the clock would count as the middle and be cut at the switch strip.
      if (!p->visible || (p->nhit == 0 && p->kind != P_MCLOCK)) continue;
      if (p->kind == P_DASH && !o->sys_mode) continue;
      if (p->kind == P_PLAYER && !pl.proxy) continue;
      cairo_rectangle_int_t rr = {(int)p->bx, (int)p->by, (int)p->bw, (int)p->bh};
      if (!p->side) rr = (cairo_rectangle_int_t){(int)p->x, (int)p->y, (int)ceil(p->w), (int)ceil(p->h)};
      cairo_region_union_rectangle(r, &rr);
    }
  }
  GdkWindow *gw = gtk_widget_get_window(o->win);
  if (gw) gdk_window_input_shape_combine_region(gw, r, 0, 0);
  cairo_region_destroy(r);
}

// Hover and press highlights: ease every hit's amounts towards their targets. A hit whose
// amount moved gets its rect re-rendered and re-warped, and only that area repainted.
// Returns whether anything is still moving.
static gboolean hover_step(Out *o, double dt) {
  gboolean busy = FALSE;
  double kin = 1 - exp(-dt / 0.05), kout = 1 - exp(-dt / 0.12), kp = 1 - exp(-dt / 0.03);
  // the hovered panel hasn't been drawn yet (its hits are unknown): keep the clock running
  if (o->hov_panel >= 0 && o->hov_hit && (!o->p[o->hov_panel].content || o->p[o->hov_panel].dirty)) busy = TRUE;
  for (int i = 0; i < NPANELS; i++) {
    Panel *p = &o->p[i];
    guint64 seen = 0;
    for (int k = 0; p->visible && k < p->nhit; k++) {
      Hit *h = &p->hits[k];
      int id = h->id;
      if (id <= 0 || id >= 64 || (seen >> id & 1)) continue;
      seen |= (guint64)1 << id;
      gboolean over = o->hov_panel == i && o->hov_hit == id;
      double th = over ? 1 : 0;
      double tp = over && o->press_panel == i && o->press_hit == id ? 1 : 0;
      float *hv = &o->hv[i][id], *pv = &o->pv[i][id];
      if (*hv == th && *pv == tp) continue;
      *hv += (th - *hv) * (th > *hv ? kin : kout);
      *pv += (tp - *pv) * kp;
      if (fabs(*hv - th) < 0.004) *hv = th;
      if (fabs(*pv - tp) < 0.004) *pv = tp;
      if (*hv != th || *pv != tp) busy = TRUE;
      // repaint the button, with room for its halo
      double x0 = h->x - 16, y0 = h->y - 16, x1 = h->x + h->w + 16, y1 = h->y + h->h + 16;
      if (p->part) {
        p->px0 = fmin(p->px0, x0); p->py0 = fmin(p->py0, y0);
        p->px1 = fmax(p->px1, x1); p->py1 = fmax(p->py1, y1);
      } else {
        p->px0 = x0; p->py0 = y0; p->px1 = x1; p->py1 = y1;
      }
      p->part = TRUE;
      panel_pose(o, p);
      double sx0, sy0, sx1, sy1;
      panel_rect_screen(p, x0, y0, x1, y1, &sx0, &sy0, &sx1, &sy1);
      int qx = (int)floor(sx0) - 2, qy = (int)floor(sy0) - 2;
      gtk_widget_queue_draw_area(o->area, qx, qy, (int)ceil(sx1) - qx + 4, (int)ceil(sy1) - qy + 4);
    }
    // hits that went away (a dismissed note, a killed process) start from nothing next time
    for (int id = 1; id < 64; id++)
      if (!(seen >> id & 1)) o->hv[i][id] = o->pv[i][id] = 0;
  }
  return busy;
}

// Marks a local rect of a panel to re-render and repaint this frame.
static void part_add(Out *o, Panel *p, double x0, double y0, double x1, double y1) {
  if (!p->visible) return;
  if (p->part) {
    p->px0 = fmin(p->px0, x0); p->py0 = fmin(p->py0, y0);
    p->px1 = fmax(p->px1, x1); p->py1 = fmax(p->py1, y1);
  } else {
    p->px0 = x0; p->py0 = y0; p->px1 = x1; p->py1 = y1;
  }
  p->part = TRUE;
  panel_pose(o, p);
  double sx0, sy0, sx1, sy1;
  panel_rect_screen(p, x0, y0, x1, y1, &sx0, &sy0, &sx1, &sy1);
  int qx = (int)floor(sx0) - 2, qy = (int)floor(sy0) - 2;
  gtk_widget_queue_draw_area(o->area, qx, qy, (int)ceil(sx1) - qx + 4, (int)ceil(sy1) - qy + 4);
}

// What each content animation repaints: only the cells it moves.
static void anim_parts(Out *o, int k) {
  Panel *P = o->p;
  switch (k) {
    case A_MINUTE:
      part_add(o, &P[P_CLOCK], 20, 14, P[P_CLOCK].w - 20, 18 + 88 * 1.32);
      part_add(o, &P[P_MCLOCK], 0, 0, P[P_MCLOCK].w, 38 * 1.36);
      break;
    case A_TODAY: {
      double cx, cy;
      if (P[P_CLOCK].visible && clock_today(&P[P_CLOCK], &cx, &cy))
        part_add(o, &P[P_CLOCK], cx - 18, cy - 18, cx + 18, cy + 18);
      break;
    }
    case A_TRACK:
      part_add(o, &P[P_PLAYER], 118, 50, P[P_PLAYER].w - 24, 118);
      part_add(o, &P[P_MTRACK], 0, 58, P[P_MTRACK].w, P[P_MTRACK].h);
      if (nplsrc > 1) part_add(o, &P[P_PLAYER], 120, 10, P[P_PLAYER].w - 20, 46);  // the source's name
      break;
    case A_PSW:  // the dots' pill
      part_add(o, &P[P_PLAYER], P[P_PLAYER].w / 2, 14, P[P_PLAYER].w - 16, 48);
      part_add(o, &P[P_MTRACK], 0, 10, P[P_MTRACK].w - 52, 42);
      break;
    case A_ART:
      part_add(o, &P[P_PLAYER], 26, 46, 108, 128);
      part_add(o, &P[P_MTRACK], P[P_MTRACK].w - 54, 0, P[P_MTRACK].w, 54);
      break;
    case A_NOTES:
      part_add(o, &P[P_NOTES], 0, 40, P[P_NOTES].w, P[P_NOTES].h);
      break;
    case A_DASH:
      if (o->sys_mode || o->dash_up) {
        gboolean compact = o->portrait || P[P_DASH].w < 1000;
        part_add(o, &P[P_DASH], 0, 0, P[P_DASH].w, (compact ? 200 : 250) + 22);
      }
      break;
  }
}

// Steps the content animations; returns whether any is still running. Each one gets a last
// repaint at its end state, then stops.
static gboolean anims_step(Out *o) {
  gboolean busy = FALSE;
  double t = now_s();
  for (int k = 0; k < NANIM; k++) {
    if (!o->an_t0[k]) continue;
    anim_parts(o, k);
    if (t - o->an_t0[k] >= anim_dur(k)) o->an_t0[k] = 0;  // drawn once more at 1, then done
    else busy = TRUE;
  }
  return busy;
}

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
  Out *o = data;
  double t = now_s();
  gint64 ft = gdk_frame_clock_get_frame_time(clock);
  double dt = o->anim_last ? fmin((ft - o->anim_last) / 1e6, 0.1) : 1.0 / 120;
  o->anim_last = ft;
  gboolean hovering = o->shown && hover_step(o, dt);
  gboolean animating = o->shown && anims_step(o);
  // Only the flat system-mode parts and drags animate; everything else is static.
  // The pill slides inside the switch only: that small panel is all that repaints. The system
  // panel itself fades and rises in niri (with the cards' stash), on the GPU.
  gboolean sliding = o->shown && t - o->sys_t < 0.4;
  if (sliding) {
    o->p[P_SWITCH].dirty = TRUE;
    panel_queue(o, P_SWITCH);
  }
  // after switching off, the system panel stays drawn until niri has faded it, then goes
  if (o->shown && !o->sys_mode && o->dash_up && t - o->sys_t >= DASH_LINGER) {
    o->dash_up = FALSE;
    panel_queue(o, P_DASH);
  }
  gboolean lingering = o->shown && o->dash_up && !o->sys_mode;
  if (o->shown && o->dragging && o->press_panel >= 0) panel_queue(o, o->press_panel);
  if (sliding || lingering || (o->shown && o->dragging)) return G_SOURCE_CONTINUE;
  if (hovering || animating) return G_SOURCE_CONTINUE;
  o->tick = 0;
  o->anim_last = 0;
  return G_SOURCE_REMOVE;
}

// The area a panel can cover, its perspective image included.
static void panel_queue(Out *o, int kind) {
  Panel *p = &o->p[kind];
  if (!p->visible) return;
  int x = (int)floor(p->x), y = (int)floor(p->y), w = (int)ceil(p->w), h = (int)ceil(p->h);
  if (p->side) {
    // in perspective the receding edge moves towards the middle of the screen: repaint the
    // panel's projected box, not its flat one
    panel_pose(o, p);
    int x1 = (int)fmax(x + w, ceil(p->bx + p->bw)), y1 = (int)fmax(y + h, ceil(p->by + p->bh));
    x = (int)fmin(x, floor(p->bx)) - 2;
    y = (int)fmin(y, floor(p->by)) - 2;
    w = x1 - x + 4;
    h = y1 - y + 4;
  }
  if (kind == P_DASH) { y -= 4; h += 32; }
  gtk_widget_queue_draw_area(o->area, x, y, w, h);
}

static void out_queue_all(Out *o) {
  for (int i = 0; i < NPANELS; i++) panel_queue(o, i);
}

// Run the frame clock for the hover animations only (they repaint just their buttons).
static void out_anim(Out *o) {
  if (o->shown && !o->tick) {
    o->anim_last = 0;
    o->tick = gtk_widget_add_tick_callback(o->area, on_tick, o, NULL);
  }
}

// Start (or keep) the frame-clock animation; only the panels' areas are repainted.
static void out_redraw(Out *o) {
  if (!o->shown) return;
  out_queue_all(o);
  if (!o->tick) o->tick = gtk_widget_add_tick_callback(o->area, on_tick, o, NULL);
}

// ── Actions ────────────────────────────────────────────────────────────────

static void spawn(const char *cmd) { g_spawn_command_line_async(cmd, NULL); }

static void player_call(const char *method, GVariant *args) {
  if (!pl.proxy) return;
  g_dbus_proxy_call(pl.proxy, method, args, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void volume_apply(double v) {
  v = fmax(0, fmin(1.5, v));
  vol.pending = v;
  vol.muted = FALSE;
  double t = now_s();
  if (t - vol.sent_at > 0.035) {  // ~30 ms throttle while dragging
    char cmd[128];
    snprintf(cmd, sizeof cmd, "wpctl set-volume -l 1.5 @DEFAULT_SINK@ %.3f", v);
    spawn(cmd);
    vol.sent_at = t;
  }
  mark_dirty(P_PLAYER);
}

static void bt_toggle_power(void);
static void bt_device_toggle(int i);

static void player_use(int i);

// Shows source i (wrapping), by a click on the dots or the name, or the wheel over the track.
static void player_switch(int i) {
  if (nplsrc < 2) return;
  i = ((i % nplsrc) + nplsrc) % nplsrc;
  if (i != plsel) player_use(i);
}

static void out_set_sys(Out *o, gboolean on) {
  if (o->sys_mode == on) return;
  o->sys_mode = on;
  o->sys_t = now_s();
  o->p[P_SWITCH].dirty = TRUE;
  if (on) {
    procs_sample();
    o->p[P_DASH].dirty = TRUE;
    o->dash_up = TRUE;
    o->an_t0[A_DASH] = now_s();
    panel_queue(o, P_DASH);
  }
  panel_queue(o, P_SWITCH);
  niri_request(on ? "{\"Action\":{\"SetOverviewWorkspacesHidden\":{\"hidden\":true}}}"
                  : "{\"Action\":{\"SetOverviewWorkspacesHidden\":{\"hidden\":false}}}");
  out_anim(o);
}

static void activate(Out *o, int panel, int id, double frac) {
  if (id == H_PLAY) player_call("PlayPause", NULL);
  else if (id == H_NEXT) player_call("Next", NULL);
  else if (id == H_PREV) player_call("Previous", NULL);
  else if (id == H_PSRC) player_switch(plsel + 1);
  else if (id >= H_PDOT0 && id < H_PDOT0 + NPL) player_switch(id - H_PDOT0);
  else if (id == H_SEEK && pl.can_seek && pl.length_us > 0) {
    gint64 pos = (gint64)(clamp01(frac) * pl.length_us);
    player_call("SetPosition", g_variant_new("(ox)", pl.trackid[0] ? pl.trackid : "/", pos));
    pl.pos_us = pos;
    pl.pos_at = now_s();
  } else if (id == H_MUTE) {
    spawn("wpctl set-mute @DEFAULT_SINK@ toggle");
    vol.muted = !vol.muted;
    mark_dirty(P_PLAYER);
  } else if (id == H_BTPOW) bt_toggle_power();
  else if (id >= H_BTDEV0 && id < H_BTDEV0 + NBT) bt_device_toggle(id - H_BTDEV0);
  else if (id >= H_NOTE0 && id < H_NOTE0 + NNOTE) {
    int i = id - H_NOTE0;
    if (i < nnotes) {
      g_hash_table_add(dismissed, GUINT_TO_POINTER(notes[i].id));
      notes_snapshot();
      memmove(&notes[i], &notes[i + 1], sizeof(Note) * (nnotes - i - 1));
      nnotes--;
      notes_changed();
      mark_dirty(P_NOTES);
    }
  } else if (id == H_SW_WS) out_set_sys(o, FALSE);
  else if (id == H_SW_SYS) out_set_sys(o, TRUE);
  else if (id >= H_KILL0 && id < H_KILL0 + NPROC) {
    int i = id - H_KILL0;
    if (i < nprocs) {
      if (o->armed_pid == procs[i].pid && now_s() < o->armed_until) {
        kill(procs[i].pid, SIGTERM);
        o->armed_pid = 0;
        procs_sample();
      } else {
        o->armed_pid = procs[i].pid;
        o->armed_until = now_s() + 2.5;
      }
      o->p[P_DASH].dirty = TRUE;
      out_redraw(o);
    }
  }
  (void)panel;
}

// Pointer position on the screen -> a panel and its local coordinates.
static Panel *panel_at(Out *o, double x, double y, double *lx, double *ly, Hit **hit) {
  for (int i = NPANELS - 1; i >= 0; i--) {
    Panel *p = &o->p[i];
    if (!p->visible || !p->content) continue;
    if (p->kind == P_DASH && !o->sys_mode) continue;
    if (p->kind == P_PLAYER && !pl.proxy) continue;
    double u, v;
    if (p->side) m3_apply(p->hinv, x, y, &u, &v);
    else { u = x - p->x; v = y - p->y; }
    if (u < 0 || v < 0 || u > p->w || v > p->h) continue;
    *lx = u;
    *ly = v;
    *hit = NULL;
    for (int k = 0; k < p->nhit; k++) {
      Hit *h = &p->hits[k];
      if (u >= h->x && u <= h->x + h->w && v >= h->y && v <= h->y + h->h) *hit = h;
    }
    return p;
  }
  return NULL;
}

static double hit_frac(Panel *p, int id, double lx) {
  if (id == H_SEEK) return clamp01((lx - 28) / (p->w - 56));
  if (id == H_VOL) {
    double sx = 64, sw = p->w - 64 - 28 - 58;
    return clamp01((lx - sx) / sw) * 1.5;
  }
  return 0;
}

static void hover_set(Out *o, int panel, int hit);

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer data) {
  Out *o = data;
  if (e->button != 1 || e->type != GDK_BUTTON_PRESS) return FALSE;
  // ignore clicks while the panels are still sliding in
  if (o->open_t > 0 && now_s() - o->open_t < 0.35) return TRUE;
  double lx, ly;
  Hit *h;
  Panel *p = panel_at(o, e->x, e->y, &lx, &ly, &h);
  o->press_panel = p ? p->kind : -1;
  o->press_hit = h ? h->id : 0;
  hover_set(o, p && h ? p->kind : -1, h ? h->id : 0);
  out_anim(o);
  if (h && (h->id == H_SEEK || h->id == H_VOL)) {
    o->dragging = TRUE;
    o->drag_frac = hit_frac(p, h->id, lx) / (h->id == H_VOL ? 1.5 : 1);
    if (h->id == H_VOL) volume_apply(hit_frac(p, h->id, lx));
    p->dirty = TRUE;
    out_redraw(o);
  }
  return TRUE;
}

// What is under the pointer: the highlight follows it, and buttons get a hand cursor.
static void hover_set(Out *o, int panel, int hit) {
  if (o->hov_panel == panel && o->hov_hit == hit) return;
  gboolean had = o->hov_hit != 0;
  o->hov_panel = panel;
  o->hov_hit = hit;
  GdkWindow *gw = gtk_widget_get_window(o->area);
  if (gw && had != (hit != 0)) {
    GdkCursor *c = hit ? gdk_cursor_new_from_name(gtk_widget_get_display(o->area), "pointer") : NULL;
    gdk_window_set_cursor(gw, c);
    if (c) g_object_unref(c);
  }
  out_anim(o);
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer data) {
  Out *o = data;
  if (!o->dragging) hover_set(o, -1, 0);
  return FALSE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data) {
  Out *o = data;
  if (!o->dragging) {
    double lx, ly;
    Hit *h;
    Panel *p = panel_at(o, e->x, e->y, &lx, &ly, &h);
    hover_set(o, p && h ? p->kind : -1, p && h ? h->id : 0);
  }
  if (o->dragging) {
    double lx, ly;
    Hit *h;
    Panel *p = &o->p[o->press_panel];
    // follow the pointer even off the bar: project onto the panel
    double u, v;
    if (p->side) m3_apply(p->hinv, e->x, e->y, &u, &v);
    else { u = e->x - p->x; v = e->y - p->y; }
    (void)lx; (void)ly; (void)h;
    double f = hit_frac(p, o->press_hit, u);
    if (o->press_hit == H_VOL) volume_apply(f);
    o->drag_frac = o->press_hit == H_VOL ? f / 1.5 : f;
    p->dirty = TRUE;
    out_redraw(o);
  }
  return FALSE;
}

static gboolean on_release(GtkWidget *w, GdkEventButton *e, gpointer data) {
  Out *o = data;
  if (e->button != 1) return FALSE;
  double lx, ly;
  Hit *h;
  Panel *p = panel_at(o, e->x, e->y, &lx, &ly, &h);
  if (o->dragging) {
    o->dragging = FALSE;
    Panel *dp = &o->p[o->press_panel];
    if (o->press_hit == H_SEEK) activate(o, o->press_panel, H_SEEK, o->drag_frac);  // commit on release
    if (o->press_hit == H_VOL) {
      vol.sent_at = 0;
      volume_apply(o->drag_frac * 1.5);  // make sure the last value lands
    }
    dp->dirty = TRUE;
  } else if (p && h && p->kind == o->press_panel && h->id == o->press_hit) {
    activate(o, p->kind, h->id, 0);  // only when released on what was pressed
  }
  o->press_hit = 0;
  hover_set(o, p && h ? p->kind : -1, p && h ? h->id : 0);
  out_redraw(o);
  return TRUE;
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *e, gpointer data) {
  Out *o = data;
  double lx, ly;
  Hit *h;
  Panel *p = panel_at(o, e->x, e->y, &lx, &ly, &h);
  if (p && p->kind == P_PLAYER && h && (h->id == H_VOL || h->id == H_MUTE)) {
    double dy = e->direction == GDK_SCROLL_UP ? -1 : e->direction == GDK_SCROLL_DOWN ? 1 : e->delta_y;
    double v = (vol.pending >= 0 ? vol.pending : vol.v) - (dy > 0 ? 0.02 : -0.02);
    vol.sent_at = 0;
    volume_apply(v);
  } else if (p && nplsrc > 1 && ((p->kind == P_PLAYER && ly < 136) || p->kind == P_MTRACK)) {
    // Over the track (cover, title, header): the wheel or a swipe flips through the sources,
    // one per gesture-sized step, never faster than a step per 0.28 s.
    static double acc, last;
    double d = 0, t = now_s();
    if (e->direction == GDK_SCROLL_UP || e->direction == GDK_SCROLL_LEFT) d = -1;
    else if (e->direction == GDK_SCROLL_DOWN || e->direction == GDK_SCROLL_RIGHT) d = 1;
    else if (e->direction == GDK_SCROLL_SMOOTH) {
      acc += fabs(e->delta_x) > fabs(e->delta_y) ? e->delta_x : e->delta_y;
      if (fabs(acc) >= 1.5) d = acc > 0 ? 1 : -1;
    }
    if (d != 0 && t - last >= 0.28) {
      acc = 0;
      last = t;
      player_switch(plsel + (int)d);
    } else if (d != 0 && e->direction != GDK_SCROLL_SMOOTH) {
      acc = 0;
    }
  }
  return TRUE;
}

// ── Show / hide with the overview ──────────────────────────────────────────

static char focused_output[64];
static gboolean overview_open;

static void refresh_on_open(void);

static void outs_sync(void) {
  for (guint i = 0; i < outs->len; i++) {
    Out *o = g_ptr_array_index(outs, i);
    // Shown on every output while the overview is open: niri draws it only where the overview
    // is (faded with its progress) and gives it input only there, so the HUD never has to
    // guess which monitor that is.
    gboolean want = overview_open;
    if (want && !o->shown) {
      o->shown = TRUE;
      o->drawn = TRUE;
      o->open_t = now_s();
      o->sys_mode = FALSE;
      o->sys_t = 0;
      o->dash_up = FALSE;
      for (int k = 0; k < NPANELS; k++) o->p[k].dirty = TRUE;
      // today's ring draws once the flanks have come in
      memset(o->an_t0, 0, sizeof o->an_t0);
      o->an_t0[A_TODAY] = o->open_t + 0.3;
      refresh_on_open();
      out_redraw(o);
    } else if (!want && o->shown) {
      // niri fades the panels out as they are; nothing redraws until the next open.
      o->shown = FALSE;
      o->dragging = FALSE;
      o->hov_panel = -1;
      o->hov_hit = 0;
      memset(o->hv, 0, sizeof o->hv);
      memset(o->pv, 0, sizeof o->pv);
      if (o->tick) {
        gtk_widget_remove_tick_callback(o->area, o->tick);
        o->tick = 0;
      }
      out_update_input(o);
    }
  }
}

// ── Data refresh ───────────────────────────────────────────────────────────

static gboolean any_shown(void) {
  for (guint i = 0; i < outs->len; i++)
    if (((Out *)g_ptr_array_index(outs, i))->shown) return TRUE;
  return FALSE;
}

static void volume_read_done(GObject *src, GAsyncResult *res, gpointer data) {
  char *out = NULL;
  if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, NULL) && out) {
    double v;
    if (sscanf(out, "Volume: %lf", &v) == 1) {
      if (fabs(v - vol.v) > 1e-3 || vol.muted != (strstr(out, "MUTED") != NULL)) {
        vol.v = v;
        vol.muted = strstr(out, "MUTED") != NULL;
        if (now_s() - vol.sent_at > 0.5) vol.pending = -1;
        mark_dirty(P_PLAYER);
      }
    }
  }
  g_free(out);
}

static void volume_read(void) {
  GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                                    "wpctl", "get-volume", "@DEFAULT_SINK@", NULL);
  if (p) {
    g_subprocess_communicate_utf8_async(p, NULL, NULL, volume_read_done, NULL);
    g_object_unref(p);
  }
}

static void notes_done(GObject *src, GAsyncResult *res, gpointer data) {
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
  if (!r) return;
  GVariant *arr = g_variant_get_child_value(r, 0);
  notes_snapshot();
  nnotes = 0;
  gsize n = g_variant_n_children(arr);
  for (gsize i = 0; i < n && nnotes < NNOTE; i++) {
    GVariant *d = g_variant_get_child_value(arr, i);
    guint id = 0;
    const char *app = "", *sum = "", *body = "";
    guchar urg = 1;
    g_variant_lookup(d, "id", "u", &id);
    g_variant_lookup(d, "app-name", "&s", &app);
    g_variant_lookup(d, "summary", "&s", &sum);
    g_variant_lookup(d, "body", "&s", &body);
    g_variant_lookup(d, "urgency", "y", &urg);
    if (!g_hash_table_contains(dismissed, GUINT_TO_POINTER(id))) {
      Note *nt = &notes[nnotes++];
      nt->id = id;
      g_strlcpy(nt->app, app, sizeof nt->app);
      g_strlcpy(nt->summary, sum, sizeof nt->summary);
      g_strlcpy(nt->body, body, sizeof nt->body);
      for (char *c = nt->body; *c; c++) if (*c == '\n') *c = ' ';
      nt->critical = urg >= 2;
      note_ago(nt, nt->ago, sizeof nt->ago);
    }
    g_variant_unref(d);
  }
  g_variant_unref(arr);
  g_variant_unref(r);
  notes_changed();
  mark_dirty(P_NOTES);
}

static GDBusConnection *session;

static void notes_read(void) {
  g_dbus_connection_call(session, "org.freedesktop.Notifications", "/fr/emersion/Mako", "fr.emersion.Mako",
                         "ListHistory", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 500, NULL,
                         notes_done, NULL);
}

// Arrival times. A second connection becomes a bus monitor for Notify calls (it can only
// listen from then on). On each call, the ids mako shows a moment later that have no time yet
// get "now"; a Notify that replaces an id renews that id's time.
static void stamp(guint id, gint64 t) {
  gint64 *v = g_new(gint64, 1);
  *v = t;
  g_hash_table_replace(note_time, GUINT_TO_POINTER(id), v);
}

static void notes_stamp_done(GObject *src, GAsyncResult *res, gpointer data) {
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
  if (!r) return;
  gint64 t = *(gint64 *)data;
  GVariant *arr = g_variant_get_child_value(r, 0);
  gsize n = g_variant_n_children(arr);
  for (gsize i = 0; i < n; i++) {
    GVariant *d = g_variant_get_child_value(arr, i);
    guint id = 0;
    if (g_variant_lookup(d, "id", "u", &id) && !g_hash_table_contains(note_time, GUINT_TO_POINTER(id)))
      stamp(id, t);
    g_variant_unref(d);
  }
  g_variant_unref(arr);
  g_variant_unref(r);
  g_free(data);
  if (any_shown()) notes_read();  // a notification arrived while the overview is open
}

static gboolean notes_stamp(gpointer when) {
  g_dbus_connection_call(session, "org.freedesktop.Notifications", "/fr/emersion/Mako", "fr.emersion.Mako",
                         "ListNotifications", NULL, G_VARIANT_TYPE("(aa{sv})"), G_DBUS_CALL_FLAGS_NONE, 500, NULL,
                         notes_stamp_done, when);
  return G_SOURCE_REMOVE;
}

typedef struct { guint replaces; gint64 t; } NotifyCall;

static gboolean on_notify_call(gpointer data) {
  NotifyCall *c = data;
  if (c->replaces) stamp(c->replaces, c->t);
  gint64 *t = g_new(gint64, 1);
  *t = c->t;
  g_free(c);
  // give mako a moment to show it
  g_timeout_add(150, notes_stamp, t);
  return G_SOURCE_REMOVE;
}

// Runs on GDBus's worker thread: hand over to the main loop, and swallow the watched calls (a
// monitor must never answer them). Replies and signals pass, BecomeMonitor's own reply among them.
static GDBusMessage *monitor_filter(GDBusConnection *c, GDBusMessage *m, gboolean incoming, gpointer d) {
  if (!incoming || g_dbus_message_get_message_type(m) != G_DBUS_MESSAGE_TYPE_METHOD_CALL) return m;
  if (!g_strcmp0(g_dbus_message_get_member(m), "Notify") &&
      !g_strcmp0(g_dbus_message_get_interface(m), "org.freedesktop.Notifications")) {
    NotifyCall *nc = g_new0(NotifyCall, 1);
    nc->t = g_get_real_time();
    GVariant *b = g_dbus_message_get_body(m);
    if (b && g_variant_n_children(b) > 1) {
      GVariant *r = g_variant_get_child_value(b, 1);
      if (g_variant_is_of_type(r, G_VARIANT_TYPE_UINT32)) nc->replaces = g_variant_get_uint32(r);
      g_variant_unref(r);
    }
    g_idle_add(on_notify_call, nc);
  }
  g_object_unref(m);
  return NULL;
}

static void notes_watch(void) {
  char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  if (!addr) return;
  GError *err = NULL;
  GDBusConnection *mon = g_dbus_connection_new_for_address_sync(
      addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL,
      NULL, &err);
  g_free(addr);
  if (!mon) {
    g_printerr("midve-hud: no bus monitor for notification times: %s\n", err->message);
    g_error_free(err);
    return;
  }
  g_dbus_connection_set_exit_on_close(mon, FALSE);
  g_dbus_connection_add_filter(mon, monitor_filter, NULL, NULL);
  const char *rules[] = {"type='method_call',interface='org.freedesktop.Notifications',member='Notify'", NULL};
  GVariant *r = g_dbus_connection_call_sync(mon, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                            "org.freedesktop.DBus.Monitoring", "BecomeMonitor",
                                            g_variant_new("(^asu)", rules, 0u), NULL, G_DBUS_CALL_FLAGS_NONE, 1000,
                                            NULL, &err);
  if (!r) {
    g_printerr("midve-hud: no bus monitor for notification times: %s\n", err->message);
    g_error_free(err);
    g_object_unref(mon);
    return;
  }
  g_variant_unref(r);  // the connection is kept for the life of the HUD
}

// Content that moves on its own (track progress, live dot, Bluetooth breathing): 10 Hz, and
// only that panel is repainted.
static gboolean every_tenth(gpointer d) {
  if (!any_shown()) return G_SOURCE_CONTINUE;
  gboolean busy = FALSE;
  for (int i = 0; i < bt.n; i++) busy |= bt.d[i].busy;
  if (busy) mark_dirty(P_BT);
  if (pl.proxy && pl.title[0]) {
    if (!pl.can_seek || pl.length_us <= 0) {
      if (pl.playing) mark_dirty(P_PLAYER);  // the live dot pulses
    } else if (pl.playing) {
      // Redraw only when something visible moved: the time text (seconds) or the bar by a
      // device pixel (the bar is about 600 device px wide on DP-1).
      static long long last_key = -1;
      double frac = (double)player_pos_now() / pl.length_us;
      long long key = (long long)(player_pos_now() / 1000000) * 4096 + (long long)(clamp01(frac) * 600);
      if (key != last_key) {
        last_key = key;
        mark_dirty(P_PLAYER);
      }
    }
  }
  return G_SOURCE_CONTINUE;
}

static void pos_done(GObject *src, GAsyncResult *res, gpointer data) {
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
  if (!r) return;
  GVariant *v;
  g_variant_get(r, "(v)", &v);
  if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) {
    pl.pos_us = g_variant_get_int64(v);
    pl.pos_at = now_s();
  }
  g_variant_unref(v);
  g_variant_unref(r);
}

static gboolean every_second(gpointer d) {
  // the minute changed: its digits roll (only while the HUD is up; anim_start checks)
  {
    GDateTime *nw = g_date_time_new_now_local();
    char *hm = g_date_time_format(nw, "%H:%M");
    static char last[8];
    if (last[0] && strcmp(hm, last)) {
      g_strlcpy(clock_prev, last, sizeof clock_prev);
      anim_start(A_MINUTE, 0);
    }
    g_strlcpy(last, hm, sizeof last);
    g_free(hm);
    g_date_time_unref(nw);
  }
  // Graph samples keep coming while the overview is closed (a few /proc reads and one NVML
  // query: microseconds), so the graphs open with the last minute in them. Nothing is drawn.
  sys_sample();
  if (!any_shown()) return G_SOURCE_CONTINUE;
  volume_read();
  gboolean sys_on = FALSE;
  for (guint i = 0; i < outs->len; i++) sys_on |= ((Out *)g_ptr_array_index(outs, i))->sys_mode;
  if (sys_on) procs_sample();
  mark_dirty(P_CLOCK);
  mark_dirty(P_MCLOCK);
  // "5 min ago" moves on: repaint the notes only when a label changes
  for (int i = 0; i < nnotes; i++) {
    char ago[sizeof notes[i].ago];
    note_ago(&notes[i], ago, sizeof ago);
    if (strcmp(ago, notes[i].ago)) {
      memcpy(notes[i].ago, ago, sizeof ago);
      mark_dirty(P_NOTES);
    }
  }
  mark_dirty(P_DASH);
  if (pl.proxy) {
    // re-read the position once a second (players don't signal it)
    g_dbus_connection_call(session, pl.bus, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "Get",
                           g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"),
                           G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 300, NULL, pos_done, NULL);
  }
  return G_SOURCE_CONTINUE;
}

// ── MPRIS ──────────────────────────────────────────────────────────────────

// A new cover (NULL: none). While the HUD is up the old one is kept for the cross-fade.
static void art_swap(cairo_surface_t *img) {
  if (art_old) cairo_surface_destroy(art_old);
  art_old = NULL;
  art_old_set = FALSE;
  if (any_shown()) {
    art_old = pl.art;
    art_old_set = TRUE;
    anim_start(A_ART, 0);
  } else if (pl.art) {
    cairo_surface_destroy(pl.art);
  }
  pl.art = img;
}

static void art_loaded(GObject *src, GAsyncResult *res, gpointer data) {
  GdkPixbuf *pb = gdk_pixbuf_new_from_stream_finish(res, NULL);
  if (!pb) return;
  GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, 160, 160, GDK_INTERP_BILINEAR);
  art_swap(gdk_cairo_surface_create_from_pixbuf(sc, 1, NULL));
  g_object_unref(sc);
  g_object_unref(pb);
  mark_dirty(P_PLAYER);
  mark_dirty(P_MTRACK);
}

static void art_opened(GObject *src, GAsyncResult *res, gpointer data) {
  GFileInputStream *s = g_file_read_finish(G_FILE(src), res, NULL);
  if (!s) return;
  gdk_pixbuf_new_from_stream_async(G_INPUT_STREAM(s), NULL, art_loaded, NULL);
  g_object_unref(s);
}

static void player_read(void) {
  if (!pl.proxy) return;
  GVariant *v;
  char was_title[256], was_artist[256];
  g_strlcpy(was_title, pl.title, sizeof was_title);
  g_strlcpy(was_artist, pl.artist, sizeof was_artist);
  gboolean has_art = FALSE;
  pl.title[0] = pl.artist[0] = pl.trackid[0] = 0;
  pl.length_us = 0;
  if ((v = g_dbus_proxy_get_cached_property(pl.proxy, "Metadata"))) {
    const char *s;
    gint64 len;
    if (g_variant_lookup(v, "xesam:title", "&s", &s)) g_strlcpy(pl.title, s, sizeof pl.title);
    GVariant *a = g_variant_lookup_value(v, "xesam:artist", G_VARIANT_TYPE("as"));
    if (a) {
      if (g_variant_n_children(a)) {
        GVariant *c = g_variant_get_child_value(a, 0);
        g_strlcpy(pl.artist, g_variant_get_string(c, NULL), sizeof pl.artist);
        g_variant_unref(c);
      }
      g_variant_unref(a);
    }
    if (g_variant_lookup(v, "mpris:length", "x", &len)) pl.length_us = len;
    else {
      guint64 ul;
      if (g_variant_lookup(v, "mpris:length", "t", &ul)) pl.length_us = (gint64)ul;
    }
    GVariant *tid = g_variant_lookup_value(v, "mpris:trackid", NULL);
    if (tid) {
      g_strlcpy(pl.trackid, g_variant_get_string(tid, NULL), sizeof pl.trackid);
      g_variant_unref(tid);
    }
    if (g_variant_lookup(v, "mpris:artUrl", "&s", &s) && s[0]) {
      has_art = TRUE;
      if (strcmp(s, pl.art_url)) {
        g_strlcpy(pl.art_url, s, sizeof pl.art_url);
        GFile *f = g_file_new_for_uri(s);
        g_file_read_async(f, G_PRIORITY_LOW, NULL, art_opened, NULL);
        g_object_unref(f);
      }
    }
    g_variant_unref(v);
  }
  // a track without a cover shows the placeholder, not the last track's cover
  if (!has_art && pl.title[0] && strcmp(pl.title, was_title)) {
    pl.art_url[0] = 0;
    if (pl.art) art_swap(NULL);
  }
  gboolean src_changed = strcmp(pl.name, pl.rname) != 0;
  char was_src[48];
  g_strlcpy(was_src, pl.rname, sizeof was_src);
  g_strlcpy(pl.rname, pl.name, sizeof pl.rname);
  if (strcmp(pl.title, was_title) || strcmp(pl.artist, was_artist) || src_changed) {
    // While the old pair is still leaving, it stays the old pair and the newest track is
    // what arrives. Later on, what's on screen now becomes the pair that leaves.
    double e = anim_progress_any(A_TRACK);
    if (!(anim_running(A_TRACK) && e < 0.42)) {
      g_strlcpy(trk_old_title, was_title, sizeof trk_old_title);
      g_strlcpy(trk_old_artist, was_artist, sizeof trk_old_artist);
      g_strlcpy(trk_old_src, was_src, sizeof trk_old_src);
      // nothing was playing: the new pair only arrives
      anim_start(A_TRACK, was_title[0] ? 0 : -0.5 * anim_dur(A_TRACK));
    }
  }
  if ((v = g_dbus_proxy_get_cached_property(pl.proxy, "PlaybackStatus"))) {
    pl.playing = !strcmp(g_variant_get_string(v, NULL), "Playing");
    g_variant_unref(v);
  }
  if ((v = g_dbus_proxy_get_cached_property(pl.proxy, "Position"))) {
    pl.pos_us = g_variant_get_int64(v);
    pl.pos_at = now_s();
    g_variant_unref(v);
  }
#define CAP(name, field)                                                     \
  if ((v = g_dbus_proxy_get_cached_property(pl.proxy, name))) {             \
    field = g_variant_get_boolean(v);                                        \
    g_variant_unref(v);                                                      \
  } else field = FALSE;
  CAP("CanSeek", pl.can_seek);
  CAP("CanGoNext", pl.can_next);
  CAP("CanGoPrevious", pl.can_prev);
  CAP("CanPlay", pl.can_play);
  CAP("CanPause", pl.can_pause);
  mark_dirty(P_PLAYER);
  mark_dirty(P_MTRACK);
}

static void on_player_props(GDBusProxy *p, GVariant *changed, GStrv inval, gpointer d) { player_read(); }

static void on_player_seeked(GDBusProxy *p, gchar *sender, gchar *signal, GVariant *params, gpointer d) {
  if (!strcmp(signal, "Seeked")) {
    g_variant_get(params, "(x)", &pl.pos_us);
    pl.pos_at = now_s();
    mark_dirty(P_PLAYER);
  }
}

// Shows source i: a proxy on it, and its state read.
static void player_use(int i) {
  if (pl.proxy) { g_object_unref(pl.proxy); pl.proxy = NULL; }
  g_free(pl.bus);
  pl.bus = NULL;
  if (i < 0 || i >= nplsrc) {
    plsel = -1;
    pl.title[0] = pl.name[0] = pl.rname[0] = 0;
    mark_dirty(P_PLAYER);
    mark_dirty(P_MTRACK);
    return;
  }
  if (plsel != i && plsel >= 0) {
    plsel_old = plsel;
    anim_start(A_PSW, 0);
  }
  plsel = i;
  pl.bus = g_strdup(plsrc[i].bus);
  g_strlcpy(pl.name, plsrc[i].name, sizeof pl.name);
  pl.proxy = g_dbus_proxy_new_sync(session, G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES, NULL, pl.bus,
                                   "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, NULL);
  if (pl.proxy) {
    g_signal_connect(pl.proxy, "g-properties-changed", G_CALLBACK(on_player_props), NULL);
    g_signal_connect(pl.proxy, "g-signal", G_CALLBACK(on_player_seeked), NULL);
  }
  player_read();
}

static int plsrc_cmp(const void *a, const void *b) { return strcmp(((PlSrc *)a)->bus, ((PlSrc *)b)->bus); }

static GVariant *bus_get(const char *bus, const char *iface, const char *prop) {
  GVariant *r = g_dbus_connection_call_sync(session, bus, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
                                            "Get", g_variant_new("(ss)", iface, prop), G_VARIANT_TYPE("(v)"),
                                            G_DBUS_CALL_FLAGS_NONE, 300, NULL, NULL);
  if (!r) return NULL;
  GVariant *v;
  g_variant_get(r, "(v)", &v);
  g_variant_unref(r);
  return v;
}

// Lists the sources. The shown one stays while it exists; at the start, or when it goes, the
// one playing (else the first) takes its place. A source that starts playing is shown at once
// (on_any_player_props), so "the last one started" is what's up unless the dots say otherwise.
static void players_scan(void) {
  GVariant *r = g_dbus_connection_call_sync(session, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                            "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"),
                                            G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
  if (!r) return;
  PlSrc list[NPL];
  int n = 0;
  GVariantIter *it;
  const char *name;
  g_variant_get(r, "(as)", &it);
  while (n < NPL && g_variant_iter_next(it, "&s", &name)) {
    if (strncmp(name, "org.mpris.MediaPlayer2.", 23) || !strcmp(name, "org.mpris.MediaPlayer2.playerctld")) continue;
    PlSrc *s = &list[n];
    memset(s, 0, sizeof *s);
    g_strlcpy(s->bus, name, sizeof s->bus);
    // the owner and the name don't change while the bus name lives: keep them from the last scan
    int k;
    for (k = 0; k < nplsrc && strcmp(plsrc[k].bus, name); k++) ;
    if (k < nplsrc) {
      g_strlcpy(s->owner, plsrc[k].owner, sizeof s->owner);
      g_strlcpy(s->name, plsrc[k].name, sizeof s->name);
    } else {
      GVariant *o = g_dbus_connection_call_sync(session, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", name),
                                                G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 300, NULL, NULL);
      if (o) {
        const char *u;
        g_variant_get(o, "(&s)", &u);
        g_strlcpy(s->owner, u, sizeof s->owner);
        g_variant_unref(o);
      }
      GVariant *id = bus_get(name, "org.mpris.MediaPlayer2", "Identity");
      if (id && g_variant_is_of_type(id, G_VARIANT_TYPE_STRING) && g_variant_get_string(id, NULL)[0]) {
        char *low = g_utf8_strdown(g_variant_get_string(id, NULL), -1);
        g_strlcpy(s->name, low, sizeof s->name);
        g_free(low);
      } else {
        // org.mpris.MediaPlayer2.chromium.instance123 -> chromium
        g_strlcpy(s->name, name + 23, sizeof s->name);
        char *dot = strchr(s->name, '.');
        if (dot) *dot = 0;
      }
      if (id) g_variant_unref(id);
    }
    GVariant *st = bus_get(name, "org.mpris.MediaPlayer2.Player", "PlaybackStatus");
    if (st && g_variant_is_of_type(st, G_VARIANT_TYPE_STRING)) s->playing = !strcmp(g_variant_get_string(st, NULL), "Playing");
    if (st) g_variant_unref(st);
    n++;
  }
  g_variant_iter_free(it);
  g_variant_unref(r);
  qsort(list, n, sizeof *list, plsrc_cmp);

  char keep[128] = "";
  if (pl.bus) g_strlcpy(keep, pl.bus, sizeof keep);
  int oldn = nplsrc;
  memcpy(plsrc, list, sizeof *list * n);
  nplsrc = n;
  int pick = -1;
  for (int i = 0; i < n && keep[0]; i++)
    if (!strcmp(plsrc[i].bus, keep)) pick = i;
  if (pick >= 0) {
    plsel = pick;  // the same source, maybe at another index
    plsel_old = -1;
    if (oldn != n) {  // the dots changed
      mark_dirty(P_PLAYER);
      mark_dirty(P_MTRACK);
    }
    player_read();
    return;
  }
  for (int i = 0; i < n && pick < 0; i++)
    if (plsrc[i].playing) pick = i;
  if (pick < 0 && n) pick = 0;
  plsel = -1;
  plsel_old = -1;
  player_use(pick);
}

// Any source's PlaybackStatus. One that starts playing is shown (the last one started wins).
static void on_any_player_props(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                                const gchar *signal, GVariant *params, gpointer d) {
  const char *ifc, *st;
  GVariant *ch;
  g_variant_get(params, "(&s@a{sv}@as)", &ifc, &ch, NULL);
  if (!strcmp(ifc, "org.mpris.MediaPlayer2.Player") && g_variant_lookup(ch, "PlaybackStatus", "&s", &st)) {
    for (int i = 0; i < nplsrc; i++) {
      if (strcmp(plsrc[i].owner, sender)) continue;
      gboolean was = plsrc[i].playing;
      plsrc[i].playing = !strcmp(st, "Playing");
      if (plsrc[i].playing && !was && i != plsel) player_use(i);
      else if (nplsrc > 1 && was != plsrc[i].playing) {  // the dots show who plays
        mark_dirty(P_PLAYER);
        mark_dirty(P_MTRACK);
      }
      break;
    }
  }
  g_variant_unref(ch);
}

static void on_name_owner(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                          const gchar *signal, GVariant *params, gpointer d) {
  const char *name;
  g_variant_get(params, "(&s&s&s)", &name, NULL, NULL);
  if (!strncmp(name, "org.mpris.MediaPlayer2.", 23)) players_scan();
}

// ── BlueZ ──────────────────────────────────────────────────────────────────

static GDBusConnection *system_bus;

static void bt_read(void) {
  bt.n = 0;
  bt.have = FALSE;
  if (!bt.om) return;
  GList *objs = g_dbus_object_manager_get_objects(bt.om);
  BtDev tmp[32];
  int nt = 0;
  for (GList *l = objs; l; l = l->next) {
    GDBusObject *ob = l->data;
    GDBusInterface *ad = g_dbus_object_get_interface(ob, "org.bluez.Adapter1");
    if (ad) {
      bt.have = TRUE;
      g_strlcpy(bt.adapter, g_dbus_object_get_object_path(ob), sizeof bt.adapter);
      GVariant *v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(ad), "Powered");
      if (v) { bt.powered = g_variant_get_boolean(v); g_variant_unref(v); }
      g_object_unref(ad);
    }
    GDBusInterface *dv = g_dbus_object_get_interface(ob, "org.bluez.Device1");
    if (dv && nt < 32) {
      GVariant *v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dv), "Paired");
      gboolean paired = v && g_variant_get_boolean(v);
      if (v) g_variant_unref(v);
      if (paired) {
        BtDev *d = &tmp[nt++];
        memset(d, 0, sizeof *d);
        d->battery = -1;
        g_strlcpy(d->path, g_dbus_object_get_object_path(ob), sizeof d->path);
        if ((v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dv), "Alias")) ||
            (v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dv), "Name"))) {
          g_strlcpy(d->name, g_variant_get_string(v, NULL), sizeof d->name);
          g_variant_unref(v);
        }
        if ((v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dv), "Icon"))) {
          g_strlcpy(d->icon, g_variant_get_string(v, NULL), sizeof d->icon);
          g_variant_unref(v);
        }
        if ((v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(dv), "Connected"))) {
          d->connected = g_variant_get_boolean(v);
          g_variant_unref(v);
        }
        GDBusInterface *bat = g_dbus_object_get_interface(ob, "org.bluez.Battery1");
        if (bat) {
          if ((v = g_dbus_proxy_get_cached_property(G_DBUS_PROXY(bat), "Percentage"))) {
            d->battery = g_variant_get_byte(v);
            g_variant_unref(v);
          }
          g_object_unref(bat);
        }
        // keep a busy/error state across refreshes
        for (int k = 0; k < bt.n + NBT && k < NBT; k++)
          if (!strcmp(bt.d[k].path, d->path)) { d->busy = bt.d[k].busy; d->err_until = bt.d[k].err_until; }
      }
    }
    if (dv) g_object_unref(dv);
  }
  g_list_free_full(objs, g_object_unref);
  // connected first, then by name
  for (int i = 0; i < nt; i++)
    for (int j = i + 1; j < nt; j++)
      if ((tmp[j].connected && !tmp[i].connected) ||
          (tmp[j].connected == tmp[i].connected && g_ascii_strcasecmp(tmp[j].name, tmp[i].name) < 0)) {
        BtDev x = tmp[i]; tmp[i] = tmp[j]; tmp[j] = x;
      }
  bt.n = MIN(nt, NBT);
  memcpy(bt.d, tmp, sizeof(BtDev) * bt.n);
  mark_dirty(P_BT);
}

static void bt_changed(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *p, GVariant *ch, GStrv inv,
                       gpointer d) {
  // a property changed: the device is no longer busy
  for (int i = 0; i < bt.n; i++)
    if (!strcmp(bt.d[i].path, g_dbus_object_get_object_path(G_DBUS_OBJECT(o)))) bt.d[i].busy = FALSE;
  bt_read();
}

static void bt_obj(GDBusObjectManager *m, GDBusObject *o, gpointer d) { bt_read(); }

static void bt_call_done(GObject *src, GAsyncResult *res, gpointer data) {
  char *path = data;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  for (int i = 0; i < bt.n; i++)
    if (!strcmp(bt.d[i].path, path)) {
      bt.d[i].busy = FALSE;
      if (err) bt.d[i].err_until = now_s() + 3;
    }
  if (r) g_variant_unref(r);
  if (err) g_error_free(err);
  g_free(path);
  mark_dirty(P_BT);
}

static void bt_device_toggle(int i) {
  if (i >= bt.n || !bt.powered || bt.d[i].busy) return;
  BtDev *d = &bt.d[i];
  d->busy = TRUE;
  g_dbus_connection_call(system_bus, "org.bluez", d->path, "org.bluez.Device1",
                         d->connected ? "Disconnect" : "Connect", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 20000, NULL,
                         bt_call_done, g_strdup(d->path));
  mark_dirty(P_BT);
  for (guint k = 0; k < outs->len; k++) out_redraw(g_ptr_array_index(outs, k));
}

static void bt_toggle_power(void) {
  if (!bt.have) return;
  g_dbus_connection_call(system_bus, "org.bluez", bt.adapter, "org.freedesktop.DBus.Properties", "Set",
                         g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(!bt.powered)),
                         NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
  bt.powered = !bt.powered;  // optimistic; the signal confirms
  mark_dirty(P_BT);
}

static void refresh_on_open(void) {
  volume_read();
  notes_read();
  players_scan();
  bt_read();
}

// ── niri event stream ──────────────────────────────────────────────────────

static GHashTable *ws_output;  // workspace id -> output name
static GDataInputStream *ev_in;

static void ev_next(void);

static void ev_handle(JsonObject *ev) {
  if (json_object_has_member(ev, "WorkspacesChanged")) {
    JsonArray *a = json_object_get_array_member(json_object_get_object_member(ev, "WorkspacesChanged"), "workspaces");
    g_hash_table_remove_all(ws_output);
    for (guint i = 0; i < json_array_get_length(a); i++) {
      JsonObject *w = json_array_get_object_element(a, i);
      if (json_object_get_null_member(w, "output")) continue;
      const char *out = json_object_get_string_member(w, "output");
      gint64 id = json_object_get_int_member(w, "id");
      g_hash_table_insert(ws_output, GSIZE_TO_POINTER((gsize)id), g_strdup(out));
      if (json_object_get_boolean_member(w, "is_focused")) g_strlcpy(focused_output, out, sizeof focused_output);
    }
  } else if (json_object_has_member(ev, "WorkspaceActivated")) {
    JsonObject *o = json_object_get_object_member(ev, "WorkspaceActivated");
    if (json_object_get_boolean_member(o, "focused")) {
      const char *out = g_hash_table_lookup(ws_output, GSIZE_TO_POINTER((gsize)json_object_get_int_member(o, "id")));
      // while the overview is open it stays on its output (the patch); don't follow focus
      if (out && !overview_open) g_strlcpy(focused_output, out, sizeof focused_output);
    }
  } else if (json_object_has_member(ev, "OverviewOpenedOrClosed")) {
    overview_open = json_object_get_boolean_member(json_object_get_object_member(ev, "OverviewOpenedOrClosed"), "is_open");
    outs_sync();
  }
}

static void ev_line(GObject *src, GAsyncResult *res, gpointer data) {
  gsize len;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, NULL);
  if (!line) {
    g_warning("midve-hud: lost the niri event stream");
    gtk_main_quit();
    return;
  }
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, line, len, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (JSON_NODE_HOLDS_OBJECT(root)) ev_handle(json_node_get_object(root));
  }
  g_object_unref(p);
  g_free(line);
  ev_next();
}

static void ev_next(void) { g_data_input_stream_read_line_async(ev_in, G_PRIORITY_DEFAULT, NULL, ev_line, NULL); }

// ── Outputs ────────────────────────────────────────────────────────────────

// niri output names by monitor model (GTK3 knows the model, niri the connector).
static GHashTable *model_to_name;

static void outputs_query(void) {
  GSocketConnection *c = niri_connect();
  if (!c) return;
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(c));
  g_output_stream_write_all(out, "\"Outputs\"\n", 10, NULL, NULL, NULL);
  GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(c)));
  char *line = g_data_input_stream_read_line(in, NULL, NULL, NULL);
  if (line) {
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, line, -1, NULL)) {
      JsonObject *r = json_node_get_object(json_parser_get_root(p));
      JsonObject *ok = json_object_get_object_member(r, "Ok");
      JsonObject *outs_o = ok ? json_object_get_object_member(ok, "Outputs") : NULL;
      if (outs_o) {
        GList *keys = json_object_get_members(outs_o);
        for (GList *k = keys; k; k = k->next) {
          JsonObject *o = json_object_get_object_member(outs_o, k->data);
          const char *model = json_object_get_string_member(o, "model");
          g_hash_table_insert(model_to_name, g_strdup(model), g_strdup(k->data));
        }
        g_list_free(keys);
      }
    }
    g_object_unref(p);
    g_free(line);
  }
  g_object_unref(in);
  g_object_unref(c);
}

static void out_size(GtkWidget *w, GdkRectangle *a, gpointer data) {
  Out *o = data;
  if (o->W == a->width && o->H == a->height && o->scale == gtk_widget_get_scale_factor(w)) return;
  for (int i = 0; i < NPANELS; i++) {
    if (o->p[i].content) cairo_surface_destroy(o->p[i].content);
    if (o->p[i].warped) cairo_surface_destroy(o->p[i].warped);
  }
  o->W = a->width;
  o->H = a->height;
  o->scale = gtk_widget_get_scale_factor(w);
  out_layout(o);
}

static void out_add(GdkMonitor *mon) {
  Out *o = g_new0(Out, 1);
  o->mon = mon;
  const char *model = gdk_monitor_get_model(mon);
  const char *name = model ? g_hash_table_lookup(model_to_name, model) : NULL;
  g_strlcpy(o->name, name ? name : (model ? model : "?"), sizeof o->name);
  o->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_layer_init_for_window(GTK_WINDOW(o->win));
  gtk_layer_set_namespace(GTK_WINDOW(o->win), "midve-hud");
  gtk_layer_set_layer(GTK_WINDOW(o->win), GTK_LAYER_SHELL_LAYER_BACKGROUND);
  gtk_layer_set_monitor(GTK_WINDOW(o->win), mon);
  for (int e = 0; e < GTK_LAYER_SHELL_EDGE_ENTRY_NUMBER; e++) gtk_layer_set_anchor(GTK_WINDOW(o->win), e, TRUE);
  gtk_layer_set_exclusive_zone(GTK_WINDOW(o->win), -1);
  gtk_layer_set_keyboard_mode(GTK_WINDOW(o->win), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
  gtk_widget_set_app_paintable(o->win, TRUE);
  GdkScreen *sc = gtk_widget_get_screen(o->win);
  GdkVisual *vis = gdk_screen_get_rgba_visual(sc);
  if (vis) gtk_widget_set_visual(o->win, vis);
  o->area = gtk_drawing_area_new();
  gtk_widget_add_events(o->area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                                     GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(o->area, "draw", G_CALLBACK(on_draw), o);
  g_signal_connect(o->area, "size-allocate", G_CALLBACK(out_size), o);
  g_signal_connect(o->area, "button-press-event", G_CALLBACK(on_press), o);
  g_signal_connect(o->area, "button-release-event", G_CALLBACK(on_release), o);
  g_signal_connect(o->area, "motion-notify-event", G_CALLBACK(on_motion), o);
  g_signal_connect(o->area, "scroll-event", G_CALLBACK(on_scroll), o);
  g_signal_connect(o->area, "leave-notify-event", G_CALLBACK(on_leave), o);
  gtk_container_add(GTK_CONTAINER(o->win), o->area);
  o->press_panel = -1;
  o->hov_panel = -1;
  g_ptr_array_add(outs, o);
  // Mapped for good: niri shows it only in the overview (faded with its progress).
  gtk_widget_show_all(o->win);
}

// SIGUSR1 toggles system mode on the output where the overview is open (handy for a bind,
// and for testing without a pointer).
static gboolean on_usr1(gpointer d) {
  for (guint i = 0; i < outs->len; i++) {
    Out *o = g_ptr_array_index(outs, i);
    if (o->shown) out_set_sys(o, !o->sys_mode);
  }
  return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
  if (getenv("MIDVE_HUD_SLOW")) anim_slow = fmax(1, atof(getenv("MIDVE_HUD_SLOW")));
  gtk_init(&argc, &argv);
  prof = g_getenv("MIDVE_HUD_PROF") != NULL;
  // On a niri without the patch the HUD would sit in the backdrop without input; stay out.
  if (!niri_has_patch()) {
    g_printerr("midve-hud: this niri doesn't have the overview HUD patch; exiting\n");
    return 0;
  }
  outs = g_ptr_array_new();
  ws_output = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  model_to_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  dismissed = g_hash_table_new(g_direct_hash, g_direct_equal);
  note_time = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  vol.pending = -1;
  nvml_load();
  sys_sample();

  outputs_query();
  GdkDisplay *dpy = gdk_display_get_default();
  for (int i = 0; i < gdk_display_get_n_monitors(dpy); i++) out_add(gdk_display_get_monitor(dpy, i));

  session = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
  if (session) notes_watch();
  if (session)
    g_dbus_connection_signal_subscribe(session, "org.freedesktop.DBus", "org.freedesktop.DBus",
                                       "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner, NULL, NULL);
  if (session)
    g_dbus_connection_signal_subscribe(session, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                       "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_any_player_props, NULL, NULL);
  if (system_bus) {
    bt.om = g_dbus_object_manager_client_new_sync(system_bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez",
                                                  "/", NULL, NULL, NULL, NULL, NULL);
    if (bt.om) {
      g_signal_connect(bt.om, "interface-proxy-properties-changed", G_CALLBACK(bt_changed), NULL);
      g_signal_connect(bt.om, "object-added", G_CALLBACK(bt_obj), NULL);
      g_signal_connect(bt.om, "object-removed", G_CALLBACK(bt_obj), NULL);
    }
  }

  GSocketConnection *ev = niri_connect();
  if (!ev) {
    g_printerr("midve-hud: can't reach niri ($NIRI_SOCKET)\n");
    return 1;
  }
  g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(ev)), "\"EventStream\"\n", 14, NULL, NULL,
                            NULL);
  ev_in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(ev)));
  ev_next();
  g_timeout_add_seconds(1, every_second, NULL);
  g_timeout_add(100, every_tenth, NULL);
  g_unix_signal_add(SIGUSR1, on_usr1, NULL);
  gtk_main();
  return 0;
}
