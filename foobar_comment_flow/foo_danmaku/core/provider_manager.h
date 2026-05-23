#pragma once
/* provider_manager.h
 *
 * Owns the global MusicClientHandle (g_music) and its lifecycle:
 *   - on_init : discover + load all provider DLLs from the components directory
 *   - on_quit : destroy the client and unload all providers
 *
 * Provider priority is persisted via foobar2000 cfg_string (comma-separated
 * DLL filenames).  The preferences page calls save_provider_order() to update
 * both the live provider list and the stored config.
 */

#include "music_client.h"
#include <string>
#include <vector>

/* ── global client handle ──────────────────────────── */

/** Initialised in initquit::on_init(), destroyed in on_quit().
 *  NULL before on_init() and after on_quit(). */
extern MusicClientHandle g_music;

/* ── provider discovery ────────────────────────────── */

/** Scan @p dir for DLLs that export music_provider_vtable.
 *  Returns full paths sorted alphabetically. */
std::vector<std::wstring> discover_providers(const std::wstring& dir);

/* ── provider order persistence ────────────────────── */

/** Re-read cfg_provider_order, (re)load providers in that order into g_music.
 *  Safe to call from the UI thread only. */
void reload_providers();

/** Persist @p ordered_paths (full paths) to cfg_provider_order and
 *  immediately reorder the live providers in g_music via
 *  music_client_reorder_providers().
 *  @p ordered_paths must be a permutation of the currently loaded paths. */
void save_provider_order(const std::vector<std::wstring>& ordered_paths);
