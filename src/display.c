// waybar CFFI plugin: asteroidz display/monitor configuration.
//  - Bar pill: monitor icon + current resolution·refresh of the active output.
//  - Click opens a popup to configure that output: resolution + refresh, scale,
//    VRR, HDR (instant), SDR reference luminance (instant), and HDR tone-map
//    luminances (max/min/max-fall).
//  - State comes from `amsg get all-monitors` / `get monitor <name>` (the modes
//    array was added to asteroidz's IPC for the resolution picker). HDR toggle
//    and SDR luminance apply live via `amsg dispatch`; resolution/scale/VRR and
//    the HDR luminances are written to a plugin-owned monitors.kdl (sourced by
//    config.kdl) and applied with `amsg dispatch reload_config`, which
//    re-modesets the live output.
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
  GtkWidget *box, *icon, *label;
  WbPop pop;
  char *icon_dir; int icon_size;
  char *monitors_kdl;      // plugin-owned output config (sourced by config.kdl)
  char output_name[64];

  // current state (read from the compositor)
  int cur_w, cur_h, cur_refresh_mhz;
  double scale;
  int hdr, hdr_capable, vrr, vrr_capable;
  int sdr_lum;
  double hdr_max, hdr_min, hdr_fall;
  int bitdepth;
  char icc[512];
  GArray *modes;           // Mode[]

  // popup widgets (valid only while the popup is open)
  GtkWidget *w_res, *w_refresh, *w_scale, *w_vrr, *w_hdr, *w_sdr,
            *w_hmax, *w_hmin, *w_hfall, *w_status;
  int loading;             // suppress change handlers during rebuild
} Inst;

// ─── amsg helpers ────────────────────────────────────────────────────────────
// amsg resolves the live IPC socket itself (env, else newest asteroidz-*.sock),
// so a stale ASTEROIDZ_INSTANCE_SIGNATURE in waybar's env is fine.
static char *amsg_out(const char *args) {
  char *cmd = g_strdup_printf("amsg %s", args);
  char *out = NULL;
  g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL);
  g_free(cmd);
  return out;   // caller frees; may be NULL
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

// ─── read compositor state ───────────────────────────────────────────────────
static void find_active_output(Inst *self) {
  self->output_name[0] = '\0';
  char *out = amsg_out("get all-monitors");
  if (!out) return;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, out, -1, NULL)) {
    JsonNode *root = json_parser_get_root(p);
    JsonArray *arr = NULL;
    if (JSON_NODE_HOLDS_ARRAY(root)) arr = json_node_get_array(root);
    else if (JSON_NODE_HOLDS_OBJECT(root) &&
             json_object_has_member(json_node_get_object(root), "monitors"))
      arr = json_object_get_array_member(json_node_get_object(root), "monitors");
    if (arr) {
      guint n = json_array_get_length(arr);
      for (guint i = 0; i < n; i++) {
        JsonObject *m = json_array_get_object_element(arr, i);
        if (!self->output_name[0])   // fallback: first monitor
          g_strlcpy(self->output_name, jstr(m, "name"), sizeof self->output_name);
        if (jbool(m, "active")) {
          g_strlcpy(self->output_name, jstr(m, "name"), sizeof self->output_name);
          break;
        }
      }
    }
  }
  g_object_unref(p);
  g_free(out);
}

static void read_state(Inst *self) {
  if (!self->output_name[0]) find_active_output(self);
  if (!self->output_name[0]) return;
  char *args = g_strdup_printf("get monitor %s", self->output_name);
  char *out = amsg_out(args);
  g_free(args);
  if (!out) return;
  JsonParser *p = json_parser_new();
  if (json_parser_load_from_data(p, out, -1, NULL)) {
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    self->cur_w = jint(o, "mode_width", 0);
    self->cur_h = jint(o, "mode_height", 0);
    self->cur_refresh_mhz = jint(o, "mode_refresh", 0);
    self->scale = jdbl(o, "scale", 1.0);
    self->hdr = jbool(o, "hdr_enabled");
    self->hdr_capable = jbool(o, "hdr_capable");
    self->vrr = jbool(o, "vrr_enabled");
    self->vrr_capable = jbool(o, "vrr_capable");
    self->sdr_lum = jint(o, "sdr_luminance", 203);
    self->hdr_max = jdbl(o, "hdr_max_luminance", 0);
    self->hdr_min = jdbl(o, "hdr_min_luminance", 0);
    self->hdr_fall = jdbl(o, "hdr_max_fall", 0);
    self->bitdepth = jint(o, "bitdepth", 0);
    g_strlcpy(self->icc, jstr(o, "icc_profile"), sizeof self->icc);
    if (self->modes) g_array_set_size(self->modes, 0);
    else self->modes = g_array_new(FALSE, FALSE, sizeof(Mode));
    if (json_object_has_member(o, "modes")) {
      JsonArray *ms = json_object_get_array_member(o, "modes");
      guint n = json_array_get_length(ms);
      for (guint i = 0; i < n; i++) {
        JsonObject *m = json_array_get_object_element(ms, i);
        Mode md = { jint(m, "width", 0), jint(m, "height", 0), jint(m, "refresh", 0) };
        if (md.w > 0 && md.h > 0) g_array_append_val(self->modes, md);
      }
    }
  }
  g_object_unref(p);
  g_free(out);
}

// ─── bar pill ────────────────────────────────────────────────────────────────
static void update_bar(Inst *self) {
  wb_icon_set(self->icon, "display.svg");
  char t[48];
  if (self->cur_w > 0)
    g_snprintf(t, sizeof t, "%d·%dHz", self->cur_h,
               (int)lround(self->cur_refresh_mhz / 1000.0));
  else
    g_snprintf(t, sizeof t, "display");
  gtk_label_set_text(GTK_LABEL(self->label), t);
}

// ─── monitors.kdl writer ─────────────────────────────────────────────────────
static void fmt_refresh(int mhz, char *buf, size_t n) {
  double hz = mhz / 1000.0;
  if (fabs(hz - lround(hz)) < 0.05) g_snprintf(buf, n, "%ld", lround(hz));
  else g_snprintf(buf, n, "%.3f", hz);
}
static void write_monitors_kdl(Inst *self, int sel_w, int sel_h, int sel_mhz,
                               double sel_scale, int sel_vrr, int sel_hdr,
                               double hmax, double hmin, double hfall) {
  GString *s = g_string_new(
      "// Managed by the waybar-display plugin — rewritten on each change.\n"
      "// Fields map to the asteroidz `output` block; reload_config applies live.\n");
  g_string_append_printf(s, "output %s {", self->output_name);
  if (sel_hdr) g_string_append(s, " hdr;");
  g_string_append_printf(s, " scale %.4g;", sel_scale);
  if (sel_w > 0 && sel_h > 0) {
    char rb[32]; fmt_refresh(sel_mhz, rb, sizeof rb);
    g_string_append_printf(s, " width %d; height %d; refresh %s;", sel_w, sel_h, rb);
  }
  g_string_append_printf(s, " vrr %d;", sel_vrr ? 1 : 0);
  if (sel_hdr) {
    if (self->bitdepth > 0) g_string_append_printf(s, " bit-depth %d;", self->bitdepth);
    if (hmax > 0) g_string_append_printf(s, " max-luminance %.4g;", hmax);
    if (hmin > 0) g_string_append_printf(s, " min-luminance %.4g;", hmin);
    if (hfall > 0) g_string_append_printf(s, " max-fall %.4g;", hfall);
  }
  if (self->icc[0]) g_string_append_printf(s, " icc-profile \"%s\";", self->icc);
  g_string_append(s, " }\n");
  g_file_set_contents(self->monitors_kdl, s->str, s->len, NULL);
  g_string_free(s, TRUE);
}

static void set_status(Inst *self, const char *msg) {
  if (self->w_status) gtk_label_set_text(GTK_LABEL(self->w_status), msg);
}

// ─── apply ───────────────────────────────────────────────────────────────────
// selected mode from the two combos
static int selected_mode(Inst *self, int *w, int *h, int *mhz) {
  const char *rid = self->w_refresh ?
      gtk_combo_box_get_active_id(GTK_COMBO_BOX(self->w_refresh)) : NULL;
  if (!rid) return 0;
  int i = atoi(rid);   // index into self->modes
  if (i < 0 || (guint)i >= self->modes->len) return 0;
  Mode m = g_array_index(self->modes, Mode, i);
  *w = m.w; *h = m.h; *mhz = m.refresh_mhz;
  return 1;
}

static void on_apply(GtkButton *b, gpointer d) {
  (void)b; Inst *self = d;
  int w = self->cur_w, h = self->cur_h, mhz = self->cur_refresh_mhz;
  selected_mode(self, &w, &h, &mhz);
  double scale = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_scale));
  int vrr = gtk_switch_get_active(GTK_SWITCH(self->w_vrr));
  int hdr = gtk_switch_get_active(GTK_SWITCH(self->w_hdr));
  double hmax = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_hmax));
  double hmin = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_hmin));
  double hfall = gtk_spin_button_get_value(GTK_SPIN_BUTTON(self->w_hfall));
  write_monitors_kdl(self, w, h, mhz, scale, vrr, hdr, hmax, hmin, hfall);
  amsg_dispatch("reload_config");
  set_status(self, "Applied");
  // refresh cached state so the bar label + next open reflect the change
  self->cur_w = w; self->cur_h = h; self->cur_refresh_mhz = mhz;
  self->scale = scale; self->vrr = vrr; self->hdr = hdr;
  update_bar(self);
}

// HDR toggle: live via dispatch, and persisted to monitors.kdl so a later
// reload keeps it. toggle_hdr flips the active output's HDR immediately.
static gboolean on_hdr_switch(GtkSwitch *sw, gboolean state, gpointer d) {
  (void)sw; Inst *self = d;
  if (self->loading) return FALSE;
  if (state != self->hdr) amsg_dispatch("toggle_hdr");
  self->hdr = state;
  return FALSE;   // let the switch update its visual state
}
static void on_sdr_changed(GtkRange *r, gpointer d) {
  Inst *self = d;
  if (self->loading) return;
  int v = (int)lround(gtk_range_get_value(r));
  char verb[48]; g_snprintf(verb, sizeof verb, "set_sdr_luminance,%d", v);
  amsg_dispatch(verb);
  self->sdr_lum = v;
}
// changing resolution repopulates the refresh combo for that resolution
static void on_res_changed(GtkComboBox *c, gpointer d) {
  Inst *self = d;
  if (self->loading || !self->w_refresh) return;
  const char *res = gtk_combo_box_get_active_id(c);   // "WxH"
  if (!res) return;
  int rw = 0, rh = 0; sscanf(res, "%dx%d", &rw, &rh);
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(self->w_refresh));
  int best = -1; double best_hz = -1;
  for (guint i = 0; i < self->modes->len; i++) {
    Mode m = g_array_index(self->modes, Mode, i);
    if (m.w != rw || m.h != rh) continue;
    char id[16], lbl[32], rb[24];
    g_snprintf(id, sizeof id, "%u", i);
    fmt_refresh(m.refresh_mhz, rb, sizeof rb);
    g_snprintf(lbl, sizeof lbl, "%s Hz", rb);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(self->w_refresh), id, lbl);
    double hz = m.refresh_mhz / 1000.0;
    if (hz > best_hz) { best_hz = hz; best = i; }   // default: highest refresh
    if (rw == self->cur_w && rh == self->cur_h &&
        abs(m.refresh_mhz - self->cur_refresh_mhz) < 100) {
      char cid[16]; g_snprintf(cid, sizeof cid, "%u", i);
      gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->w_refresh), cid);
      best = -1;   // current selected; don't override
    }
  }
  if (best >= 0) {
    char cid[16]; g_snprintf(cid, sizeof cid, "%d", best);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->w_refresh), cid);
  }
}

// ─── popup ───────────────────────────────────────────────────────────────────
static GtkWidget *row(GtkWidget *v, const char *label, GtkWidget *ctl) {
  GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *l = gtk_label_new(label);
  gtk_widget_set_halign(l, GTK_ALIGN_START);
  gtk_widget_set_hexpand(l, TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(l), "dp-key");
  gtk_box_pack_start(GTK_BOX(r), l, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(r), ctl, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(v), r, FALSE, FALSE, 0);
  return r;
}
static void head(GtkWidget *v, const char *txt) {
  GtkWidget *h = gtk_label_new(txt);
  gtk_widget_set_halign(h, GTK_ALIGN_START);
  gtk_style_context_add_class(gtk_widget_get_style_context(h), "dp-head");
  gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
}

static void rebuild_popover(Inst *self) {
  read_state(self);
  GtkWidget *old = gtk_bin_get_child(GTK_BIN(self->pop.win));
  if (old) gtk_widget_destroy(old);
  self->loading = 1;
  GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_style_context_add_class(gtk_widget_get_style_context(v), "dp-pop");
  gtk_widget_set_margin_start(v, 18); gtk_widget_set_margin_end(v, 18);
  gtk_widget_set_margin_top(v, 16); gtk_widget_set_margin_bottom(v, 16);
  gtk_widget_set_size_request(v, 440, -1);

  char title[96];
  g_snprintf(title, sizeof title, "Display · %s",
             self->output_name[0] ? self->output_name : "?");
  head(v, title);

  // ── resolution + refresh ──
  self->w_res = gtk_combo_box_text_new();
  // unique resolutions, highest first
  for (guint i = 0; i < self->modes->len; i++) {
    Mode m = g_array_index(self->modes, Mode, i);
    char id[16]; g_snprintf(id, sizeof id, "%dx%d", m.w, m.h);
    // skip if already added
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
  self->w_refresh = gtk_combo_box_text_new();
  g_signal_connect(self->w_res, "changed", G_CALLBACK(on_res_changed), self);
  {
    char cur_res[16]; g_snprintf(cur_res, sizeof cur_res, "%dx%d", self->cur_w, self->cur_h);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(self->w_res), cur_res);
  }
  row(v, "Resolution", self->w_res);
  row(v, "Refresh", self->w_refresh);

  // ── scale ──
  self->w_scale = gtk_spin_button_new_with_range(0.5, 3.0, 0.05);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_scale), self->scale);
  row(v, "Scale", self->w_scale);

  // ── VRR ──
  self->w_vrr = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(self->w_vrr), self->vrr);
  gtk_widget_set_sensitive(self->w_vrr, self->vrr_capable);
  gtk_widget_set_valign(self->w_vrr, GTK_ALIGN_CENTER);
  row(v, self->vrr_capable ? "VRR (adaptive sync)" : "VRR (unsupported)", self->w_vrr);

  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  head(v, "HDR");
  // ── HDR toggle (instant) ──
  self->w_hdr = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(self->w_hdr), self->hdr);
  gtk_widget_set_sensitive(self->w_hdr, self->hdr_capable);
  gtk_widget_set_valign(self->w_hdr, GTK_ALIGN_CENTER);
  g_signal_connect(self->w_hdr, "state-set", G_CALLBACK(on_hdr_switch), self);
  row(v, self->hdr_capable ? "HDR" : "HDR (unsupported)", self->w_hdr);

  // ── HDR luminances ──
  self->w_hmax = gtk_spin_button_new_with_range(0, 10000, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_hmax), self->hdr_max);
  row(v, "Max luminance (nits)", self->w_hmax);
  self->w_hfall = gtk_spin_button_new_with_range(0, 10000, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_hfall), self->hdr_fall);
  row(v, "Max frame-avg (nits)", self->w_hfall);
  self->w_hmin = gtk_spin_button_new_with_range(0, 10, 0.001);
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(self->w_hmin), 3);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(self->w_hmin), self->hdr_min);
  row(v, "Min luminance (nits)", self->w_hmin);

  // ── SDR reference luminance (instant) ──
  self->w_sdr = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 80, 1000, 1);
  gtk_scale_set_draw_value(GTK_SCALE(self->w_sdr), TRUE);
  gtk_scale_set_value_pos(GTK_SCALE(self->w_sdr), GTK_POS_RIGHT);
  gtk_widget_set_hexpand(self->w_sdr, TRUE);
  gtk_range_set_value(GTK_RANGE(self->w_sdr), self->sdr_lum);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->w_sdr), "dp-scale");
  g_signal_connect(self->w_sdr, "value-changed", G_CALLBACK(on_sdr_changed), self);
  row(v, "SDR luminance", self->w_sdr);

  gtk_box_pack_start(GTK_BOX(v), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);
  // ── apply row (resolution/scale/VRR/HDR-lum need a reload) ──
  GtkWidget *ar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  self->w_status = gtk_label_new("");
  gtk_widget_set_halign(self->w_status, GTK_ALIGN_START);
  gtk_widget_set_hexpand(self->w_status, TRUE);
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
  on_res_changed(GTK_COMBO_BOX(self->w_res), self);   // populate refresh combo
}
static void rebuild_cb(gpointer user) { rebuild_popover(user); }

static gboolean on_click(GtkWidget *w, GdkEventButton *ev, gpointer d) {
  (void)w; Inst *self = d;
  if (ev->button != 1) return FALSE;
  wbpop_toggle(&self->pop);
  return TRUE;
}

static GtkWidget *mklabel(const char *t, const char *cls) {
  GtkWidget *l = gtk_label_new(t);
  gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(l), cls);
  return l;
}
void *wbcffi_init(const wbcffi_init_info *info, const wbcffi_config_entry *entries, size_t entries_len) {
  Inst *self = g_new0(Inst, 1);
  self->icon_size = 24;
  self->scale = 1.0;
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
  GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  self->icon = wb_icon_new(self->icon_dir, self->icon_size, NULL);
  gtk_widget_set_valign(self->icon, GTK_ALIGN_CENTER);
  gtk_style_context_add_class(gtk_widget_get_style_context(self->icon), "dp-icon");
  self->label = mklabel("display", "dp-label");
  gtk_box_pack_start(GTK_BOX(h), self->icon, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(h), self->label, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(self->box), h);
  wbpop_init(&self->pop, self->box, rebuild_cb, self);
  g_signal_connect(self->box, "button-press-event", G_CALLBACK(on_click), self);
  gtk_container_add(root, self->box);
  gtk_widget_show_all(GTK_WIDGET(root));

  read_state(self);
  update_bar(self);
  return self;
}
void wbcffi_deinit(void *instance) {
  Inst *self = instance;
  wbpop_destroy(&self->pop);
  if (self->modes) g_array_free(self->modes, TRUE);
  g_free(self->icon_dir);
  g_free(self->monitors_kdl);
  g_free(self);
}
