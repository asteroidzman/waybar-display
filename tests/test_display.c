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
	CHECK_STR(m0->sel_icc, m0->icc, "sel_icc (the editable ICC path) is seeded from the reported icc_profile");

	g_object_unref(fp);

	// ── fmt_refresh: must not collapse a fractional mode onto a neighbour ──
	// 59.94 and 60.000 are separately-listed real modes 0.06 Hz apart, so any
	// rounding tolerance here silently changes which one a reload selects.
	char rb[32];
	fmt_refresh(60000, rb, sizeof rb);
	CHECK_STR(rb, "60", "fmt_refresh writes a whole-Hz mode as a bare integer");
	fmt_refresh(59940, rb, sizeof rb);
	CHECK_STR(rb, "59.940", "fmt_refresh keeps 59.94 fractional rather than rounding to 60");
	fmt_refresh(59951, rb, sizeof rb);
	CHECK_STR(rb, "59.951", "fmt_refresh keeps a mode within 0.05Hz of an integer fractional");
	fmt_refresh(143999, rb, sizeof rb);
	CHECK_STR(rb, "143.999", "fmt_refresh keeps 143.999 distinct from 144");
	fmt_refresh(144000, rb, sizeof rb);
	CHECK_STR(rb, "144", "fmt_refresh writes 144.000 as a bare integer");

	// ── pick_refresh_index: closest match, not last-within-tolerance ──
	// Real HDMI-A-1 1920x1080 mode list, in the order the driver reports it:
	// two separate 60.000 entries, with the 59.940 entry after both.
	GArray *md = g_array_new(FALSE, FALSE, sizeof(Mode));
	Mode modes_fixture[] = {
		{1920, 1080,  60000}, {1920, 1080, 100000}, {1920, 1080, 74973},
		{1920, 1080,  60000}, {1920, 1080,  59940}, {1920, 1080, 50000},
		{1280,  720,  60000},
	};
	for (size_t i = 0; i < sizeof modes_fixture / sizeof modes_fixture[0]; i++)
		g_array_append_val(md, modes_fixture[i]);

	CHECK(pick_refresh_index(md, 1920, 1080, 1920, 1080, 59940) == 4,
		"pick_refresh_index picks the exact 59.940 entry, not a 60.000 within 0.1Hz");
	CHECK(pick_refresh_index(md, 1920, 1080, 1920, 1080, 60000) == 0,
		"pick_refresh_index picks the FIRST 60.000 entry on a tie, not the later duplicate");
	CHECK(pick_refresh_index(md, 1920, 1080, 1920, 1080, 74973) == 2,
		"pick_refresh_index picks an exact non-60 match");
	CHECK(pick_refresh_index(md, 1920, 1080, 1920, 1080, 59000) == 4,
		"pick_refresh_index picks the nearest entry when nothing matches exactly");
	CHECK(pick_refresh_index(md, 1920, 1080, 1280, 720, 60000) == 1,
		"pick_refresh_index falls back to the highest refresh when changing resolution");
	CHECK(pick_refresh_index(md, 800, 600, 1920, 1080, 60000) == -1,
		"pick_refresh_index returns -1 for a resolution with no modes");

	// Reversing the list must not change the answer -- the old code landed on
	// the right entry only because the exact match happened to come last.
	GArray *rev = g_array_new(FALSE, FALSE, sizeof(Mode));
	for (int i = (int)(sizeof modes_fixture / sizeof modes_fixture[0]) - 1; i >= 0; i--)
		g_array_append_val(rev, modes_fixture[i]);
	Mode picked = g_array_index(rev, Mode, pick_refresh_index(rev, 1920, 1080, 1920, 1080, 59940));
	CHECK(picked.refresh_mhz == 59940,
		"pick_refresh_index still finds 59.940 when the driver reports modes in the opposite order");
	g_array_free(md, TRUE);
	g_array_free(rev, TRUE);

	// ── comment preservation ──
	// A hand-written note above an output block records WHY it is configured
	// that way; losing it on rewrite makes the setting look arbitrary.
	char *tmpf = NULL;
	gint tmpfd = g_file_open_tmp("wbdisp-XXXXXX.kdl", &tmpf, NULL);
	CHECK(tmpfd >= 0, "test can create a temp monitors.kdl");
	close(tmpfd);
	const char *sample =
		KDL_HEADER_1 "\n"
		KDL_HEADER_2 "\n"
		"// 59.94, not 60, deliberately: matching DP-1 lets mclk switch.\n"
		"// That trips a DCN 3.2 bug -- RGB snow mid-screen.\n"
		"output HDMI-A-1 { scale 0.75; refresh 59.94; }\n"
		"output DP-1 { scale 1; refresh 60; }\n";
	g_file_set_contents(tmpf, sample, -1, NULL);

	char *cmts = NULL;
	char *line = find_existing_output_line(tmpf, "HDMI-A-1", &cmts);
	CHECK(line != NULL, "find_existing_output_line finds the HDMI-A-1 block");
	CHECK(line && strstr(line, "59.94") != NULL, "the preserved line keeps its 59.94 refresh");
	CHECK(cmts && strstr(cmts, "DCN 3.2") != NULL,
		"the comment lines directly above the block are returned with it");
	CHECK(cmts && strstr(cmts, "Managed by") == NULL,
		"the generated header is NOT returned as an attached comment (it would duplicate each write)");
	g_free(line); g_free(cmts);

	line = find_existing_output_line(tmpf, "DP-1", &cmts);
	CHECK(line != NULL, "find_existing_output_line finds the DP-1 block");
	CHECK(cmts && *cmts == '\0',
		"a block with no comment above it gets an empty string, not the previous block's comment");
	g_free(line); g_free(cmts);

	line = find_existing_output_line(tmpf, "DP", &cmts);
	CHECK(line == NULL, "find_existing_output_line does not prefix-match a shorter output name");
	g_free(line); g_free(cmts);

	// ── write_monitors_kdl end-to-end ──
	// The real scenario: the user opens the dialog on HDMI-A-1, changes only
	// the scale, and hits Apply. HDMI-A-1's block is regenerated from its
	// sel_* fields while DP-1's is carried over verbatim -- and neither the
	// deliberate 59.94 nor the comment explaining it may be lost.
	Inst w = {0};
	w.monitors_kdl = tmpf;
	w.mons = g_array_new(FALSE, TRUE, sizeof(Mon));
	Mon wm[2] = {0};
	g_strlcpy(wm[0].name, "HDMI-A-1", sizeof wm[0].name);
	wm[0].sel_w = 1920; wm[0].sel_h = 1080; wm[0].sel_mhz = 59940;
	wm[0].sel_scale = 0.75; wm[0].sel_x = 3840; wm[0].sel_y = 0;
	g_strlcpy(wm[1].name, "DP-1", sizeof wm[1].name);
	wm[1].sel_w = 3840; wm[1].sel_h = 2160; wm[1].sel_mhz = 60000;
	wm[1].sel_scale = 1.0;
	g_array_append_vals(w.mons, wm, 2);

	write_monitors_kdl(&w, "HDMI-A-1");

	char *after = NULL;
	g_file_get_contents(tmpf, &after, NULL, NULL);
	CHECK(after && strstr(after, "refresh 59.940") != NULL,
		"after an Apply on HDMI-A-1 its refresh is still 59.940, not rounded to 60");
	CHECK(after && strstr(after, "DCN 3.2") != NULL,
		"after an Apply the comment explaining the 59.94 survives");
	CHECK(after && strstr(after, "output DP-1 { scale 1; refresh 60; }") != NULL,
		"the untouched DP-1 block is carried over verbatim");
	// The header must appear exactly once -- attaching it as a comment would
	// make the file grow by two lines on every single Apply.
	int hdr_count = 0;
	for (const char *p = after; (p = strstr(p, "Managed by")); p++) hdr_count++;
	CHECK(hdr_count == 1, "the generated header appears exactly once, not duplicated per write");

	// A second Apply must be a fixed point, not accumulate anything.
	write_monitors_kdl(&w, "HDMI-A-1");
	char *after2 = NULL;
	g_file_get_contents(tmpf, &after2, NULL, NULL);
	CHECK(after && after2 && strcmp(after, after2) == 0,
		"a second Apply with unchanged values reproduces the file byte-for-byte");
	g_free(after); g_free(after2);
	g_array_free(w.mons, TRUE);

	g_unlink(tmpf);
	g_free(tmpf);


	// ── avif/jxl support in the wallpaper browser ──
	CHECK(is_image("a.avif") && is_image("A.AVIF"), "is_image accepts .avif (case-insensitive)");
	CHECK(is_image("a.jxl"), "is_image accepts .jxl");
	CHECK(is_image("a.jpg") && is_image("a.png") && is_image("a.webp"),
		"is_image still accepts the original SDR formats");
	CHECK(!is_image("a.txt") && !is_image("noextension"), "is_image rejects non-images");

	// CICP transfer parsed from the real files in ~/Pictures, when present.
	// These assert only if the file exists, so the suite stays runnable
	// anywhere; the parser itself is pinned by the synthetic case below.
	// The 8K file is the important one: it carries an ICC 'prof' colr box
	// BEFORE the 'nclx' one, pushing the CICP data to offset ~13810.
	const char *pq_files[] = {
		"/home/ralf/Pictures/20260314-cascade_0853_wp169.avif",
		"/home/ralf/Pictures/GiauPass-synthhdr.avif",
	};
	for (size_t i = 0; i < 2; i++) {
		if (!g_file_test(pq_files[i], G_FILE_TEST_EXISTS)) continue;
		CHECK(image_transfer_characteristics(pq_files[i]) == CICP_TF_PQ,
			"image_transfer_characteristics reads PQ (16) from a real HDR10 AVIF");
	}

	// An ICC 'prof' colr box must be skipped, not read as colour codes.
	char *pf = NULL;
	gint pfd = g_file_open_tmp("wbdisp-prof-XXXXXX.bin", &pf, NULL);
	close(pfd);
	const unsigned char twobox[] = {
		0x00,0x00,0x00,0x0c, 'c','o','l','r', 'p','r','o','f',
		0xde,0xad,0xbe,0xef,
		0x00,0x00,0x00,0x13, 'c','o','l','r', 'n','c','l','x',
		0x00,0x09, 0x00,0x10, 0x00,0x09, 0x80 };
	FILE *pbf = fopen(pf, "wb"); fwrite(twobox, 1, sizeof twobox, pbf); fclose(pbf);
	CHECK(image_transfer_characteristics(pf) == 16,
		"image_transfer_characteristics skips an ICC 'prof' colr box and finds the nclx one");
	g_unlink(pf); g_free(pf);

	char *cf = NULL;
	gint cfd = g_file_open_tmp("wbdisp-colr-XXXXXX.bin", &cf, NULL);
	CHECK(cfd >= 0, "test can create a temp file for the colr parser");
	close(cfd);
	// minimal 'colr'/'nclx' box: primaries=9 transfer=16 matrix=9
	const unsigned char box[] = {
		0x00,0x00,0x00,0x13, 'c','o','l','r', 'n','c','l','x',
		0x00,0x09, 0x00,0x10, 0x00,0x09, 0x80 };
	unsigned char pad[64] = {0};
	FILE *bf = fopen(cf, "wb");
	fwrite(pad, 1, sizeof pad, bf); fwrite(box, 1, sizeof box, bf); fclose(bf);
	CHECK(image_transfer_characteristics(cf) == 16,
		"image_transfer_characteristics finds a colr/nclx box past a header");
	g_unlink(cf); g_free(cf);

	char *nf = NULL;
	gint nfd = g_file_open_tmp("wbdisp-nocolr-XXXXXX.bin", &nf, NULL);
	close(nfd);
	g_file_set_contents(nf, "not an avif at all", -1, NULL);
	CHECK(image_transfer_characteristics(nf) == -1,
		"image_transfer_characteristics returns -1 when there is no colr box");
	CHECK(image_transfer_characteristics("/nonexistent/xx.avif") == -1,
		"image_transfer_characteristics returns -1 for an unreadable file");
	g_unlink(nf); g_free(nf);

	printf("----\n%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
