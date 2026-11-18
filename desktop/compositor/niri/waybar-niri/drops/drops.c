// niri-drops: a waybar CFFI module that shows niri workspaces as dots, with
// the active one a glossy lilac capsule that moves like a falling drop.
//
// On a switch the capsule gathers into a drop and slides to the new dot. As
// it lands it stands up a little, squashes flat, settles with a slight
// overshoot, and a ring spreads out from it as on water. Dots it passes fade
// under it.
//
// Nothing restarts when you switch again mid-way: the position runs on a
// critically damped spring whose target just moves (speed carries over), the
// shape eases towards the drop while it travels, and the landing plays only
// once the drop has actually arrived. Flick through workspaces and the drop
// glides from one to the next.
//
// Variant "C" of claude.ai/artifact/1iqnGcYkmp2LEH4zzFecKF; the stretch,
// squash and settle order follows the water_drop_nav_bar package.
//
// It listens to niri's event stream ($NIRI_SOCKET) and draws only while the
// drop is moving; at rest it costs nothing. Click a dot to go to it.
//
// Config: "module_path": "<path to niri-drops.so>", "output": "DP-1".

#include "waybar_cffi_module.h"

#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

const size_t wbcffi_version = 2;

// Geometry, in logical px.
#define PITCH 16.0  // distance between dot centres
#define RD 3.0      // dot radius (6px dots)
#define R 3.5       // capsule half height (7px tall)
#define HW 9.0      // capsule half width (18px wide)
#define EDGE 7.0    // room left and right for the capsule, its glow and the ring
#define SPRING_K 420.0  // position spring (unit mass, critically damped)
#define LAND 0.30       // seconds: stand up, squash, settle
#define RING 0.26       // seconds the ring takes to spread
#define MAXWS 32

typedef struct {
  guint64 id;
  int idx;
  gboolean active, empty, urgent;
} Ws;

typedef struct {
  GtkWidget *area;
  char *output;
  Ws ws[MAXWS];
  int n;
  double x, v, target;  // position (px) and velocity
  double hw, hh;        // current half width and height of the drop
  double hw0, hh0;      // its shape when the landing began
  double land;          // landing progress 0..1, or -1
  double ring, ring_x;  // ring progress 0..1 (or -1) and where it spreads
  gboolean moving, placed;
  gint64 last_us;
  int to;
  guint tick;
  int hover;
  GSocketConnection *conn;
  GDataInputStream *in;
  GCancellable *cancel;
} Mod;

static double x0_(void) { return EDGE + HW; }
static double centre(double i) { return x0_() + i * PITCH; }
static int width_for(int n) { return (int)ceil(2 * x0_() + (n > 0 ? n - 1 : 0) * PITCH); }

static int active_slot(Mod *m) {
  for (int i = 0; i < m->n; i++)
    if (m->ws[i].active) return i;
  return -1;
}

static double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static double ss(double e0, double e1, double x) {
  double t = clamp01((x - e0) / (e1 - e0));
  return t * t * (3 - 2 * t);
}

static double out_back(double t) {
  const double c = 1.4;
  return 1 + (c + 1) * pow(t - 1, 3) + c * pow(t - 1, 2);
}

// Rounded box, corner radius the smaller half side: a capsule of any proportions.
static double sd_box(double px, double py, double x, double y, double hw, double hh) {
  double r = fmin(hw, hh);
  double qx = fabs(px - x) - (hw - r), qy = fabs(py - y) - (hh - r);
  return hypot(fmax(qx, 0), fmax(qy, 0)) + fmin(fmax(qx, qy), 0) - r;
}

// ── Drawing ────────────────────────────────────────────────────────────────

static void dot_colour(Mod *m, int j, double *r, double *g, double *b, double *a) {
  const Ws *ws = &m->ws[j];
  if (ws->urgent) { *r = 0.769; *g = 0.416; *b = 0.502; *a = 1.0; return; }
  *r = 0.925; *g = 0.878; *b = 0.933;
  *a = j == m->hover ? 0.60 : ws->empty ? 0.15 : 0.35;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
  Mod *m = data;
  int W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
  int s = gtk_widget_get_scale_factor(w);
  if (W <= 0 || H <= 0 || m->n == 0) return FALSE;
  double cy = H / 2.0;
  gboolean has = m->placed && m->to >= 0 && m->to < m->n;
  int Wd = W * s;

  double x = m->x, hw = m->hw, hh = m->hh, ring = m->ring, xring = m->ring_x;
  double yd = cy + (R - hh) * 0.4;  // it sits on the line as it squashes

  double dr[MAXWS], dg[MAXWS], db[MAXWS], da[MAXWS];
  for (int j = 0; j < m->n; j++) {
    dot_colour(m, j, &dr[j], &dg[j], &db[j], &da[j]);
    if (has) da[j] *= clamp01((fabs(centre(j) - x) - hw - RD + 2) / 4);  // fade under the drop
  }

  cairo_surface_t *img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, Wd, H * s);
  unsigned char *pix = cairo_image_surface_get_data(img);
  int stride = cairo_image_surface_get_stride(img);
  cairo_surface_flush(img);

  int y_lo = (int)floor((cy - 9) * s), y_hi = (int)ceil((cy + 9) * s);
  if (y_lo < 0) y_lo = 0;
  if (y_hi > H * s) y_hi = H * s;

  for (int yy = y_lo; yy < y_hi; yy++) {
    double py = (yy + 0.5) / s;
    guint32 *row = (guint32 *)(pix + yy * stride);
    // lilac: light on top, deeper below, with a soft light along the top
    double tt = clamp01((py - (cy - R)) / (2 * R));
    double lr = 0.99 + (0.80 - 0.99) * tt, lg = 0.96 + (0.67 - 0.96) * tt, lb = 0.99 + (0.86 - 0.99) * tt;
    double band = clamp01(1.0 - fabs(py - (cy - R * 0.45)) / 0.9);
    for (int xx = 0; xx < Wd; xx++) {
      double px = (xx + 0.5) / s;
      double A = 0, Cr = 0, Cg = 0, Cb = 0;  // premultiplied

      int j = (int)floor((px - x0_()) / PITCH + 0.5);
      if (j >= 0 && j < m->n && da[j] > 0) {
        double c = clamp01(0.5 - (hypot(px - centre(j), py - cy) - RD) * s) * da[j];
        A = c; Cr = dr[j] * c; Cg = dg[j] * c; Cb = db[j] * c;
      }
      if (has) {
        double d = sd_box(px, py, x, yd, hw, hh);
        double cov = clamp01(0.5 - d * s);
        double g = d > 0 ? 0.2 * exp(-d / 2) * (1 - cov) : 0;  // glow
        if (ring >= 0) {                                         // the ring on landing
          double rr = 5 + ring * 10;
          double dd = fabs(hypot((px - xring) * 0.55, py - cy) - rr * 0.55);
          g = fmax(g, (1 - ring) * 0.55 * clamp01(1 - dd * 1.6));
        }
        if (g > 0) {
          Cr = Cr * (1 - g) + 0.86 * g; Cg = Cg * (1 - g) + 0.76 * g; Cb = Cb * (1 - g) + 0.89 * g;
          A = A * (1 - g) + g;
        }
        if (cov > 0) {
          double hl = band * 0.35;
          double r = lr + (1 - lr) * hl, gg = lg + (1 - lg) * hl, bb = lb + (1 - lb) * hl;
          Cr = Cr * (1 - cov) + r * cov; Cg = Cg * (1 - cov) + gg * cov; Cb = Cb * (1 - cov) + bb * cov;
          A = A * (1 - cov) + cov;
        }
      }
      row[xx] = ((guint32)(A * 255 + 0.5) << 24) | ((guint32)(Cr * 255 + 0.5) << 16) |
                ((guint32)(Cg * 255 + 0.5) << 8) | (guint32)(Cb * 255 + 0.5);
    }
  }
  cairo_surface_mark_dirty(img);
  cairo_surface_set_device_scale(img, s, s);
  cairo_set_source_surface(cr, img, 0, 0);
  cairo_paint(cr);
  cairo_surface_destroy(img);
  return FALSE;
}

// ── Motion ─────────────────────────────────────────────────────────────────

static gboolean on_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
  Mod *m = data;
  gint64 now = gdk_frame_clock_get_frame_time(clock);
  double dt = m->last_us ? (now - m->last_us) / 1e6 : 1.0 / 120;
  if (dt > 0.05) dt = 0.05;
  m->last_us = now;

  // position: a critically damped spring, so a new target never restarts it
  const double c = 2 * sqrt(SPRING_K);
  for (double t = 0; t < dt; t += 0.001) {
    double h = fmin(0.001, dt - t);
    m->v += (-SPRING_K * (m->x - m->target) - c * m->v) * h;
    m->x += m->v * h;
  }

  if (m->moving) {
    // travelling: ease the shape towards the round drop
    double a = 1 - exp(-dt / 0.06);
    m->hw += (4.6 - m->hw) * a;
    m->hh += (4.2 - m->hh) * a;
    // close enough: land now rather than wait out the spring's slow last pixels
    if (fabs(m->x - m->target) < 2.0) {
      m->moving = FALSE;
      m->land = 0;
      m->hw0 = m->hw; m->hh0 = m->hh;
    }
  } else if (m->land >= 0) {
    m->land = fmin(1, m->land + dt / LAND);
    double L = m->land, hw = m->hw0, hh = m->hh0;
    double k = ss(0, 0.25, L);           // stands up
    hw += (3.0 - hw) * k; hh += (5.2 - hh) * k;
    k = ss(0.25, 0.5, L);                // squashes flat
    hw += (11.5 - hw) * k; hh += (2.6 - hh) * k;
    if (L > 0.5) {                       // settles
      k = out_back((L - 0.5) / 0.5);
      hw = 11.5 + (HW - 11.5) * k; hh = 2.6 + (R - 2.6) * k;
    }
    m->hw = hw; m->hh = hh;
    if (L >= 0.2 && m->ring < 0) { m->ring = 0; m->ring_x = m->target; }
    if (L >= 1) { m->land = -1; m->hw = HW; m->hh = R; m->x = m->target; m->v = 0; }
  }
  if (m->ring >= 0) {
    m->ring += dt / RING;
    if (m->ring >= 1) m->ring = -1;
  }

  gtk_widget_queue_draw(w);
  if (!m->moving && m->land < 0 && m->ring < 0) {
    m->tick = 0;
    m->last_us = 0;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void retarget(Mod *m) {
  int s = active_slot(m);
  gtk_widget_set_size_request(m->area, width_for(m->n), -1);
  if (s < 0) { gtk_widget_queue_draw(m->area); return; }
  if (!m->placed) {
    m->to = s;
    m->x = m->target = centre(s); m->v = 0;
    m->hw = HW; m->hh = R; m->land = m->ring = -1;
    m->placed = TRUE;
    gtk_widget_queue_draw(m->area);
    return;
  }
  if (s == m->to) { gtk_widget_queue_draw(m->area); return; }
  m->to = s;
  m->target = centre(s);
  m->moving = TRUE;
  m->land = -1;  // a landing in progress gives way; the shape eases back into a drop
  if (!m->tick) {
    m->last_us = 0;
    m->tick = gtk_widget_add_tick_callback(m->area, on_tick, m, NULL);
  }
}

// ── niri IPC ───────────────────────────────────────────────────────────────

static int cmp_ws(const void *x, const void *y) {
  return ((const Ws *)x)->idx - ((const Ws *)y)->idx;
}

static Ws *find(Mod *m, guint64 id) {
  for (int i = 0; i < m->n; i++)
    if (m->ws[i].id == id) return &m->ws[i];
  return NULL;
}

static void handle_event(Mod *m, JsonObject *ev) {
  if (json_object_has_member(ev, "WorkspacesChanged")) {
    JsonArray *arr = json_object_get_array_member(
        json_object_get_object_member(ev, "WorkspacesChanged"), "workspaces");
    m->n = 0;
    for (guint i = 0; i < json_array_get_length(arr) && m->n < MAXWS; i++) {
      JsonObject *o = json_array_get_object_element(arr, i);
      const char *out = json_object_get_null_member(o, "output") ? NULL
                                                                 : json_object_get_string_member(o, "output");
      if (m->output && (!out || strcmp(out, m->output) != 0)) continue;
      Ws *w = &m->ws[m->n++];
      w->id = json_object_get_int_member(o, "id");
      w->idx = json_object_get_int_member(o, "idx");
      w->active = json_object_get_boolean_member(o, "is_active");
      w->urgent = json_object_get_boolean_member(o, "is_urgent");
      w->empty = json_object_get_null_member(o, "active_window_id");
    }
    qsort(m->ws, m->n, sizeof(Ws), cmp_ws);
    retarget(m);
  } else if (json_object_has_member(ev, "WorkspaceActivated")) {
    JsonObject *o = json_object_get_object_member(ev, "WorkspaceActivated");
    Ws *w = find(m, json_object_get_int_member(o, "id"));
    if (!w) return;  // another output
    for (int i = 0; i < m->n; i++) m->ws[i].active = FALSE;
    w->active = TRUE;
    retarget(m);
  } else if (json_object_has_member(ev, "WorkspaceActiveWindowChanged")) {
    JsonObject *o = json_object_get_object_member(ev, "WorkspaceActiveWindowChanged");
    Ws *w = find(m, json_object_get_int_member(o, "workspace_id"));
    if (!w) return;
    w->empty = json_object_get_null_member(o, "active_window_id");
    gtk_widget_queue_draw(m->area);
  } else if (json_object_has_member(ev, "WorkspaceUrgencyChanged")) {
    JsonObject *o = json_object_get_object_member(ev, "WorkspaceUrgencyChanged");
    Ws *w = find(m, json_object_get_int_member(o, "id"));
    if (!w) return;
    w->urgent = json_object_get_boolean_member(o, "urgent");
    gtk_widget_queue_draw(m->area);
  }
}

static void read_next(Mod *m);

static void on_line(GObject *src, GAsyncResult *res, gpointer data) {
  GError *err = NULL;
  gsize len;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &err);
  if (!line) {
    if (err) {
      if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning("niri-drops: event stream: %s", err->message);
      g_error_free(err);
    }
    return;
  }
  Mod *m = data;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, line, len, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    if (JSON_NODE_HOLDS_OBJECT(root)) handle_event(m, json_node_get_object(root));
  }
  g_object_unref(p);
  g_free(line);
  read_next(m);
}

static void read_next(Mod *m) {
  g_data_input_stream_read_line_async(m->in, G_PRIORITY_DEFAULT, m->cancel, on_line, m);
}

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

// ── Input ──────────────────────────────────────────────────────────────────

static int slot_at(Mod *m, double x) {
  int i = (int)floor((x - x0_()) / PITCH + 0.5);
  return i >= 0 && i < m->n ? i : -1;
}

static gboolean on_press(GtkWidget *w, GdkEventButton *e, gpointer data) {
  Mod *m = data;
  if (e->button != 1 || e->type != GDK_BUTTON_PRESS) return FALSE;
  int i = slot_at(m, e->x);
  if (i < 0) return FALSE;
  char req[128];
  g_snprintf(req, sizeof req, "{\"Action\":{\"FocusWorkspace\":{\"reference\":{\"Id\":%" G_GUINT64_FORMAT "}}}}",
             m->ws[i].id);
  niri_request(req);
  return TRUE;
}

static gboolean on_motion(GtkWidget *w, GdkEventMotion *e, gpointer data) {
  Mod *m = data;
  int i = slot_at(m, e->x);
  if (i >= 0 && m->ws[i].active) i = -1;
  if (i != m->hover) { m->hover = i; gtk_widget_queue_draw(w); }
  return FALSE;
}

static gboolean on_leave(GtkWidget *w, GdkEventCrossing *e, gpointer data) {
  Mod *m = data;
  if (m->hover != -1) { m->hover = -1; gtk_widget_queue_draw(w); }
  return FALSE;
}

static void on_realize(GtkWidget *w, gpointer data) {
  GdkCursor *c = gdk_cursor_new_from_name(gtk_widget_get_display(w), "pointer");
  gdk_window_set_cursor(gtk_widget_get_window(w), c);
  if (c) g_object_unref(c);
}

// ── CFFI entry points ──────────────────────────────────────────────────────

void *wbcffi_init(const wbcffi_init_info *info, const wbcffi_config_entry *cfg, size_t n) {
  Mod *m = g_new0(Mod, 1);
  m->hover = -1;
  m->to = -1;
  for (size_t i = 0; i < n; i++) {
    if (strcmp(cfg[i].key, "output") == 0) {
      // ABI 2: values are JSON
      JsonParser *p = json_parser_new();
      if (json_parser_load_from_data(p, cfg[i].value, -1, NULL) &&
          JSON_NODE_HOLDS_VALUE(json_parser_get_root(p)))
        m->output = g_strdup(json_node_get_string(json_parser_get_root(p)));
      g_object_unref(p);
    }
  }

  m->area = gtk_drawing_area_new();
  gtk_widget_add_events(m->area, GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(m->area, "draw", G_CALLBACK(on_draw), m);
  g_signal_connect(m->area, "button-press-event", G_CALLBACK(on_press), m);
  g_signal_connect(m->area, "motion-notify-event", G_CALLBACK(on_motion), m);
  g_signal_connect(m->area, "leave-notify-event", G_CALLBACK(on_leave), m);
  g_signal_connect(m->area, "realize", G_CALLBACK(on_realize), m);
  gtk_widget_set_size_request(m->area, width_for(1), -1);
  gtk_container_add(info->get_root_widget(info->obj), m->area);
  gtk_widget_show(m->area);

  m->conn = niri_connect();
  if (!m->conn) {
    g_warning("niri-drops: can't reach niri ($NIRI_SOCKET)");
    return m;
  }
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(m->conn));
  g_output_stream_write_all(out, "\"EventStream\"\n", 14, NULL, NULL, NULL);
  m->in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(m->conn)));
  m->cancel = g_cancellable_new();
  read_next(m);
  return m;
}

void wbcffi_deinit(void *instance) {
  Mod *m = instance;
  if (m->cancel) { g_cancellable_cancel(m->cancel); g_object_unref(m->cancel); }
  if (m->tick) gtk_widget_remove_tick_callback(m->area, m->tick);
  if (m->in) g_object_unref(m->in);
  if (m->conn) g_object_unref(m->conn);
  g_free(m->output);
  g_free(m);
}
