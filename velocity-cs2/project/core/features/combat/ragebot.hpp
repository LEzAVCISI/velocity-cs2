#pragma once

#include "../../config.hpp"
#include "../../sdk/valve/classes/c_cs_player_pawn.hpp"
#include "../../sdk/valve/classes/game_enums.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <cstdio>

class i_csgo_input;
class c_user_cmd;

class c_ragebot {
public:
	static constexpr int k_debug_max_targets = 16;
	static constexpr int k_log_ticks_per_file = 500;

	struct debug_target_t {
		char  name[48]{};
		int   index = 0;
		int   hp = 0;
		int   min_dmg = 0;
		int   hitbox_id = -1;
		float dist = 0.f;
		float fov = 0.f;
		float damage = 0.f;
		float hitchance = 0.f; // 0-100 estimated, -1 unknown
		float aim_x = 0.f;
		float aim_y = 0.f;
		float aim_z = 0.f;
		bool  scanned = false;
		bool  can_hit = false;
		bool  is_best = false;
		bool  penetrated = false;
	};

	struct debug_frame_t {
		bool  valid = false;
		bool  can_fire = false;
		bool  is_autostop = false;
		bool  fired = false;
		bool  have_target = false;
		bool  pen_ready = false;
		bool  autowall = false;
		bool  silent = false;
		bool  auto_fire = false;
		bool  on_ground = false;
		bool  hitchance_pass = false;
		float spread = 0.f;
		float inaccuracy = 0.f;
		float speed = 0.f;
		float eye_x = 0.f, eye_y = 0.f, eye_z = 0.f;
		float aim_x = 0.f, aim_y = 0.f, aim_z = 0.f;
		float ang_x = 0.f, ang_y = 0.f;
		float punch_x = 0.f, punch_y = 0.f;
		float best_damage = 0.f;
		float best_fov = 0.f;
		float best_dist = 0.f;
		int   hist_count = 0;
		int   clip = 0;
		int   next_attack = 0;
		int   tick_base = 0;
		int   weapon_cat = 0;
		int   weapon_def = 0;
		int   hitchance_needed = 0;
		int   min_damage = 0;
		int   point_scale = 0;
		int   hitboxes_mask = 0;
		int   multipoint_mask = 0;
		int   sel_mode = 0;
		int   candidate_count = 0;
		int   best_hp = 0;
		int   best_index = 0;
		int   hitbox_id = -1;
		int   hitbox_bit = 0;
		int   zoom = 0;
		int   local_flags = 0;
		char  block[64]{};
		char  weapon_name[48]{};
		int   target_count = 0;
		debug_target_t targets[k_debug_max_targets]{};
	};

	void run(i_csgo_input* input, c_user_cmd* cmd);

	debug_frame_t snapshot_debug() const {
		std::lock_guard lock(m_debug_mutex);
		return m_debug;
	}

	static int category_from_weapon(c_base_player_weapon* weapon);
	static int resolve_min_damage(const c_config::ragebot_t::weapon_t& wpn, int target_hp, bool override_active);

private:
	void publish_debug(const debug_frame_t& frame);

	mutable std::mutex m_debug_mutex;
	debug_frame_t m_debug{};
};

inline const auto g_ragebot = std::make_unique<c_ragebot>();

namespace ragebot {
inline int category_from_weapon(c_base_player_weapon* weapon) {
	return c_ragebot::category_from_weapon(weapon);
}

inline int resolve_min_damage(const c_config::ragebot_t::weapon_t& wpn, int target_hp, bool override_active) {
	return c_ragebot::resolve_min_damage(wpn, target_hp, override_active);
}
} // namespace ragebot
