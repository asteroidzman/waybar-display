// Vendored from Waybar: resources/custom_modules/cffi_example/waybar_cffi_module.h
#pragma once

#include <gtk/gtk.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Waybar ABI version. 2 is the latest version
extern const size_t wbcffi_version;

/// Private Waybar CFFI module
typedef struct wbcffi_module wbcffi_module;

/// Waybar module information
typedef struct {
  /// Waybar CFFI object pointer
  wbcffi_module* obj;

  /// Waybar version string
  const char* waybar_version;

  /// Returns the waybar widget allocated for this module
  /// @param obj Waybar CFFI object pointer
  GtkContainer* (*get_root_widget)(wbcffi_module* obj);

  /// Queues a request for calling wbcffi_update() on the next GTK main event
  /// loop iteration
  /// @param obj Waybar CFFI object pointer
  void (*queue_update)(wbcffi_module*);
} wbcffi_init_info;

/// Config key-value pair
typedef struct {
  const char* key;
  const char* value;
} wbcffi_config_entry;

void* wbcffi_init(const wbcffi_init_info* init_info, const wbcffi_config_entry* config_entries,
                  size_t config_entries_len);
void wbcffi_deinit(void* instance);
void wbcffi_update(void* instance);
void wbcffi_refresh(void* instance, int signal);
void wbcffi_doaction(void* instance, const char* action_name);

#ifdef __cplusplus
}
#endif
