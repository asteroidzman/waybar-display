// Unit tests for display.c's pure JSON-parsing logic (jint/jdbl/jbool/jstr,
// parse_monitors) -- no GTK init, no Wayland, no amsg/compositor process.
// #includes the plugin source directly to reach its `static` functions
// without changing their visibility for production code; this file
// supplies its own main() (display.c has none), so nothing conflicts.
// json-glib itself needs no display connection (unlike GTK), so this can
// exercise real parsing against a synthetic "get all-monitors" payload
// shaped exactly like the real IPC schema (src/ipc/ipc.h's
// build_monitor_json) without needing a live compositor.
#include "../src/display.c"
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
	else { printf("ok - %s\n", msg); } \
} while (0)
#define CHECK_STR(a, b, msg) CHECK(strcmp((a), (b)) == 0, msg)
#define CHECK_DOUBLE_EQ(a, b, msg) CHECK(((a) - (b) < 0.001 && (b) - (a) < 0.001), msg)

static JsonArray *parse_monitors_json(const char *json, JsonParser **out_parser) {
	JsonParser *p = json_parser_new();
	if (!json_parser_load_from_data(p, json, -1, NULL)) {
		fprintf(stderr, "FAIL: could not parse test fixture JSON\n");
		failures++;
		g_object_unref(p);
		return NULL;
	}
	*out_parser = p;
	return json_object_get_array_member(json_node_get_object(json_parser_get_root(p)), "monitors");
}

int main(void) {
	// jint/jdbl/jbool/jstr: present, missing (default), and null-string cases
	JsonParser *jp = json_parser_new();
	json_parser_load_from_data(jp,
		"{\"w\":1920,\"scale\":1.5,\"active\":true,\"name\":\"DP-1\",\"nullname\":null}",
		-1, NULL);
	JsonObject *o = json_node_get_object(json_parser_get_root(jp));
	CHECK(jint(o, "w", -1) == 1920, "jint reads a present integer field");
	CHECK(jint(o, "missing", -1) == -1, "jint falls back to the default for a missing field");
	CHECK_DOUBLE_EQ(jdbl(o, "scale", 0), 1.5, "jdbl reads a present double field");
	CHECK_DOUBLE_EQ(jdbl(o, "missing", 9.0), 9.0, "jdbl falls back to the default for a missing field");
	CHECK(jbool(o, "active") == 1, "jbool reads a present true field");
	CHECK(jbool(o, "missing") == 0, "jbool defaults false for a missing field");
	CHECK_STR(jstr(o, "name"), "DP-1", "jstr reads a present string field");
	CHECK_STR(jstr(o, "missing"), "", "jstr falls back to empty string for a missing field");
	CHECK_STR(jstr(o, "nullname"), "", "jstr falls back to empty string for a JSON null value");
	g_object_unref(jp);

	// parse_monitors against a payload shaped like the real IPC schema
	// (src/ipc/ipc.h's build_monitor_json / get all-monitors)
	const char *fixture =
		"{\"monitors\":[{"
		"\"name\":\"DP-1\",\"active\":true,"
		"\"x\":0,\"y\":0,\"width\":3840,\"height\":2160,"
		"\"mode_width\":3840,\"mode_height\":2160,\"mode_refresh\":143999,"
		"\"scale\":1.0,"
		"\"hdr_enabled\":true,\"hdr_capable\":true,"
		"\"vrr_enabled\":false,\"vrr_capable\":true,"
		"\"hdr_max_luminance\":418.0,\"hdr_min_luminance\":0.079,\"hdr_max_fall\":418.0,"
		"\"bitdepth\":10,\"icc_profile\":\"/home/ralf/FI32U.icm\","
		"\"sdr_luminance\":280,"
		"\"modes\":[{\"width\":3840,\"height\":2160,\"refresh\":60000},"
		"{\"width\":1920,\"height\":1080,\"refresh\":60000}]"
		"},{"
		"\"name\":\"HDMI-A-1\",\"active\":false,"
		"\"x\":3840,\"y\":0,\"width\":2560,\"height\":1440,"
		"\"mode_width\":1920,\"mode_height\":1080,\"mode_refresh\":59940,"
		"\"scale\":0.75,"
		"\"hdr_enabled\":false,\"hdr_capable\":false,"
		"\"vrr_enabled\":false,\"vrr_capable\":false,"
		"\"hdr_max_luminance\":0,\"hdr_min_luminance\":0,\"hdr_max_fall\":0,"
		"\"bitdepth\":0,\"icc_profile\":\"\","
		"\"sdr_luminance\":203,"
		"\"modes\":[]"
		"}]}";
	JsonParser *fp = NULL;
	JsonArray *arr = parse_monitors_json(fixture, &fp);
	CHECK(arr != NULL, "test fixture parses");

	Inst inst = {0};
	parse_monitors(&inst, arr);

	CHECK(inst.mons->len == 2, "parse_monitors produces one Mon per monitor in the array");
	Mon *m0 = &g_array_index(inst.mons, Mon, 0);
	CHECK_STR(m0->name, "DP-1", "first monitor's name round-trips");
	CHECK(m0->active == 1, "first monitor's active flag round-trips");
	CHECK(m0->cur_w == 3840 && m0->cur_h == 2160, "first monitor's current mode size round-trips");
	CHECK(m0->hdr == 1 && m0->hdr_capable == 1, "first monitor's HDR flags round-trip");
	CHECK_DOUBLE_EQ(m0->hdr_max, 418.0, "first monitor's hdr_max_luminance round-trips");
	CHECK(m0->bitdepth == 10, "first monitor's bitdepth round-trips");
	CHECK_STR(m0->icc, "/home/ralf/FI32U.icm", "first monitor's icc_profile round-trips");
	CHECK(inst.sdr_lum == 280, "sdr_luminance is seeded from the FIRST monitor only");
	CHECK(m0->modes->len == 2, "first monitor's modes array round-trips (2 entries)");
	Mode *mode0 = &g_array_index(m0->modes, Mode, 0);
	CHECK(mode0->w == 3840 && mode0->h == 2160 && mode0->refresh_mhz == 60000,
		"a mode entry's width/height/refresh round-trip");

	Mon *m1 = &g_array_index(inst.mons, Mon, 1);
	CHECK_STR(m1->name, "HDMI-A-1", "second monitor's name round-trips");
	CHECK(m1->active == 0, "second monitor's active flag round-trips (false)");
	CHECK(m1->modes->len == 0, "second monitor's empty modes array round-trips as zero entries");
	CHECK_DOUBLE_EQ(m1->scale, 0.75, "second monitor's fractional scale round-trips");

	// seeded "edit" fields mirror the just-parsed current values
	CHECK(m0->sel_w == m0->cur_w && m0->sel_h == m0->cur_h,
		"edited-value fields are seeded from the parsed current mode");

	g_object_unref(fp);

	printf("----\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
