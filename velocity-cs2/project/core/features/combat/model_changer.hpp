#pragma once

#include "../../core/main.hpp"
#include <string>
#include <vector>
#include <mutex>

// Custom model changer: scans csgo/characters/models for user-dropped .vmdl_c
// files and lets us swap the local player's model to one of them. Unlike the
// agent changer (which only picks from models the game already shipped/loaded),
// these custom models are unknown to the engine, so we must precache them via
// the ResourceSystem before SetModel or the engine renders the error model /
// crashes. The actual SetModel call is driven from the skin changer's run loop
// (stage 7) so it reuses its safe local-pawn context; this class only owns the
// model list and the precache machinery.
class c_model_changer {
public:
	struct model_entry_t {
		std::string name;   // display name (file stem), first entry is "[ off ]"
		std::string path;   // engine-relative "characters/.../foo.vmdl", empty for off
	};

	// Resolve precache function/interface and scan the models folder once.
	void initialize();

	// Rescan the models folder (menu "refresh" button).
	void refresh();

	std::vector<const char*> model_names() const {
		std::lock_guard<std::mutex> lk(m_models_mutex);
		return m_model_names_cstr;
	}
	int model_count() const { return (int)m_models.size(); }

	// Engine-relative path for the configured selection, or nullptr when the
	// selection is out of range or points at the "[ off ]" entry.
	const char* selected_model_path() const;

	// Precache a model path through the ResourceSystem. No-op if the precache
	// machinery couldn't be resolved (SetModel then just runs un-precached,
	// same behaviour as the agent changer).
	void precache(const char* path);

	bool is_initialized() const { return m_initialized; }

private:
	void scan();

	// initialize() is called from both the menu render thread and the game
	// thread (skin_changer stage 7). call_once makes the one-time resolve+scan
	// run exactly once and blocks the second caller until it finishes, so no
	// reader ever touches m_models while scan() is rebuilding it.
	std::once_flag m_init_once;
	bool m_initialized = false;

	// Guards m_models against a menu-thread refresh() rebuilding the list while
	// the game thread reads it in selected_model_path().
	mutable std::mutex m_models_mutex;
	// selected_model_path() copies the chosen path here under the lock and
	// returns a pointer to it, so the caller's pointer can't dangle if the list
	// is rebuilt right after the lock is released.
	mutable std::string m_selected_cache;

	std::vector<model_entry_t> m_models;
	std::vector<const char*> m_model_names_cstr;

	// Resolved lazily in initialize().
	void* m_resource_system = nullptr;
	void* (*m_precache_fn)(void*, void*, const char*) = nullptr;
	const char* (__fastcall* m_buffer_insert_fn)(void*, int, const char*, int, bool) = nullptr;
};

inline const auto g_model_changer = std::make_unique<c_model_changer>();
