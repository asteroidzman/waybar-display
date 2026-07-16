// waybar CFFI plugin: asteroidz display/monitor configuration (multi-monitor).
//  - Bar pill: monitor icon + active output's resolution·refresh.
//  - Click opens a popup with:
//      * a draggable layout canvas — arrange the monitors' positions (snaps to
//        neighbouring edges); click a monitor to select it,
//      * per-monitor controls for the selected output: resolution + refresh,
//        scale, VRR, HDR, HDR max/min/max-fall luminances, X/Y position,
//      * a global SDR reference-luminance slider (applied live).
//  - State from `amsg get all-monitors` (per-monitor incl. the modes array and
//    logical x/y). HDR toggle + SDR luminance apply live via `amsg dispatch`.
//    Resolution/scale/VRR/HDR-luminance/position are written to the plugin-owned
//    monitors.kdl (one `output` block per monitor, sourced by config.kdl) and
//    applied with `reload_config`, which re-modesets + re-lays-out live.
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "wbcommon.h"
#include "waybar_cffi_module.h"
const size_t wbcffi_version = 1;

typedef struct { int w, h, refresh_mhz; } Mode;

typedef struct {
  char name[64];
  int active;
  int lx, ly, lw, lh;      // logical rect (position + size) reported now
  int cur_w, cur_h, cur_refresh_mhz;
  double scale;
  int hdr, hdr_capable, vrr, vrr_capable;
  double hdr_max, hdr_min, hdr_fall;
  int bitdepth;
  char icc[512];
  GArray *modes;           // Mode[]
  // edited values (widgets / canvas write here; Apply serializes these)
  int sel_w, sel_h, sel_mhz, sel_x, sel_y, sel_vrr, sel_hdr;
  double sel_scale, sel_hmax, sel_hmin, sel_hfall;
} Mon;

typedef struct {
  GtkWidget *box, *bar_area;   // bar pill: a mini layout drawing (active highlighted)
  guint poll_id;               // refresh the mini layout on hotplug
  WbPop pop;
  char *icon_dir; int icon_size;
  char *monitors_kdl;
  int sdr_lum;             // global reference luminance
  GArray *mons;            // Mon[]
  int sel;                 // selected monitor index

  GtkWidget *canvas, *w_res, *w_refresh, *w_scale, *w_vrr, *w_hdr, *w_sdr,
            *w_hmax, *w_hmin, *w_hfall, *w_posx, *w_posy, *w_status, *w_montitle;
  int loading;
  int drag;                // dragging a monitor on the canvas?
  int drag_ox, drag_oy;    // grab offset within the monitor (real coords)
} Inst;

// ─── amsg ────────────────────────────────────────────────────────────────────
static char *amsg_out(const char *args) {
  char *cmd = g_strdup_printf("amsg %s", args);
  char *out = NULL;
  g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL);
  g_free(cmd);
  return out;
}
static void amsg_dispatch(const char *verb) {
  const char *argv[] = {"amsg", "dispatch", verb, NULL};
  GSubprocess *sp = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                            G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (sp) g_object_unref(sp);
}
static int jint(JsonObject *o, const char *k, int def) {
  return json_object_has_member(o, k) ? (int)json_object_get_int_member(o, k) : def;
}
static double jdbl(JsonObject *o, const char *k, double def) {
  return json_object_has_member(o, k) ? json_object_get_double_member(o, k) : def;
}
static int jbool(JsonObject *o, const char *k) {
  return json_object_has_member(o, k) && json_object_get_boolean_member(o, k);
}
static const char *jstr(JsonObject *o, const char *k) {
  return (json_object_has_member(o, k) &&
          !JSON_NODE_HOLDS_NULL(json_object_get_member(o, k)))
             ? json_object_get_string_member(o, k) : "";
}

static void mons_clear(Inst *self) {
  if (!self->mons) { self->mons = g_array_new(FALSE, TRUE, sizeof(Mon)); return; }
  for (guint i = 0; i < self->mons->len; i++) {
    Mon *m = &g_array_index(self->mons, Mon, i);
    if (m->modes) g_array_free(m->modes, TRUE);
  }
  g_array_set_size(self->mons, 0);
}

static void read_all(Inst *self) {
  mons_clear(self);
  char *out = amsg_out("get all-monitors");
  if (!out) return;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, out, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    JsonArray *arr = JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root)
        : json_object_get_array_member(json_node_get_object(root), "monitors");
    guint n = arr ? json_array_get_length(arr) : 0;
    for (guint i = 0; i < n; i++) {
      JsonObject *o = json_array_get_object_element(arr, i);
      Mon m; memset(&m, 0, sizeof m);
      g_strlcpy(m.name, jstr(o, "name"), sizeof m.name);
      m.active = jbool(o, "active");
      m.lx = jint(o, "x", 0); m.ly = jint(o, "y", 0);
      m.lw = jint(o, "width", 0); m.lh = jint(o, "height", 0);
      m.cur_w = jint(o, "mode_width", 0);
      m.cur_h = jint(o, "mode_height", 0);
      m.cur_refresh_mhz = jint(o, "mode_refresh", 0);
      m.scale = jdbl(o, "scale", 1.0);
      m.hdr = jbool(o, "hdr_enabled"); m.hdr_capable = jbool(o, "hdr_capable");
      m.vrr = jbool(o, "vrr_enabled"); m.vrr_capable = jbool(o, "vrr_capable");
      m.hdr_max = jdbl(o, "hdr_max_luminance", 0);
      m.hdr_min = jdbl(o, "hdr_min_luminance", 0);
      m.hdr_fall = jdbl(o, "hdr_max_fall", 0);
      m.bitdepth = jint(o, "bitdepth", 0);
      g_strlcpy(m.icc, jstr(o, "icc_profile"), sizeof m.icc);
      if (i == 0) self->sdr_lum = jint(o, "sdr_luminance", 203);
      m.modes = g_array_new(FALSE, FALSE, sizeof(Mode));
      if (json_object_has_member(o, "modes")) {
        JsonArray *ms = json_object_get_array_member(o, "modes");
        for (guint j = 0; j < json_array_get_length(ms); j++) {
          JsonObject *mm = json_array_get_object_element(ms, j);
          Mode md = { jint(mm, "width", 0), jint(mm, "height", 0), jint(mm, "refresh", 0) };
          if (md.w > 0 && md.h > 0) g_array_append_val(m.modes, md);
        }
      }
      // seed edits from current
      m.sel_w = m.cur_w; m.sel_h = m.cur_h; m.sel_mhz = m.cur_refresh_mhz;
      m.sel_x = m.lx; m.sel_y = m.ly; m.sel_scale = m.scale;
      m.sel_vrr = m.vrr; m.sel_hdr = m.hdr;
      m.sel_hmax = m.hdr_max; m.sel_hmin = m.hdr_min; m.sel_hfall = m.hdr_fall;
      g_array_append_val(self->mons, m);
    }
  }
  g_object_unref(p);
  g_free(out);
  if (self->sel >= (int)self->mons->len) self->sel = 0;
}

static Mon *cur_mon(Inst *self) {
  if (!self->mons || self->sel >= (int)self->mons->len) return NULL;
  return &g_array_index(self->mons, Mon, self->sel);
}

// Pango layout using the widget's interface font (CSS "font", i.e. Ubuntu).
static PangoLayout *themed_layout(GtkWidget *wdg, cairo_t *cr) {
  PangoLayout *pl = pango_cairo_create_layout(cr);
  PangoFontDescription *fd = NULL;
  gtk_style_context_get(gtk_widget_get_style_context(wdg),
                        gtk_widget_get_state_flags(wdg), "font", &fd, NULL);
  if (fd) { pango_layout_set_font_description(pl, fd); pango_font_description_free(fd); }
  return pl;
}

// ─── bar pill: mini layout of the outputs, active one highlighted ─────────────
static gboolean bar_draw(GtkWidget *wdg, cairo_t *cr, gpointer d) {
  Inst *self = d;
  if (!self->mons || !self->mons->len) return FALSE;
  int cw = gtk_widget_get_allocated_width(wdg), ch = gtk_widget_get_allocated_height(wdg);
  GdkRGBA accent;
  gtk_style_context_get_color(gtk_widget_get_style_context(wdg),
                              gtk_widget_get_state_flags(wdg), &accent);
  GdkRGBA dim = { accent.red, accent.green, accent.blue, 0.45 };
  // bbox of current logical rects
  int minx = INT32_MAX, miny = INT32_MAX, maxx = INT32_MIN, maxy = INT32_MIN;
  for (guint i = 0; i < self->mons->len; i++) {
    Mon *m = &g_array_index(self->mons, Mon, i);
    int w = m->lw > 0 ? m->lw : 1, h = m->lh > 0 ? m->lh : 1;
    if (m->lx < minx) minx = m->lx;
    if (m->ly < miny) miny = m->ly;
    if (m->lx + w > maxx) maxx = m->lx + w;
    if (m->ly + h > maxy) maxy = m->ly + h;
  }
  double m = 2;   // px padding
  double sx = (cw - 2 * m) / (double)(maxx - minx > 0 ? maxx - minx : 1);
  double sy = (ch - 2 * m) / (double)(maxy - miny > 0 ? maxy - miny : 1);
  double s = sx < sy ? sx : sy;
  double ox = m - minx * s + (cw - 2 * m - (maxx - minx) * s) / 2;
  double oy = m - miny * s + (ch - 2 * m - (maxy - miny) * s) / 2;
  for (guint i = 0; i < self->mons->len; i++) {
    Mon *mon = &g_array_index(self->mons, Mon, i);
    double rx = ox + mon->lx * s, ry = oy + mon->ly * s;
    double rw = (mon->lw > 0 ? mon->lw : 1) * s, rh = (mon->lh > 0 ? mon->lh : 1) * s;
    if (rw < 3) rw = 3;
    if (rh < 3) rh = 3;
    GdkRGBA c = mon->active ? accent : dim;
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, mon->active ? 0.9 : 0.22);
    cairo_rectangle(cr, rx, ry, rw - 1, rh - 1);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, c.red, c.green, c.blue, mon->active ? 1.0 : 0.55);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, rx + 0.5, ry + 0.5, rw - 2, rh - 2);
    cairo_stroke(cr);
  }
  return FALSE;
}
static void update_bar(Inst *self) {
  if (self->bar_area) gtk_widget_queue_draw(self->bar_area);
}

// ─── monitors.kdl writer (all monitors) ──────────────────────────────────────
static void fmt_refresh(int mhz, char *buf, size_t n) {
  double hz = mhz / 1000.0;
  if (fabs(hz - lround(hz)) < 0.05) g_snprintf(buf, n, "%ld", lround(hz));
  else g_snprintf(buf, n, "%.3f", hz);
}
static void write_monitors_kdl(Inst *self) {
  GString *s = g_string_new(
      "// Managed by the waybar-display plugin — rewritten on each change.\n"
      "// One `output` block per monitor; reload_config applies live.\n");
  for (guint i = 0; i < self->mons->len; i++) {
    Mon *m = &g_array_index(self->mons, Mon, i);
    g_string_append_printf(s, "output %s {", m->name);
    if (m->sel_hdr) g_string_append(s, " hdr;");
    g_string_append_printf(s, " scale %.4g;", m->sel_scale);
    if (m->sel_w > 0 && m->sel_h > 0) {
      char rb[32]; fmt_refresh(m->sel_mhz, rb, sizeof rb);
      g_string_append_printf(s, " width %d; height %d; refresh %s;", m->sel_w, m->sel_h, rb);
    }
    g_string_append_printf(s, " x %d; y %d; vrr %d;", m->sel_x, m->sel_y, m->sel_vrr ? 1 : 0);
    if (m->sel_hdr) {
      if (m->bitdepth > 0) g_string_append_printf(s, " bit-depth %d;", m->bitdepth);
      if (m->sel_hmax > 0) g_string_append_printf(s, " max-luminance %.4g;", m->sel_hmax);
      if (m->sel_hmin > 0) g_string_append_printf(s, " min-luminance %.4g;", m->sel_hmin);
      if (m->sel_hfall > 0) g_string_append_printf(s, " max-fall %.4g;", m->sel_hfall);
    }
    if (m->icc[0]) g_string_append_printf(s, " icc-profile \"%s\";", m->icc);
    g_string_append(s, " }\n");
  }
  g_file_set_contents(self->monitors_kdl, s->str, s->len, NULL);
  g_string_free(s, TRUE);
}

static void set_status(Inst *self, const char *msg) {
  if (self->w_status) gtk_label_set_text(GTK_LABEL(self->w_status), msg);
}

// ─── layout canvas ───────────────────────────────────────────────────────────
// effective logical rect of a monitor from its EDITED values (mode/scale),
// falling back to reported logical size.
static void mon_rect(Mon *m, int *x, int *y, int *w, int *h) {
  *x = m->sel_x; *y = m->sel_y;
  if (m->sel_w > 0 && m->sel_scale > 0.01) {
    *w = (int)lround(m->sel_w / m->sel_scale);
    *h = (int)lround(m->sel_h / m->sel_scale);
  } else { *w = m->lw; *h = m->lh; }
}
// world→canvas transform: fit all monitors into the drawing area with margin
static void canvas_xform(Inst *self, int cw, int ch, double *sc, int *ox, int *oy) {
  int minx = INT32_MAX, miny = INT32_MAX, maxx = INT32_MIN, maxy = INT32_MIN;
  for (guint i = 0; i < self->mons->len; i++) {
    int x, y, w, h; mon_rect(&g_array_index(self->mons, Mon, i), &x, &y, &w, &h);
    if (x < minx) minx = x;
    if (y < miny) miny = y;
    if (x + w > maxx) maxx = x + w;
    if (y + h > maxy) maxy = y + h;
  }
  if (minx == INT32_MAX) { *sc = 0.05; *ox = 0; *oy = 0; return; }
  double m = 16;   // canvas padding
  double sx = (cw - 2 * m) / (double)(maxx - minx > 0 ? maxx - minx : 1);
  double sy = (ch - 2 * m) / (double)(maxy - miny > 0 ? maxy - miny : 1);
  double s = sx < sy ? sx : sy;
  if (s > 0.12) s = 0.12;   // don't zoom in too far for a single small setup
  *sc = s;
  *ox = (int)(m - minx * s + (cw - 2 * m - (maxx - minx) * s) / 2);
  *oy = (int)(m - miny * s + (ch - 2 * m - (maxy - miny) * s) / 2);
}

static gboolean canvas_draw(GtkWidget *wdg, cairo_t *cr, gpointer d) {
  Inst *self = d;
  int cw = gtk_widget_get_allocated_width(wdg), ch = gtk_widget_get_allocated_height(wdg);
  GtkStyleContext *ctx = gtk_widget_get_style_context(wdg);
  GdkRGBA accent;   // widget `color` = @primary (CSS): used for the selected fill
  gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &accent);
  GdkRGBA fg = { 0.86, 0.88, 0.93, 1.0 };   // neutral strokes/labels
  if (!self->mons || !self->mons->len) return FALSE;
  double s; int ox, oy; canvas_xform(self, cw, ch, &s, &ox, &oy);
  for (guint i = 0; i < self->mons->len; i++) {
    Mon *m = &g_array_index(self->mons, Mon, i);
    int x, y, w, h; mon_rect(m, &x, &y, &w, &h);
    double rx = ox + x * s, ry = oy + y * s, rw = w * s, rh = h * s;
    int seld = ((int)i == self->sel);
    // fill
    if (seld) cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.30);
    else cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.10);
    cairo_rectangle(cr, rx + 1, ry + 1, rw - 2, rh - 2);
    cairo_fill(cr);
    // border
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, seld ? 0.95 : 0.5);
    cairo_set_line_width(cr, seld ? 2.5 : 1.5);
    cairo_rectangle(cr, rx + 1, ry + 1, rw - 2, rh - 2);
    cairo_stroke(cr);
    // label
    char lbl[80];
    g_snprintf(lbl, sizeof lbl, "%s\n%d×%d", m->name, m->sel_w, m->sel_h);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.95);
    PangoLayout *pl = themed_layout(wdg, cr);
    pango_layout_set_text(pl, lbl, -1);
    pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
    int pw, ph; pango_layout_get_pixel_size(pl, &pw, &ph);
    cairo_move_to(cr, rx + (rw - pw) / 2, ry + (rh - ph) / 2);
    pango_cairo_show_layout(cr, pl);
    g_object_unref(pl);
  }
  return FALSE;
}

static int mon_at(Inst *self, double px, double py) {
  int cw = gtk_widget_get_allocated_width(self->canvas), ch = gtk_widget_get_allocated_height(self->canvas);
  double s; int ox, oy; canvas_xform(self, cw, ch, &s, &ox, &oy);
  for (int i = (int)self->mons->len - 1; i >= 0; i--) {
    int x, y, w, h; mon_rect(&g_array_index(self->mons, Mon, i), &x, &y, &w, &h);
    double rx = ox + x * s, ry = oy + y * s;
    if (px >= rx && px <= rx + w * s && py >= ry && py <= ry + h * s) return i;
  }
  return -1;
}

static void select_mon(Inst *self, int idx);   // fwd

static gboolean canvas_press(GtkWidget *wdg, GdkEventButton *e, gpointer d) {
  Inst *self = d;
  int i = mon_at(self, e->x, e->y);
  if (i < 0) return FALSE;
  select_mon(self, i);
  Mon *m = &g_array_index(self->mons, Mon, i);
  int cw = gtk_widget_get_allocated_width(wdg), ch = gtk_widget_get_allocated_height(wdg);
  double s; int ox, oy; canvas_xform(self, cw, ch, &s, &ox, &oy);
  self->drag = 1;
  self->drag_ox = (int)((e->x - ox) / s) - m->sel_x;   // grab offset in world coords
  self->drag_oy = (int)((e->y - oy) / s) - m->sel_y;
  return TRUE;
}
static gboolean canvas_motion(GtkWidget *wdg, GdkEventMotion *e, gpointer d) {
  Inst *self = d;
  if (!self->drag) return FALSE;
  Mon *m = cur_mon(self); if (!m) return FALSE;
  int cw = gtk_widget_get_allocated_width(wdg), ch = gtk_widget_get_allocated_height(wdg);
  double s; int ox, oy; canvas_xform(self, cw, ch, &s, &ox, &oy);
  m->sel_x = (int)((e->x - ox) / s) - self->drag_ox;
  m->sel_y = (int)((e->y - oy) / s) - self->drag_oy;
  gtk_widget_queue_draw(self->canvas);
  return TRUE;
}
// snap the dragged monitor's edges to neighbours and remove gaps/overlaps
static void snap_selected(Inst *self) {
  Mon *m = cur_mon(self); if (!m) return;
  int mx, my, mw, mh; mon_rect(m, &mx, &my, &mw, &mh);
  const int TH = 120;   // world-coord snap threshold
  int bestdx = TH + 1, bestdy = TH + 1, sdx = 0, sdy = 0;
  for (guint i = 0; i < self->mons->len; i++) {
    if ((int)i == self->sel) continue;
    int ox, oy, ow, oh; mon_rect(&g_array_index(self->mons, Mon, i), &ox, &oy, &ow, &oh);
    // horizontal: snap left-to-right / right-to-left / align-left / align-right
    int cand_x[] = { ox + ow - mx, ox - (mx + mw), ox - mx, (ox + ow) - (mx + mw) };
    for (int k = 0; k < 4; k++) if (abs(cand_x[k]) < abs(bestdx)) { bestdx = cand_x[k]; sdx = 1; }
    int cand_y[] = { oy + oh - my, oy - (my + mh), oy - my, (oy + oh) - (my + mh) };
    for (int k = 0; k < 4; k++) if (abs(cand_y[k]) < abs(bestdy)) { bestdy = cand_y[k]; sdy = 1; }
  }
  if (sdx && abs(bestdx) <= TH) m->sel_x += bestdx;
  if (sdy && abs(bestdy) <= TH) m->sel_y += bestdy;
}
static void refresh_pos_fields(Inst *self);   // fwd
static gboolean canvas_release(GtkWidget *wdg, GdkEventButton *e, gpointer d) {
  (void)wdg; (void)e; Inst *self = d;
  if (!self->drag) return FALSE;
  self->drag = 0;
  snap_selected(self);
  refresh_pos_fields(self);
  gtk_widget_queue_draw(self->canvas);
  return TRUE;
}

// ─── detail controls ─────────────────────────────────────────────────────────
static void on_res_changed(GtkComboBox *c, gpointer d);   // fwd

static void refresh_pos_fields(Inst *self) {
  Mon *m = cur_mon(self); if (!m || !self->w_posx) return;
  self->loading = 1;
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_posx), m->sel_x);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_posy), m->sel_y);
  self->loading = 0;
}

static void on_apply(GtkButton *b, gpointer d) {
  (void)b; Inst *self = d;
  Mon *m = cur_mon(self);
  if (m) {
    // pull the selected mode from the refresh combo (id = index into m->modes)
    const char *rid = gtk_combo_box_get_active_id(GTK_COMBO_BOX(self->w_refresh));
    if (rid) {
      int mi = atoi(rid);
      if (mi >= 0 && (guint)mi < m->modes->len) {
        Mode md = g_array_index(m->modes, Mode, mi);
        m->sel_w = md.w; m->sel_h = md.h; m->sel_mhz = md.refresh_mhz;
      }
    }
    m->sel_scale = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_scale));
    m->sel_vrr = gtk_switch_get_active(GTK_SWITCH(self->w_vrr));
    m->sel_hdr = gtk_switch_get_active(GTK_SWITCH(self->w_hdr));
    m->sel_hmax = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_hmax));
    m->sel_hmin = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_hmin));
    m->sel_hfall = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_hfall));
    m->sel_x = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_posx));
    m->sel_y = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_posy));
  }
  write_monitors_kdl(self);
  amsg_dispatch("reload_config");
  set_status(self, "Applied");
}

static void on_hdr_switch2(GtkSwitch *sw, gboolean state, gpointer d) {
  (void)sw; Inst *self = d; Mon *m = cur_mon(self);
  if (self->loading || !m) return;
  // toggle_hdr acts on the ACTIVE output; only fire live if this is it.
  if (m->active && state != m->hdr) amsg_dispatch("toggle_hdr");
  m->sel_hdr = state;
}
static void on_sdr_changed(GtkRange *r, gpointer d) {
  Inst *self = d;
  if (self->loading) return;
  int v = (int)lround(gtk_range_get_value(r));
  char verb[48]; g_snprintf(verb, sizeof verb, "set_sdr_luminance,%d", v);
  amsg_dispatch(verb); self->sdr_lum = v;
}
static void on_posxy_changed(GtkSpinButton *sp, gpointer d) {
  (void)sp; Inst *self = d; Mon *m = cur_mon(self);
  if (self->loading || !m) return;
  m->sel_x = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_posx));
  m->sel_y = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_posy));
  gtk_widget_queue_draw(self->canvas);
}
static void on_res_changed(GtkComboBox *c, gpointer d) {
  Inst *self = d; Mon *m = cur_mon(self);
  if (self->loading || !m || !self->w_refresh) return;
  const char *res = gtk_combo_box_get_active_id(c);
  if (!res) return;
  int rw = 0, rh = 0; sscanf(res, "%dx%d", &rw, &rh);
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(self->w_refresh));
  int best = -1; double best_hz = -1; int chose = 0;
  for (guint i = 0; i < m->modes->len; i++) {
    Mode md = g_array_index(m->modes, Mode, i);
    if (md.w != rw || md.h != rh) continue;
    char id[16], lbl[32], rb[24];
    g_snprintf(id, sizeof id, "%u", i);
    fmt_refresh(md.refresh_mhz, rb, sizeof rb);
    g_snprintf(lbl, sizeof lbl, "%s Hz", rb);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->w_refresh), id, lbl);
    if (md.refresh_mhz / 1000.0 > best_hz) { best_hz = md.refresh_mhz / 1000.0; best = i; }
    if (rw == m->sel_w && rh == m->sel_h && abs(md.refresh_mhz - m->sel_mhz) < 100) {
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->w_refresh), id); chose = 1;
    }
  }
  if (!chose && best >= 0) {
    char cid[16]; g_snprintf(cid, sizeof cid, "%d", best);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->w_refresh), cid);
  }
}

// ─── popup build ─────────────────────────────────────────────────────────────
static GtkWidget *row(GtkWidget *v, const char *label, GtkWidget *ctl) {
  GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *l = gtk_label_new(label);
  gtk_widget_set_halign(l, GTK_ALIGN_START); gtk_widget_set_hexpand(l, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(l), "dp-key");
  gtk_box_pack_start(GTK_BOX(r), l, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(r), ctl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), r, FALSE, FALSE, 0);
  return r;
}

// populate the per-monitor detail widgets from mons[sel]
static void load_details(Inst *self) {
  Mon *m = cur_mon(self); if (!m) return;
  self->loading = 1;
  char title[96];
  g_snprintf(title, sizeof title, "%s%s", m->name, m->active ? "  (active)" : "");
  gtk_label_set_text(GTK_LABEL(self->w_montitle), title);
  // resolution combo
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(self->w_res));
  for (guint i = 0; i < m->modes->len; i++) {
    Mode md = g_array_index(m->modes, Mode, i);
    char id[16]; g_snprintf(id, sizeof id, "%dx%d", md.w, md.h);
    // dedupe
    int dup = 0;
    GtkTreeModel *mdl = gtk_combo_box_get_model(GTK_COMBO_BOX(self->w_res));
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(mdl, &it)) {
      do {
        char *eid; gtk_tree_model_get(mdl, &it, 1, &eid, -1);
        if (eid && !strcmp(eid, id)) dup = 1;
        g_free(eid);
      } while (!dup && gtk_tree_model_iter_next(mdl, &it));
    }
    if (!dup) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->w_res), id, id);
  }
  { char cur[16]; g_snprintf(cur, sizeof cur, "%dx%d", m->sel_w, m->sel_h);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->w_res), cur); }
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_scale), m->sel_scale);
  gtk_switch_set_active(GTK_SWITCH(self->w_vrr), m->sel_vrr);
  gtk_widget_set_sensitive(self->w_vrr, m->vrr_capable);
  gtk_switch_set_active(GTK_SWITCH(self->w_hdr), m->sel_hdr);
  gtk_widget_set_sensitive(self->w_hdr, m->hdr_capable);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_hmax), m->sel_hmax);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_hmin), m->sel_hmin);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_hfall), m->sel_hfall);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_posx), m->sel_x);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_posy), m->sel_y);
  self->loading = 0;
  on_res_changed(GTK_COMBO_BOX(self->w_res), self);   // refresh combo
}

static void select_mon(Inst *self, int idx) {
  if (idx < 0 || idx >= (int)self->mons->len) return;
  self->sel = idx;
  load_details(self);
  if (self->canvas) gtk_widget_queue_draw(self->canvas);
}

static void head(GtkWidget *v, const char *txt) {
  GtkWidget *h = gtk_label_new(txt);
  gtk_widget_set_halign(h, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(h), "dp-head");
  gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
}

static void rebuild_popover(Inst *self) {
  read_all(self);
  GtkWidget *old = gtk_bin_get_child(GTK_BIN(self->pop.win));
  if (old) gtk_widget_destroy(old);
  self->loading = 1;
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_style_context_add_class(gtk_widget_get_style_context(v), "dp-pop");
  gtk_widget_set_margin_start(v, 18); gtk_widget_set_margin_end(v, 18);
  gtk_widget_set_margin_top(v, 16); gtk_widget_set_margin_bottom(v, 16);
  gtk_widget_set_size_request(v, 460, -1);

  head(v, "Displays");
  // ── layout canvas ──
  self->canvas = gtk_drawing_area_new();
  gtk_widget_set_size_request(self->canvas, 424, 180);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->canvas), "dp-canvas");
  gtk_widget_add_events(self->canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                      GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK);
  g_signal_connect(self->canvas, "draw", G_CALLBACK(canvas_draw), self);
  g_signal_connect(self->canvas, "button-press-event", G_CALLBACK(canvas_press), self);
  g_signal_connect(self->canvas, "motion-notify-event", G_CALLBACK(canvas_motion), self);
  g_signal_connect(self->canvas, "button-release-event", G_CALLBACK(canvas_release), self);
  gtk_box_pack_start(GTK_BOX(v), self->canvas, FALSE, FALSE, 0);
  GtkWidget *hint = gtk_label_new("drag to arrange · click to select");
  gtk_widget_set_halign(hint, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dp-status");
  gtk_box_pack_start(GTK_BOX(v), hint, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  self->w_montitle = gtk_label_new("");
  gtk_widget_set_halign(self->w_montitle, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->w_montitle), "dp-head");
  gtk_box_pack_start(GTK_BOX(v), self->w_montitle, FALSE, FALSE, 0);

  self->w_res = gtk_combo_box_text_new();
  g_signal_connect(self->w_res, "changed", G_CALLBACK(on_res_changed), self);
  self->w_refresh = gtk_combo_box_text_new();
  row(v, "Resolution", self->w_res);
  row(v, "Refresh", self->w_refresh);
  self->w_scale = gtk_spin_button_new_with_range(0.5, 3.0, 0.05);
  row(v, "Scale", self->w_scale);
  { GtkWidget *pb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    self->w_posx = gtk_spin_button_new_with_range(-20000, 20000, 1);
    self->w_posy = gtk_spin_button_new_with_range(-20000, 20000, 1);
    g_signal_connect(self->w_posx, "value-changed", G_CALLBACK(on_posxy_changed), self);
    g_signal_connect(self->w_posy, "value-changed", G_CALLBACK(on_posxy_changed), self);
    gtk_box_pack_start(GTK_BOX(pb), self->w_posx, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pb), self->w_posy, FALSE, FALSE, 0);
    row(v, "Position X / Y", pb); }
  self->w_vrr = gtk_switch_new(); gtk_widget_set_valign(self->w_vrr, GTK_ALIGN_CENTER);
  row(v, "VRR (adaptive sync)", self->w_vrr);

  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  head(v, "HDR");
  self->w_hdr = gtk_switch_new(); gtk_widget_set_valign(self->w_hdr, GTK_ALIGN_CENTER);
  g_signal_connect(self->w_hdr, "state-set", G_CALLBACK(on_hdr_switch2), self);
  row(v, "HDR", self->w_hdr);
  self->w_hmax = gtk_spin_button_new_with_range(0, 10000, 1);
  row(v, "Max luminance (nits)", self->w_hmax);
  self->w_hfall = gtk_spin_button_new_with_range(0, 10000, 1);
  row(v, "Max frame-avg (nits)", self->w_hfall);
  self->w_hmin = gtk_spin_button_new_with_range(0, 10, 0.001);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(self->w_hmin), 3);
  row(v, "Min luminance (nits)", self->w_hmin);
  self->w_sdr = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 80, 1000, 1);
  gtk_scale_set_draw_value(GTK_SCALE(self->w_sdr), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(self->w_sdr), GTK_POS_RIGHT);
  gtk_widget_set_hexpand(self->w_sdr, TRUE);
  gtk_range_set_value(GTK_RANGE(self->w_sdr), self->sdr_lum);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->w_sdr), "dp-scale");
  g_signal_connect(self->w_sdr, "value-changed", G_CALLBACK(on_sdr_changed), self);
  row(v, "SDR luminance (global)", self->w_sdr);

  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  GtkWidget *ar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  self->w_status = gtk_label_new("");
  gtk_widget_set_halign(self->w_status, GTK_ALIGN_START); gtk_widget_set_hexpand(self->w_status, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->w_status), "dp-status");
  GtkWidget *apply = gtk_button_new_with_label("Apply");
  gtk_style_context_add_class(gtk_widget_get_style_context(apply), "dp-apply");
  g_signal_connect(apply, "clicked", G_CALLBACK(on_apply), self);
  gtk_box_pack_start(GTK_BOX(ar), self->w_status, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(ar), apply, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), ar, FALSE, FALSE, 0);

  g_object_set_data(G_OBJECT(self->pop.win), "wb-focus", apply);
  gtk_container_add(GTK_CONTAINER(self->pop.win), v);
  gtk_widget_show_all(v);
  self->loading = 0;
  // default selection: the active monitor
  int def = 0;
  for (guint i = 0; i < self->mons->len; i++)
    if (g_array_index(self->mons, Mon, i).active) { def = i; break; }
  select_mon(self, def);
}
static void rebuild_cb(gpointer user) { rebuild_popover(user); }

static gboolean on_click(GtkWidget *w, GdkEventButton *ev, gpointer d) {
  (void)w; Inst *self = d;
  if (ev->button != 1) return FALSE;
  wbpop_toggle(&self->pop);
  return TRUE;
}

// Refresh the mini layout on monitor hotplug. Skip while the popup is open so
// we don't rebuild the mons array under the live editing widgets.
static gboolean poll_cb(gpointer d) {
  Inst *self = d;
  if (!wbpop_visible(&self->pop)) { read_all(self); update_bar(self); }
  return G_SOURCE_CONTINUE;
}

void *wbcffi_init(const wbcffi_init_info *info, const wbcffi_config_entry *entries, size_t entries_len) {
  Inst *self = g_new0(Inst, 1);
  self->icon_size = 24;
  for (size_t i = 0; i < entries_len; i++) {
    if (!strcmp(entries[i].key, "icon-size")) { self->icon_size = atoi(entries[i].value); if (self->icon_size < 8) self->icon_size = 8; }
    else if (!strcmp(entries[i].key, "icon-dir")) { g_free(self->icon_dir); self->icon_dir = g_strdup(entries[i].value); }
    else if (!strcmp(entries[i].key, "monitors-kdl")) { g_free(self->monitors_kdl); self->monitors_kdl = g_strdup(entries[i].value); }
  }
  if (!self->icon_dir) {
    const char *dh = g_getenv("XDG_DATA_HOME");
    self->icon_dir = (dh && *dh) ? g_build_filename(dh, "waybar-display", NULL)
                                 : g_build_filename(g_get_home_dir(), ".local/share/waybar-display", NULL);
  }
  if (!self->monitors_kdl)
    self->monitors_kdl = g_build_filename(g_get_home_dir(), ".config/asteroidz/monitors.kdl", NULL);

  GtkContainer *root = info->get_root_widget(info->obj);
  self->box = gtk_event_box_new();
  gtk_widget_set_name(self->box, "display");
  gtk_widget_add_events(self->box, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_margin_start(self->box, 6); gtk_widget_set_margin_end(self->box, 6);
  // bar pill: a mini layout drawing (active monitor highlighted)
  self->bar_area = gtk_drawing_area_new();
  int bw = self->icon_size * 5 / 3;   // ~landscape aspect for the mini layout
  gtk_widget_set_size_request(self->bar_area, bw, self->icon_size);
  gtk_widget_set_valign(self->bar_area, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->bar_area), "dp-icon");
  g_signal_connect(self->bar_area, "draw", G_CALLBACK(bar_draw), self);
  gtk_container_add(GTK_CONTAINER(self->box), self->bar_area);
  wbpop_init(&self->pop, self->box, rebuild_cb, self);
  g_signal_connect(self->box, "button-press-event", G_CALLBACK(on_click), self);
  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));

  read_all(self);
  update_bar(self);
  self->poll_id = g_timeout_add_seconds(4, poll_cb, self);
  return self;
}
void wbcffi_deinit(void *instance) {
  Inst *self = instance;
  if (self->poll_id) g_source_remove(self->poll_id);
  wbpop_destroy(&self->pop);
  mons_clear(self);
  if (self->mons) g_array_free(self->mons, TRUE);
  g_free(self->icon_dir);
  g_free(self->monitors_kdl);
  g_free(self);
}
