#include "model_changer.hpp"

#include <windows.h>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace {
	// Minimal CBufferString mirror (tier0). Only what precache needs: a small
	// buffer string we can Insert() a path into. Layout must match the game's.
	struct c_buffer_string {
		int m_length;
		int m_allocated_size;
		union {
			char* m_ptr;
			char m_inline[8];
		};

		c_buffer_string() : m_length(0), m_allocated_size(0x80000000 | 0x40000000 | 8) {
			m_ptr = nullptr;
		}
	};

	// tier0 export: ?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z
	constexpr char k_buffer_insert_export[] = "?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z";
	// resourcesystem.dll precache (BLoadResourceManifest-style) entry point.
	constexpr char k_precache_pattern[] = "40 53 55 57 48 81 EC 80 00 00 00 48 8B 01 49 8B E8 48 8B FA";

	// Model packs ship the full character model alongside partial pieces (cloth
	// meshes, no-skeleton variants, physics/LOD fragments). Those partial vmdls
	// have no proper player skeleton, so SetModel on the pawn crashes deep in the
	// engine's bone/anim setup (not catchable by SEH). Skip anything whose name
	// contains one of these fragment markers so only full models reach the list.
	constexpr const char* k_fragment_markers[] = {
		"nohitbox", "no_hitbox", "nohbox", "hitbox",
		"cloth", "swim", "prefab", "arm",
		"_lod", "gib", "phys", "ragdoll",
		"attach", "_part", "helmet", "hat", "mask", "prop", "normal", "player_model", "dress",
		"sk2model", "gfl2", "sleeve",
	};

	bool is_fragment_model(std::string name) {
		std::transform(name.begin(), name.end(), name.begin(),
			[](unsigned char c) { return (char)std::tolower(c); });
		for (const char* marker : k_fragment_markers)
			if (name.find(marker) != std::string::npos)
				return true;
		return false;
	}

	// path::string() throws std::system_error when the filename has characters
	// the active code page can't represent (e.g. CJK model packs). u8string()
	// never throws on encoding, so go through it. Works under both C++17
	// (std::string) and C++20 (std::u8string) via a raw byte copy.
	std::string to_utf8(const fs::path& p) {
		std::string out;
		try {
			auto u8s = p.u8string();
			out.assign(reinterpret_cast<const char*>(u8s.data()), u8s.size());
		}
		catch (...) {
		}
		return out;
	}
}

void c_model_changer::initialize() {
	// Runs exactly once even though the menu render thread and the game thread
	// both call this. The second caller blocks here until the first finishes
	// the (slow, disk-walking) scan(), so nobody reads m_models mid-rebuild.
	std::call_once(m_init_once, [this]() {
		// ResourceSystem013 out of resourcesystem.dll (self-contained; the module
		// isn't in the shared module list).
		if (auto* rs = GetModuleHandleA("resourcesystem.dll")) {
			using create_interface_t = void* (*)(const char*, int*);
			if (auto create_interface = reinterpret_cast<create_interface_t>(GetProcAddress(rs, "CreateInterface")))
				m_resource_system = create_interface("ResourceSystem013", nullptr);
		}

		m_precache_fn = reinterpret_cast<void* (*)(void*, void*, const char*)>(
			g_opcodes->scan("resourcesystem.dll", k_precache_pattern));

		if (auto* tier0 = GetModuleHandleA("tier0.dll"))
			m_buffer_insert_fn = reinterpret_cast<const char* (__fastcall*)(void*, int, const char*, int, bool)>(
				GetProcAddress(tier0, k_buffer_insert_export));

		scan();

		m_initialized = true;
	});
}

void c_model_changer::refresh() {
	scan();
}

void c_model_changer::scan() {
	// Build the whole list into a local first, then swap it into place under the
	// lock. This keeps the (slow) disk walk out of the critical section, so the
	// game thread's selected_model_path() never blocks for long, and the live
	// m_models is only ever replaced atomically -- never seen half-cleared.
	std::vector<model_entry_t> models;
	models.push_back({ "[ off ]", "" });

	// CWD at runtime is <install>\game\bin\win64; the models live under
	// <install>\game\csgo\characters\models.
	char cwd[MAX_PATH]{};
	GetCurrentDirectoryA(MAX_PATH, cwd);
	std::string root = cwd;

	if (auto pos = root.find("bin\\win64"); pos != std::string::npos)
		root.replace(pos, 9, "csgo\\characters\\models");

	// Whole scan is guarded: a single unreadable/oddly-named file must never
	// throw out of here, because this runs on the menu render thread and an
	// escaping exception tears down the ImGui frame (menu vanishes).
	try {
		std::error_code ec;
		if (fs::exists(root, ec)) {
			for (auto it = fs::recursive_directory_iterator(root, ec);
			     it != fs::recursive_directory_iterator(); it.increment(ec)) {
				if (ec)
					break;

				const auto& path = it->path();
				if (path.extension() != ".vmdl_c")
					continue;

				// Drop partial/no-skeleton pieces that would crash the pawn on apply.
				std::string stem = to_utf8(path.stem());
				if (stem.empty() || is_fragment_model(stem) || std::isdigit(static_cast<unsigned char>(stem[0])))
					continue;

				std::string full = to_utf8(path);
				auto cpos = full.find("characters\\");
				if (cpos == std::string::npos)
					cpos = full.find("characters/");
				if (cpos == std::string::npos)
					continue;

				std::string rel = full.substr(cpos);
				std::replace(rel.begin(), rel.end(), '\\', '/');
				// engine wants the source .vmdl path, not the compiled .vmdl_c
				if (auto vpos = rel.rfind(".vmdl_c"); vpos != std::string::npos)
					rel = rel.substr(0, vpos) + ".vmdl";

				models.push_back({ stem, rel });
			}
		}
	}
	catch (...) {
	}

	// Sort alphabetically, keep "[ off ]" first.
	if (models.size() > 1)
		std::sort(models.begin() + 1, models.end(),
			[](const auto& a, const auto& b) { return a.name < b.name; });

	// Publish atomically, and rebuild the cstr view (points into m_models) under
	// the same lock so the menu never sees names pointing at freed strings.
	std::lock_guard<std::mutex> lk(m_models_mutex);
	m_models = std::move(models);
	m_model_names_cstr.clear();
	m_model_names_cstr.reserve(m_models.size());
	for (auto& m : m_models)
		m_model_names_cstr.push_back(m.name.c_str());
}

const char* c_model_changer::selected_model_path() const {
	// Runs on the game thread; a menu-thread refresh() can rebuild m_models
	// concurrently. Copy the path out under the lock so the returned pointer
	// stays valid even if the list is replaced right after we unlock.
	std::lock_guard<std::mutex> lk(m_models_mutex);
	const int idx = g_cfg->model_changer.m_selected_model;
	if (idx <= 0 || idx >= (int)m_models.size())
		return nullptr;

	const std::string& path = m_models[idx].path;
	if (path.empty())
		return nullptr;

	m_selected_cache = path;
	return m_selected_cache.c_str();
}

void c_model_changer::precache(const char* path) {
	if (!m_precache_fn || !m_resource_system || !m_buffer_insert_fn || !path || !path[0])
		return;

	__try {
		c_buffer_string names{};
		m_buffer_insert_fn(&names, 0, path, -1, false);
		m_precache_fn(m_resource_system, &names, "");
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}
}
