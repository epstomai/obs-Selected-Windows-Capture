/*
Selected Windows Capture
Copyright (C) 2026 obs-selected-windows-moncap contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "selected-windows-capture.h"
#include "win-enum.h"

#include <util/platform.h>
#include <util/threading.h>

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_OBS_FRONTEND_API
#include <obs-frontend-api.h>
#endif

#define MAX_SELECTED SWC_MAX_WINDOWS
#define MAX_COMPOSITE 64
#define SWC_METHOD_BITBLT 1
#define SWC_METHOD_WGC 2
#define SWC_HWND_LINGER 0.75f
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

#define SWC_TEXT(x) obs_module_text(x)

struct swc_sel {
	char encoded[SWC_STR];
};

struct swc_child {
	HWND hwnd;
	RECT rect;
	char encoded[SWC_STR];
	char filter_key[SWC_STR];
	obs_source_t *source;
	int method;
	float dead_time;
};

struct swc_filter_bank {
	char key[SWC_STR];
	obs_data_array_t *filters;
};

struct swc_drop_src {
	obs_source_t *source;
	char filter_key[SWC_STR];
};

struct swc_method_pref {
	HWND hwnd;
	int method;
	int flips;
};

struct swc_source {
	obs_source_t *source;
	obs_source_t *display;

	pthread_mutex_t mutex;

	char monitor_id[128];
	struct swc_monitor monitor;
	bool monitor_ok;

	int layout;
	int filter_mode;
	int match_mode;
	int capture_method;
	bool capture_cursor;
	bool force_sdr;
	bool include_tool_windows;
	bool follow_active;
	int hole_mode;
	uint32_t hole_color;

	char applied_monitor_id[128];
	int applied_method;
	bool applied_cursor;
	bool applied_sdr;
	bool display_configured;

	struct swc_sel selected[MAX_SELECTED];
	int selected_count;

	struct swc_window windows[SWC_MAX_WINDOWS];
	bool keep[SWC_MAX_WINDOWS];
	float unseen[SWC_MAX_WINDOWS];
	int window_count;

	struct swc_child children[MAX_COMPOSITE];
	struct swc_method_pref prefs[MAX_COMPOSITE];
	int pref_n;
	struct swc_filter_bank filter_bank[MAX_SELECTED];
	int filter_bank_n;
	float filter_save_accum;
	float display_dead;
	bool mask_failed;

	gs_effect_t *mask_effect;
	gs_texrender_t *capture_rt;
	gs_texrender_t *mask_rt;

	uint32_t width;
	uint32_t height;

	volatile bool destroying;
	bool captures_live;
};

#define SWC_MAX_INSTANCES 16

static pthread_mutex_t swc_live_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct swc_source *swc_live[SWC_MAX_INSTANCES];
static int swc_live_n;
#ifdef HAVE_OBS_FRONTEND_API
static bool swc_frontend_hooked;
#endif

static void destroy_all_children(struct swc_source *ctx);
static void destroy_display(struct swc_source *ctx);

static void swc_live_add(struct swc_source *ctx)
{
	pthread_mutex_lock(&swc_live_mutex);
	if (swc_live_n < SWC_MAX_INSTANCES)
		swc_live[swc_live_n++] = ctx;
	pthread_mutex_unlock(&swc_live_mutex);
}

static void swc_live_remove(struct swc_source *ctx)
{
	pthread_mutex_lock(&swc_live_mutex);
	for (int i = 0; i < swc_live_n; i++) {
		if (swc_live[i] != ctx)
			continue;
		swc_live[i] = swc_live[swc_live_n - 1];
		swc_live_n--;
		break;
	}
	pthread_mutex_unlock(&swc_live_mutex);
}

static bool swc_has_captures(struct swc_source *ctx)
{
	bool live = false;

	if (!ctx)
		return false;
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->display)
		live = true;
	for (int i = 0; i < MAX_COMPOSITE && !live; i++) {
		if (ctx->children[i].source)
			live = true;
	}
	pthread_mutex_unlock(&ctx->mutex);
	return live;
}

static void swc_pump_ms(uint32_t ms)
{
#ifdef _WIN32
	const DWORD end = GetTickCount() + ms;

	while ((LONG)(end - GetTickCount()) > 0) {
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				PostQuitMessage((int)msg.wParam);
				return;
			}
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		MsgWaitForMultipleObjects(0, NULL, FALSE, 8, QS_ALLINPUT);
	}
#else
	os_sleep_ms(ms);
#endif
}

static void swc_request_stop(struct swc_source *ctx)
{
	if (!ctx)
		return;
	os_atomic_set_bool(&ctx->destroying, true);
}

static void swc_wait_captures_stopped(struct swc_source *ctx)
{
	const DWORD end = GetTickCount() + 1500;

	if (!ctx)
		return;
	os_atomic_set_bool(&ctx->destroying, true);
	while (swc_has_captures(ctx) && (LONG)(end - GetTickCount()) > 0)
		swc_pump_ms(16);
}

#ifdef HAVE_OBS_FRONTEND_API
static void swc_frontend_event(enum obs_frontend_event event, void *unused)
{
	struct swc_source *copy[SWC_MAX_INSTANCES];
	int n = 0;

	UNUSED_PARAMETER(unused);
	if (event != OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN &&
	    event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP &&
	    event != OBS_FRONTEND_EVENT_EXIT)
		return;

	pthread_mutex_lock(&swc_live_mutex);
	n = swc_live_n;
	for (int i = 0; i < n; i++)
		copy[i] = swc_live[i];
	pthread_mutex_unlock(&swc_live_mutex);

	for (int i = 0; i < n; i++)
		swc_request_stop(copy[i]);
	for (int i = 0; i < n; i++)
		swc_wait_captures_stopped(copy[i]);
}
#endif

void swc_capture_module_load(void)
{
#ifdef HAVE_OBS_FRONTEND_API
	if (!swc_frontend_hooked) {
		obs_frontend_add_event_callback(swc_frontend_event, NULL);
		swc_frontend_hooked = true;
	}
#endif
}

void swc_capture_module_unload(void)
{
#ifdef HAVE_OBS_FRONTEND_API
	if (swc_frontend_hooked) {
		obs_frontend_remove_event_callback(swc_frontend_event, NULL);
		swc_frontend_hooked = false;
	}
#endif
}

/* ------------------------------------------------------------------------- */

static bool encoded_in_selected(struct swc_source *ctx, const struct swc_window *window)
{
	for (int i = 0; i < ctx->selected_count; i++) {
		if (swc_window_matches(ctx->match_mode, window, ctx->selected[i].encoded))
			return true;
	}
	return false;
}

static bool exe_is_obs(const char *exe)
{
	return exe && exe[0] &&
	       (_stricmp(exe, "obs64.exe") == 0 || _stricmp(exe, "obs32.exe") == 0 || _stricmp(exe, "obs.exe") == 0);
}

static bool window_is_obs(const struct swc_window *window)
{
	return window && exe_is_obs(window->exe);
}

static bool window_should_keep(struct swc_source *ctx, const struct swc_window *window)
{
	const bool listed = encoded_in_selected(ctx, window);

	if (window_is_obs(window))
		return false;
	return ctx->filter_mode == SWC_MODE_INCLUDE ? listed : !listed;
}

static long window_area(const struct swc_window *window)
{
	long w;
	long h;

	if (!window)
		return 0;
	w = (long)window->rect.right - (long)window->rect.left;
	h = (long)window->rect.bottom - (long)window->rect.top;
	if (w < 0)
		w = 0;
	if (h < 0)
		h = 0;
	return w * h;
}

static void drop_small_satellites(struct swc_window *list, bool *keep, int n)
{
	bool done[SWC_MAX_WINDOWS];

	memset(done, 0, sizeof(done));
	for (int i = 0; i < n; i++) {
		long max_area = 0;

		if (!keep[i] || done[i] || !list[i].exe[0])
			continue;
		if (!swc_match_by_exe_only(list[i].exe, list[i].class_name))
			continue;
		for (int j = 0; j < n; j++) {
			if (!keep[j] || _stricmp(list[j].exe, list[i].exe) != 0)
				continue;
			done[j] = true;
			if (window_area(&list[j]) > max_area)
				max_area = window_area(&list[j]);
		}
		if (max_area < 20000)
			continue;
		for (int j = 0; j < n; j++) {
			if (!keep[j] || _stricmp(list[j].exe, list[i].exe) != 0)
				continue;
			if (window_area(&list[j]) * 8L < max_area)
				keep[j] = false;
		}
	}
}

static void load_selected(struct swc_source *ctx, obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_get_array(settings, "windows");
	int count = 0;

	if (arr) {
		const size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n && count < MAX_SELECTED; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			const char *val = obs_data_get_string(item, "value");
			if (val && val[0]) {
				snprintf(ctx->selected[count].encoded, sizeof(ctx->selected[count].encoded), "%s", val);
				count++;
			}
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}
	ctx->selected_count = count;
}

static void array_add_unique(obs_data_t *settings, const char *encoded)
{
	obs_data_array_t *arr;
	size_t n;

	if (!encoded || !encoded[0])
		return;

	arr = obs_data_get_array(settings, "windows");
	if (!arr) {
		arr = obs_data_array_create();
		obs_data_set_array(settings, "windows", arr);
	}

	n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *val = obs_data_get_string(item, "value");
		const bool same = val && strcmp(val, encoded) == 0;
		obs_data_release(item);
		if (same) {
			obs_data_array_release(arr);
			return;
		}
	}

	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "value", encoded);
	obs_data_array_push_back(arr, item);
	obs_data_release(item);
	obs_data_array_release(arr);
}

static void array_remove_encoded(obs_data_t *settings, const char *encoded)
{
	obs_data_array_t *arr;
	size_t n;

	if (!encoded || !encoded[0])
		return;

	arr = obs_data_get_array(settings, "windows");
	if (!arr)
		return;

	n = obs_data_array_count(arr);
	for (size_t i = n; i > 0; i--) {
		obs_data_t *item = obs_data_array_item(arr, i - 1);
		const char *val = obs_data_get_string(item, "value");
		const bool same = val && strcmp(val, encoded) == 0;
		obs_data_release(item);
		if (same)
			obs_data_array_erase(arr, i - 1);
	}
	obs_data_array_release(arr);
}

static void ascii_lower_copy(const char *src, char *dst, size_t dst_sz)
{
	size_t i = 0;
	if (!dst || !dst_sz)
		return;
	if (!src)
		src = "";
	for (; src[i] && i + 1 < dst_sz; i++) {
		char c = src[i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		dst[i] = c;
	}
	dst[i] = 0;
}

static void group_key_from_parts(const char *title, const char *class_name, const char *exe, char *key, size_t key_sz)
{
	char exel[256];

	if (!key || !key_sz)
		return;
	ascii_lower_copy(exe, exel, sizeof(exel));
	if (class_name && strcmp(class_name, "ApplicationFrameWindow") == 0)
		snprintf(key, key_sz, "t:%s:%s", title ? title : "", exel);
	else if (swc_match_by_exe_only(exe, class_name))
		snprintf(key, key_sz, "e:%s", exel);
	else
		snprintf(key, key_sz, "c:%s:%s", class_name ? class_name : "", exel);
}

static void group_key_from_encoded(const char *encoded, char *key, size_t key_sz)
{
	char title[512];
	char class_name[256];
	char exe[256];

	if (!key || !key_sz)
		return;
	key[0] = 0;
	if (!encoded || !encoded[0])
		return;
	if (!swc_parse_encoded(encoded, title, sizeof(title), class_name, sizeof(class_name), exe, sizeof(exe))) {
		snprintf(key, key_sz, "x:%s", encoded);
		return;
	}
	group_key_from_parts(title, class_name, exe, key, key_sz);
}

static void array_remove_group(obs_data_t *settings, const char *encoded);

static bool group_is_selected(obs_data_t *settings, const char *key)
{
	obs_data_array_t *arr;
	size_t n;

	if (!settings || !key || !key[0])
		return false;
	arr = obs_data_get_array(settings, "windows");
	if (!arr)
		return false;
	n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *val = obs_data_get_string(item, "value");
		char got[SWC_STR];

		group_key_from_encoded(val, got, sizeof(got));
		obs_data_release(item);
		if (got[0] && strcmp(got, key) == 0) {
			obs_data_array_release(arr);
			return true;
		}
	}
	obs_data_array_release(arr);
	return false;
}

static void array_upsert_group(obs_data_t *settings, const char *encoded)
{
	if (!encoded || !encoded[0])
		return;
	array_remove_group(settings, encoded);
	array_add_unique(settings, encoded);
}

static void array_remove_group(obs_data_t *settings, const char *encoded)
{
	obs_data_array_t *arr;
	char want[SWC_STR];
	size_t n;

	if (!encoded || !encoded[0])
		return;
	group_key_from_encoded(encoded, want, sizeof(want));
	if (!want[0]) {
		array_remove_encoded(settings, encoded);
		return;
	}

	arr = obs_data_get_array(settings, "windows");
	if (!arr)
		return;

	n = obs_data_array_count(arr);
	for (size_t i = n; i > 0; i--) {
		obs_data_t *item = obs_data_array_item(arr, i - 1);
		const char *val = obs_data_get_string(item, "value");
		char got[SWC_STR];
		group_key_from_encoded(val, got, sizeof(got));
		obs_data_release(item);
		if (got[0] && strcmp(got, want) == 0)
			obs_data_array_erase(arr, i - 1);
	}
	obs_data_array_release(arr);
}

static void persist_monitor_id(obs_data_t *settings)
{
	const char *id = obs_data_get_string(settings, "monitor_id");
	struct swc_monitor mon;

	if (id && id[0] && strcmp(id, "DUMMY") != 0)
		return;
	if (swc_active_monitor(&mon))
		obs_data_set_string(settings, "monitor_id", mon.device_id);
}

static int enum_picker_windows_ex(struct swc_source *ctx, obs_data_t *settings, struct swc_window *out, int max)
{
	uint32_t flags = SWC_ENUM_PICKER | SWC_ENUM_MINIMIZED;

	UNUSED_PARAMETER(ctx);
	if (settings && obs_data_get_bool(settings, "tool_windows"))
		flags |= SWC_ENUM_TOOLWINDOWS;
	return swc_enum_windows(NULL, flags, out, max);
}

static int enum_picker_windows(obs_data_t *settings, struct swc_window *out, int max)
{
	return enum_picker_windows_ex(NULL, settings, out, max);
}

static bool resolve_window_from_list(obs_data_t *settings, const char *pick, char *encoded, size_t encoded_sz)
{
	struct swc_window windows[SWC_MAX_WINDOWS];
	int n;

	if (!pick || !pick[0] || !encoded || !encoded_sz)
		return false;

	n = enum_picker_windows(settings, windows, SWC_MAX_WINDOWS);
	for (int i = 0; i < n; i++) {
		if (strcmp(windows[i].encoded, pick) == 0 || strcmp(windows[i].desc, pick) == 0) {
			snprintf(encoded, encoded_sz, "%s", windows[i].encoded);
			return true;
		}
	}

	if (strchr(pick, ':')) {
		snprintf(encoded, encoded_sz, "%s", pick);
		return true;
	}
	return false;
}

static void commit_selected(struct swc_source *ctx, obs_data_t *settings)
{
	pthread_mutex_lock(&ctx->mutex);
	load_selected(ctx, settings);
	pthread_mutex_unlock(&ctx->mutex);
	obs_source_update(ctx->source, settings);
}

static void swc_bank_load_unlocked(struct swc_source *ctx, obs_data_t *settings);
static void swc_bank_save_to_settings(struct swc_source *ctx, obs_data_t *settings);
static void persist_bank_to_source_settings(struct swc_source *ctx);

static void migrate_pick_window(obs_data_t *settings)
{
	obs_data_array_t *arr = obs_data_get_array(settings, "windows");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	const char *pick;
	char encoded[SWC_STR];

	if (arr)
		obs_data_array_release(arr);
	if (n > 0)
		return;

	pick = obs_data_get_string(settings, "pick_window");
	if (!pick || !pick[0])
		return;
	if (resolve_window_from_list(settings, pick, encoded, sizeof(encoded)))
		array_add_unique(settings, encoded);
}

static void apply_settings(struct swc_source *ctx, obs_data_t *settings)
{
	const char *monitor_id;

	persist_monitor_id(settings);
	migrate_pick_window(settings);
	if (!obs_data_get_bool(settings, "migrated_wgc_composite")) {
		obs_data_set_int(settings, "layout", SWC_LAYOUT_COMPOSITE);
		obs_data_set_int(settings, "method", SWC_METHOD_WGC);
		obs_data_set_bool(settings, "migrated_wgc_composite", true);
	}
	monitor_id = obs_data_get_string(settings, "monitor_id");

	pthread_mutex_lock(&ctx->mutex);
	snprintf(ctx->monitor_id, sizeof(ctx->monitor_id), "%s", monitor_id ? monitor_id : "");
	ctx->filter_mode = (int)obs_data_get_int(settings, "filter_mode");
	ctx->layout = SWC_LAYOUT_COMPOSITE;
	ctx->match_mode = SWC_MATCH_CLASS_EXE;
	ctx->capture_method = SWC_METHOD_WGC;
	ctx->capture_cursor = true;
	ctx->force_sdr = false;
	ctx->include_tool_windows = false;
	ctx->follow_active = true;
	ctx->hole_mode = 0;
	ctx->hole_color = 0;
	ctx->mask_failed = false;
	ctx->display_dead = 0.0f;
	load_selected(ctx, settings);
	swc_bank_load_unlocked(ctx, settings);
	pthread_mutex_unlock(&ctx->mutex);
}

static void filter_key_from_encoded(const char *encoded, char *key, size_t key_sz)
{
	char title[512];
	char class_name[256];
	char exe[256];

	if (!key || !key_sz)
		return;
	key[0] = '\0';
	if (!encoded || !encoded[0])
		return;
	if (!swc_parse_encoded(encoded, title, sizeof(title), class_name, sizeof(class_name), exe, sizeof(exe))) {
		snprintf(key, key_sz, "%s", encoded);
		return;
	}
	if (class_name[0] || exe[0])
		snprintf(key, key_sz, "%s:%s", class_name, exe);
	else
		snprintf(key, key_sz, "%s", encoded);
}

static void swc_bank_clear_unlocked(struct swc_source *ctx)
{
	for (int i = 0; i < ctx->filter_bank_n; i++) {
		if (ctx->filter_bank[i].filters) {
			obs_data_array_release(ctx->filter_bank[i].filters);
			ctx->filter_bank[i].filters = NULL;
		}
		ctx->filter_bank[i].key[0] = '\0';
	}
	ctx->filter_bank_n = 0;
}

static void swc_bank_set_unlocked(struct swc_source *ctx, const char *key, obs_data_array_t *filters)
{
	int i;
	const bool empty = !filters || obs_data_array_count(filters) == 0;

	if (!key || !key[0])
		return;

	for (i = 0; i < ctx->filter_bank_n; i++) {
		if (strcmp(ctx->filter_bank[i].key, key) == 0)
			break;
	}

	if (i == ctx->filter_bank_n) {
		if (empty)
			return;
		if (ctx->filter_bank_n >= MAX_SELECTED)
			return;
		snprintf(ctx->filter_bank[i].key, sizeof(ctx->filter_bank[i].key), "%s", key);
		ctx->filter_bank[i].filters = NULL;
		ctx->filter_bank_n++;
	}

	if (ctx->filter_bank[i].filters) {
		obs_data_array_release(ctx->filter_bank[i].filters);
		ctx->filter_bank[i].filters = NULL;
	}

	if (empty) {
		ctx->filter_bank[i] = ctx->filter_bank[ctx->filter_bank_n - 1];
		memset(&ctx->filter_bank[ctx->filter_bank_n - 1], 0, sizeof(ctx->filter_bank[0]));
		ctx->filter_bank_n--;
		return;
	}

	obs_data_array_addref(filters);
	ctx->filter_bank[i].filters = filters;
}

static void swc_bank_load_unlocked(struct swc_source *ctx, obs_data_t *settings)
{
	obs_data_array_t *arr;
	size_t n;

	swc_bank_clear_unlocked(ctx);
	if (!settings)
		return;

	arr = obs_data_get_array(settings, "window_filters");
	if (!arr)
		return;

	n = obs_data_array_count(arr);
	for (size_t i = 0; i < n && ctx->filter_bank_n < MAX_SELECTED; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *key = obs_data_get_string(item, "key");
		obs_data_array_t *filters = obs_data_get_array(item, "filters");
		swc_bank_set_unlocked(ctx, key, filters);
		if (filters)
			obs_data_array_release(filters);
		obs_data_release(item);
	}
	obs_data_array_release(arr);
}

static void swc_bank_save_to_settings(struct swc_source *ctx, obs_data_t *settings)
{
	obs_data_array_t *arr;

	if (!settings)
		return;

	arr = obs_data_array_create();
	pthread_mutex_lock(&ctx->mutex);
	for (int i = 0; i < ctx->filter_bank_n; i++) {
		obs_data_t *item;
		if (!ctx->filter_bank[i].key[0] || !ctx->filter_bank[i].filters)
			continue;
		item = obs_data_create();
		obs_data_set_string(item, "key", ctx->filter_bank[i].key);
		obs_data_set_array(item, "filters", ctx->filter_bank[i].filters);
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&ctx->mutex);
	obs_data_set_array(settings, "window_filters", arr);
	obs_data_array_release(arr);
}

static void persist_bank_to_source_settings(struct swc_source *ctx)
{
	obs_data_t *settings;
	if (!ctx || !ctx->source)
		return;
	settings = obs_source_get_settings(ctx->source);
	swc_bank_save_to_settings(ctx, settings);
	obs_data_release(settings);
}

static void harvest_filters_from_source(struct swc_source *ctx, obs_source_t *src, const char *key)
{
	obs_data_t *data;
	obs_data_array_t *filters;

	if (!ctx || !src || !key || !key[0])
		return;

	data = obs_save_source(src);
	if (!data)
		return;
	filters = obs_data_get_array(data, "filters");
	pthread_mutex_lock(&ctx->mutex);
	swc_bank_set_unlocked(ctx, key, filters);
	pthread_mutex_unlock(&ctx->mutex);
	if (filters)
		obs_data_array_release(filters);
	obs_data_release(data);
}

static void apply_bank_filters(struct swc_source *ctx, obs_source_t *src, const char *key)
{
	obs_data_array_t *filters = NULL;
	size_t n;

	if (!ctx || !src || !key || !key[0])
		return;

	pthread_mutex_lock(&ctx->mutex);
	for (int i = 0; i < ctx->filter_bank_n; i++) {
		if (strcmp(ctx->filter_bank[i].key, key) != 0)
			continue;
		filters = ctx->filter_bank[i].filters;
		if (filters)
			obs_data_array_addref(filters);
		break;
	}
	pthread_mutex_unlock(&ctx->mutex);

	if (!filters)
		return;

	n = obs_data_array_count(filters);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(filters, i);
		obs_source_t *filter;
		if (!item)
			continue;
		obs_data_set_string(item, "uuid", "");
		filter = obs_load_private_source(item);
		if (filter) {
			obs_source_filter_add(src, filter);
			obs_source_release(filter);
		}
		obs_data_release(item);
	}
	obs_data_array_release(filters);
}

static void on_child_filter_change(void *data, calldata_t *cd)
{
	struct swc_source *ctx = data;
	obs_source_t *source = calldata_ptr(cd, "source");
	char key[SWC_STR] = {0};

	if (!ctx || !source || os_atomic_load_bool(&ctx->destroying))
		return;

	pthread_mutex_lock(&ctx->mutex);
	for (int i = 0; i < MAX_COMPOSITE; i++) {
		if (ctx->children[i].source == source) {
			snprintf(key, sizeof(key), "%s", ctx->children[i].filter_key);
			break;
		}
	}
	pthread_mutex_unlock(&ctx->mutex);

	if (!key[0]) {
		obs_data_t *s = obs_source_get_settings(source);
		const char *window = obs_data_get_string(s, "window");
		filter_key_from_encoded(window, key, sizeof(key));
		obs_data_release(s);
	}
	if (!key[0])
		return;

	harvest_filters_from_source(ctx, source, key);
	persist_bank_to_source_settings(ctx);
}

static void hook_child_filters(struct swc_source *ctx, obs_source_t *src)
{
	signal_handler_t *h;
	if (!ctx || !src)
		return;
	h = obs_source_get_signal_handler(src);
	if (!h)
		return;
	signal_handler_connect(h, "filter_add", on_child_filter_change, ctx);
	signal_handler_connect(h, "filter_remove", on_child_filter_change, ctx);
	signal_handler_connect(h, "reorder_filters", on_child_filter_change, ctx);
}

static void unhook_child_filters(struct swc_source *ctx, obs_source_t *src)
{
	signal_handler_t *h;
	if (!ctx || !src)
		return;
	h = obs_source_get_signal_handler(src);
	if (!h)
		return;
	signal_handler_disconnect(h, "filter_add", on_child_filter_change, ctx);
	signal_handler_disconnect(h, "filter_remove", on_child_filter_change, ctx);
	signal_handler_disconnect(h, "reorder_filters", on_child_filter_change, ctx);
}

static void flush_dropped_children(struct swc_source *ctx, struct swc_drop_src *drop, int drop_n, bool persist)
{
	const bool shutting_down = ctx && os_atomic_load_bool(&ctx->destroying);

	for (int i = 0; i < drop_n; i++) {
		if (!drop[i].source)
			continue;
		unhook_child_filters(ctx, drop[i].source);
		if (persist && !shutting_down)
			harvest_filters_from_source(ctx, drop[i].source, drop[i].filter_key);
		obs_source_remove_active_child(ctx->source, drop[i].source);
		obs_source_release(drop[i].source);
	}
	if (drop_n && persist && !shutting_down)
		persist_bank_to_source_settings(ctx);
}

static void queue_drop_child(struct swc_source *ctx, int i, struct swc_drop_src *drop, int *drop_n)
{
	if (!ctx->children[i].source || *drop_n >= MAX_COMPOSITE)
		return;
	drop[*drop_n].source = ctx->children[i].source;
	snprintf(drop[*drop_n].filter_key, sizeof(drop[*drop_n].filter_key), "%s", ctx->children[i].filter_key);
	memset(&ctx->children[i], 0, sizeof(ctx->children[i]));
	(*drop_n)++;
}

static obs_source_t *create_window_capture(const char *encoded, int method, bool cursor, bool force_sdr)
{
	obs_data_t *s;
	obs_source_t *src;
	char title[512];
	char class_name[256];
	char exe[256];
	char name[512];

	if (!encoded || !encoded[0])
		return NULL;

	title[0] = 0;
	class_name[0] = 0;
	exe[0] = 0;
	if (swc_parse_encoded(encoded, title, sizeof(title), class_name, sizeof(class_name), exe, sizeof(exe)))
		snprintf(name, sizeof(name), "%s (%s)", title[0] ? title : class_name, exe);
	else
		snprintf(name, sizeof(name), "%s", encoded);

	s = obs_data_create();
	obs_data_set_string(s, "window", encoded);
	obs_data_set_int(s, "method", method);
	obs_data_set_int(s, "priority", swc_window_capture_priority(exe, class_name));
	obs_data_set_bool(s, "cursor", cursor);
	obs_data_set_bool(s, "client_area", swc_wgc_client_area(exe, class_name));
	obs_data_set_bool(s, "compatibility",
			 method == SWC_METHOD_BITBLT || swc_is_paint_tool_sai(exe, class_name) ||
				 swc_class_is_unstable(class_name));
	obs_data_set_bool(s, "force_sdr", force_sdr);
	src = obs_source_create_private("window_capture", name, s);
	obs_data_release(s);
	return src;
}

static void remember_display_settings(struct swc_source *ctx, const char *monitor_id, int method, bool cursor,
				      bool force_sdr)
{
	snprintf(ctx->applied_monitor_id, sizeof(ctx->applied_monitor_id), "%s", monitor_id ? monitor_id : "");
	ctx->applied_method = method;
	ctx->applied_cursor = cursor;
	ctx->applied_sdr = force_sdr;
	ctx->display_configured = true;
}

static bool display_settings_match(struct swc_source *ctx, const char *monitor_id, int method, bool cursor,
				   bool force_sdr)
{
	return ctx->display_configured && ctx->applied_method == method && ctx->applied_cursor == cursor &&
	       ctx->applied_sdr == force_sdr && strcmp(ctx->applied_monitor_id, monitor_id ? monitor_id : "") == 0;
}

static void update_display_settings(struct swc_source *ctx)
{
	obs_data_t *s;
	obs_source_t *display;
	char monitor_id[128];
	int method;
	bool cursor;
	bool force_sdr;

	pthread_mutex_lock(&ctx->mutex);
	display = ctx->display ? obs_source_get_ref(ctx->display) : NULL;
	snprintf(monitor_id, sizeof(monitor_id), "%s", ctx->monitor_id);
	method = ctx->capture_method;
	cursor = ctx->capture_cursor;
	force_sdr = ctx->force_sdr;
	if (display && display_settings_match(ctx, monitor_id, method, cursor, force_sdr)) {
		pthread_mutex_unlock(&ctx->mutex);
		obs_source_release(display);
		return;
	}
	remember_display_settings(ctx, monitor_id, method, cursor, force_sdr);
	pthread_mutex_unlock(&ctx->mutex);

	if (!display)
		return;

	s = obs_data_create();
	obs_data_set_string(s, "monitor_id", monitor_id);
	obs_data_set_int(s, "method", method);
	obs_data_set_bool(s, "capture_cursor", cursor);
	obs_data_set_bool(s, "force_sdr", force_sdr);
	obs_source_update(display, s);
	obs_data_release(s);
	obs_source_release(display);
}

static void destroy_display(struct swc_source *ctx)
{
	obs_source_t *display;

	pthread_mutex_lock(&ctx->mutex);
	display = ctx->display;
	ctx->display = NULL;
	pthread_mutex_unlock(&ctx->mutex);

	if (!display)
		return;
	ctx->display_configured = false;
	obs_source_remove_active_child(ctx->source, display);
	obs_source_release(display);
}

static void ensure_display(struct swc_source *ctx)
{
	obs_data_t *s;
	obs_source_t *display;
	char monitor_id[128];
	int method;
	bool cursor;
	bool force_sdr;

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->display) {
		pthread_mutex_unlock(&ctx->mutex);
		return;
	}
	snprintf(monitor_id, sizeof(monitor_id), "%s", ctx->monitor_id);
	method = ctx->capture_method;
	cursor = ctx->capture_cursor;
	force_sdr = ctx->force_sdr;
	pthread_mutex_unlock(&ctx->mutex);

	if (!monitor_id[0]) {
		struct swc_monitor mon;
		if (swc_active_monitor(&mon) || swc_find_monitor("", &mon))
			snprintf(monitor_id, sizeof(monitor_id), "%s", mon.device_id);
	}

	s = obs_data_create();
	obs_data_set_string(s, "monitor_id", monitor_id);
	obs_data_set_int(s, "method", method);
	obs_data_set_bool(s, "capture_cursor", cursor);
	obs_data_set_bool(s, "force_sdr", force_sdr);
	display = obs_source_create_private("monitor_capture", "swc-monitor", s);
	obs_data_release(s);

	if (!display) {
		blog(LOG_ERROR, "[selected-windows-capture] Failed to create monitor_capture source");
		return;
	}

	obs_source_add_active_child(ctx->source, display);

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->display) {
		pthread_mutex_unlock(&ctx->mutex);
		obs_source_remove_active_child(ctx->source, display);
		obs_source_release(display);
		return;
	}
	ctx->display = display;
	remember_display_settings(ctx, monitor_id, method, cursor, force_sdr);
	pthread_mutex_unlock(&ctx->mutex);
}

static void destroy_all_children(struct swc_source *ctx)
{
	struct swc_drop_src drop[MAX_COMPOSITE];
	int n = 0;

	pthread_mutex_lock(&ctx->mutex);
	for (int i = 0; i < MAX_COMPOSITE; i++)
		queue_drop_child(ctx, i, drop, &n);
	pthread_mutex_unlock(&ctx->mutex);

	flush_dropped_children(ctx, drop, n, !os_atomic_load_bool(&ctx->destroying));
}

static int method_for_window(const struct swc_window *w)
{
	UNUSED_PARAMETER(w);
	return SWC_METHOD_WGC;
}

static bool encoded_needs_wgc(const char *encoded)
{
	char title[512];
	char class_name[256];
	char exe[256];

	if (!encoded || !encoded[0])
		return false;
	if (!swc_parse_encoded(encoded, title, sizeof(title), class_name, sizeof(class_name), exe, sizeof(exe)))
		return false;
	return swc_needs_wgc_only(exe, class_name);
}

static int lookup_method_pref(struct swc_source *ctx, HWND hwnd, int fallback)
{
	for (int i = 0; i < ctx->pref_n; i++) {
		if (ctx->prefs[i].hwnd == hwnd)
			return ctx->prefs[i].method;
	}
	return fallback;
}

static void remember_method_pref(struct swc_source *ctx, HWND hwnd, int method)
{
	for (int i = 0; i < ctx->pref_n; i++) {
		if (ctx->prefs[i].hwnd == hwnd) {
			ctx->prefs[i].method = method;
			ctx->prefs[i].flips++;
			return;
		}
	}
	if (ctx->pref_n >= MAX_COMPOSITE)
		return;
	ctx->prefs[ctx->pref_n].hwnd = hwnd;
	ctx->prefs[ctx->pref_n].method = method;
	ctx->prefs[ctx->pref_n].flips = 1;
	ctx->pref_n++;
}

static int pref_flips(struct swc_source *ctx, HWND hwnd)
{
	for (int i = 0; i < ctx->pref_n; i++) {
		if (ctx->prefs[i].hwnd == hwnd)
			return ctx->prefs[i].flips;
	}
	return 0;
}

static void sync_composite_children(struct swc_source *ctx)
{
	HWND keep_hwnds[MAX_COMPOSITE];
	char keep_enc[MAX_COMPOSITE][SWC_STR];
	char keep_key[MAX_COMPOSITE][SWC_STR];
	char sel_enc[MAX_SELECTED][SWC_STR];
	RECT keep_rect[MAX_COMPOSITE];
	int keep_methods[MAX_COMPOSITE];
	int keep_n = 0;
	int sel_n = 0;

	if (os_atomic_load_bool(&ctx->destroying))
		return;
	int filter_mode;

	pthread_mutex_lock(&ctx->mutex);
	filter_mode = ctx->filter_mode;
	for (int i = 0; i < ctx->window_count && keep_n < MAX_COMPOSITE; i++) {
		if (!ctx->keep[i])
			continue;
		keep_hwnds[keep_n] = ctx->windows[i].hwnd;
		snprintf(keep_enc[keep_n], sizeof(keep_enc[keep_n]), "%s", ctx->windows[i].encoded);
		filter_key_from_encoded(keep_enc[keep_n], keep_key[keep_n], sizeof(keep_key[keep_n]));
		keep_rect[keep_n] = ctx->windows[i].rect;
		keep_methods[keep_n] =
			lookup_method_pref(ctx, ctx->windows[i].hwnd, method_for_window(&ctx->windows[i]));
		if (encoded_needs_wgc(keep_enc[keep_n]))
			keep_methods[keep_n] = SWC_METHOD_WGC;
		keep_n++;
	}
	sel_n = ctx->selected_count;
	if (sel_n > MAX_SELECTED)
		sel_n = MAX_SELECTED;
	for (int i = 0; i < sel_n; i++)
		snprintf(sel_enc[i], sizeof(sel_enc[i]), "%s", ctx->selected[i].encoded);
	pthread_mutex_unlock(&ctx->mutex);

	if (filter_mode == SWC_MODE_INCLUDE) {
		for (int i = 0; i < sel_n && keep_n < MAX_COMPOSITE; i++) {
			char key[SWC_STR];
			bool already = false;
			RECT empty = {0};

			filter_key_from_encoded(sel_enc[i], key, sizeof(key));
			if (!key[0])
				continue;
			for (int k = 0; k < keep_n; k++) {
				if (strcmp(keep_key[k], key) == 0) {
					already = true;
					break;
				}
			}
			if (already)
				continue;
			keep_hwnds[keep_n] = NULL;
			snprintf(keep_enc[keep_n], sizeof(keep_enc[keep_n]), "%s", sel_enc[i]);
			snprintf(keep_key[keep_n], sizeof(keep_key[keep_n]), "%s", key);
			keep_rect[keep_n] = empty;
			keep_methods[keep_n] = SWC_METHOD_WGC;
			keep_n++;
		}
	}

	struct swc_drop_src drop[MAX_COMPOSITE];
	int drop_n = 0;
	int keep_child[MAX_COMPOSITE];
	bool child_used[MAX_COMPOSITE];

	for (int k = 0; k < keep_n; k++)
		keep_child[k] = -1;
	for (int i = 0; i < MAX_COMPOSITE; i++)
		child_used[i] = false;

	pthread_mutex_lock(&ctx->mutex);

	/* Assign a capture to each live window by HWND first. Matching by the
	 * class+executable key alone is not enough: several windows share a key
	 * (Packet Tracer dialogs, Chrome windows, Notepad tabs), so a capture
	 * whose window closed would be kept forever and drawn at its last
	 * position as a duplicate. */
	for (int k = 0; k < keep_n; k++) {
		if (!keep_hwnds[k])
			continue;
		for (int i = 0; i < MAX_COMPOSITE; i++) {
			if (!ctx->children[i].source || child_used[i])
				continue;
			if (ctx->children[i].method != keep_methods[k])
				continue;
			if (ctx->children[i].hwnd != keep_hwnds[k])
				continue;
			keep_child[k] = i;
			child_used[i] = true;
			break;
		}
	}

	/* A selected window that is not open yet has no HWND. Reuse its capture
	 * by key so filters survive the window temporarily disappearing. */
	for (int k = 0; k < keep_n; k++) {
		if (keep_child[k] >= 0 || keep_hwnds[k] || !keep_key[k][0])
			continue;
		for (int i = 0; i < MAX_COMPOSITE; i++) {
			if (!ctx->children[i].source || child_used[i])
				continue;
			if (ctx->children[i].method != keep_methods[k])
				continue;
			if (!ctx->children[i].filter_key[0] ||
			    strcmp(ctx->children[i].filter_key, keep_key[k]) != 0)
				continue;
			keep_child[k] = i;
			child_used[i] = true;
			break;
		}
	}

	for (int i = 0; i < MAX_COMPOSITE; i++) {
		if (ctx->children[i].source && !child_used[i])
			queue_drop_child(ctx, i, drop, &drop_n);
	}

	for (int k = 0; k < keep_n; k++) {
		int i = keep_child[k];

		if (i < 0)
			continue;
		if (keep_hwnds[k])
			ctx->children[i].hwnd = keep_hwnds[k];
		if (keep_rect[k].right > keep_rect[k].left && keep_rect[k].bottom > keep_rect[k].top)
			ctx->children[i].rect = keep_rect[k];
		snprintf(ctx->children[i].encoded, sizeof(ctx->children[i].encoded), "%s", keep_enc[k]);
		snprintf(ctx->children[i].filter_key, sizeof(ctx->children[i].filter_key), "%s", keep_key[k]);
	}
	pthread_mutex_unlock(&ctx->mutex);

	flush_dropped_children(ctx, drop, drop_n, true);

	for (int k = 0; k < keep_n; k++) {
		int slot = -1;
		obs_source_t *src;
		bool cursor;
		bool force_sdr;

		if (keep_child[k] >= 0)
			continue;

		pthread_mutex_lock(&ctx->mutex);
		cursor = ctx->capture_cursor;
		force_sdr = ctx->force_sdr;
		for (int i = 0; i < MAX_COMPOSITE; i++) {
			if (!ctx->children[i].source) {
				slot = i;
				break;
			}
		}
		pthread_mutex_unlock(&ctx->mutex);
		if (slot < 0)
			continue;

		src = create_window_capture(keep_enc[k], keep_methods[k], cursor, force_sdr);
		if (!src)
			continue;
		obs_source_add_active_child(ctx->source, src);
		apply_bank_filters(ctx, src, keep_key[k]);
		hook_child_filters(ctx, src);

		pthread_mutex_lock(&ctx->mutex);
		if (ctx->children[slot].source) {
			pthread_mutex_unlock(&ctx->mutex);
			unhook_child_filters(ctx, src);
			obs_source_remove_active_child(ctx->source, src);
			obs_source_release(src);
			continue;
		}
		ctx->children[slot].source = src;
		ctx->children[slot].hwnd = keep_hwnds[k];
		ctx->children[slot].rect = keep_rect[k];
		ctx->children[slot].method = keep_methods[k];
		snprintf(ctx->children[slot].encoded, sizeof(ctx->children[slot].encoded), "%s", keep_enc[k]);
		snprintf(ctx->children[slot].filter_key, sizeof(ctx->children[slot].filter_key), "%s", keep_key[k]);
		pthread_mutex_unlock(&ctx->mutex);
		blog(LOG_INFO, "[selected-windows-capture] capturing %s via %s", keep_enc[k],
		     keep_methods[k] == SWC_METHOD_BITBLT ? "BitBlt" : "WGC");
	}
}

static void retarget_monitor_for_include(struct swc_source *ctx, struct swc_window *list, const bool *keep, int n)
{
	struct swc_monitor mons[SWC_MAX_MONITORS];
	int mn;
	int best = -1;
	long best_area = 0;

	mn = swc_enum_monitors(mons, SWC_MAX_MONITORS);
	for (int m = 0; m < mn; m++) {
		long area = 0;
		for (int i = 0; i < n; i++) {
			RECT hit;
			if (!keep[i])
				continue;
			if (IntersectRect(&hit, &list[i].rect, &mons[m].rect))
				area += (long)(hit.right - hit.left) * (long)(hit.bottom - hit.top);
		}
		if (area > best_area) {
			best_area = area;
			best = m;
		}
	}
	if (best < 0)
		return;

	snprintf(ctx->monitor_id, sizeof(ctx->monitor_id), "%s", mons[best].device_id);
	ctx->monitor = mons[best];
	ctx->monitor_ok = true;
	ctx->width = (uint32_t)(mons[best].rect.right - mons[best].rect.left);
	ctx->height = (uint32_t)(mons[best].rect.bottom - mons[best].rect.top);
}

static void refresh_window_snapshot(struct swc_source *ctx, float seconds)
{
	struct swc_window list[SWC_MAX_WINDOWS];
	bool keep[SWC_MAX_WINDOWS];
	float unseen[SWC_MAX_WINDOWS];
	int prev_n;
	int n;
	float dt = seconds > 0.0f ? seconds : 0.016f;

	n = swc_enum_windows(NULL, 0, list, SWC_MAX_WINDOWS);

	pthread_mutex_lock(&ctx->mutex);
	for (int i = 0; i < n; i++) {
		keep[i] = window_should_keep(ctx, &list[i]);
		unseen[i] = 0.0f;
	}
	drop_small_satellites(list, keep, n);
	prev_n = ctx->window_count;
	for (int p = 0; p < prev_n && n < SWC_MAX_WINDOWS; p++) {
		HWND hwnd;
		bool found = false;
		float u;
		RECT r;

		if (!ctx->keep[p] || !ctx->windows[p].hwnd)
			continue;
		hwnd = ctx->windows[p].hwnd;
		for (int i = 0; i < n; i++) {
			if (list[i].hwnd == hwnd) {
				found = true;
				break;
			}
		}
		if (found)
			continue;
		u = ctx->unseen[p] + dt;
		if (u > SWC_HWND_LINGER)
			continue;
		if (!window_should_keep(ctx, &ctx->windows[p]))
			continue;
		if (!IsWindow(hwnd) || IsIconic(hwnd))
			continue;
		list[n] = ctx->windows[p];
		if (GetWindowRect(hwnd, &r) && (r.right - r.left) > 1 && (r.bottom - r.top) > 1)
			list[n].rect = r;
		keep[n] = true;
		unseen[n] = u;
		n++;
	}
	retarget_monitor_for_include(ctx, list, keep, n);
	ctx->window_count = n;
	if (n) {
		memcpy(ctx->windows, list, (size_t)n * sizeof(list[0]));
		memcpy(ctx->keep, keep, (size_t)n * sizeof(keep[0]));
		memcpy(ctx->unseen, unseen, (size_t)n * sizeof(unseen[0]));
	}
	pthread_mutex_unlock(&ctx->mutex);
}

static void recover_dead_captures(struct swc_source *ctx, float seconds)
{
	struct swc_drop_src drop[MAX_COMPOSITE];
	int drop_n = 0;

	if (os_atomic_load_bool(&ctx->destroying))
		return;

	pthread_mutex_lock(&ctx->mutex);
	for (int i = 0; i < MAX_COMPOSITE; i++) {
		uint32_t w;
		uint32_t h;
		int next;

		if (!ctx->children[i].source)
			continue;
		w = obs_source_get_width(ctx->children[i].source);
		h = obs_source_get_height(ctx->children[i].source);
		if (w > 1 && h > 1) {
			ctx->children[i].dead_time = 0.0f;
			continue;
		}
		ctx->children[i].dead_time += seconds;
		if (encoded_needs_wgc(ctx->children[i].encoded) ||
		    (ctx->children[i].hwnd && IsWindow(ctx->children[i].hwnd) &&
		     (GetWindowLongPtrW(ctx->children[i].hwnd, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP))) {
			if (ctx->children[i].dead_time < 2.5f)
				continue;
			if (pref_flips(ctx, ctx->children[i].hwnd) >= 3)
				continue;
			next = SWC_METHOD_WGC;
			remember_method_pref(ctx, ctx->children[i].hwnd, next);
			blog(LOG_INFO, "[selected-windows-capture] %s WGC not ready, retry WGC",
			     ctx->children[i].encoded);
			queue_drop_child(ctx, i, drop, &drop_n);
			continue;
		}
		if (ctx->children[i].dead_time < 1.2f)
			continue;
		if (pref_flips(ctx, ctx->children[i].hwnd) >= 2)
			continue;

		next = (ctx->children[i].method == SWC_METHOD_WGC) ? SWC_METHOD_BITBLT : SWC_METHOD_WGC;
		remember_method_pref(ctx, ctx->children[i].hwnd, next);
		blog(LOG_INFO, "[selected-windows-capture] %s capture failed, retry via %s",
		     ctx->children[i].encoded, next == SWC_METHOD_BITBLT ? "BitBlt" : "WGC");
		queue_drop_child(ctx, i, drop, &drop_n);
	}
	pthread_mutex_unlock(&ctx->mutex);

	flush_dropped_children(ctx, drop, drop_n, true);
}

/* ------------------------------------------------------------------------- */

static void draw_solid_rect(float x, float y, float w, float h, const struct vec4 *color)
{
	gs_effect_t *solid;
	if (w < 1.0f || h < 1.0f)
		return;
	solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), color);
	gs_matrix_push();
	gs_matrix_translate3f(x, y, 0.0f);
	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(NULL, 0, (uint32_t)w, (uint32_t)h);
	gs_matrix_pop();
}

static void color_from_obs(uint32_t color, struct vec4 *out)
{
	vec4_set(out, (float)((color >> 0) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f,
		 (float)((color >> 16) & 0xFF) / 255.0f, 1.0f);
}

static bool render_source_to(gs_texrender_t *rt, obs_source_t *src, uint32_t cx, uint32_t cy)
{
	struct vec4 clear;
	if (!rt || !src || !cx || !cy)
		return false;
	gs_texrender_reset(rt);
	if (!gs_texrender_begin(rt, cx, cy))
		return false;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	obs_source_video_render(src);
	gs_texrender_end(rt);
	return gs_texrender_get_texture(rt) != NULL;
}

static bool render_window_mask(struct swc_source *ctx, uint32_t cx, uint32_t cy, const RECT *mon)
{
	struct vec4 black;
	struct vec4 white;
	struct swc_window windows[SWC_MAX_WINDOWS];
	bool keep[SWC_MAX_WINDOWS];
	int n = 0;
	const float mon_w = (float)(mon->right - mon->left);
	const float mon_h = (float)(mon->bottom - mon->top);
	const float sx = (mon_w > 0.0f) ? (float)cx / mon_w : 1.0f;
	const float sy = (mon_h > 0.0f) ? (float)cy / mon_h : 1.0f;

	if (!ctx->mask_rt || !cx || !cy)
		return false;

	pthread_mutex_lock(&ctx->mutex);
	n = ctx->window_count;
	if (n) {
		memcpy(windows, ctx->windows, (size_t)n * sizeof(windows[0]));
		memcpy(keep, ctx->keep, (size_t)n * sizeof(keep[0]));
	}
	pthread_mutex_unlock(&ctx->mutex);

	gs_texrender_reset(ctx->mask_rt);
	if (!gs_texrender_begin(ctx->mask_rt, cx, cy))
		return false;

	vec4_zero(&black);
	vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, ctx->filter_mode == SWC_MODE_EXCLUDE ? &white : &black, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(false);

	for (int i = n - 1; i >= 0; i--) {
		const RECT *r = &windows[i].rect;
		float x = ((float)r->left - (float)mon->left) * sx;
		float y = ((float)r->top - (float)mon->top) * sy;
		float w = (float)(r->right - r->left) * sx;
		float h = (float)(r->bottom - r->top) * sy;
		draw_solid_rect(x, y, w, h, keep[i] ? &white : &black);
	}

	gs_blend_state_pop();
	gs_texrender_end(ctx->mask_rt);
	return gs_texrender_get_texture(ctx->mask_rt) != NULL;
}

static void render_masked(struct swc_source *ctx)
{
	obs_source_t *display;
	gs_texture_t *capture_tex;
	gs_texture_t *mask_tex;
	uint32_t cx;
	uint32_t cy;
	RECT mon;
	struct vec4 fill;

	pthread_mutex_lock(&ctx->mutex);
	display = ctx->display ? obs_source_get_ref(ctx->display) : NULL;
	mon = ctx->monitor.rect;
	pthread_mutex_unlock(&ctx->mutex);

	if (!display)
		return;

	cx = obs_source_get_width(display);
	cy = obs_source_get_height(display);
	if (!cx || !cy) {
		obs_source_release(display);
		return;
	}

	if (!render_source_to(ctx->capture_rt, display, cx, cy)) {
		obs_source_release(display);
		return;
	}
	obs_source_release(display);

	if (!render_window_mask(ctx, cx, cy, &mon))
		return;

	capture_tex = gs_texrender_get_texture(ctx->capture_rt);
	mask_tex = gs_texrender_get_texture(ctx->mask_rt);
	if (!capture_tex || !mask_tex)
		return;

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (ctx->hole_mode == 1) {
		color_from_obs(ctx->hole_color, &fill);
		draw_solid_rect(0.0f, 0.0f, (float)cx, (float)cy, &fill);
	}

	if (ctx->mask_effect) {
		gs_effect_t *effect = ctx->mask_effect;
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), capture_tex);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "mask"), mask_tex);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(capture_tex, 0, 0, 0);
	} else {
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), capture_tex);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(capture_tex, 0, 0, 0);
	}

	gs_blend_state_pop();
}

static void render_composite(struct swc_source *ctx)
{
	struct swc_window windows[SWC_MAX_WINDOWS];
	bool keep[SWC_MAX_WINDOWS];
	int n = 0;
	RECT mon;
	uint32_t cx = ctx->width;
	uint32_t cy = ctx->height;
	struct vec4 fill;

	if (!cx || !cy)
		return;

	pthread_mutex_lock(&ctx->mutex);
	mon = ctx->monitor.rect;
	n = ctx->window_count;
	if (n) {
		memcpy(windows, ctx->windows, (size_t)n * sizeof(windows[0]));
		memcpy(keep, ctx->keep, (size_t)n * sizeof(keep[0]));
	}
	pthread_mutex_unlock(&ctx->mutex);

	const float mon_w = (float)(mon.right - mon.left);
	const float mon_h = (float)(mon.bottom - mon.top);
	const float sx = (mon_w > 0.0f) ? (float)cx / mon_w : 1.0f;
	const float sy = (mon_h > 0.0f) ? (float)cy / mon_h : 1.0f;

	gs_blend_state_push();
	gs_enable_blending(true);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (ctx->hole_mode == 1) {
		color_from_obs(ctx->hole_color, &fill);
		draw_solid_rect(0.0f, 0.0f, (float)cx, (float)cy, &fill);
	}

	obs_source_t *child_src[MAX_COMPOSITE];
	HWND child_hwnd[MAX_COMPOSITE];
	RECT child_rect[MAX_COMPOSITE];
	int child_n = 0;

	pthread_mutex_lock(&ctx->mutex);
	for (int c = 0; c < MAX_COMPOSITE; c++) {
		if (!ctx->children[c].source)
			continue;
		child_src[child_n] = obs_source_get_ref(ctx->children[c].source);
		child_hwnd[child_n] = ctx->children[c].hwnd;
		child_rect[child_n] = ctx->children[c].rect;
		child_n++;
	}
	pthread_mutex_unlock(&ctx->mutex);

	for (int i = n - 1; i >= 0; i--) {
		obs_source_t *src = NULL;
		uint32_t sw;
		uint32_t sh;
		float x;
		float y;
		float dw;
		float dh;

		if (!keep[i])
			continue;
		for (int c = 0; c < child_n; c++) {
			if (child_hwnd[c] == windows[i].hwnd) {
				src = child_src[c];
				child_src[c] = NULL;
				break;
			}
		}
		if (!src)
			continue;

		sw = obs_source_get_width(src);
		sh = obs_source_get_height(src);
		if (!sw || !sh) {
			obs_source_release(src);
			continue;
		}
		x = ((float)windows[i].rect.left - (float)mon.left) * sx;
		y = ((float)windows[i].rect.top - (float)mon.top) * sy;
		dw = (float)(windows[i].rect.right - windows[i].rect.left) * sx;
		dh = (float)(windows[i].rect.bottom - windows[i].rect.top) * sy;

		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0.0f);
		if (sw && sh)
			gs_matrix_scale3f(dw / (float)sw, dh / (float)sh, 1.0f);
		obs_source_video_render(src);
		gs_matrix_pop();
		obs_source_release(src);
	}

	for (int c = 0; c < child_n; c++) {
		obs_source_t *src = child_src[c];
		uint32_t sw;
		uint32_t sh;
		float x;
		float y;
		float dw;
		float dh;
		const RECT *r;

		if (!src)
			continue;
		if (child_hwnd[c] && !IsWindow(child_hwnd[c])) {
			obs_source_release(src);
			continue;
		}
		sw = obs_source_get_width(src);
		sh = obs_source_get_height(src);
		r = &child_rect[c];
		if (sw && sh && (r->right - r->left) > 1 && (r->bottom - r->top) > 1) {
			x = ((float)r->left - (float)mon.left) * sx;
			y = ((float)r->top - (float)mon.top) * sy;
			dw = (float)(r->right - r->left) * sx;
			dh = (float)(r->bottom - r->top) * sy;
			gs_matrix_push();
			gs_matrix_translate3f(x, y, 0.0f);
			gs_matrix_scale3f(dw / (float)sw, dh / (float)sh, 1.0f);
			obs_source_video_render(src);
			gs_matrix_pop();
		} else if (sw && sh) {
			obs_source_video_render(src);
		}
		obs_source_release(src);
	}

	gs_blend_state_pop();
}

/* ------------------------------------------------------------------------- */

static const char *swc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return SWC_TEXT("SelectedWindowsCapture");
}

static void *swc_create(obs_data_t *settings, obs_source_t *source)
{
	struct swc_source *ctx = bzalloc(sizeof(*ctx));
	char *effect_path;

	ctx->source = source;
	os_atomic_set_bool(&ctx->destroying, false);
	pthread_mutex_init(&ctx->mutex, NULL);
	apply_settings(ctx, settings);
	swc_live_add(ctx);

	obs_enter_graphics();
	effect_path = obs_module_file("window-mask.effect");
	if (effect_path) {
		ctx->mask_effect = gs_effect_create_from_file(effect_path, NULL);
		bfree(effect_path);
	}
	if (!ctx->mask_effect)
		blog(LOG_ERROR, "[selected-windows-capture] Could not load window-mask.effect");
	ctx->capture_rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	ctx->mask_rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	return ctx;
}

static void harvest_all_live_children(struct swc_source *ctx)
{
	obs_source_t *children[MAX_COMPOSITE];
	char keys[MAX_COMPOSITE][SWC_STR];
	int n = 0;

	pthread_mutex_lock(&ctx->mutex);
	for (int i = 0; i < MAX_COMPOSITE; i++) {
		if (!ctx->children[i].source)
			continue;
		children[n] = obs_source_get_ref(ctx->children[i].source);
		snprintf(keys[n], sizeof(keys[n]), "%s", ctx->children[i].filter_key);
		n++;
	}
	pthread_mutex_unlock(&ctx->mutex);

	for (int i = 0; i < n; i++) {
		if (!children[i])
			continue;
		harvest_filters_from_source(ctx, children[i], keys[i]);
		obs_source_release(children[i]);
	}
}

static void swc_save(void *data, obs_data_t *settings)
{
	struct swc_source *ctx = data;
	if (!os_atomic_load_bool(&ctx->destroying))
		harvest_all_live_children(ctx);
	swc_bank_save_to_settings(ctx, settings);
}

static void swc_destroy(void *data)
{
	struct swc_source *ctx = data;

	os_atomic_set_bool(&ctx->destroying, true);
	swc_live_remove(ctx);
	swc_wait_captures_stopped(ctx);
	destroy_all_children(ctx);
	pthread_mutex_lock(&ctx->mutex);
	swc_bank_clear_unlocked(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	destroy_display(ctx);

	obs_enter_graphics();
	if (ctx->mask_effect)
		gs_effect_destroy(ctx->mask_effect);
	if (ctx->capture_rt)
		gs_texrender_destroy(ctx->capture_rt);
	if (ctx->mask_rt)
		gs_texrender_destroy(ctx->mask_rt);
	obs_leave_graphics();

	pthread_mutex_destroy(&ctx->mutex);
	bfree(ctx);
}

static void swc_update(void *data, obs_data_t *settings)
{
	struct swc_source *ctx = data;
	apply_settings(ctx, settings);
	update_display_settings(ctx);
}

static void swc_defaults(obs_data_t *settings)
{
	struct swc_monitor mon;

	obs_data_set_default_string(settings, "monitor_id", "");
	if (swc_active_monitor(&mon))
		obs_data_set_default_string(settings, "monitor_id", mon.device_id);
	obs_data_set_default_int(settings, "layout", SWC_LAYOUT_COMPOSITE);
	obs_data_set_default_int(settings, "method", SWC_METHOD_WGC);
	obs_data_set_default_bool(settings, "capture_cursor", true);
	obs_data_set_default_bool(settings, "force_sdr", false);
	obs_data_set_default_int(settings, "filter_mode", SWC_MODE_INCLUDE);
	obs_data_set_default_int(settings, "match_mode", SWC_MATCH_CLASS_EXE);
	obs_data_set_default_bool(settings, "tool_windows", false);
	obs_data_set_default_bool(settings, "follow_active", true);
	obs_data_set_default_int(settings, "hole_mode", 0);
	obs_data_set_default_int(settings, "hole_color", 0x000000);
}

static uint32_t swc_width(void *data)
{
	struct swc_source *ctx = data;
	uint32_t w;
	pthread_mutex_lock(&ctx->mutex);
	w = ctx->width;
	pthread_mutex_unlock(&ctx->mutex);
	return w;
}

static uint32_t swc_height(void *data)
{
	struct swc_source *ctx = data;
	uint32_t h;
	pthread_mutex_lock(&ctx->mutex);
	h = ctx->height;
	pthread_mutex_unlock(&ctx->mutex);
	return h;
}

static void swc_tick(void *data, float seconds)
{
	struct swc_source *ctx = data;
	struct swc_monitor monitor;
	bool monitor_ok;
	int layout;
	bool follow;
	bool monitor_changed = false;

	if (os_atomic_load_bool(&ctx->destroying)) {
		if (ctx->captures_live) {
			destroy_all_children(ctx);
			destroy_display(ctx);
			ctx->captures_live = false;
		}
		return;
	}
	if (!obs_source_showing(ctx->source)) {
		if (ctx->captures_live) {
			destroy_all_children(ctx);
			destroy_display(ctx);
			ctx->captures_live = false;
		}
		return;
	}
	ctx->captures_live = true;

	pthread_mutex_lock(&ctx->mutex);
	follow = ctx->follow_active;
	layout = ctx->layout;
	pthread_mutex_unlock(&ctx->mutex);

	if (follow && swc_active_monitor(&monitor)) {
		pthread_mutex_lock(&ctx->mutex);
		monitor_changed = strcmp(ctx->monitor_id, monitor.device_id) != 0;
		snprintf(ctx->monitor_id, sizeof(ctx->monitor_id), "%s", monitor.device_id);
		ctx->monitor = monitor;
		ctx->monitor_ok = true;
		ctx->width = (uint32_t)(monitor.rect.right - monitor.rect.left);
		ctx->height = (uint32_t)(monitor.rect.bottom - monitor.rect.top);
		pthread_mutex_unlock(&ctx->mutex);
		monitor_ok = true;
	} else {
		pthread_mutex_lock(&ctx->mutex);
		monitor_ok = swc_find_monitor(ctx->monitor_id, &monitor);
		if (monitor_ok) {
			ctx->monitor = monitor;
			ctx->width = (uint32_t)(monitor.rect.right - monitor.rect.left);
			ctx->height = (uint32_t)(monitor.rect.bottom - monitor.rect.top);
		}
		ctx->monitor_ok = monitor_ok;
		pthread_mutex_unlock(&ctx->mutex);
	}

	refresh_window_snapshot(ctx, seconds);

	if (layout == SWC_LAYOUT_MASK && !ctx->mask_failed) {
		obs_source_t *display;
		uint32_t cx = 0;
		uint32_t cy = 0;

		destroy_all_children(ctx);
		ensure_display(ctx);
		if (monitor_changed)
			update_display_settings(ctx);
		pthread_mutex_lock(&ctx->mutex);
		display = ctx->display ? obs_source_get_ref(ctx->display) : NULL;
		pthread_mutex_unlock(&ctx->mutex);
		if (display) {
			cx = obs_source_get_width(display);
			cy = obs_source_get_height(display);
			obs_source_release(display);
		}
		if (cx > 1 && cy > 1) {
			pthread_mutex_lock(&ctx->mutex);
			ctx->width = cx;
			ctx->height = cy;
			ctx->display_dead = 0.0f;
			pthread_mutex_unlock(&ctx->mutex);
		} else {
			pthread_mutex_lock(&ctx->mutex);
			ctx->display_dead += seconds;
			if (ctx->display_dead > 2.0f) {
				ctx->mask_failed = true;
				blog(LOG_INFO, "[selected-windows-capture] display capture failed, falling back to window composite");
			}
			pthread_mutex_unlock(&ctx->mutex);
		}
	}

	if (layout != SWC_LAYOUT_MASK || ctx->mask_failed) {
		destroy_display(ctx);
		sync_composite_children(ctx);
		recover_dead_captures(ctx, seconds);
		pthread_mutex_lock(&ctx->mutex);
		ctx->filter_save_accum += seconds;
		if (ctx->filter_save_accum >= 2.0f) {
			ctx->filter_save_accum = 0.0f;
			pthread_mutex_unlock(&ctx->mutex);
			if (!os_atomic_load_bool(&ctx->destroying)) {
				harvest_all_live_children(ctx);
				persist_bank_to_source_settings(ctx);
			}
		} else {
			pthread_mutex_unlock(&ctx->mutex);
		}
	}
}

static void swc_render(void *data, gs_effect_t *effect)
{
	struct swc_source *ctx = data;
	uint32_t cx;
	uint32_t cy;
	UNUSED_PARAMETER(effect);

	if (os_atomic_load_bool(&ctx->destroying))
		return;

	pthread_mutex_lock(&ctx->mutex);
	cx = ctx->width;
	cy = ctx->height;
	pthread_mutex_unlock(&ctx->mutex);
	if (!cx || !cy)
		return;

	if (ctx->layout == SWC_LAYOUT_MASK && !ctx->mask_failed)
		render_masked(ctx);
	else
		render_composite(ctx);
}

static void swc_enum_active(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct swc_source *ctx = data;
	obs_source_t *display;
	obs_source_t *children[MAX_COMPOSITE];
	int n = 0;

	pthread_mutex_lock(&ctx->mutex);
	display = ctx->display ? obs_source_get_ref(ctx->display) : NULL;
	for (int i = 0; i < MAX_COMPOSITE; i++) {
		if (ctx->children[i].source)
			children[n++] = obs_source_get_ref(ctx->children[i].source);
	}
	pthread_mutex_unlock(&ctx->mutex);

	if (display) {
		cb(ctx->source, display, param);
		obs_source_release(display);
	}
	for (int i = 0; i < n; i++) {
		cb(ctx->source, children[i], param);
		obs_source_release(children[i]);
	}
}

static enum gs_color_space swc_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	struct swc_source *ctx = data;
	obs_source_t *display;
	enum gs_color_space space = GS_CS_SRGB;

	pthread_mutex_lock(&ctx->mutex);
	display = ctx->display ? obs_source_get_ref(ctx->display) : NULL;
	pthread_mutex_unlock(&ctx->mutex);
	if (display) {
		space = obs_source_get_color_space(display, count, preferred_spaces);
		obs_source_release(display);
	}
	return space;
}

/* ------------------------------------------------------------------------- */

static uint32_t hash_encoded(const char *s)
{
	uint32_t h = 2166136261u;
	if (!s)
		return h;
	for (; *s; s++) {
		h ^= (unsigned char)*s;
		h *= 16777619u;
	}
	return h;
}

static bool swc_props_syncing;
static DWORD swc_props_sync_tick;

static void swc_begin_props_sync(void)
{
	swc_props_syncing = true;
	swc_props_sync_tick = GetTickCount();
}

static bool swc_in_props_sync(void)
{
	if (!swc_props_syncing)
		return false;
	if ((GetTickCount() - swc_props_sync_tick) > 500) {
		swc_props_syncing = false;
		return false;
	}
	return true;
}

#ifdef _WIN32
static void swc_defer_clear_focus(void)
{
	HWND hwnd = GetActiveWindow();
	if (hwnd)
		PostMessage(hwnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
}
#else
static void swc_defer_clear_focus(void) {}
#endif

static void friendly_exe_name(const char *exe, char *out, size_t n);

static void window_display_name(obs_data_t *settings, const char *encoded, char *desc, size_t desc_sz)
{
	struct swc_window live[SWC_MAX_WINDOWS];
	char title[512];
	char class_name[256];
	char exe[256];
	char app[256];
	char key[SWC_STR];
	int n;
	int count = 0;

	if (!desc || !desc_sz)
		return;
	desc[0] = '\0';
	if (!encoded || !encoded[0])
		return;
	if (!swc_parse_encoded(encoded, title, sizeof(title), class_name, sizeof(class_name), exe, sizeof(exe))) {
		snprintf(desc, desc_sz, "%s", encoded);
		return;
	}
	friendly_exe_name(exe, app, sizeof(app));
	if (!app[0])
		snprintf(app, sizeof(app), "%s", title[0] ? title : class_name);
	group_key_from_encoded(encoded, key, sizeof(key));
	n = enum_picker_windows(settings, live, SWC_MAX_WINDOWS);
	for (int i = 0; i < n; i++) {
		char live_key[SWC_STR];
		group_key_from_parts(live[i].title, live[i].class_name, live[i].exe, live_key, sizeof(live_key));
		if (key[0] && strcmp(live_key, key) == 0)
			count++;
	}
	if (count > 1)
		snprintf(desc, desc_sz, "%s (%d)", app, count);
	else
		snprintf(desc, desc_sz, "%s", app);
}

static void fill_filter_window_list(obs_property_t *list, obs_data_t *settings)
{
	obs_data_array_t *arr;
	char added[MAX_SELECTED][SWC_STR];
	char first_enc[SWC_STR] = {0};
	int added_n = 0;
	const char *cur;
	bool cur_found = false;
	size_t n;

	if (!list)
		return;

	obs_property_list_clear(list);
	if (!settings)
		return;

	arr = obs_data_get_array(settings, "windows");
	if (!arr)
		return;

	cur = obs_data_get_string(settings, "filter_window");
	n = obs_data_array_count(arr);
	for (size_t i = 0; i < n && added_n < MAX_SELECTED; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		const char *enc = obs_data_get_string(item, "value");
		char key[SWC_STR];
		char desc[SWC_STR];
		bool seen = false;

		group_key_from_encoded(enc, key, sizeof(key));
		if (!enc || !enc[0] || !key[0]) {
			obs_data_release(item);
			continue;
		}
		for (int a = 0; a < added_n; a++) {
			if (strcmp(added[a], key) == 0) {
				seen = true;
				break;
			}
		}
		if (seen) {
			obs_data_release(item);
			continue;
		}
		window_display_name(settings, enc, desc, sizeof(desc));
		obs_property_list_add_string(list, desc[0] ? desc : enc, enc);
		snprintf(added[added_n], sizeof(added[added_n]), "%s", key);
		if (!first_enc[0])
			snprintf(first_enc, sizeof(first_enc), "%s", enc);
		if (cur && cur[0]) {
			char cur_key[SWC_STR];
			group_key_from_encoded(cur, cur_key, sizeof(cur_key));
			if (strcmp(cur, enc) == 0 || (cur_key[0] && strcmp(cur_key, key) == 0))
				cur_found = true;
		}
		added_n++;
		obs_data_release(item);
	}
	obs_data_array_release(arr);

	if (added_n == 0) {
		obs_data_set_default_string(settings, "filter_window", "");
	} else if (!cur_found && first_enc[0]) {
		if (obs_data_has_user_value(settings, "filter_window"))
			obs_data_set_string(settings, "filter_window", first_enc);
		else
			obs_data_set_default_string(settings, "filter_window", first_enc);
	}
}

static obs_source_t *ensure_capture_child(struct swc_source *ctx, const char *encoded)
{
	struct swc_window live[SWC_MAX_WINDOWS];
	char key[SWC_STR];
	HWND hwnd = NULL;
	RECT rect = {0};
	int n;
	int method;
	int slot = -1;
	bool cursor;
	bool force_sdr;
	obs_source_t *existing = NULL;
	obs_source_t *src;

	if (!ctx || !encoded || !encoded[0])
		return NULL;

	filter_key_from_encoded(encoded, key, sizeof(key));
	n = swc_enum_windows(NULL, SWC_ENUM_TOOLWINDOWS, live, SWC_MAX_WINDOWS);
	for (int i = 0; i < n; i++) {
		if (swc_window_matches(SWC_MATCH_CLASS_EXE, &live[i], encoded)) {
			hwnd = live[i].hwnd;
			rect = live[i].rect;
			break;
		}
	}

	pthread_mutex_lock(&ctx->mutex);
	method = lookup_method_pref(ctx, hwnd, SWC_METHOD_WGC);
	if (encoded_needs_wgc(encoded))
		method = SWC_METHOD_WGC;
	cursor = ctx->capture_cursor;
	force_sdr = ctx->force_sdr;
	for (int i = 0; i < MAX_COMPOSITE; i++) {
		if (ctx->children[i].source && ctx->children[i].method == method) {
			const bool hwnd_match = hwnd && ctx->children[i].hwnd == hwnd;
			const bool key_match = key[0] && strcmp(ctx->children[i].filter_key, key) == 0;
			if (hwnd_match || key_match) {
				ctx->children[i].hwnd = hwnd ? hwnd : ctx->children[i].hwnd;
				if (hwnd)
					ctx->children[i].rect = rect;
				snprintf(ctx->children[i].encoded, sizeof(ctx->children[i].encoded), "%s", encoded);
				snprintf(ctx->children[i].filter_key, sizeof(ctx->children[i].filter_key), "%s", key);
				existing = obs_source_get_ref(ctx->children[i].source);
				break;
			}
		}
		if (!ctx->children[i].source && slot < 0)
			slot = i;
	}
	pthread_mutex_unlock(&ctx->mutex);

	if (existing)
		return existing;
	if (slot < 0)
		return NULL;

	src = create_window_capture(encoded, method, cursor, force_sdr);
	if (!src)
		return NULL;
	obs_source_add_active_child(ctx->source, src);
	apply_bank_filters(ctx, src, key);
	hook_child_filters(ctx, src);

	pthread_mutex_lock(&ctx->mutex);
	if (ctx->children[slot].source) {
		pthread_mutex_unlock(&ctx->mutex);
		unhook_child_filters(ctx, src);
		obs_source_remove_active_child(ctx->source, src);
		obs_source_release(src);
		return obs_source_get_ref(ctx->children[slot].source);
	}
	ctx->children[slot].source = src;
	ctx->children[slot].hwnd = hwnd;
	ctx->children[slot].rect = rect;
	ctx->children[slot].method = method;
	snprintf(ctx->children[slot].encoded, sizeof(ctx->children[slot].encoded), "%s", encoded);
	snprintf(ctx->children[slot].filter_key, sizeof(ctx->children[slot].filter_key), "%s", key);
	pthread_mutex_unlock(&ctx->mutex);
	return obs_source_get_ref(src);
}

static bool filter_mode_changed(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	struct swc_source *ctx = priv;
	obs_property_t *fw = obs_properties_get(props, "filter_window");
	obs_property_t *ob = obs_properties_get(props, "open_filters");
	obs_property_t *hint = obs_properties_get(props, "exclude_hint");
	const bool include = (int)obs_data_get_int(settings, "filter_mode") == SWC_MODE_INCLUDE;

	UNUSED_PARAMETER(p);
	if (fw) {
		obs_property_set_visible(fw, include);
		if (include)
			fill_filter_window_list(fw, settings);
	}
	if (ob)
		obs_property_set_visible(ob, include);
	if (hint)
		obs_property_set_visible(hint, !include);
	if (ctx)
		obs_source_update(ctx->source, settings);
	swc_begin_props_sync();
	return true;
}

static bool open_filters_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct swc_source *ctx = data;
	obs_data_t *settings;
	const char *encoded;
	obs_source_t *child;

	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	if (!ctx)
		return false;

	settings = obs_source_get_settings(ctx->source);
	encoded = obs_data_get_string(settings, "filter_window");
	child = ensure_capture_child(ctx, encoded);
	obs_data_release(settings);

	if (!child)
		return false;
#ifdef HAVE_OBS_FRONTEND_API
	obs_frontend_open_source_filters(child);
#endif
	obs_source_release(child);
	return false;
}

static const char *checkbox_encoded(obs_property_t *p, obs_data_t *settings);

static bool window_toggled(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	struct swc_source *ctx = priv;
	const char *name = obs_property_name(p);
	char enc_key[48];
	const char *encoded;
	bool on;

	if (swc_in_props_sync())
		return false;

	encoded = checkbox_encoded(p, settings);
	if (!encoded || !encoded[0]) {
		snprintf(enc_key, sizeof(enc_key), "%s.enc", name);
		encoded = obs_data_get_string(settings, enc_key);
	}
	on = obs_data_get_bool(settings, name);
	if (on) {
		array_upsert_group(settings, encoded);
		if (encoded && encoded[0])
			obs_data_set_string(settings, "filter_window", encoded);
	} else {
		array_remove_group(settings, encoded);
	}
	if (ctx)
		commit_selected(ctx, settings);
	fill_filter_window_list(obs_properties_get(props, "filter_window"), settings);
	swc_begin_props_sync();
	return true;
}

struct swc_pick {
	char key[384];
	char encoded[SWC_STR];
	char exe[128];
	char class_name[128];
	char title[256];
	char label[256];
	long area;
	int count;
	bool running;
};

static void ellipsize_utf8(char *s, size_t max_bytes)
{
	size_t n;
	size_t i;

	if (!s)
		return;
	n = strlen(s);
	if (n <= max_bytes || max_bytes < 4)
		return;
	i = max_bytes - 3;
	while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80)
		i--;
	s[i] = '.';
	s[i + 1] = '.';
	s[i + 2] = '.';
	s[i + 3] = 0;
}

static void friendly_exe_name(const char *exe, char *out, size_t n)
{
	static const struct {
		const char *exe;
		const char *name;
	} map[] = {
		{"chrome.exe", "Google Chrome"},
		{"msedge.exe", "Microsoft Edge"},
		{"firefox.exe", "Firefox"},
		{"brave.exe", "Brave"},
		{"opera.exe", "Opera"},
		{"code.exe", "Visual Studio Code"},
		{"discord.exe", "Discord"},
		{"slack.exe", "Slack"},
		{"telegram.exe", "Telegram"},
		{"wechat.exe", "WeChat"},
		{"qq.exe", "QQ"},
		{"spotify.exe", "Spotify"},
		{"steam.exe", "Steam"},
		{"blender.exe", "Blender"},
		{"sai2.exe", "PaintTool SAI"},
		{"sai.exe", "PaintTool SAI"},
		{"photoshop.exe", "Photoshop"},
		{"illustrator.exe", "Illustrator"},
		{"affinity.exe", "Affinity"},
		{"affinityphoto.exe", "Affinity Photo"},
		{"affinitydesigner.exe", "Affinity Designer"},
		{"clipstudiopaint.exe", "CLIP STUDIO PAINT"},
		{"krita.exe", "Krita"},
		{"steamwebhelper.exe", "Steam"},
		{"applicationframehost.exe", "Windows app"},
		{"windowsterminal.exe", "Windows Terminal"},
		{"openconsole.exe", "Windows Terminal"},
		{"obs64.exe", "OBS Studio"},
		{"obs32.exe", "OBS Studio"},
		{"explorer.exe", "Explorer"},
		{"python.exe", "Python"},
		{NULL, NULL},
	};
	size_t len;

	if (!out || !n)
		return;
	out[0] = 0;
	if (!exe || !exe[0])
		return;
	for (int i = 0; map[i].exe; i++) {
		if (_stricmp(exe, map[i].exe) == 0) {
			snprintf(out, n, "%s", map[i].name);
			return;
		}
	}
	snprintf(out, n, "%s", exe);
	len = strlen(out);
	if (len > 4 && _stricmp(out + len - 4, ".exe") == 0)
		out[len - 4] = 0;
}

static void format_pick_label(struct swc_pick *pick)
{
	char app[256];

	if (!pick)
		return;
	friendly_exe_name(pick->exe, app, sizeof(app));
	if (!app[0])
		snprintf(app, sizeof(app), "%s", pick->title[0] ? pick->title : pick->class_name);
	if (pick->count > 1) {
		snprintf(pick->label, sizeof(pick->label), "%s (%d)", app, pick->count);
	} else if (swc_match_by_exe_only(pick->exe, pick->class_name) || !pick->title[0] ||
		   _stricmp(pick->title, app) == 0) {
		snprintf(pick->label, sizeof(pick->label), "%s", app);
	} else {
		snprintf(pick->label, sizeof(pick->label), "%s  ·  %s", app, pick->title);
		ellipsize_utf8(pick->label, 64);
	}
}

static int pick_cmp(const void *a, const void *b)
{
	const struct swc_pick *pa = a;
	const struct swc_pick *pb = b;
	int by_name;

	if (pa->running != pb->running)
		return pa->running ? -1 : 1;
	by_name = _stricmp(pa->label, pb->label);
	if (by_name)
		return by_name;
	return _stricmp(pa->exe, pb->exe);
}

static int find_pick(struct swc_pick *picks, int n, const char *key)
{
	if (!key || !key[0])
		return -1;
	for (int i = 0; i < n; i++) {
		if (strcmp(picks[i].key, key) == 0)
			return i;
	}
	return -1;
}

static const char *checkbox_encoded(obs_property_t *p, obs_data_t *settings)
{
	char enc_key[40];
	const char *name;
	const char *enc;

	name = obs_property_name(p);
	if (!name)
		return "";
	snprintf(enc_key, sizeof(enc_key), "%s.enc", name);
	enc = settings ? obs_data_get_string(settings, enc_key) : NULL;
	if (enc && enc[0])
		return enc;
	enc = obs_property_long_description(p);
	return enc ? enc : "";
}

static bool text_has(const char *hay, const char *needle)
{
	size_t nlen;

	if (!needle || !needle[0])
		return true;
	if (!hay || !hay[0])
		return false;
	nlen = strlen(needle);
	for (const char *p = hay; *p; p++) {
		size_t i = 0;
		while (i < nlen) {
			unsigned char a = (unsigned char)p[i];
			unsigned char b = (unsigned char)needle[i];
			if (!a)
				break;
			if (a >= 'A' && a <= 'Z')
				a = (unsigned char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (unsigned char)(b - 'A' + 'a');
			if (a != b)
				break;
			i++;
		}
		if (i == nlen)
			return true;
	}
	return false;
}

static int apply_search_filter(obs_properties_t *props, obs_data_t *settings, const char *q)
{
	obs_property_t *prop;
	int shown = 0;

	if (!props)
		return 0;
	prop = obs_properties_first(props);
	while (prop) {
		const enum obs_property_type type = obs_property_get_type(prop);
		if (type == OBS_PROPERTY_GROUP) {
			int n = apply_search_filter(obs_property_group_content(prop), settings, q);
			obs_property_set_visible(prop, n > 0);
			shown += n;
		} else if (type == OBS_PROPERTY_BOOL) {
			const char *name = obs_property_name(prop);
			if (name && name[0] == 'w' && name[1] == '.') {
				const char *desc = obs_property_description(prop);
				const char *enc = checkbox_encoded(prop, settings);
				const bool vis = text_has(desc, q) || text_has(enc, q);
				obs_property_set_visible(prop, vis);
				if (vis)
					shown++;
			}
		}
		obs_property_next(&prop);
	}
	return shown;
}

static bool win_search_changed(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(priv);
	UNUSED_PARAMETER(p);
	apply_search_filter(props, settings, obs_data_get_string(settings, "win_search"));
	return true;
}

static void add_window_checkbox(obs_properties_t *group, struct swc_source *ctx, obs_data_t *settings,
			       const char *encoded, const char *key, const char *desc, bool checked)
{
	char bool_key[32];
	char enc_key[40];
	obs_property_t *p;
	const uint32_t h = hash_encoded(key && key[0] ? key : encoded);

	snprintf(bool_key, sizeof(bool_key), "w.%08x", h);
	snprintf(enc_key, sizeof(enc_key), "w.%08x.enc", h);
	if (settings) {
		obs_data_set_default_string(settings, enc_key, encoded);
		obs_data_set_string(settings, enc_key, encoded);
		obs_data_set_default_bool(settings, bool_key, checked);
		obs_data_set_bool(settings, bool_key, checked);
	}
	p = obs_properties_add_bool(group, bool_key, desc);
	obs_property_set_modified_callback2(p, window_toggled, ctx);
}

static void add_window_checkboxes(obs_properties_t *props, struct swc_source *ctx, obs_data_t *settings)
{
	struct swc_window live[SWC_MAX_WINDOWS];
	struct swc_pick picks[SWC_MAX_WINDOWS];
	obs_properties_t *run_g;
	obs_properties_t *stop_g;
	obs_data_array_t *arr;
	int n = 0;
	int pick_n = 0;
	int run_n = 0;
	int stop_n = 0;
	char run_title[128];
	char stop_title[128];

	memset(picks, 0, sizeof(picks));
	if (settings)
		n = enum_picker_windows_ex(ctx, settings, live, SWC_MAX_WINDOWS);

	for (int i = 0; i < n; i++) {
		char key[SWC_STR];
		int idx;

		if (window_is_obs(&live[i]))
			continue;
		group_key_from_parts(live[i].title, live[i].class_name, live[i].exe, key, sizeof(key));
		idx = find_pick(picks, pick_n, key);
		if (idx >= 0) {
			long area = window_area(&live[i]);
			picks[idx].count++;
			if (area > picks[idx].area) {
				picks[idx].area = area;
				snprintf(picks[idx].encoded, sizeof(picks[idx].encoded), "%s", live[i].encoded);
				snprintf(picks[idx].class_name, sizeof(picks[idx].class_name), "%s",
					 live[i].class_name);
				snprintf(picks[idx].title, sizeof(picks[idx].title), "%s", live[i].title);
			}
			continue;
		}
		if (pick_n >= SWC_MAX_WINDOWS)
			break;
		idx = pick_n++;
		snprintf(picks[idx].key, sizeof(picks[idx].key), "%s", key);
		snprintf(picks[idx].encoded, sizeof(picks[idx].encoded), "%s", live[i].encoded);
		snprintf(picks[idx].exe, sizeof(picks[idx].exe), "%s", live[i].exe);
		snprintf(picks[idx].class_name, sizeof(picks[idx].class_name), "%s", live[i].class_name);
		snprintf(picks[idx].title, sizeof(picks[idx].title), "%s", live[i].title);
		picks[idx].area = window_area(&live[i]);
		picks[idx].count = 1;
		picks[idx].running = true;
	}

	arr = settings ? obs_data_get_array(settings, "windows") : NULL;
	if (arr) {
		const size_t an = obs_data_array_count(arr);
		for (size_t i = 0; i < an; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			const char *enc = obs_data_get_string(item, "value");
			char key[SWC_STR];
			char title[512];
			char class_name[256];
			char exe[256];
			int idx;

			group_key_from_encoded(enc, key, sizeof(key));
			idx = find_pick(picks, pick_n, key);
			if (idx >= 0 || !enc || !enc[0] || pick_n >= SWC_MAX_WINDOWS) {
				obs_data_release(item);
				continue;
			}
			if (!swc_parse_encoded(enc, title, sizeof(title), class_name, sizeof(class_name), exe,
					      sizeof(exe))) {
				obs_data_release(item);
				continue;
			}
			if (exe_is_obs(exe)) {
				obs_data_release(item);
				continue;
			}
			idx = pick_n++;
			snprintf(picks[idx].key, sizeof(picks[idx].key), "%s", key);
			snprintf(picks[idx].encoded, sizeof(picks[idx].encoded), "%s", enc);
			snprintf(picks[idx].exe, sizeof(picks[idx].exe), "%s", exe);
			snprintf(picks[idx].class_name, sizeof(picks[idx].class_name), "%s", class_name);
			snprintf(picks[idx].title, sizeof(picks[idx].title), "%s", title);
			picks[idx].count = 1;
			picks[idx].running = false;
			obs_data_release(item);
		}
		obs_data_array_release(arr);
	}

	for (int i = 0; i < pick_n; i++)
		format_pick_label(&picks[i]);
	if (pick_n > 1)
		qsort(picks, (size_t)pick_n, sizeof(picks[0]), pick_cmp);

	for (int i = 0; i < pick_n; i++) {
		if (picks[i].running)
			run_n++;
		else
			stop_n++;
	}

	if (pick_n == 0) {
		obs_property_t *info = obs_properties_add_text(props, "win_empty",
							       SWC_TEXT("SelectedWindowsCapture.NoWindows"),
							       OBS_TEXT_INFO);
		obs_property_text_set_info_type(info, OBS_TEXT_INFO_WARNING);
		return;
	}

	if (run_n) {
		run_g = obs_properties_create();
		snprintf(run_title, sizeof(run_title), "%s (%d)", SWC_TEXT("SelectedWindowsCapture.Running"), run_n);
		for (int i = 0; i < pick_n; i++) {
			if (!picks[i].running)
				continue;
			add_window_checkbox(run_g, ctx, settings, picks[i].encoded, picks[i].key, picks[i].label,
					    group_is_selected(settings, picks[i].key));
		}
		obs_properties_add_group(props, "run_group", run_title, OBS_GROUP_NORMAL, run_g);
	}

	if (stop_n) {
		stop_g = obs_properties_create();
		snprintf(stop_title, sizeof(stop_title), "%s (%d)", SWC_TEXT("SelectedWindowsCapture.Stopped"), stop_n);
		for (int i = 0; i < pick_n; i++) {
			if (picks[i].running)
				continue;
			add_window_checkbox(stop_g, ctx, settings, picks[i].encoded, picks[i].key, picks[i].label,
					    true);
		}
		obs_properties_add_group(props, "stop_group", stop_title, OBS_GROUP_NORMAL, stop_g);
	}
}

typedef void (*swc_window_bool_cb)(obs_property_t *p, obs_data_t *settings);

static void walk_window_bools(obs_properties_t *props, swc_window_bool_cb cb, obs_data_t *settings)
{
	obs_property_t *prop;

	if (!props || !cb)
		return;

	prop = obs_properties_first(props);
	while (prop) {
		const enum obs_property_type type = obs_property_get_type(prop);
		if (type == OBS_PROPERTY_GROUP) {
			walk_window_bools(obs_property_group_content(prop), cb, settings);
		} else if (type == OBS_PROPERTY_BOOL) {
			const char *name = obs_property_name(prop);
			if (name && name[0] == 'w' && name[1] == '.')
				cb(prop, settings);
		}
		obs_property_next(&prop);
	}
}

static void window_bool_select(obs_property_t *p, obs_data_t *settings)
{
	if (!obs_property_visible(p))
		return;
	obs_data_set_bool(settings, obs_property_name(p), true);
	array_upsert_group(settings, checkbox_encoded(p, settings));
}

static void window_bool_clear(obs_property_t *p, obs_data_t *settings)
{
	obs_data_set_bool(settings, obs_property_name(p), false);
}

static bool add_all_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct swc_source *ctx = data;
	obs_data_t *settings;

	UNUSED_PARAMETER(p);
	if (!ctx)
		return false;

	settings = obs_source_get_settings(ctx->source);
	walk_window_bools(props, window_bool_select, settings);
	commit_selected(ctx, settings);
	fill_filter_window_list(obs_properties_get(props, "filter_window"), settings);
	obs_data_release(settings);
	swc_begin_props_sync();
	swc_defer_clear_focus();
	return true;
}

static bool clear_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct swc_source *ctx = data;
	obs_data_t *settings;
	obs_data_array_t *arr;

	UNUSED_PARAMETER(p);
	if (!ctx)
		return false;

	settings = obs_source_get_settings(ctx->source);
	walk_window_bools(props, window_bool_clear, settings);
	arr = obs_data_array_create();
	obs_data_set_array(settings, "windows", arr);
	obs_data_array_release(arr);
	commit_selected(ctx, settings);
	fill_filter_window_list(obs_properties_get(props, "filter_window"), settings);
	obs_data_release(settings);
	swc_begin_props_sync();
	swc_defer_clear_focus();
	return true;
}

static void add_window_actions(obs_properties_t *props, struct swc_source *ctx, obs_data_t *settings)
{
	obs_properties_add_button2(props, "add_all", SWC_TEXT("SelectedWindowsCapture.AddAll"), add_all_clicked, ctx);
	obs_properties_add_button2(props, "clear_windows", SWC_TEXT("SelectedWindowsCapture.Clear"), clear_clicked,
				   ctx);

#ifdef HAVE_OBS_FRONTEND_API
	{
		obs_property_t *fw;
		obs_property_t *ob;
		const bool show = !settings || (int)obs_data_get_int(settings, "filter_mode") == SWC_MODE_INCLUDE;

		fw = obs_properties_add_list(props, "filter_window", SWC_TEXT("SelectedWindowsCapture.FilterWindow"),
					     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		fill_filter_window_list(fw, settings);
		ob = obs_properties_add_button2(props, "open_filters", SWC_TEXT("SelectedWindowsCapture.OpenFilters"),
						open_filters_clicked, ctx);
		obs_property_set_visible(fw, show);
		obs_property_set_visible(ob, show);
	}
#else
	UNUSED_PARAMETER(settings);
#endif
}

static void rebuild_window_list(obs_properties_t *props, struct swc_source *ctx, obs_data_t *settings)
{
	swc_begin_props_sync();
	obs_properties_remove_by_name(props, "run_group");
	obs_properties_remove_by_name(props, "stop_group");
	obs_properties_remove_by_name(props, "win_empty");
	obs_properties_remove_by_name(props, "add_all");
	obs_properties_remove_by_name(props, "clear_windows");
	obs_properties_remove_by_name(props, "filter_window");
	obs_properties_remove_by_name(props, "open_filters");

	add_window_checkboxes(props, ctx, settings);
	if (settings)
		apply_search_filter(props, settings, obs_data_get_string(settings, "win_search"));
	add_window_actions(props, ctx, settings);
}

static bool refresh_windows_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct swc_source *ctx = data;
	obs_data_t *settings;

	UNUSED_PARAMETER(p);
	if (!ctx)
		return false;

	settings = obs_source_get_settings(ctx->source);
	rebuild_window_list(props, ctx, settings);
	obs_data_release(settings);
	swc_defer_clear_focus();
	return true;
}

static obs_properties_t *swc_properties(void *data)
{
	struct swc_source *ctx = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	obs_data_t *settings = ctx ? obs_source_get_settings(ctx->source) : NULL;

	swc_begin_props_sync();

	if (settings) {
		persist_monitor_id(settings);
		migrate_pick_window(settings);
	}
	p = obs_properties_add_list(props, "filter_mode", SWC_TEXT("SelectedWindowsCapture.FilterMode"),
				    OBS_COMBO_TYPE_RADIO, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, SWC_TEXT("SelectedWindowsCapture.FilterMode.Include"), SWC_MODE_INCLUDE);
	obs_property_list_add_int(p, SWC_TEXT("SelectedWindowsCapture.FilterMode.Exclude"), SWC_MODE_EXCLUDE);
	obs_property_set_modified_callback2(p, filter_mode_changed, ctx);

	p = obs_properties_add_text(props, "exclude_hint", SWC_TEXT("SelectedWindowsCapture.ExcludeHint"),
				    OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_NORMAL);
	obs_property_set_visible(p, settings && (int)obs_data_get_int(settings, "filter_mode") == SWC_MODE_EXCLUDE);

	p = obs_properties_add_text(props, "win_search", SWC_TEXT("SelectedWindowsCapture.Search"), OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback2(p, win_search_changed, ctx);
	obs_properties_add_button2(props, "refresh_windows", SWC_TEXT("SelectedWindowsCapture.Refresh"),
				   refresh_windows_clicked, ctx);

	add_window_checkboxes(props, ctx, settings);
	if (settings)
		apply_search_filter(props, settings, obs_data_get_string(settings, "win_search"));
	add_window_actions(props, ctx, settings);

	if (settings) {
		if (ctx) {
			pthread_mutex_lock(&ctx->mutex);
			load_selected(ctx, settings);
			pthread_mutex_unlock(&ctx->mutex);
		}
		obs_data_release(settings);
	}

	return props;
}

struct obs_source_info selected_windows_capture_info = {
	.id = "selected_windows_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB,
	.get_name = swc_get_name,
	.create = swc_create,
	.destroy = swc_destroy,
	.update = swc_update,
	.video_tick = swc_tick,
	.video_render = swc_render,
	.get_width = swc_width,
	.get_height = swc_height,
	.get_defaults = swc_defaults,
	.get_properties = swc_properties,
	.save = swc_save,
	.enum_active_sources = swc_enum_active,
	.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
	.video_get_color_space = swc_color_space,
};
