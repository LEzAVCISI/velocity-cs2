#include "ragebot.hpp"

#include "../autowall/autowall.hpp"
#include "../lagcomp/lagcomp.hpp"
#include "../prediction/prediction.hpp"
#include "../../core/main.hpp"
#include "../../sdk/buttons.hpp"
#include "../../sdk/offsets.hpp"
#include "../../sdk/tick.hpp"
#include "../../sdk/verified_features.hpp"
#include "../../sdk/memsafe.hpp"
#include "../../sdk/valve/schema/schema.hpp"
#include "../../sdk/valve/classes/c_cs_player_pawn.hpp"
#include "../../sdk/valve/interfaces/vtables/i_csgo_input.hpp"
#include "../../sdk/valve/signatures/signatures.hpp"
#include "../shared/input_buttons.hpp"
#include "../../../ui/menu/elements/bind.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace {
// sdk-info protobufs.json CSubtickMoveStep size=56; presence bits @ +0x10 (nCachedBits).
// Engine CreateMove fill uses CreateNewSubtickMoveStep + ProtobufAdd — NEVER our
// linked protobuf RepeatedPtrField::Add (arena mismatch → InternalExtend crash).
constexpr std::uint64_t k_step_bit_button = 0x1ull;
constexpr std::uint64_t k_step_bit_pressed = 0x2ull;
constexpr std::uint64_t k_step_bit_when = 0x4ull;
constexpr std::uint64_t k_step_bit_analog_fwd = 0x8ull;
constexpr std::uint64_t k_step_bit_analog_left = 0x10ull;

struct alignas(8) engine_subtick_step_t {
	void*        vtable{};          // 0x00
	std::uint32_t has_bits{};       // 0x08
	std::uint32_t pad_0c{};
	std::uint64_t cached_bits{};    // 0x10 — presence (sdk-info / IDA)
	std::uint64_t button{};         // 0x18
	bool         pressed{};         // 0x20
	std::uint8_t pad_21[3]{};
	float        when{};            // 0x24
	float        analog_forward{};  // 0x28
	float        analog_left{};     // 0x2C
	std::uint32_t pad_end[2]{};     // 0x30..0x37 → 56
};
static_assert(sizeof(engine_subtick_step_t) == 56, "sdk-info CSubtickMoveStep size");
static_assert(offsetof(engine_subtick_step_t, cached_bits) == 0x10, "presence @ +0x10");
static_assert(offsetof(engine_subtick_step_t, button) == 0x18, "button @ +0x18");
static_assert(offsetof(engine_subtick_step_t, when) == 0x24, "when @ +0x24");
static_assert(offsetof(engine_subtick_step_t, analog_forward) == 0x28, "analog_fwd @ +0x28");

// sdk-info protobufs.json: CBaseUserCmdPB.subtick_moves @ offset 24 (0x18).
struct engine_repeated_t {
	struct rep_t {
		int allocated_size{};
		engine_subtick_step_t* elements[1];
	};
	void* arena{};
	int   current_size{};
	int   total_size{};
	rep_t* rep{};
};

engine_repeated_t* subtick_field(CBaseUserCmdPB* base) {
	if (!base || !memsafe::is_user_ptr(base))
		return nullptr;
	return reinterpret_cast<engine_repeated_t*>(
		reinterpret_cast<std::uint8_t*>(base) + 0x18);
}

std::uint8_t* rel32_target(std::uint8_t* call_insn) {
	if (!call_insn || call_insn[0] != 0xE8)
		return nullptr;
	const std::int32_t disp = *reinterpret_cast<std::int32_t*>(call_insn + 1);
	return call_insn + 5 + disp;
}

// CreateNewSubtickMoveStep(arena) + ProtobufAddToRepeated(field, step) — sdk-info.
engine_subtick_step_t* engine_add_subtick(CBaseUserCmdPB* base) {
	if (!base)
		return nullptr;

	auto* field = subtick_field(base);
	if (!field)
		return nullptr;
	if (field->current_size >= 30)
		return nullptr;

	// Reuse pre-allocated slot (CreateMove fill path).
	if (field->rep && memsafe::is_user_ptr(field->rep)) {
		const int cur = field->current_size;
		const int cap = field->rep->allocated_size;
		if (cur >= 0 && cur < cap) {
			engine_subtick_step_t* slot = field->rep->elements[cur];
			if (slot && memsafe::is_user_ptr(slot)) {
				field->current_size = cur + 1;
				if (field->total_size < field->current_size)
					field->total_size = field->current_size;
				std::memset(reinterpret_cast<std::uint8_t*>(slot) + 0x08, 0, 56 - 0x08);
				return slot;
			}
		}
	}

	using fn_create_t = engine_subtick_step_t*(__fastcall*)(void* arena);
	using fn_add_t = engine_subtick_step_t*(__fastcall*)(engine_repeated_t* field, engine_subtick_step_t* step);

	static fn_create_t create_fn = nullptr;
	static fn_add_t add_fn = nullptr;
	static bool resolved = false;
	if (!resolved) {
		resolved = true;
		create_fn = reinterpret_cast<fn_create_t>(SIG("CreateNewSubtickMoveStep"));
		// Same call-site as sdk-info: E8 create ; 48 8B D0 ; 48 8B CE ; E8 add
		if (g_opcodes) {
			auto* hit = reinterpret_cast<std::uint8_t*>(
				g_opcodes->scan("client.dll", "E8 ? ? ? ? 48 8B D0 48 8B CE E8 ? ? ? ? 48 8B C8"));
			if (hit) {
				if (!create_fn)
					create_fn = reinterpret_cast<fn_create_t>(rel32_target(hit));
				add_fn = reinterpret_cast<fn_add_t>(rel32_target(hit + 0xB));
			}
		}
	}
	if (!create_fn || !add_fn)
		return nullptr;

	engine_subtick_step_t* step = nullptr;
	__try {
		step = create_fn(field->arena);
		if (!step || !memsafe::is_user_ptr(step))
			return nullptr;
		step = add_fn(field, step);
		if (!step || !memsafe::is_user_ptr(step))
			return nullptr;
		std::memset(reinterpret_cast<std::uint8_t*>(step) + 0x08, 0, 56 - 0x08);
		return step;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

// Fallback: sdk-info QueueForceSubtickMove — same allocator family as CreateMove.
engine_subtick_step_t* queue_force_subtick(CBaseUserCmdPB* base) {
	if (!base)
		return nullptr;
	using fn_t = engine_subtick_step_t*(__fastcall*)(CBaseUserCmdPB*);
	static fn_t fn = nullptr;
	if (!fn)
		fn = reinterpret_cast<fn_t>(SIG("QueueForceSubtickMove"));
	if (!fn)
		return nullptr;
	__try {
		auto* step = fn(base);
		if (!step || !memsafe::is_user_ptr(step))
			return nullptr;
		// Prefer Message* (vtable in user range). If caller returned _impl_/has_bits,
		// rewind 0x08 to the Message base (sdk-info has_bits_offset = 16, vtable = 0).
		if (reinterpret_cast<std::uintptr_t>(step->vtable) < 0x10000ull) {
			auto* maybe = reinterpret_cast<engine_subtick_step_t*>(
				reinterpret_cast<std::uint8_t*>(step) - 0x08);
			if (memsafe::is_user_ptr(maybe)
				&& reinterpret_cast<std::uintptr_t>(maybe->vtable) >= 0x10000ull)
				step = maybe;
			else
				return nullptr;
		}
		std::memset(reinterpret_cast<std::uint8_t*>(step) + 0x08, 0, 56 - 0x08);
		return step;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

engine_subtick_step_t* alloc_subtick_step(CBaseUserCmdPB* base) {
	// Engine input holds at most 12 subtick steps per cmd. Appending past that
	// (or repeatedly into a reused cmd) makes prediction slice the tick into
	// dozens of sub-steps — visible as slow-motion viewmodel/anims.
	if (base && base->subtick_moves_size() >= 12)
		return nullptr;
	if (auto* s = engine_add_subtick(base))
		return s;
	return queue_force_subtick(base);
}

void fill_analog_subtick(engine_subtick_step_t* step, float when, float fwd, float left) {
	if (!step)
		return;
	step->button = 0;
	step->pressed = false;
	step->when = std::clamp(when, 0.f, 0.999f);
	step->analog_forward = fwd;
	step->analog_left = left;
	step->has_bits = 0;
	step->cached_bits = k_step_bit_when | k_step_bit_analog_fwd | k_step_bit_analog_left;
}

void fill_button_subtick(engine_subtick_step_t* step, std::uint64_t button, bool pressed, float when) {
	if (!step)
		return;
	step->button = button;
	step->pressed = pressed;
	step->when = std::clamp(when, 0.f, 0.999f);
	step->analog_forward = 0.f;
	step->analog_left = 0.f;
	step->has_bits = 0;
	step->cached_bits = k_step_bit_button | k_step_bit_pressed | k_step_bit_when;
}

constexpr float k_pi = 3.14159265358979323846f;
// sdk-info: C_BaseModelEntity::m_vecViewOffset = 3704 (0xE78). 0xE70 is stale.
constexpr std::ptrdiff_t k_view_offset = 0xE78;
// sdk-info: CCSPlayer_AimPunchServices::m_predictableBaseAngle = 0x50
constexpr std::ptrdiff_t k_aim_punch_angle = 0x50;
constexpr int k_hc_seeds = 64;
constexpr float k_hc_hit_radius = 22.0f;
constexpr int k_max_bt_records = 3;
constexpr int k_sphere_points = 4;
constexpr int k_max_points_per_hb = 6;
constexpr int k_max_pen_tests = 12;
constexpr float k_stop_speed = 18.f;      // legacy early-peek brake floor
constexpr float k_air_stop_speed = 25.f;
constexpr float k_extrap_min_speed = 18.f;
constexpr float k_sv_friction = 5.2f;
constexpr float k_sv_stopspeed = 80.f;
constexpr float k_sv_accelerate = 5.5f;
constexpr float k_max_wish = 450.f;
constexpr float k_accuracy_speed_frac = 0.34f; // Valve remap — below this, move inac ~0
constexpr int k_hc_seeds_precision = 96;

autowall::pen_result_t fire_bullet_safe(
	const vec3_t& start,
	const vec3_t& end,
	c_cs_player_pawn* local,
	c_cs_player_pawn* target,
	c_cs_weapon_base_v_data* weapon_data,
	bool allow_penetration,
	int hitbox_id)
{
	return g_autowall->fire_bullet(start, end, local, target, weapon_data, allow_penetration, hitbox_id);
}

template <typename T>
T read_ptr(std::uintptr_t address) {
	if (!address)
		return T{};
	return *reinterpret_cast<T*>(address);
}

template <typename T>
void write_ptr(std::uintptr_t address, const T& value) {
	if (!address)
		return;
	*reinterpret_cast<T*>(address) = value;
}

std::uintptr_t client_base() {
	return g_modules ? g_modules->m_modules.client_dll.get() : 0;
}

void* entity_by_index(int index) {
	if (!g_interfaces || !g_interfaces->m_entity_system)
		return nullptr;
	return g_interfaces->m_entity_system->get_base_entity(index);
}

float normalize_yaw(float yaw) {
	while (yaw > 180.0f) yaw -= 360.0f;
	while (yaw < -180.0f) yaw += 360.0f;
	return yaw;
}

vec3_t normalize_angles(vec3_t ang) {
	ang.x = std::clamp(ang.x, -89.0f, 89.0f);
	ang.y = normalize_yaw(ang.y);
	ang.z = 0.0f;
	return ang;
}

vec3_t calc_angle(const vec3_t& src, const vec3_t& dst) {
	const vec3_t delta = dst - src;
	const float hyp = std::sqrt(delta.x * delta.x + delta.y * delta.y);
	return normalize_angles(vec3_t(
		std::atan2f(-delta.z, hyp) * (180.0f / k_pi),
		std::atan2f(delta.y, delta.x) * (180.0f / k_pi),
		0.0f
	));
}

float angle_distance(const vec3_t& a, const vec3_t& b) {
	const vec3_t d = normalize_angles(vec3_t(a.x - b.x, a.y - b.y, 0.0f));
	return std::sqrt(d.x * d.x + d.y * d.y);
}

void angle_vectors(const vec3_t& angles, vec3_t* forward, vec3_t* right = nullptr, vec3_t* up = nullptr) {
	const float pitch = angles.x * (k_pi / 180.0f);
	const float yaw = angles.y * (k_pi / 180.0f);
	const float cp = std::cosf(pitch), sp = std::sinf(pitch);
	const float cy = std::cosf(yaw), sy = std::sinf(yaw);
	if (forward)
		*forward = vec3_t(cp * cy, cp * sy, -sp);
	if (right)
		*right = vec3_t(-sy, cy, 0.0f);
	if (up)
		*up = vec3_t(sp * cy, sp * sy, cp);
}

vec3_t get_eye_position(c_cs_player_pawn* pawn) {
	if (!pawn || !memsafe::is_user_ptr(pawn))
		return {};
	return pawn->get_eye_position_world();
}

vec3_t get_aim_punch(c_cs_player_pawn* local) {
	if (!local || !memsafe::is_user_ptr(local))
		return {};
	// sdk-info: m_pAimPunchServices @ 0x14B8, m_predictableBaseAngle @ +0x50
	const auto punch_service = read_ptr<std::uintptr_t>(
		reinterpret_cast<std::uintptr_t>(local) + cs2::verified::Aimbot::C_CSPlayerPawn__m_pAimPunchServices
	);
	if (!punch_service || !memsafe::is_user_ptr(reinterpret_cast<void*>(punch_service)))
		return {};
	const vec3_t punch = read_ptr<vec3_t>(punch_service + k_aim_punch_angle);
	if (!punch.is_valid())
		return {};
	// Reject garbage (stale +0x40 used to read ~600k)
	if (std::fabs(punch.x) > 90.f || std::fabs(punch.y) > 180.f || std::fabs(punch.z) > 90.f)
		return {};
	return punch;
}

std::uint16_t get_def_index(c_base_player_weapon* weapon) {
	if (!weapon)
		return 0;
	return read_ptr<std::uint16_t>(
		reinterpret_cast<std::uintptr_t>(weapon) + cs2::verified::ESP::C_BasePlayerWeapon__m_iItemDefinitionIndex
	);
}

float get_recoil_index(c_base_player_weapon* weapon) {
	if (!weapon)
		return 0.f;
	float v = read_ptr<float>(
		reinterpret_cast<std::uintptr_t>(weapon) + cs2::verified::Aimbot::C_CSWeaponBase__m_flRecoilIndex
	);
	return std::isfinite(v) ? v : 0.f;
}

// ---------- RNG / hitchance spread (Valve ran1 — matches CalcSpread) ----------
class valve_rng {
public:
	void seed(int s) {
		m_state = -std::abs(s);
		if (m_state == 0)
			m_state = -1;
		m_index = 0;
		m_seeded = false;
	}

	int generate() {
		if (!m_seeded) {
			int v = -m_state;
			if (v < 1)
				v = 1;
			for (int j = 39; j >= 0; --j) {
				v = lcg(v);
				if (v < 0)
					v += 2147483647;
				if (j < 32)
					m_table[j] = v;
			}
			m_state = v;
			m_index = m_table[0];
			m_seeded = true;
		}
		m_state = lcg(m_state);
		if (m_state < 0)
			m_state += 2147483647;
		const int idx = m_index / 0x4000000;
		m_index = m_table[idx];
		m_table[idx] = m_state;
		return m_index;
	}

	float random_float(float min = 0.0f, float max = 1.0f) {
		const auto raw = generate();
		const auto norm = std::fminf(0.99999988f, static_cast<float>(raw) * 4.6566129e-10f);
		return min + norm * (max - min);
	}

private:
	static int lcg(int state) {
		return 16807 * (state % 127773) - 2836 * (state / 127773);
	}

	int m_state{ 0 };
	int m_index{ 0 };
	int m_table[32]{};
	bool m_seeded{ false };
};

struct spread_vec_t { float x; float y; };

spread_vec_t calculate_spread(int seed, float inaccuracy, float spread, float recoil_index,
	int item_def_idx, int weapon_mode, int num_bullets)
{
	constexpr std::uint16_t revolver_id = 64;
	constexpr std::uint16_t negev_id = 28;
	constexpr auto two_pi = 2.0f * k_pi;
	(void)num_bullets;

	valve_rng rng;
	rng.seed(seed);

	auto inac_r = rng.random_float(0.0f, 1.0f);
	auto inac_a = rng.random_float(0.0f, two_pi);

	// R8 secondary reshape is mode==1, NOT num_bullets==1.
	if (item_def_idx == revolver_id && weapon_mode == 1)
		inac_r = 1.0f - (inac_r * inac_r);
	else if (item_def_idx == negev_id && recoil_index < 3.0f) {
		auto v = inac_r; auto c = 3;
		do { --c; v *= v; } while (static_cast<float>(c) > recoil_index);
		inac_r = 1.0f - v;
	}
	inac_r *= inaccuracy;

	auto spr_r = rng.random_float(0.0f, 1.0f);
	auto spr_a = rng.random_float(0.0f, two_pi);

	if (item_def_idx == revolver_id && weapon_mode == 1)
		spr_r = 1.0f - (spr_r * spr_r);
	else if (item_def_idx == negev_id && recoil_index < 3.0f) {
		auto v = spr_r; auto c = 3;
		do { --c; v *= v; } while (static_cast<float>(c) > recoil_index);
		spr_r = 1.0f - v;
	}
	spr_r *= spread;

	return {
		std::cosf(inac_a) * inac_r + std::cosf(spr_a) * spr_r,
		std::sinf(inac_a) * inac_r + std::sinf(spr_a) * spr_r
	};
}

int weapon_mode_of(c_cs_weapon_base* weapon) {
	if (!weapon)
		return 0;
	// client_dll.hpp: C_CSWeaponBase::m_weaponMode = 0x17B8 (0x17D8 was wrong / drifted)
	static std::uint32_t off = 0;
	static bool once = false;
	if (!once) {
		off = schema_get_offset("client.dll", "C_CSWeaponBase", "m_weaponMode");
		if (!off)
			off = 0x17B8;
		once = true;
	}
	return read_ptr<int>(reinterpret_cast<std::uintptr_t>(weapon) + off);
}

std::string normalize_bind_name(std::string key) {
	for (auto& c : key)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	if (key == "MOUSELEFT" || key == "MOUSE1") key = "M1";
	else if (key == "MOUSERIGHT" || key == "MOUSE2") key = "M2";
	else if (key == "MOUSEMIDDLE" || key == "MOUSE3") key = "M3";
	else if (key == "LEFTALT" || key == "RIGHTALT") key = "ALT";
	else if (key == "LEFTCTRL" || key == "RIGHTCTRL") key = "CTRL";
	else if (key == "LEFTSHIFT" || key == "RIGHTSHIFT") key = "SHIFT";
	else if (key == "DELETE") key = "DEL";
	else if (key == "INSERT") key = "INS";
	return key;
}

bool bind_active(const std::string& key, int mode) {
	const std::string norm = key.empty() ? std::string{} : normalize_bind_name(key);
	UI::InitBind("rage_dmg_override", norm.c_str());
	auto& e = UI::g_binds["rage_dmg_override"];
	if (e.key.empty() && !norm.empty())
		e.key = norm;
	e.mode = static_cast<UI::BindMode>(mode);
	return UI::IsBindActive("rage_dmg_override");
}

void clear_csgo_attack_input(i_csgo_input* input) {
	if (!input)
		return;
	constexpr std::uintptr_t k0 = 0x250, k1 = 0x258, k2 = 0x260, k3 = 0x268;
	constexpr std::uint64_t k_atk = IN_ATTACK;
	auto* b = reinterpret_cast<std::uint8_t*>(input);
	*reinterpret_cast<std::uint64_t*>(b + k0) &= ~k_atk;
	*reinterpret_cast<std::uint64_t*>(b + k1) &= ~k_atk;
	*reinterpret_cast<std::uint64_t*>(b + k2) &= ~k_atk;
	*reinterpret_cast<std::uint64_t*>(b + k3) &= ~k_atk;
}

void edge_csgo_attack_input(i_csgo_input* input) {
	if (!input)
		return;
	constexpr std::uintptr_t k0 = 0x250, k1 = 0x258, k2 = 0x260, k3 = 0x268;
	constexpr std::uint64_t k_atk = IN_ATTACK;
	auto* b = reinterpret_cast<std::uint8_t*>(input);
	*reinterpret_cast<std::uint64_t*>(b + k0) &= ~k_atk;
	*reinterpret_cast<std::uint64_t*>(b + k2) &= ~k_atk;
	*reinterpret_cast<std::uint64_t*>(b + k3) &= ~k_atk;
	*reinterpret_cast<std::uint64_t*>(b + k1) |= k_atk;
}

bool is_semi_weapon(c_base_player_weapon* weapon) {
	if (!weapon)
		return false;
	const std::uint16_t def = get_def_index(weapon);
	// R8 must HOLD attack to cock — never treat as rising-edge semi.
	if (def == WEAPON_REVOLVER)
		return false;
	// deagle
	if (def == WEAPON_DEAGLE)
		return true;
	// AWP / SSG / autos still need edge? Lefrizzel treats bolt snipers as semi.
	if (def == WEAPON_AWP || def == WEAPON_SSG08)
		return true;
	// common pistols
	if (def == WEAPON_DUAL_BERETTAS || def == WEAPON_FIVE_SEVEN || def == WEAPON_GLOCK
		|| def == WEAPON_P250 || def == WEAPON_HKP2000 || def == WEAPON_TEC9
		|| def == WEAPON_USP_S || def == WEAPON_CZ75)
		return true;

	if (auto* data = weapon->get_weapon_data()) {
		const int t = data->m_weapon_type();
		if (t == WEAPONTYPE_PISTOL)
			return true;
		if (t == WEAPONTYPE_SNIPER_RIFLE) {
			// auto snipers spray — treat as full-auto
			if (def == WEAPON_G3SG1 || def == WEAPON_SCAR20)
				return false;
			return true;
		}
		if (t == WEAPONTYPE_SUBMACHINEGUN || t == WEAPONTYPE_RIFLE || t == WEAPONTYPE_MACHINEGUN)
			return false;
	}
	return false;
}

// Lefrizzel: semi needs rising edge + release next CM; full-auto holds without re-edge.
bool s_semi_need_release = false;
bool s_auto_held = false;
i_csgo_input* s_attack_input = nullptr;

void strip_attack(c_user_cmd* cmd) {
	if (!cmd)
		return;
	cmd->m_button_state.set_button_state(IN_ATTACK, c_in_button_state::IN_BUTTON_UP);
	if (auto* base = cmd->get_base_cmd()) {
		if (auto* pb = base->mutable_buttons_pb()) {
			const std::uint64_t bit = static_cast<std::uint64_t>(IN_ATTACK);
			pb->set_buttonstate1(pb->buttonstate1() & ~bit);
			pb->set_buttonstate2(pb->buttonstate2() & ~bit);
			pb->set_buttonstate3(pb->buttonstate3() & ~bit);
		}
	}
	cmd->pb.set_attack1_start_history_index(-1);
	s_auto_held = false;
}

void set_attack(c_user_cmd* cmd, bool down) {
	if (!cmd)
		return;
	if (!down) {
		strip_attack(cmd);
		clear_csgo_attack_input(s_attack_input);
		s_semi_need_release = false;
		return;
	}

	const bool semi = is_semi_weapon(
		g_ctx && g_ctx->m_local_pawn
			? reinterpret_cast<c_cs_player_pawn*>(g_ctx->m_local_pawn)->get_active_weapon()
			: nullptr);

	if (semi) {
		strip_attack(cmd);
		clear_csgo_attack_input(s_attack_input);
	}

	const bool already_down = s_auto_held;
	cmd->m_button_state.set_button_state(
		IN_ATTACK,
		(semi || !already_down)
			? c_in_button_state::IN_BUTTON_UP_DOWN
			: c_in_button_state::IN_BUTTON_DOWN);

	if (auto* base = cmd->get_base_cmd()) {
		if (auto* pb = base->mutable_buttons_pb()) {
			const std::uint64_t bit = static_cast<std::uint64_t>(IN_ATTACK);
			pb->set_buttonstate1(pb->buttonstate1() | bit);
			if (semi || !already_down)
				pb->set_buttonstate2(pb->buttonstate2() | bit);
		}
	}

	if (semi) {
		edge_csgo_attack_input(s_attack_input);
		s_semi_need_release = true;
	} else {
		s_auto_held = true;
	}

	const int hist_count = cmd->pb.input_history_size();
	if (hist_count > 0)
		cmd->pb.set_attack1_start_history_index(hist_count - 1);
}

void set_attack2(c_user_cmd* cmd, bool down) {
	if (!cmd)
		return;

	cmd->m_button_state.set_button_state(
		IN_ATTACK2,
		down ? c_in_button_state::IN_BUTTON_DOWN : c_in_button_state::IN_BUTTON_UP);

	if (auto* base = cmd->get_base_cmd()) {
		if (auto* pb = base->mutable_buttons_pb()) {
			const std::uint64_t bit = static_cast<std::uint64_t>(IN_ATTACK2);
			if (down) {
				pb->set_buttonstate1(pb->buttonstate1() | bit);
			} else {
				pb->set_buttonstate1(pb->buttonstate1() & ~bit);
				pb->set_buttonstate2(pb->buttonstate2() & ~bit);
				pb->set_buttonstate3(pb->buttonstate3() & ~bit);
			}
		}
	}
}

void apply_auto_stop(c_user_cmd* cmd, c_cs_player_pawn* local, const vec3_t& view, bool allow_air = false) {
	if (!cmd || !local)
		return;

	const bool on_ground = (local->m_flags() & FL_ONGROUND) != 0;
	if (!on_ground && !allow_air)
		return;
	// Never fight rage airstrafe — air stop is a no-op while autostrafe is on.
	if (!on_ground && g_cfg && g_cfg->movement.m_autostrafe)
		return;

	vec3_t vel = local->m_vec_abs_velocity();
	if (g_prediction) {
		if (const auto* pred = g_prediction->get_local_data()) {
			if (pred->m_valid && pred->m_velocity.is_valid())
				vel = pred->m_velocity;
		}
	}
	vel.z = 0.f;
	const float speed = vel.length_2d();

	auto* base = cmd->get_base_cmd();
	if (!base)
		return;

	auto clear_move = [&]() {
		base->set_forwardmove(0.0f);
		base->set_leftmove(0.0f);
		base->set_upmove(0.0f);
		movement_input::set_button(cmd, IN_FORWARD, false);
		movement_input::set_button(cmd, IN_BACK, false);
		movement_input::set_button(cmd, IN_MOVELEFT, false);
		movement_input::set_button(cmd, IN_MOVERIGHT, false);
		if (g_interfaces && g_interfaces->m_csgo_input) {
			g_interfaces->m_csgo_input->forward_move = 0.0f;
			g_interfaces->m_csgo_input->left_move = 0.0f;
		}
	};

	// Already under accuracy threshold — zero wish, never duck-brake.
	if (speed < 1.0f) {
		clear_move();
		return;
	}

	// View-aligned basis.
	const float yaw = view.y * (k_pi / 180.0f);
	const float sy = std::sinf(yaw);
	const float cy = std::cosf(yaw);
	const vec3_t fwd(cy, sy, 0.f);
	const vec3_t right(-sy, cy, 0.f);

	// Max opposite wish on X/Y — Fast Counter-Strafe (strafe-based only, NO IN_DUCK).
	float fmove = -vel.dot(fwd);
	float smove = -vel.dot(right);
	const float len = std::sqrt(fmove * fmove + smove * smove);
	if (len < 1e-4f) {
		clear_move();
		return;
	}

	fmove = (fmove / len) * k_max_wish;
	smove = (smove / len) * k_max_wish;

	base->set_forwardmove(fmove);
	base->set_leftmove(smove);
	base->set_upmove(0.0f);

	if (g_interfaces && g_interfaces->m_csgo_input) {
		g_interfaces->m_csgo_input->forward_move = fmove;
		g_interfaces->m_csgo_input->left_move = smove;
	}

	// Explicitly never engage duck for braking.
	movement_input::set_button(cmd, IN_DUCK, false);

	movement_input::set_button(cmd, IN_FORWARD, fmove > 1.f);
	movement_input::set_button(cmd, IN_BACK, fmove < -1.f);
	movement_input::set_button(cmd, IN_MOVELEFT, smove > 1.f);
	movement_input::set_button(cmd, IN_MOVERIGHT, smove < -1.f);

	// Engine subtick analog (CreateNewSubtickMoveStep) — never protobuf Add.
	const float af = fmove / k_max_wish;
	const float al = smove / k_max_wish;
	if (auto* s0 = alloc_subtick_step(base))
		fill_analog_subtick(s0, 0.0f, af, al);
	if (auto* s1 = alloc_subtick_step(base))
		fill_analog_subtick(s1, 0.45f, af, al);
}

// Weapon-specific speed where move inaccuracy remaps ~off (Valve 0.34 * maxspeed).
float accuracy_speed_threshold(c_cs_weapon_base* weapon, c_cs_weapon_base_v_data* data) {
	float max_spd = 250.f;
	if (weapon) {
		const float w = weapon->get_max_speed();
		if (std::isfinite(w) && w > 1.f)
			max_spd = w;
	}
	if (data) {
		const int mode = weapon_mode_of(weapon);
		const int mi = (mode == 1) ? 1 : 0;
		const firing_float_t& ms = data->m_max_speed();
		if (std::isfinite(ms[mi]) && ms[mi] > 1.f)
			max_spd = ms[mi];
	}
	return (std::max)(15.f, max_spd * k_accuracy_speed_frac);
}

// Predict 2D speed after N ticks of max opposite wish (friction then accelerate).
float predict_speed_after_fast_stop(vec3_t vel, int ticks, float max_speed) {
	vel.z = 0.f;
	float speed = vel.length_2d();
	if (speed < 1.f || ticks <= 0)
		return speed;

	const float max_spd = (std::max)(max_speed, 1.f);
	for (int i = 0; i < ticks; ++i) {
		// Friction (ground).
		if (speed > 0.f) {
			const float control = (speed < k_sv_stopspeed) ? k_sv_stopspeed : speed;
			const float drop = control * k_sv_friction * INTERVAL_PER_TICK;
			const float new_speed = (std::max)(0.f, speed - drop);
			if (speed > 1e-4f)
				vel = vel * (new_speed / speed);
			speed = new_speed;
		}
		if (speed < 1.f) {
			speed = 0.f;
			break;
		}
		// Accelerate opposite velocity at sv_accelerate * maxspeed.
		const vec3_t wishdir = vel * (-1.f / speed);
		const float wishspeed = (std::min)(max_spd, k_max_wish);
		const float currentspeed = vel.dot(wishdir); // negative
		float addspeed = wishspeed - currentspeed;
		if (addspeed <= 0.f)
			continue;
		float accelspeed = k_sv_accelerate * INTERVAL_PER_TICK * max_spd;
		if (accelspeed > addspeed)
			accelspeed = addspeed;
		vel.x += wishdir.x * accelspeed;
		vel.y += wishdir.y * accelspeed;
		speed = vel.length_2d();
	}
	return speed;
}

// CSGO/CS2: move_inac contributes (speed/max)^2 * InaccuracyMove.
__declspec(noinline) void read_weapon_accuracy(c_cs_weapon_base* weapon, float* spread, float* inaccuracy);

// Engine-accurate accuracy prediction at the FIRE subtick.
//
// The server runs, every tick: penalty = target + (penalty - target) * exp(-dt/recovery),
// where target is the stance baseline (stand + (speed/max)^2 * move). Our cmd
// sequences the counter-strafe at subtick 0 and +attack at subtick ~1.0, so by
// the time the shot is sampled the brake has already cut speed AND the penalty
// has decayed one step toward the new (lower) baseline. Modeling that exactly
// lets us fire the first physically-valid tick instead of waiting for the live
// read to catch up 1-2 ticks later.
//
// `brake_ticks` > 0 simulates that many ticks of Fast Counter-Strafe before the
// shot; 0 still applies one decay step (friction-only), because firing at
// subtick ~1.0 always benefits from the current tick's decay.
void predict_fire_bloom(
	c_cs_weapon_base* weapon,
	c_cs_weapon_base_v_data* data,
	vec3_t vel,
	int brake_ticks,
	float* out_spread,
	float* out_inaccuracy)
{
	float spread = 0.f, inac_live = 0.f;
	read_weapon_accuracy(weapon, &spread, &inac_live);
	*out_spread = spread;
	*out_inaccuracy = inac_live;
	if (!weapon || !data)
		return;

	const int mode = weapon_mode_of(weapon);
	const int mi = (mode == 1) ? 1 : 0;
	float max_spd = weapon->get_max_speed();
	const firing_float_t& ms = data->m_max_speed();
	if (std::isfinite(ms[mi]) && ms[mi] > 1.f)
		max_spd = ms[mi];
	if (!(max_spd > 1.f))
		max_spd = 250.f;

	float move_tbl = 0.f;
	const firing_float_t& im = data->m_inaccuracy_move();
	if (std::isfinite(im[mi]) && im[mi] >= 0.f)
		move_tbl = im[mi];

	float stand_tbl = 0.f;
	const firing_float_t& is = data->m_inaccuracy_stand();
	if (std::isfinite(is[mi]) && is[mi] >= 0.f)
		stand_tbl = is[mi];

	float recovery = data->m_recovery_time_stand();
	if (!std::isfinite(recovery) || recovery <= 0.001f)
		recovery = 0.3f;

	// Start from the exact networked penalty; fall back to the live total read.
	float penalty = weapon->m_accuracy_penalty();
	if (!std::isfinite(penalty) || penalty < 0.f || penalty > 1.f)
		penalty = inac_live;
	// Never model below what the sig-read reports as baseline composition —
	// the live read may include components (jump/fire) the tables don't.
	const float extra = (std::max)(0.f, inac_live - penalty);

	const float decay = std::exp(-INTERVAL_PER_TICK / recovery);

	vel.z = 0.f;
	const int steps = (std::max)(1, brake_ticks);
	float speed = vel.length_2d();
	for (int k = 0; k < steps; ++k) {
		if (brake_ticks > 0)
			speed = predict_speed_after_fast_stop(vel, 1, max_spd);
		else if (speed > 1.f) {
			// Friction only — wish is zeroed under threshold.
			const float control = (speed < k_sv_stopspeed) ? k_sv_stopspeed : speed;
			speed = (std::max)(0.f, speed - control * k_sv_friction * INTERVAL_PER_TICK);
		}
		if (brake_ticks > 0 && speed > 1e-4f) {
			const float cur = vel.length_2d();
			if (cur > 1e-4f)
				vel = vel * (speed / cur);
		}

		const float t = std::clamp(speed / max_spd, 0.f, 1.f);
		const float target = stand_tbl + t * t * move_tbl;
		penalty = target + (penalty - target) * decay;
	}

	float predicted = (std::max)(0.f, penalty) + extra;
	if (!std::isfinite(predicted) || predicted > 1.f)
		predicted = inac_live;
	// Never claim better accuracy than the physical floor for this stance.
	predicted = (std::max)(predicted, stand_tbl * 0.95f);
	*out_inaccuracy = predicted;
}

int local_tick_base() {
	if (g_prediction) {
		if (const auto* pred = g_prediction->get_local_data()) {
			if (pred->m_valid && pred->m_tick_base > 0)
				return pred->m_tick_base;
		}
	}
	if (g_ctx) {
		if (auto* controller = reinterpret_cast<c_cs_player_controller*>(g_ctx->m_local_controller))
			return static_cast<int>(controller->m_tick_base());
	}
	return 0;
}

bool is_revolver_weapon(c_base_player_weapon* weapon) {
	return weapon && get_def_index(weapon) == WEAPON_REVOLVER;
}

bool revolver_fire_ready(c_cs_weapon_base* weapon, int tick_base) {
	if (!weapon)
		return false;
	const int ready = weapon->m_postpone_fire_ready_ticks();
	// 0 / negative means unset — treat as not cocked yet.
	if (ready <= 0)
		return false;
	return ready <= tick_base;
}

// Keeps R8 cocked. Returns true when hammer is ready to fire this tick.
bool run_auto_revolver(c_user_cmd* cmd, c_cs_weapon_base* weapon, bool want_fire_now) {
	if (!cmd || !weapon || !is_revolver_weapon(weapon))
		return true;

	const int tick = local_tick_base();
	const bool ready = revolver_fire_ready(weapon, tick);

	if (!ready) {
		set_attack(cmd, true); // cock
		return false;
	}

	if (want_fire_now) {
		set_attack(cmd, true);
		return true;
	}

	set_attack(cmd, false);
	return true;
}

bool can_fire(c_cs_player_pawn* local, c_base_player_weapon* weapon) {
	if (!local || !weapon)
		return false;
	if (local->m_health() <= 0 || local->is_throwing())
		return false;
	if (local->m_move_type() == MOVETYPE_LADDER || local->m_move_type() == MOVETYPE_NOCLIP)
		return false;
	if (weapon->m_clip1() <= 0 || weapon->m_in_reload())
		return false;

	// Prefer prediction tickbase (matches shoot tick); controller can lag 1.
	const int tick_base = local_tick_base();
	const int next = weapon->m_next_primary_attack();

	// next stuck / wrong SCHEMA / desync must NOT soft-lock rage forever.
	// Normal cycle gap is a few ticks; >64 or negative gap = ignore gate.
	if (next > 0 && tick_base > 0) {
		const int delta = next - tick_base;
		if (delta > 0 && delta <= 64 && next > tick_base + 1)
			return false;
	}

	if (is_revolver_weapon(weapon)) {
		auto* cs_wpn = static_cast<c_cs_weapon_base*>(weapon);
		if (!revolver_fire_ready(cs_wpn, tick_base > 0 ? tick_base : local_tick_base()))
			return false;
	}
	return true;
}

void write_aim_angles(i_csgo_input* input, c_user_cmd* cmd, vec3_t angles, bool silent, const vec3_t* shoot_pos = nullptr) {
	if (!cmd)
		return;
	angles = normalize_angles(angles);
	if (!angles.is_valid())
		return;

	const int hist_count = cmd->pb.input_history_size();
	const int hist_idx = hist_count > 0 ? hist_count - 1 : -1;
	if (hist_idx >= 0)
		cmd->pb.set_attack1_start_history_index(hist_idx);

	// Only stamp the attack history tip — rewriting every hist entry desyncs aim.
	if (hist_idx >= 0) {
		auto* hist = cmd->pb.mutable_input_history(hist_idx);
		if (hist) {
			if (auto* va = hist->mutable_view_angles()) {
				va->set_x(angles.x);
				va->set_y(angles.y);
				va->set_z(0.f);
			}
			// Server fire origin must match the eye we solved angles from.
			if (shoot_pos && shoot_pos->is_valid() && !shoot_pos->is_zero()) {
				if (auto* sp = hist->mutable_shoot_position()) {
					sp->set_x(shoot_pos->x);
					sp->set_y(shoot_pos->y);
					sp->set_z(shoot_pos->z);
					sp->set_w(0.f);
				}
			}
			if (g_prediction) {
				if (const auto* pred = g_prediction->get_local_data()) {
					if (pred->m_valid && pred->m_shoot_tick > 0)
						hist->set_player_tick_count(pred->m_shoot_tick);
					if (pred->m_valid)
						hist->set_player_tick_fraction(pred->m_player_tick_fraction);
				}
			}
		}
	}

	if (auto* base = cmd->get_base_cmd()) {
		if (auto* va = base->mutable_viewangles()) {
			va->set_x(angles.x);
			va->set_y(angles.y);
			va->set_z(0.f);
		}
	}

	if (!silent && input) {
		vec3_t out = angles;
		input->set_view_angles(out);
	}
}

void process_backtrack(c_user_cmd* cmd, lag_record_t* record) {
	if (!cmd || !record)
		return;

	// Target pose tick only — never rewrite local client_tick (that desyncs pred/render).
	const int tick_shoot = TIME_TO_TICKS(record->m_simulation_time);
	const auto* pred = g_prediction ? g_prediction->get_local_data() : nullptr;

	const int hist_count = cmd->pb.input_history_size();
	if (hist_count <= 0)
		return;

	cmd->pb.set_attack1_start_history_index(hist_count - 1);

	auto* hist = cmd->pb.mutable_input_history(hist_count - 1);
	if (!hist)
		return;

	if (auto* cl = hist->mutable_cl_interp())
		cl->set_frac(0.f);

	if (auto* i0 = hist->mutable_sv_interp0()) {
		i0->set_src_tick(tick_shoot);
		i0->set_dst_tick(tick_shoot);
		i0->set_frac(0.f);
	}
	if (auto* i1 = hist->mutable_sv_interp1()) {
		i1->set_src_tick(tick_shoot);
		i1->set_dst_tick(tick_shoot);
		i1->set_frac(0.f);
	}

	hist->set_render_tick_count(tick_shoot);
	hist->set_render_tick_fraction(0.f);

	if (pred && pred->m_valid) {
		hist->set_player_tick_count(pred->m_shoot_tick > 0 ? pred->m_shoot_tick : pred->m_tick_base);
		hist->set_player_tick_fraction(pred->m_player_tick_fraction);
	} else if (g_ctx) {
		if (auto* controller = reinterpret_cast<c_cs_player_controller*>(g_ctx->m_local_controller))
			hist->set_player_tick_count(static_cast<int>(controller->m_tick_base()));
	}
}

struct aim_point_t {
	vec3_t pos{};
	int hitbox_bit = 0;
	int hitbox_id = HITBOX_HEAD;
	float damage = 0.f;
	bool extrapolated = false;
	int extrap_ticks = 0;
	vec3_t extrap_delta{};
};

struct target_t {
	c_cs_player_pawn* pawn = nullptr;
	int controller_index = 0;
	lag_record_t* record = nullptr;
	aim_point_t best{};
	float distance = 0.f;
	float fov = 0.f;
	int health = 0;
};

c_hitbox* get_hitbox_safe(c_cs_player_pawn* pawn, int hitbox_id);
int bone_index_from_hitbox(c_cs_player_pawn* pawn, c_hitbox* hb);
vec3_t point_from_record(lag_record_t* rec, int bone);
vec3_t point_from_pawn(c_cs_player_pawn* pawn, int bone);
int bone_for_hitbox_bit(int bit);
bool bone_matrix_for_index(c_cs_player_pawn* pawn, lag_record_t* record, int bone_index, matrix3x4_t& out);

// Bone indices used by player models (same family as aim/ESP).
int bone_for_hitbox_bit(int bit) {
	switch (bit) {
	case c_config::ragebot_t::hb_head: return 6;
	case c_config::ragebot_t::hb_neck: return 5;
	case c_config::ragebot_t::hb_chest: return 4;
	case c_config::ragebot_t::hb_stomach: return 3;
	case c_config::ragebot_t::hb_pelvis: return 2;
	case c_config::ragebot_t::hb_arms: return 8;
	case c_config::ragebot_t::hb_legs: return 23;
	default: return 6;
	}
}

void append_hitbox_ids_for_bit(int bit, std::vector<int>& out) {
	switch (bit) {
	case c_config::ragebot_t::hb_head:
		out.push_back(HITBOX_HEAD);
		break;
	case c_config::ragebot_t::hb_neck:
		out.push_back(HITBOX_NECK);
		break;
	case c_config::ragebot_t::hb_chest:
		// Primary chest first; extras are center-only (no multipoint spam).
		out.push_back(HITBOX_CHEST);
		out.push_back(HITBOX_UPPER_CHEST);
		out.push_back(HITBOX_LOWER_CHEST);
		break;
	case c_config::ragebot_t::hb_stomach:
		out.push_back(HITBOX_STOMACH);
		break;
	case c_config::ragebot_t::hb_pelvis:
		out.push_back(HITBOX_PELVIS);
		break;
	case c_config::ragebot_t::hb_arms:
		out.push_back(HITBOX_RIGHT_UPPER_ARM);
		out.push_back(HITBOX_LEFT_UPPER_ARM);
		out.push_back(HITBOX_RIGHT_FOREARM);
		out.push_back(HITBOX_LEFT_FOREARM);
		break;
	case c_config::ragebot_t::hb_legs:
		out.push_back(HITBOX_RIGHT_THIGH);
		out.push_back(HITBOX_LEFT_THIGH);
		out.push_back(HITBOX_RIGHT_CALF);
		out.push_back(HITBOX_LEFT_CALF);
		break;
	default:
		break;
	}
}

float estimate_max_damage(c_cs_weapon_base_v_data* data, float distance) {
	if (!data || distance <= 1.f)
		return 0.f;
	float dmg = static_cast<float>(data->m_damage()) * data->m_headshot_multiplier();
	dmg *= std::pow(data->m_range_modifier(), distance / 500.f);
	return dmg;
}

matrix3x4_t bone_to_matrix(const c_bone_data& bone) {
	c_transform tf;
	tf.m_position = vec4_t(bone.m_pos.x, bone.m_pos.y, bone.m_pos.z, 0.f);
	tf.m_rotation = bone.m_rot;
	matrix3x4_t mat{};
	tf.to_matrix(mat);
	return mat;
}

c_bone_data* live_bones(c_cs_player_pawn* pawn) {
	if (!pawn)
		return nullptr;
	auto* node = pawn->m_scene_node();
	if (!node)
		return nullptr;
	auto* skeleton = node->get_skeleton_instance();
	if (!skeleton)
		return nullptr;
	return skeleton->m_model_state().get_bone_data();
}

bool bone_matrix_for_index(c_cs_player_pawn* pawn, lag_record_t* record, int bone_index, matrix3x4_t& out) {
	if (bone_index < 0)
		return false;

	if (record && bone_index < record->m_bone_count) {
		out = bone_to_matrix(record->m_bones[bone_index]);
		return true;
	}

	if (auto* bones = live_bones(pawn)) {
		out = bone_to_matrix(bones[bone_index]);
		return true;
	}
	return false;
}

vec3_t point_from_record(lag_record_t* rec, int bone) {
	if (!rec)
		return {};
	if (bone == 6 && rec->m_head.is_valid() && !rec->m_head.is_zero())
		return rec->m_head;
	if (bone >= 0 && bone < rec->m_bone_count) {
		const auto& p = rec->m_bones[bone].m_pos;
		if (p.is_valid() && !p.is_zero())
			return p;
	}
	return rec->m_origin + vec3_t(0.f, 0.f, 64.f);
}

vec3_t point_from_pawn(c_cs_player_pawn* pawn, int bone) {
	if (!pawn || bone < 0)
		return {};
	if (auto* bones = live_bones(pawn)) {
		if (bone < 128) {
			const vec3_t p = bones[bone].m_pos;
			if (p.is_valid() && !p.is_zero())
				return p;
		}
	}
	const vec3_t p = pawn->get_bone_position(bone);
	if (p.is_valid() && !p.is_zero())
		return p;
	if (auto* node = pawn->m_scene_node()) {
		const vec3_t o = node->m_abs_origin();
		if (o.is_valid())
			return o + vec3_t(0.f, 0.f, bone == 6 ? 72.f : 64.f);
	}
	return {};
}

float dist_point_to_segment_sqr(const vec3_t& p, const vec3_t& a, const vec3_t& b) {
	const vec3_t ab = b - a;
	const float ab_len_sqr = ab.length_sqr();
	float t = 0.f;
	if (ab_len_sqr > 1e-8f)
		t = std::clamp((p - a).dot(ab) / ab_len_sqr, 0.f, 1.f);
	const vec3_t closest = a + ab * t;
	return (p - closest).length_sqr();
}

// Analytic ray (eye + t*dir, t>=0, dir unit) vs capsule (segment a→b, radius).
bool ray_hits_capsule(
	const vec3_t& eye,
	const vec3_t& dir,
	const vec3_t& cap_a,
	const vec3_t& cap_b,
	float radius)
{
	if (radius < 0.25f)
		radius = 0.25f;
	const float r_sqr = radius * radius;

	const vec3_t axis = cap_b - cap_a;
	const float axis_len = axis.length();

	// Sphere (degenerate capsule)
	if (axis_len < 1e-3f) {
		const vec3_t w = eye - cap_a;
		const float b = w.dot(dir);
		const float c = w.length_sqr() - r_sqr;
		const float disc = b * b - c;
		if (disc < 0.f)
			return false;
		const float s = std::sqrt(disc);
		float t = -b - s;
		if (t < 0.f)
			t = -b + s;
		return t >= 0.f && t <= 8192.f;
	}

	const vec3_t u = dir;
	const vec3_t v = axis;
	const vec3_t w = eye - cap_a;
	const float a = u.dot(u);
	const float b = u.dot(v);
	const float c = v.dot(v);
	const float d = u.dot(w);
	const float e = v.dot(w);
	const float denom = a * c - b * b;

	float sc = 0.f;
	float tc = 0.f;
	if (denom < 1e-8f) {
		sc = 0.f;
		tc = (c > 1e-8f) ? (e / c) : 0.f;
	} else {
		sc = (b * e - c * d) / denom;
		tc = (a * e - b * d) / denom;
	}
	tc = std::clamp(tc, 0.f, 1.f);
	if (sc < 0.f)
		sc = 0.f;

	const vec3_t on_ray = eye + u * sc;
	const vec3_t on_seg = cap_a + v * tc;
	if ((on_ray - on_seg).length_sqr() > r_sqr)
		return false;
	return sc >= 0.f && sc <= 8192.f;
}

bool hitbox_capsule_world(
	c_cs_player_pawn* pawn,
	lag_record_t* record,
	int hitbox_id,
	vec3_t& out_a,
	vec3_t& out_b,
	float& out_radius)
{
	out_a = {};
	out_b = {};
	out_radius = k_hc_hit_radius;

	c_hitbox* hb = get_hitbox_safe(pawn, hitbox_id);
	if (!hb) {
		const int bone = (hitbox_id == HITBOX_HEAD) ? 6 : bone_for_hitbox_bit(c_config::ragebot_t::hb_chest);
		out_a = record ? point_from_record(record, bone) : point_from_pawn(pawn, bone);
		out_b = out_a;
		out_radius = (hitbox_id == HITBOX_HEAD) ? 4.5f : 8.f;
		return out_a.is_valid() && !out_a.is_zero();
	}

	const int bone_index = bone_index_from_hitbox(pawn, hb);
	matrix3x4_t matrix{};
	if (!bone_matrix_for_index(pawn, record, bone_index, matrix)) {
		out_a = record ? point_from_record(record, bone_index > 0 ? bone_index : 6)
			: point_from_pawn(pawn, bone_index > 0 ? bone_index : 6);
		out_b = out_a;
		out_radius = (std::max)(2.f, hb->m_shape_radius());
		return out_a.is_valid() && !out_a.is_zero();
	}

	out_a = matrix.transform(hb->m_vec_min());
	out_b = matrix.transform(hb->m_vec_max());
	out_radius = (std::max)(1.f, hb->m_shape_radius());
	return out_a.is_valid() && out_b.is_valid();
}

__declspec(noinline) void read_weapon_accuracy(c_cs_weapon_base* weapon, float* spread, float* inaccuracy) {
	if (!spread || !inaccuracy)
		return;
	*spread = 0.f;
	*inaccuracy = 0.f;
	if (!weapon)
		return;
	*spread = weapon->get_spread();
	*inaccuracy = weapon->get_inaccuracy();
}

bool hitchance_ok(
	c_cs_player_pawn* local,
	c_cs_player_pawn* target,
	lag_record_t* record,
	c_cs_weapon_base* weapon,
	c_cs_weapon_base_v_data* data,
	const vec3_t& eye,
	const aim_point_t& aim,
	const vec3_t& cmd_angles,
	int needed_pct,
	float override_spread = -1.f,
	float override_inaccuracy = -1.f,
	float* out_pct = nullptr)
{
	if (out_pct)
		*out_pct = 0.f;
	if (needed_pct <= 0) {
		if (out_pct)
			*out_pct = 100.f;
		return true;
	}
	if (!local || !weapon || !data || !target)
		return false;

	float spread = 0.f;
	float inaccuracy = 0.f;
	if (override_spread >= 0.f && override_inaccuracy >= 0.f) {
		spread = override_spread;
		inaccuracy = override_inaccuracy;
	} else {
		read_weapon_accuracy(weapon, &spread, &inaccuracy);
	}

	// Fail closed on garbage bloom — don't invent "always hit".
	if (!std::isfinite(spread) || !std::isfinite(inaccuracy)
		|| spread < 0.f || inaccuracy < 0.f
		|| spread > 0.5f || inaccuracy > 1.f)
		return false;

	const float dist = (aim.pos - eye).length();
	if (dist < 1.f) {
		if (out_pct)
			*out_pct = 100.f;
		return true;
	}
	if (dist > data->m_range())
		return false;

	vec3_t cap_a{}, cap_b{};
	float cap_r = k_hc_hit_radius;
	if (!hitbox_capsule_world(target, record, aim.hitbox_id, cap_a, cap_b, cap_r)) {
		cap_a = aim.pos;
		cap_b = aim.pos;
		cap_r = (aim.hitbox_id == HITBOX_HEAD) ? 5.f : 9.f;
	}
	if (aim.extrapolated) {
		cap_a = cap_a + aim.extrap_delta;
		cap_b = cap_b + aim.extrap_delta;
		cap_r *= 1.06f;
	}
	if (cap_r < 1.f)
		cap_r = 1.f;

	const int wtype = data->m_weapon_type();
	const int def = get_def_index(weapon);
	const bool is_scout = (def == WEAPON_SSG08);
	const bool is_precision = (wtype == WEAPONTYPE_PISTOL) || is_scout
		|| (def == WEAPON_AWP) || (def == WEAPON_DEAGLE) || (def == WEAPON_REVOLVER);

	const float bloom = inaccuracy + spread;
	const float ang_size = cap_r / dist;
	// Only reject physically impossible cones — the old *5/*7 gate + needed>=50
	// was failing standing rifles at mid range (~50% HC locked out).
	if (bloom > ang_size * 18.f)
		return false;
	if (bloom <= ang_size * 0.75f) {
		if (out_pct)
			*out_pct = 100.f;
		return true;
	}

	const float recoil_index = get_recoil_index(weapon);
	const int num_bullets = (std::max)(1, data->m_bullets());
	const int wpn_mode = weapon_mode_of(weapon);

	const vec3_t punch = get_aim_punch(local);
	vec3_t fire_ang = normalize_angles(cmd_angles + punch);

	vec3_t fwd, right, up;
	angle_vectors(fire_ang, &fwd, &right, &up);

	if (wtype == WEAPONTYPE_SNIPER_RIFLE) {
		int zoom = 0;
		__try { zoom = reinterpret_cast<c_cs_weapon_base*>(weapon)->m_zoom_level(); }
		__except (EXCEPTION_EXECUTE_HANDLER) { zoom = 0; }
		if (zoom > 0)
			cap_r *= is_scout ? 0.95f : 0.97f;
		else
			cap_r *= 0.90f;
	}

	// Monte Carlo over Valve seeds inside the predicted spread cone.
	const int samples = is_precision ? k_hc_seeds_precision : k_hc_seeds;
	const int needed = (std::max)(1, (needed_pct * samples + 99) / 100);
	int hits = 0;
	for (int i = 0; i < samples; ++i) {
		const auto sp = calculate_spread(i + 1, inaccuracy, spread, recoil_index, def, wpn_mode, num_bullets);
		vec3_t dir{
			fwd.x - right.x * sp.x + up.x * sp.y,
			fwd.y - right.y * sp.x + up.y * sp.y,
			fwd.z - right.z * sp.x + up.z * sp.y
		};
		const float len = dir.length();
		if (len < 1e-5f)
			continue;
		dir = dir * (1.f / len);
		if (ray_hits_capsule(eye, dir, cap_a, cap_b, cap_r)) {
			++hits;
			if (hits >= needed) {
				if (out_pct)
					*out_pct = 100.f * static_cast<float>(hits) / static_cast<float>(samples);
				return true;
			}
		}
		if (hits + (samples - i - 1) < needed) {
			if (out_pct)
				*out_pct = 100.f * static_cast<float>(hits) / static_cast<float>(samples);
			return false;
		}
	}
	if (out_pct)
		*out_pct = 100.f * static_cast<float>(hits) / static_cast<float>(samples);
	return hits >= needed;
}

// Stamp +attack on a late subtick so brake (when≈0) is processed first on the same tick.
void stamp_attack_subtick(c_user_cmd* cmd, float when = 0.99f) {
	if (!cmd)
		return;
	auto* base = cmd->get_base_cmd();
	if (!base)
		return;
	if (auto* step = alloc_subtick_step(base))
		fill_button_subtick(step, static_cast<std::uint64_t>(IN_ATTACK), true, when);
}

void push_unique_point(std::vector<vec3_t>& points, const vec3_t& p) {
	if (!p.is_valid() || p.is_zero())
		return;
	for (const auto& e : points) {
		if ((e - p).length_sqr() < 0.25f)
			return;
	}
	points.push_back(p);
}

void collect_sphere_offsets(float radius, int count, std::vector<vec3_t>& out) {
	if (radius <= 0.f || count <= 0)
		return;

	static const auto k_unit = []() {
		std::array<vec3_t, k_sphere_points> pts{};
		const float phi = k_pi * (3.0f - std::sqrt(5.0f));
		for (int i = 0; i < k_sphere_points; ++i) {
			const float y = 1.f - (static_cast<float>(i) / static_cast<float>(k_sphere_points - 1)) * 2.f;
			const float radius_at_y = std::sqrt((std::max)(0.f, 1.f - y * y));
			const float theta = phi * static_cast<float>(i);
			pts[static_cast<std::size_t>(i)] = {
				std::cos(theta) * radius_at_y, y, std::sin(theta) * radius_at_y
			};
		}
		return pts;
	}();

	const int n = (std::min)(count, k_sphere_points);
	out.reserve(out.size() + static_cast<std::size_t>(n));
	for (int i = 0; i < n; ++i)
		out.push_back(k_unit[static_cast<std::size_t>(i)] * radius);
}

c_hitbox* get_hitbox_safe(c_cs_player_pawn* pawn, int hitbox_id) {
	if (!pawn || !memsafe::valid_entity(pawn) || hitbox_id < 0)
		return nullptr;
	if (auto* set = pawn->get_hitbox_set(0))
		return set->get_hitbox(hitbox_id);
	return nullptr;
}

int bone_index_from_hitbox(c_cs_player_pawn* pawn, c_hitbox* hb) {
	if (!pawn || !hb)
		return -1;
	int bone_index = -1;
	if (const char* name = hb->m_bone_name())
		bone_index = pawn->get_bone_index(name);
	return bone_index;
}

// Clamp a local-space point onto the capsule (segment min→max, radius*scale).
vec3_t clamp_local_to_capsule(const vec3_t& p, const vec3_t& mn, const vec3_t& mx, float radius) {
	const vec3_t axis = mx - mn;
	const float axis_len_sqr = axis.length_sqr();
	float t = 0.f;
	if (axis_len_sqr > 1e-8f)
		t = std::clamp((p - mn).dot(axis) / axis_len_sqr, 0.f, 1.f);
	const vec3_t on_axis = mn + axis * t;
	vec3_t lat = p - on_axis;
	const float lat_len = lat.length();
	if (lat_len > radius && lat_len > 1e-6f)
		lat = lat * (radius / lat_len);
	else if (lat_len <= 1e-6f)
		lat = {};
	return on_axis + lat;
}

bool point_above_feet(c_cs_player_pawn* pawn, const vec3_t& world) {
	if (!pawn || !world.is_valid())
		return false;
	float floor_z = world.z - 64.f;
	if (auto* node = pawn->m_scene_node()) {
		const vec3_t o = node->m_abs_origin();
		if (o.is_valid())
			floor_z = o.z - 2.f;
	}
	return world.z >= floor_z;
}

void collect_points_for_hitbox(
	c_cs_player_pawn* pawn,
	lag_record_t* record,
	int hitbox_id,
	bool want_multipoint,
	float scale_pct,
	const vec3_t& eye,
	std::vector<vec3_t>& out)
{
	if (!pawn)
		return;

	c_hitbox* hb = get_hitbox_safe(pawn, hitbox_id);
	if (!hb) {
		const int bone = (hitbox_id == HITBOX_HEAD) ? 6 : bone_for_hitbox_bit(c_config::ragebot_t::hb_chest);
		vec3_t p = record ? point_from_record(record, bone) : point_from_pawn(pawn, bone);
		if (point_above_feet(pawn, p))
			push_unique_point(out, p);
		return;
	}

	const int bone_index = bone_index_from_hitbox(pawn, hb);
	matrix3x4_t matrix{};
	if (!bone_matrix_for_index(pawn, record, bone_index, matrix)) {
		vec3_t p = record ? point_from_record(record, bone_index > 0 ? bone_index : 6)
			: point_from_pawn(pawn, bone_index > 0 ? bone_index : 6);
		if (point_above_feet(pawn, p))
			push_unique_point(out, p);
		return;
	}

	const vec3_t mn = hb->m_vec_min();
	const vec3_t mx = hb->m_vec_max();
	const float studio_r = (std::max)(1.f, hb->m_shape_radius());
	// point_scale is % of capsule radius — never exceed studio geometry.
	const float scale = std::clamp(scale_pct, 1.f, 100.f) / 100.f;
	const float r = studio_r * scale;

	const vec3_t center_local = (mn + mx) * 0.5f;
	const vec3_t center_w = matrix.transform(center_local);
	if (point_above_feet(pawn, center_w))
		push_unique_point(out, center_w);

	if (!want_multipoint || r < 0.35f)
		return;

	auto push_clamped = [&](const vec3_t& local) {
		const vec3_t clamped = clamp_local_to_capsule(local, mn, mx, r);
		const vec3_t w = matrix.transform(clamped);
		if (!point_above_feet(pawn, w))
			return;
		// Reject points that still sit outside studio radius (numerical / feet sink).
		const vec3_t wa = matrix.transform(mn);
		const vec3_t wb = matrix.transform(mx);
		if (dist_point_to_segment_sqr(w, wa, wb) > (studio_r * studio_r) + 0.5f)
			return;
		push_unique_point(out, w);
	};

	const vec3_t axis = mx - mn;
	const float axis_len = axis.length();
	const bool is_sphere = axis_len < 1.f;
	const bool is_leg = (hitbox_id == HITBOX_RIGHT_THIGH || hitbox_id == HITBOX_LEFT_THIGH
		|| hitbox_id == HITBOX_RIGHT_CALF || hitbox_id == HITBOX_LEFT_CALF
		|| hitbox_id == HITBOX_RIGHT_FOOT || hitbox_id == HITBOX_LEFT_FOOT);

	// Direction from hitbox center toward our eye (in bone local space via inverse of basis).
	vec3_t to_eye_w = eye - center_w;
	if (!to_eye_w.is_valid() || to_eye_w.length_sqr() < 1.f)
		to_eye_w = { 0.f, 0.f, 1.f };
	else
		to_eye_w = to_eye_w.normalize();

	// Approximate world→local using bone axes from the matrix columns.
	const vec3_t bone_r{ matrix[0][0], matrix[1][0], matrix[2][0] };
	const vec3_t bone_f{ matrix[0][1], matrix[1][1], matrix[2][1] };
	const vec3_t bone_u{ matrix[0][2], matrix[1][2], matrix[2][2] };
	vec3_t to_eye_l{
		to_eye_w.dot(bone_r),
		to_eye_w.dot(bone_f),
		to_eye_w.dot(bone_u)
	};
	if (to_eye_l.length_sqr() > 1e-6f)
		to_eye_l = to_eye_l.normalize();
	else
		to_eye_l = { 0.f, 0.f, 1.f };

	if (hitbox_id == HITBOX_HEAD || is_sphere) {
		// Eye-facing first (peek / crouch-adjacent heads), then laterals + top.
		push_clamped(center_local + to_eye_l * (r * 0.90f));

		// World-space laterals help when two heads face opposite and only side is exposed.
		const vec3_t up_w{ 0.f, 0.f, 1.f };
		vec3_t side_w{
			to_eye_w.y * up_w.z - to_eye_w.z * up_w.y,
			to_eye_w.z * up_w.x - to_eye_w.x * up_w.z,
			to_eye_w.x * up_w.y - to_eye_w.y * up_w.x
		};
		if (side_w.length_sqr() > 1e-6f) {
			side_w = side_w.normalize();
			const vec3_t side_l{
				side_w.dot(bone_r),
				side_w.dot(bone_f),
				side_w.dot(bone_u)
			};
			push_clamped(center_local + side_l * (r * 0.88f));
			push_clamped(center_local - side_l * (r * 0.88f));
		}
		push_clamped(center_local + vec3_t{ 0.f, 0.f, r * 0.85f }); // top
		push_clamped(center_local - to_eye_l * (r * 0.55f)); // far side (thin cover)

		std::vector<vec3_t> offsets;
		collect_sphere_offsets(r * 0.80f, k_sphere_points, offsets);
		for (const auto& o : offsets) {
			if (static_cast<int>(out.size()) >= k_max_points_per_hb)
				break;
			push_clamped(center_local + o);
		}
		return;
	}

	// Capsule: prioritize the spoke facing the shooter, then a mid ring.
	vec3_t ax = axis * (1.f / axis_len);
	vec3_t ref = (std::fabs(ax.z) < 0.9f) ? vec3_t{ 0.f, 0.f, 1.f } : vec3_t{ 1.f, 0.f, 0.f };
	vec3_t u{
		ax.y * ref.z - ax.z * ref.y,
		ax.z * ref.x - ax.x * ref.z,
		ax.x * ref.y - ax.y * ref.x
	};
	const float ul = u.length();
	if (ul < 1e-5f)
		return;
	u = u * (1.f / ul);
	const vec3_t v{
		ax.y * u.z - ax.z * u.y,
		ax.z * u.x - ax.x * u.z,
		ax.x * u.y - ax.y * u.x
	};

	const float t_mid = is_leg ? 0.45f : 0.50f;
	const float lat = r * (is_leg ? 0.70f : 0.88f);
	const vec3_t on_axis = mn + axis * t_mid;

	// Project eye dir onto capsule plane and push that spoke first.
	vec3_t planar = to_eye_l - ax * to_eye_l.dot(ax);
	if (planar.length_sqr() > 1e-6f) {
		planar = planar.normalize();
		push_clamped(on_axis + planar * lat);
	}

	const int spokes = 6;
	for (int s = 0; s < spokes; ++s) {
		if (static_cast<int>(out.size()) >= k_max_points_per_hb)
			break;
		const float theta = 2.f * k_pi * static_cast<float>(s) / static_cast<float>(spokes);
		const vec3_t local = on_axis
			+ u * (std::cos(theta) * lat)
			+ v * (std::sin(theta) * lat);
		push_clamped(local);
	}

	// Extra chest height samples help crouch peeks through overlapping bodies.
	if (!is_leg && static_cast<int>(out.size()) < k_max_points_per_hb) {
		const vec3_t high = mn + axis * 0.75f;
		if (planar.length_sqr() > 1e-6f)
			push_clamped(high + planar * (lat * 0.75f));
	}
}

bool scan_target_points(
	c_cs_player_pawn* local,
	c_cs_player_pawn* pawn,
	lag_record_t* record,
	const c_config::ragebot_t::weapon_t& wpn,
	c_cs_weapon_base_v_data* data,
	const vec3_t& eye,
	int min_damage,
	int target_hp,
	aim_point_t& out,
	const vec3_t& world_shift = {})
{
	out = {};
	if (!pawn || !data || !local || !g_autowall)
		return false;

	const int mask = wpn.m_hitboxes ? wpn.m_hitboxes : c_config::ragebot_t::hb_head;
	const int mp_mask = wpn.m_multipoints & mask;
	float best_dmg = -1.f;
	const float lethal = static_cast<float>((std::max)(1, target_hp));

	static const int k_bits[] = {
		c_config::ragebot_t::hb_head,
		c_config::ragebot_t::hb_neck,
		c_config::ragebot_t::hb_chest,
		c_config::ragebot_t::hb_stomach,
		c_config::ragebot_t::hb_pelvis,
		c_config::ragebot_t::hb_arms,
		c_config::ragebot_t::hb_legs,
	};

	// NEVER apply record bones onto the live skeleton — that desyncs the rendered
	// model / ESP / radar (players in the floor while server still has real pose).
	// Multipoints already read from record->m_bones via bone_matrix_for_index.
	(void)record;

	thread_local std::vector<vec3_t> points;
	points.clear();
	points.reserve(32);

	for (int bit : k_bits) {
		if (!(mask & bit))
			continue;

		const bool want_mp = (mp_mask & bit) != 0;
		std::vector<int> hitbox_ids;
		append_hitbox_ids_for_bit(bit, hitbox_ids);
		if (hitbox_ids.empty())
			hitbox_ids.push_back(HITBOX_HEAD);

		bool bit_done = false;
		int pen_tests = 0;
		for (int hid : hitbox_ids) {
			if (pen_tests >= k_max_pen_tests)
				break;
			points.clear();
			const bool mp_this = want_mp
				&& hid != HITBOX_UPPER_CHEST
				&& hid != HITBOX_LOWER_CHEST
				&& hid != HITBOX_RIGHT_FOREARM
				&& hid != HITBOX_LEFT_FOREARM
				&& hid != HITBOX_RIGHT_CALF
				&& hid != HITBOX_LEFT_CALF;
			collect_points_for_hitbox(pawn, record, hid, mp_this, static_cast<float>(wpn.m_point_scale), eye, points);

			// Prefer points closer to our eye ray (exposed edge of overlapping heads).
			std::sort(points.begin(), points.end(), [&](const vec3_t& a, const vec3_t& b) {
				return (a - eye).length_sqr() < (b - eye).length_sqr();
			});

			for (const auto& point_raw : points) {
				if (++pen_tests > k_max_pen_tests)
					break;
				const vec3_t point = world_shift.is_valid() ? (point_raw + world_shift) : point_raw;
				if (!point.is_valid())
					continue;
				const auto pen = fire_bullet_safe(eye, point, local, pawn, data, wpn.m_autowall, hid);
				if (!pen.hit)
					continue;
				if (pen.damage < static_cast<float>(min_damage))
					continue;

				const bool was_lethal = best_dmg >= lethal;
				const bool now_lethal = pen.damage >= lethal;
				const bool better = (!was_lethal && now_lethal)
					|| (now_lethal == was_lethal && pen.damage > best_dmg);

				if (better) {
					best_dmg = pen.damage;
					out.pos = point;
					out.hitbox_bit = bit;
					out.hitbox_id = (pen.hitbox >= 0 && pen.hitbox < 20) ? pen.hitbox : hid;
					out.damage = pen.damage;
					if (world_shift.length_sqr() > 0.25f) {
						out.extrapolated = true;
						out.extrap_ticks = 1;
						out.extrap_delta = world_shift;
					}
				}

				if (now_lethal) {
					bit_done = true;
					break;
				}
			}
			if (bit_done)
				break;
		}

		if (best_dmg >= lethal)
			break;
	}

	return best_dmg > 0.f;
}

// Ahead delta for N ticks. Prefer linear (docs §3) — friction lag aims BEHIND peeks.
// Light accel + air gravity only; ground friction disabled for short windows.
vec3_t integrate_extrap_delta(vec3_t vel, int ticks, bool on_ground, const vec3_t& accel = {}) {
	vec3_t delta{};
	if (ticks <= 0)
		return delta;

	constexpr float k_sv_gravity = 800.f;

	// Fast path: pure linear when accel is tiny (most peeks).
	if (accel.length_sqr() < 1.f) {
		if (on_ground)
			vel.z = 0.f;
		else if (ticks > 1) {
			// Average gravity over the window without stepwise friction.
			vel.z -= 0.5f * k_sv_gravity * INTERVAL_PER_TICK * static_cast<float>(ticks - 1);
		}
		return vel * (INTERVAL_PER_TICK * static_cast<float>(ticks));
	}

	for (int i = 0; i < ticks; ++i) {
		vel.x += accel.x * INTERVAL_PER_TICK;
		vel.y += accel.y * INTERVAL_PER_TICK;
		if (!on_ground) {
			vel.z += accel.z * INTERVAL_PER_TICK;
			vel.z -= k_sv_gravity * INTERVAL_PER_TICK;
		} else {
			vel.z = 0.f;
		}
		delta = delta + vel * INTERVAL_PER_TICK;
	}
	return delta;
}

// --- Target motion estimation from lag-record ORIGIN deltas (network truth). ---
// HvH: m_vecVelocity is spoofable/jittery; record positions are what the server saw.
// Detects jitter (direction flips) so lead/extrap never chases a fake strafe.
struct target_motion_t {
	vec3_t vel{};
	bool jitter = false;
	bool valid = false;
	bool on_ground = true;
};

target_motion_t estimate_target_motion(int controller_index, c_cs_player_pawn* pawn) {
	target_motion_t out{};
	if (!pawn)
		return out;

	const vec3_t live_vel = pawn->m_vec_abs_velocity();
	out.on_ground = (pawn->m_flags() & FL_ONGROUND) != 0;

	struct snap_t { vec3_t org; float t; };
	snap_t snaps[5];
	int n = 0;
	if (g_lagcomp && controller_index > 0) {
		std::vector<lag_record_t*> recs;
		recs.reserve(8);
		g_lagcomp->for_each_valid(controller_index, [&](lag_record_t& r) {
			recs.push_back(&r);
		});
		std::sort(recs.begin(), recs.end(), [](const lag_record_t* a, const lag_record_t* b) {
			return a->m_simulation_time > b->m_simulation_time;
		});
		for (auto* r : recs) {
			if (n >= 5)
				break;
			if (!r->m_origin.is_valid() || r->m_simulation_time <= 0.f)
				continue;
			if (n > 0 && std::fabs(snaps[n - 1].t - r->m_simulation_time) < 0.0001f)
				continue;
			snaps[n++] = { r->m_origin, r->m_simulation_time };
		}
		if (n > 0) {
			// newest record flags are the networked truth for ground state
			if (auto* newest = g_lagcomp->select(controller_index, c_lagcomp::select_mode::newest))
				out.on_ground = (newest->m_flags & FL_ONGROUND) != 0;
		}
	}

	bool raw_jitter = false;
	if (n >= 3) {
		// Successive step directions — flip = jitter / desync walk.
		vec3_t d0 = snaps[0].org - snaps[1].org;
		vec3_t d1 = snaps[1].org - snaps[2].org;
		d0.z = 0.f;
		d1.z = 0.f;
		const float l0 = d0.length_2d();
		const float l1 = d1.length_2d();
		if (l0 > 1.0f && l1 > 1.0f) {
			const float align = (d0 * (1.f / l0)).dot(d1 * (1.f / l1));
			if (align < -0.1f)
				raw_jitter = true;
		}

		// Net displacement over the window — jitter averages itself out to ~0.
		const float dt = snaps[0].t - snaps[n - 1].t;
		if (dt > 0.005f && dt < 0.6f) {
			const vec3_t net = (snaps[0].org - snaps[n - 1].org) * (1.f / dt);
			if (net.is_valid() && net.length_2d() < 3000.f) { // teleport guard
				out.vel = net;
				out.valid = true;
			}
		}
	}

	if (!out.valid) {
		out.vel = live_vel.is_valid() ? live_vel : vec3_t{};
		out.valid = live_vel.is_valid();
	} else if (live_vel.is_valid() && live_vel.length_2d() > 30.f && out.vel.length_2d() > 30.f) {
		// Networked velocity fighting actual displacement — fakewalk/jitter.
		if (out.vel.normalize().dot(live_vel.normalize()) < 0.f)
			raw_jitter = true;
	}

	// Hysteresis: hold the jitter verdict for a short window. A verdict that
	// toggles every tick switches the lead on/off, which is itself position
	// flicker at high velocity.
	static unsigned long long s_jitter_until[65] = {};
	const unsigned long long now_ms = GetTickCount64();
	if (controller_index > 0 && controller_index < 65) {
		if (raw_jitter)
			s_jitter_until[controller_index] = now_ms + 300;
		out.jitter = now_ms < s_jitter_until[controller_index];
	} else {
		out.jitter = raw_jitter;
	}

	return out;
}

// Always-on 1-tick arrival lead for live pose (independent of menu extrapolation).
vec3_t live_arrival_lead(c_cs_player_pawn* pawn, lag_record_t* newest, int controller_index) {
	(void)newest;
	if (!pawn)
		return {};

	const target_motion_t motion = estimate_target_motion(controller_index, pawn);
	// Jittering target: leading chases the fake step and misses — aim at actual pose.
	if (motion.jitter || !motion.valid)
		return {};
	if (!motion.vel.is_valid() || motion.vel.length_2d() < k_extrap_min_speed)
		return {};

	int lead = 1;
	if (g_prediction) {
		if (const auto* pred = g_prediction->get_local_data()) {
			if (pred->m_valid && pred->m_player_tick > 0 && pred->m_tick_base > 0) {
				const int diff = pred->m_tick_base - pred->m_player_tick;
				if (diff > 0 && diff <= 6)
					lead = 1 + diff / 3; // ~⅓ RTT, capped small
			}
		}
	}
	lead = std::clamp(lead, 1, 3);
	return integrate_extrap_delta(motion.vel, lead, motion.on_ground);
}

// Cheap peek anticipation: current LOS OR after N ticks of velocity.
bool peek_los_in_ticks(
	c_cs_player_pawn* local,
	c_cs_weapon_base_v_data* data,
	const vec3_t& eye,
	const std::vector<c_cs_player_pawn*>& pawns,
	int ticks)
{
	if (!local || !data || !g_autowall || pawns.empty())
		return false;

	vec3_t vel = local->m_vec_abs_velocity();
	if (g_prediction) {
		if (const auto* pred = g_prediction->get_local_data()) {
			if (pred->m_valid && pred->m_velocity.is_valid())
				vel = pred->m_velocity;
		}
	}
	const bool on_ground = (local->m_flags() & FL_ONGROUND) != 0;
	const vec3_t future_eye = (ticks > 0)
		? (eye + integrate_extrap_delta(vel, ticks, on_ground))
		: eye;

	for (auto* pawn : pawns) {
		if (!pawn)
			continue;
		vec3_t head = point_from_pawn(pawn, 6);
		if (!head.is_valid() || head.is_zero())
			continue;
		auto now = fire_bullet_safe(eye, head, local, pawn, data, false, HITBOX_HEAD);
		if (now.hit && now.damage > 1.f)
			return true;
		if (ticks > 0) {
			auto soon = fire_bullet_safe(future_eye, head, local, pawn, data, false, HITBOX_HEAD);
			if (soon.hit && soon.damage > 1.f)
				return true;
		}
	}
	return false;
}

bool try_extrapolate_points(
	c_cs_player_pawn* local,
	c_cs_player_pawn* pawn,
	int controller_index,
	const c_config::ragebot_t::weapon_t& wpn,
	c_cs_weapon_base_v_data* data,
	const vec3_t& eye,
	int min_damage,
	int target_hp,
	int max_ticks,
	aim_point_t& best,
	float& best_dmg)
{
	if (!pawn || !data || !local || !g_autowall || max_ticks <= 0)
		return false;

	// Displacement-based motion — never trust networked velocity alone on HvH.
	const target_motion_t motion = estimate_target_motion(controller_index, pawn);
	if (!motion.valid || motion.jitter)
		return false;

	const vec3_t vel = motion.vel;
	const vec3_t accel{}; // accel from spoofable velocity is noise on HvH — linear only
	const bool on_ground = motion.on_ground;
	lag_record_t* newest = nullptr;
	if (g_lagcomp && controller_index > 0)
		newest = g_lagcomp->select(controller_index, c_lagcomp::select_mode::newest);

	if (!vel.is_valid())
		return false;
	if (vel.length_2d() < k_extrap_min_speed)
		return false;
	if (pawn->is_throwing())
		return false;

	// Add ~local latency ticks so ahead-aim meets the server arrival time.
	int latency_ticks = 0;
	if (g_prediction) {
		if (const auto* pred = g_prediction->get_local_data()) {
			if (pred->m_valid && pred->m_player_tick > 0 && pred->m_tick_base > 0) {
				const int diff = pred->m_tick_base - pred->m_player_tick;
				if (diff > 0 && diff <= 8)
					latency_ticks = diff / 2; // conservative half-RTT in ticks
			}
		}
	}
	max_ticks = std::clamp(max_ticks + latency_ticks, 1, 10);
	const float lethal = static_cast<float>((std::max)(1, target_hp));

	// Prefer head / chest centers only (budget) — shift by integrated delta.
	struct seed_t { vec3_t pos; int hid; int bit; };
	std::vector<seed_t> seeds;
	seeds.reserve(8);

	auto push_seed = [&](int bone, int hid, int bit) {
		vec3_t p = newest ? point_from_record(newest, bone) : point_from_pawn(pawn, bone);
		if (!p.is_valid() || p.is_zero())
			return;
		seeds.push_back({ p, hid, bit });
	};

	const int mask = wpn.m_hitboxes ? wpn.m_hitboxes : c_config::ragebot_t::hb_head;
	if (mask & c_config::ragebot_t::hb_head)
		push_seed(6, HITBOX_HEAD, c_config::ragebot_t::hb_head);
	if (mask & c_config::ragebot_t::hb_neck)
		push_seed(5, HITBOX_NECK, c_config::ragebot_t::hb_neck);
	if (mask & c_config::ragebot_t::hb_chest)
		push_seed(4, HITBOX_CHEST, c_config::ragebot_t::hb_chest);
	if (mask & c_config::ragebot_t::hb_stomach)
		push_seed(3, HITBOX_STOMACH, c_config::ragebot_t::hb_stomach);
	if (seeds.empty())
		push_seed(6, HITBOX_HEAD, c_config::ragebot_t::hb_head);

	// Eye-facing lateral on head for peek clears.
	if (!seeds.empty() && (mask & c_config::ragebot_t::hb_head)) {
		vec3_t to_eye = eye - seeds[0].pos;
		if (to_eye.length_sqr() > 1.f) {
			to_eye = to_eye.normalize();
			vec3_t side = to_eye.cross(vec3_t{ 0.f, 0.f, 1.f });
			if (side.length_sqr() > 1e-6f) {
				side = side.normalize();
				seeds.push_back({ seeds[0].pos + side * 3.5f, HITBOX_HEAD, c_config::ragebot_t::hb_head });
				seeds.push_back({ seeds[0].pos - side * 3.5f, HITBOX_HEAD, c_config::ragebot_t::hb_head });
			}
		}
	}

	bool found = false;
	for (int k = 1; k <= max_ticks; ++k) {
		const vec3_t delta = integrate_extrap_delta(vel, k, on_ground, accel);
		if (!delta.is_valid() || delta.length_sqr() < 0.25f)
			continue;

		for (const auto& s : seeds) {
			const vec3_t point = s.pos + delta;
			if (!point.is_valid())
				continue;
			const auto pen = fire_bullet_safe(eye, point, local, pawn, data, wpn.m_autowall, s.hid);
			if (!pen.hit || pen.damage < static_cast<float>(min_damage))
				continue;

			const bool was_lethal = best_dmg >= lethal;
			const bool now_lethal = pen.damage >= lethal;
			const bool better = (!was_lethal && now_lethal)
				|| (now_lethal == was_lethal && pen.damage > best_dmg + 0.5f)
				|| (now_lethal && best.extrapolated && k < best.extrap_ticks
					&& std::fabs(pen.damage - best_dmg) <= 0.5f);

			if (better) {
				best_dmg = pen.damage;
				best.pos = point;
				best.hitbox_bit = s.bit;
				best.hitbox_id = (pen.hitbox >= 0 && pen.hitbox < 20) ? pen.hitbox : s.hid;
				best.damage = pen.damage;
				best.extrapolated = true;
				best.extrap_ticks = k;
				best.extrap_delta = delta;
				found = true;
			}

			if (now_lethal)
				return true;
		}
	}

	return found;
}

// Scan live pose and/or every valid lag record; keep the best aimable point.
bool scan_target(
	c_cs_player_pawn* local,
	c_cs_player_pawn* pawn,
	int controller_index,
	const c_config::ragebot_t::weapon_t& wpn,
	c_cs_weapon_base_v_data* data,
	const vec3_t& eye,
	int min_damage,
	int target_hp,
	bool use_backtrack,
	bool use_extrap,
	int extrap_ticks,
	aim_point_t& out_point,
	lag_record_t*& out_record)
{
	out_point = {};
	out_record = nullptr;
	if (!pawn || !data || !local)
		return false;

	aim_point_t best{};
	lag_record_t* best_rec = nullptr;
	float best_dmg = -1.f;
	const float lethal = static_cast<float>((std::max)(1, target_hp));

	// `margin`: a new pose must beat the current best by this much damage to
	// replace it. Live/lead compete freely; BT records need a real advantage —
	// at high velocity record poses trail the live pose by many units, and
	// letting noise-level damage differences flip the source made the aim
	// point flicker between two positions every tick.
	auto consider = [&](lag_record_t* rec, const vec3_t& shift = {}, float margin = 0.f) {
		aim_point_t pt{};
		if (!scan_target_points(local, pawn, rec, wpn, data, eye, min_damage, target_hp, pt, shift))
			return false;

		const bool was_lethal = best_dmg >= lethal;
		const bool now_lethal = pt.damage >= lethal;
		const bool better = (!was_lethal && now_lethal)
			|| (now_lethal == was_lethal && pt.damage > best_dmg + margin);

		if (better) {
			best = pt;
			best_dmg = pt.damage;
			best_rec = rec;
		}
		return now_lethal;
	};

	lag_record_t* newest = nullptr;
	if (g_lagcomp && controller_index > 0)
		newest = g_lagcomp->select(controller_index, c_lagcomp::select_mode::newest);
	const vec3_t lead = live_arrival_lead(pawn, newest, controller_index);

	// Live pose with arrival lead — the single authoritative source. Unshifted
	// pose only when the led scan found NOTHING (never as a competing option:
	// both succeeding on alternating ticks was the position flicker).
	bool live_lethal = consider(nullptr, lead);
	if (best_dmg <= 0.f && lead.length_sqr() > 0.25f)
		live_lethal = consider(nullptr);

	// Backtrack: records must clearly beat the live pose to take over.
	if (use_backtrack && g_lagcomp && controller_index > 0 && !live_lethal) {
		std::vector<lag_record_t*> records;
		records.reserve(k_max_bt_records);
		g_lagcomp->for_each_valid(controller_index, [&](lag_record_t& rec) {
			if (static_cast<int>(records.size()) < k_max_bt_records)
				records.push_back(&rec);
		});

		std::sort(records.begin(), records.end(), [](const lag_record_t* a, const lag_record_t* b) {
			return a->m_simulation_time > b->m_simulation_time;
		});

		const float bt_margin = (best_dmg > 0.f) ? 8.f : 0.f;
		for (auto* rec : records) {
			if (consider(rec, {}, bt_margin) || best_dmg >= lethal)
				break;
		}
	}

	// Extrapolation: aim-point only (never rewrite BT ticks). Skip if already lethal.
	if (use_extrap && extrap_ticks > 0 && best_dmg < lethal) {
		aim_point_t extrap_best = best;
		float extrap_dmg = best_dmg;
		if (try_extrapolate_points(local, pawn, controller_index, wpn, data, eye,
			min_damage, target_hp, extrap_ticks, extrap_best, extrap_dmg)) {
			best = extrap_best;
			best_dmg = extrap_dmg;
			best_rec = nullptr; // live history — not a lag record
		}
	}

	if (best_dmg > 0.f) {
		out_point = best;
		out_record = best_rec;
		return true;
	}

	return false;
}

bool better_target(const target_t& a, const target_t& b, int mode, const vec3_t& view) {
	(void)view;
	const bool a_lethal = a.best.damage >= static_cast<float>((std::max)(1, a.health));
	const bool b_lethal = b.best.damage >= static_cast<float>((std::max)(1, b.health));
	if (a_lethal != b_lethal)
		return a_lethal;

	switch (mode) {
	case c_config::ragebot_t::sel_closest:
		return a.distance < b.distance;
	case c_config::ragebot_t::sel_lowest_health:
		if (a.health != b.health)
			return a.health < b.health;
		return a.best.damage > b.best.damage;
	case c_config::ragebot_t::sel_closest_crosshair:
		if (std::fabs(a.fov - b.fov) > 0.01f)
			return a.fov < b.fov;
		return a.best.damage > b.best.damage;
	case c_config::ragebot_t::sel_highest_damage:
	default:
		if (std::fabs(a.best.damage - b.best.damage) > 0.5f)
			return a.best.damage > b.best.damage;
		return a.fov < b.fov;
	}
}

void run_knife_or_zeus(i_csgo_input* input, c_user_cmd* cmd, c_cs_player_pawn* local, bool want_knife, bool want_zeus) {
	auto* weapon = local->get_active_weapon();
	if (!weapon)
		return;
	auto* data = weapon->get_weapon_data();
	if (!data)
		return;

	const int type = data->m_weapon_type();
	const bool is_knife = (type == WEAPONTYPE_KNIFE || type == WEAPONTYPE_MELEE);
	const bool is_zeus = (type == WEAPONTYPE_TASER);
	if (!((want_knife && is_knife) || (want_zeus && is_zeus)))
		return;

	// Knife has no clip / weird next-attack — don't use gun can_fire gate.
	if (is_zeus && !can_fire(local, weapon))
		return;
	if (local->m_health() <= 0)
		return;

	const vec3_t eye = (g_prediction && g_prediction->get_local_data() && g_prediction->get_local_data()->m_valid)
		? g_prediction->eye() : get_eye_position(local);

	// Lefrizzel: slash <= 64, stab <= 48. When stuck to model, origin distance works.
	const float max_dist = is_zeus ? 200.f : 72.f;
	const float stab_dist = 48.f;

	c_cs_player_pawn* best = nullptr;
	float best_dist = max_dist;
	vec3_t best_pos{};
	bool prefer_stab = false;

	vec3_t local_origin{};
	if (auto* ln = local->m_scene_node())
		local_origin = ln->m_abs_origin();

	for (int i = 1; i < 65; ++i) {
		auto* controller = reinterpret_cast<c_cs_player_controller*>(entity_by_index(i));
		if (!controller || !controller->m_pawn_is_alive())
			continue;
		auto* pawn = reinterpret_cast<c_cs_player_pawn*>(entity_by_index(controller->m_pawn().get_entry_index()));
		if (!pawn || pawn == local || !pawn->is_alive() || pawn->m_team_num() == local->m_team_num())
			continue;

		vec3_t origin{};
		if (auto* node = pawn->m_scene_node())
			origin = node->m_abs_origin();
		if (!origin.is_valid() || origin.is_zero())
			continue;

		// Point-blank: use 2D origin distance (works when glued to the model).
		const float d2 = (origin - local_origin).length_2d();
		vec3_t aim = origin + vec3_t(0.f, 0.f, 36.f);
		float d = (aim - eye).length();
		if (is_knife && d2 < d)
			d = d2; // prefer horizontal stick distance for knife

		if (d >= best_dist)
			continue;

		// NO autowall/trace gate — stuck to model must still slash.
		best_dist = d;
		best = pawn;
		best_pos = aim;
		prefer_stab = is_knife && d <= stab_dist;
	}

	if (!best)
		return;

	const vec3_t punch = get_aim_punch(local);
	vec3_t ang = calc_angle(eye, best_pos) - punch;
	write_aim_angles(input, cmd, ang, false); // knife: always visible aim
	if (g_cfg->ragebot.m_auto_fire) {
		if (prefer_stab)
			set_attack2(cmd, true);
		else
			set_attack(cmd, true);
	}
}

} // namespace

int c_ragebot::category_from_weapon(c_base_player_weapon* weapon) {
	if (!weapon)
		return c_config::ragebot_t::cat_rifle;

	auto* data = weapon->get_weapon_data();
	if (!data)
		return c_config::ragebot_t::cat_rifle;

	const int def = get_def_index(weapon);
	if (def == WEAPON_DEAGLE)
		return c_config::ragebot_t::cat_deagle;
	if (def == WEAPON_REVOLVER)
		return c_config::ragebot_t::cat_revolver;

	switch (data->m_weapon_type()) {
	case WEAPONTYPE_PISTOL:
		return c_config::ragebot_t::cat_pistols;
	case WEAPONTYPE_SUBMACHINEGUN:
		return c_config::ragebot_t::cat_smg;
	case WEAPONTYPE_SHOTGUN:
		return c_config::ragebot_t::cat_shotgun;
	case WEAPONTYPE_RIFLE:
	case WEAPONTYPE_MACHINEGUN:
		return c_config::ragebot_t::cat_rifle;
	case WEAPONTYPE_SNIPER_RIFLE: {
		const char* name = data->m_name();
		if (name) {
			if (std::strstr(name, "ssg08") || std::strstr(name, "SSG"))
				return c_config::ragebot_t::cat_scout;
			if (std::strstr(name, "awp") || std::strstr(name, "AWP"))
				return c_config::ragebot_t::cat_awp;
			if (std::strstr(name, "g3sg1") || std::strstr(name, "scar20")
				|| std::strstr(name, "SCAR") || std::strstr(name, "G3SG"))
				return c_config::ragebot_t::cat_auto;
		}
		return c_config::ragebot_t::cat_scout;
	}
	default:
		return c_config::ragebot_t::cat_rifle;
	}
}

int c_ragebot::resolve_min_damage(const c_config::ragebot_t::weapon_t& wpn, int target_hp, bool override_active) {
	const int configured = (override_active && wpn.m_min_damage_override > 0)
		? wpn.m_min_damage_override
		: wpn.m_min_damage;
	return c_config::ragebot_t::effective_min_damage(configured, target_hp);
}

namespace {

const char* sel_mode_name(int mode) {
	switch (mode) {
	case c_config::ragebot_t::sel_closest: return "closest";
	case c_config::ragebot_t::sel_lowest_health: return "lowest_hp";
	case c_config::ragebot_t::sel_closest_crosshair: return "crosshair";
	case c_config::ragebot_t::sel_highest_damage: return "highest_dmg";
	default: return "unknown";
	}
}

const char* weapon_cat_name(int cat) {
	switch (cat) {
	case c_config::ragebot_t::cat_pistols: return "pistols";
	case c_config::ragebot_t::cat_deagle: return "deagle";
	case c_config::ragebot_t::cat_revolver: return "revolver";
	case c_config::ragebot_t::cat_smg: return "smg";
	case c_config::ragebot_t::cat_rifle: return "rifle";
	case c_config::ragebot_t::cat_shotgun: return "shotgun";
	case c_config::ragebot_t::cat_scout: return "scout";
	case c_config::ragebot_t::cat_auto: return "auto";
	case c_config::ragebot_t::cat_awp: return "awp";
	default: return "other";
	}
}

struct rage_disk_logger_t {
	std::mutex mutex;
	FILE* file = nullptr;
	int file_id = 1;
	int ticks_in_file = 0;
	int total_ticks = 0;
	char active_path[260]{};

	~rage_disk_logger_t() {
		std::lock_guard lock(mutex);
		close_unlocked();
	}

	void close_unlocked() {
		if (!file)
			return;
		std::fflush(file);
		std::fclose(file);
		file = nullptr;
	}

	bool open_next_unlocked() {
		close_unlocked();
		// Preferred: C:\1.log, C:\2.log, ...
		// Fallback if C:\ root is locked: C:\celerity_rage_logs\N.log
		char path[260]{};
		std::snprintf(path, sizeof(path), "C:\\%d.log", file_id);
		file = std::fopen(path, "wb");
		if (!file) {
			CreateDirectoryA("C:\\celerity_rage_logs", nullptr);
			std::snprintf(path, sizeof(path), "C:\\celerity_rage_logs\\%d.log", file_id);
			file = std::fopen(path, "wb");
		}
		if (!file)
			return false;
		std::strncpy(active_path, path, sizeof(active_path) - 1);
		std::fprintf(file,
			"=== celerity ragebot log file #%d ===\n"
			"path=%s\n"
			"ticks_per_file=%d\n"
			"====================================\n\n",
			file_id, active_path, c_ragebot::k_log_ticks_per_file);
		std::fflush(file);
		ticks_in_file = 0;
		return true;
	}

	void write_frame(const c_ragebot::debug_frame_t& f) {
		std::lock_guard lock(mutex);
		if (!file && !open_next_unlocked())
			return;

		++total_ticks;
		++ticks_in_file;

		std::fprintf(file,
			"---- tick_in_file=%d total=%d tick_base=%d ----\n"
			"block=%s fired=%d have_target=%d can_fire=%d hc_pass=%d autostop=%d pen_ready=%d\n"
			"weapon=%s def=%d cat=%s(%d) clip=%d next_atk=%d zoom=%d hist=%d\n"
			"spread=%.5f inac=%.5f bloom=%.5f speed=%.2f on_ground=%d flags=0x%X\n"
			"cfg: hc_need=%d min_dmg=%d point_scale=%d autowall=%d silent=%d autofire=%d sel=%s hb_mask=0x%X mp_mask=0x%X\n"
			"eye=(%.2f, %.2f, %.2f) aim=(%.2f, %.2f, %.2f) ang=(%.2f, %.2f) punch=(%.2f, %.2f)\n"
			"best: idx=%d hp=%d dmg=%.1f fov=%.2f dist=%.1f hitbox=%d bit=0x%X candidates=%d targets=%d\n",
			ticks_in_file, total_ticks, f.tick_base,
			f.block[0] ? f.block : "-",
			f.fired ? 1 : 0, f.have_target ? 1 : 0, f.can_fire ? 1 : 0,
			f.hitchance_pass ? 1 : 0, f.is_autostop ? 1 : 0, f.pen_ready ? 1 : 0,
			f.weapon_name[0] ? f.weapon_name : "-",
			f.weapon_def, weapon_cat_name(f.weapon_cat), f.weapon_cat,
			f.clip, f.next_attack, f.zoom, f.hist_count,
			f.spread, f.inaccuracy, f.spread + f.inaccuracy, f.speed,
			f.on_ground ? 1 : 0, f.local_flags,
			f.hitchance_needed, f.min_damage, f.point_scale,
			f.autowall ? 1 : 0, f.silent ? 1 : 0, f.auto_fire ? 1 : 0,
			sel_mode_name(f.sel_mode), f.hitboxes_mask, f.multipoint_mask,
			f.eye_x, f.eye_y, f.eye_z,
			f.aim_x, f.aim_y, f.aim_z,
			f.ang_x, f.ang_y, f.punch_x, f.punch_y,
			f.best_index, f.best_hp, f.best_damage, f.best_fov, f.best_dist,
			f.hitbox_id, f.hitbox_bit, f.candidate_count, f.target_count);

		for (int i = 0; i < f.target_count; ++i) {
			const auto& t = f.targets[i];
			std::fprintf(file,
				"  [%d] %s idx=%d hp=%d min_dmg=%d dist=%.1f fov=%.2f dmg=%.1f hc=%.0f "
				"hb=%d scanned=%d can_hit=%d best=%d pen=%d aim=(%.2f,%.2f,%.2f)\n",
				i, t.name[0] ? t.name : "?",
				t.index, t.hp, t.min_dmg, t.dist, t.fov, t.damage, t.hitchance,
				t.hitbox_id, t.scanned ? 1 : 0, t.can_hit ? 1 : 0, t.is_best ? 1 : 0,
				t.penetrated ? 1 : 0, t.aim_x, t.aim_y, t.aim_z);
		}
		std::fputc('\n', file);

		if (ticks_in_file >= c_ragebot::k_log_ticks_per_file) {
			std::fprintf(file, "=== end of file #%d (%d ticks) ===\n", file_id, ticks_in_file);
			close_unlocked();
			++file_id;
		} else if ((ticks_in_file % 25) == 0) {
			std::fflush(file);
		}
	}
};

rage_disk_logger_t& rage_disk_log() {
	static rage_disk_logger_t logger;
	return logger;
}

} // namespace

void c_ragebot::publish_debug(const debug_frame_t& frame) {
	{
		std::lock_guard lock(m_debug_mutex);
		m_debug = frame;
	}
	// Disk logging every CreateMove was the main FPS killer — only when visualize is on.
	if (frame.valid && g_cfg && g_cfg->ragebot.m_visualize_data)
		rage_disk_log().write_frame(frame);
}

namespace {
__declspec(noinline) void copy_player_name(c_cs_player_controller* ctrl, char* out, int out_n) {
	if (!out || out_n <= 0)
		return;
	out[0] = '\0';
	if (!ctrl)
		return;
	char* nm = ctrl->m_player_name();
	if (nm && nm[0])
		std::strncpy(out, nm, static_cast<std::size_t>(out_n) - 1);
}

bool read_camera_angles(i_csgo_input* input, vec3_t* out) {
	if (!input || !out)
		return false;
	*out = input->get_view_angles();
	return true;
}
} // namespace

void c_ragebot::run(i_csgo_input* input, c_user_cmd* cmd) {
	struct lagcomp_cleanup {
		~lagcomp_cleanup() {
			if (g_lagcomp)
				g_lagcomp->force_reset();
		}
	} bones_cleanup;

	s_attack_input = input;

	if (g_lagcomp)
		g_lagcomp->force_reset();

	// Lefrizzel: semi press one CM → full release next CM (rising edge for next shot).
	if (s_semi_need_release && cmd) {
		strip_attack(cmd);
		clear_csgo_attack_input(input);
		s_semi_need_release = false;
		s_auto_held = false;
	}

	if (!g_cfg || !g_cfg->ragebot.m_enabled || !input || !cmd || !g_ctx)
		return;

	auto* local = reinterpret_cast<c_cs_player_pawn*>(g_ctx->m_local_pawn);
	if (!local || !local->is_alive())
		return;

	if (g_interfaces && g_interfaces->m_engine) {
		if (!g_interfaces->m_engine->is_in_game())
			return;
	}

	auto* weapon = local->get_active_weapon();
	if (!weapon)
		return;

	auto* data = weapon->get_weapon_data();
	if (!data)
		return;

	const int wtype = data->m_weapon_type();
	if (wtype == WEAPONTYPE_KNIFE || wtype == WEAPONTYPE_MELEE || wtype == WEAPONTYPE_TASER) {
		run_knife_or_zeus(input, cmd, local, g_cfg->ragebot.m_knife_bot, g_cfg->ragebot.m_zeus_bot);
		return;
	}
	if (wtype == WEAPONTYPE_GRENADE || wtype == WEAPONTYPE_C4)
		return;

	const int cat = category_from_weapon(weapon);
	const auto& wpn = g_cfg->ragebot.weapon(cat);

	const bool override_active = g_cfg->ragebot.m_override_damage
		&& bind_active(g_cfg->ragebot.m_override_damage_key, g_cfg->ragebot.m_override_damage_mode);

	vec3_t eye = get_eye_position(local);
	if (g_prediction) {
		if (const auto* pred = g_prediction->get_local_data()) {
			if (pred->m_valid && pred->m_eye_pos.is_valid() && !pred->m_eye_pos.is_zero()) {
				const vec3_t& pe = pred->m_eye_pos;
				// Reject broken (0,y,0) — prefer world eye resolver instead.
				if (std::fabs(pe.x) >= 1.f || std::fabs(pe.z) >= 1.f)
					eye = pe;
			}
		}
	}
	if (!eye.is_valid() || eye.is_zero()
		|| (std::fabs(eye.x) < 1.f && std::fabs(eye.z) < 1.f)) {
		// No usable shoot origin — never invent aim/autostop from garbage eye.
		return;
	}

	vec3_t view{};
	if (!read_camera_angles(input, &view)) {
		if (auto* base = cmd->get_base_cmd(); base && base->has_viewangles())
			view = { base->viewangles().x(), base->viewangles().y(), 0.f };
		else
			view = read_ptr<vec3_t>(client_base() + cs2_dumper::offsets::client_dll::dwViewAngles);
	}
	view = normalize_angles(view);

	auto* cs_weapon = static_cast<c_cs_weapon_base*>(weapon);
	const bool holding_revolver = is_revolver_weapon(weapon);
	const bool auto_revolver = g_cfg->ragebot.m_auto_revolver && holding_revolver;
	const int sel_mode = g_cfg->ragebot.m_target_selection;
	const float weapon_range = data->m_range();

	struct candidate_t {
		c_cs_player_pawn* pawn = nullptr;
		int controller_index = 0;
		int health = 0;
		int min_dmg = 0;
		float distance = 0.f;
		float fov = 0.f;
		float max_possible_dmg = 0.f;
	};

	std::vector<candidate_t> candidates;
	candidates.reserve(16);
	const int local_team = local->m_team_num();

	for (int i = 1; i < 65; ++i) {
		auto* controller = reinterpret_cast<c_cs_player_controller*>(entity_by_index(i));
		if (!controller || !controller->m_pawn_is_alive())
			continue;

		auto* pawn = reinterpret_cast<c_cs_player_pawn*>(
			entity_by_index(controller->m_pawn().get_entry_index()));
		if (!pawn || pawn == local || !pawn->is_alive())
			continue;
		if (pawn->m_team_num() == local_team)
			continue;

		vec3_t head = point_from_pawn(pawn, 6);
		if (!head.is_valid() || head.is_zero()) {
			if (auto* node = pawn->m_scene_node())
				head = node->m_abs_origin() + vec3_t(0.f, 0.f, 72.f);
		}
		if (!head.is_valid() || head.is_zero())
			continue;

		const float dist = (head - eye).length();
		if (dist < 1.f || dist > weapon_range)
			continue;

		candidate_t c{};
		c.pawn = pawn;
		c.controller_index = i;
		c.health = pawn->m_health();
		c.min_dmg = resolve_min_damage(wpn, c.health, override_active);
		c.distance = dist;
		c.fov = angle_distance(view, calc_angle(eye, head));
		c.max_possible_dmg = estimate_max_damage(data, dist);
		if (c.max_possible_dmg < static_cast<float>(c.min_dmg))
			continue;
		candidates.push_back(c);
	}

	// Order by selection mode BEFORE expensive autowall so we early-out sooner.
	std::sort(candidates.begin(), candidates.end(), [&](const candidate_t& a, const candidate_t& b) {
		switch (sel_mode) {
		case c_config::ragebot_t::sel_closest:
			return a.distance < b.distance;
		case c_config::ragebot_t::sel_lowest_health:
			if (a.health != b.health)
				return a.health < b.health;
			return a.fov < b.fov;
		case c_config::ragebot_t::sel_closest_crosshair:
			return a.fov < b.fov;
		case c_config::ragebot_t::sel_highest_damage:
		default:
			// Prefer higher theoretical damage / lower HP (easier lethal) first.
			if (std::fabs(a.max_possible_dmg - b.max_possible_dmg) > 1.f)
				return a.max_possible_dmg > b.max_possible_dmg;
			if (a.health != b.health)
				return a.health < b.health;
			return a.fov < b.fov;
		}
	});

	// Cap scan budget before expensive pen tests / MT workers.
	const int max_targets = std::clamp(g_cfg->ragebot.m_max_targets, 1, 16);
	if (static_cast<int>(candidates.size()) > max_targets)
		candidates.resize(static_cast<std::size_t>(max_targets));

	const bool want_early_stop = (wpn.m_autostop & c_config::ragebot_t::as_early) != 0;
	const bool want_between = (wpn.m_autostop & c_config::ragebot_t::as_between_shots) != 0;
	const bool want_full = (wpn.m_autostop & c_config::ragebot_t::as_full_stop) != 0;
	const bool want_air_stop = (wpn.m_autostop & c_config::ragebot_t::as_in_air) != 0;
	const bool on_ground_early = (local->m_flags() & FL_ONGROUND) != 0;

	// Early stop: if we can shoot NOW, or in ~2 ticks would have LOS — stop immediately.
	if (want_early_stop && !candidates.empty() && on_ground_early) {
		vec3_t peek_vel = local->m_vec_abs_velocity();
		if (g_prediction) {
			if (const auto* pred = g_prediction->get_local_data()) {
				if (pred->m_valid && pred->m_velocity.is_valid())
					peek_vel = pred->m_velocity;
			}
		}
		const float peek_spd = peek_vel.length_2d();
		if (peek_spd > k_stop_speed) {
			std::vector<c_cs_player_pawn*> peek_pawns;
			peek_pawns.reserve(candidates.size());
			for (const auto& c : candidates)
				peek_pawns.push_back(c.pawn);
			if (peek_los_in_ticks(local, data, eye, peek_pawns, 0)
				|| peek_los_in_ticks(local, data, eye, peek_pawns, 2))
				apply_auto_stop(cmd, local, view, false);
		}
	}

	target_t best{};
	bool have_best = false;

	debug_frame_t dbg{};
	dbg.valid = true;
	dbg.weapon_cat = cat;
	dbg.weapon_def = static_cast<int>(get_def_index(weapon));
	dbg.hitchance_needed = wpn.m_hitchance;
	dbg.min_damage = wpn.m_min_damage;
	dbg.point_scale = wpn.m_point_scale;
	dbg.hitboxes_mask = wpn.m_hitboxes;
	dbg.multipoint_mask = wpn.m_multipoints;
	dbg.autowall = wpn.m_autowall;
	dbg.silent = g_cfg->ragebot.m_silent;
	dbg.auto_fire = g_cfg->ragebot.m_auto_fire;
	dbg.sel_mode = sel_mode;
	dbg.candidate_count = static_cast<int>(candidates.size());
	dbg.local_flags = local->m_flags();
	dbg.on_ground = (dbg.local_flags & FL_ONGROUND) != 0;
	dbg.eye_x = eye.x; dbg.eye_y = eye.y; dbg.eye_z = eye.z;
	dbg.zoom = cs_weapon->m_zoom_level();
	if (const char* wn = data->m_name())
		std::strncpy(dbg.weapon_name, wn, sizeof(dbg.weapon_name) - 1);
	{
		float sp = 0.f, ina = 0.f;
		read_weapon_accuracy(cs_weapon, &sp, &ina);
		dbg.spread = sp;
		dbg.inaccuracy = ina;
		dbg.speed = local->m_vec_abs_velocity().length_2d();
		if (g_prediction) {
			if (const auto* pred = g_prediction->get_local_data()) {
				if (pred->m_valid && pred->m_velocity.is_valid())
					dbg.speed = pred->m_velocity.length_2d();
			}
		}
		dbg.clip = weapon->m_clip1();
		dbg.next_attack = weapon->m_next_primary_attack();
		dbg.tick_base = local_tick_base();
		dbg.hist_count = cmd->pb.input_history_size();
		dbg.can_fire = can_fire(local, weapon);
		dbg.pen_ready = g_autowall && g_autowall->pen_ready();
		if (!dbg.pen_ready)
			std::strncpy(dbg.block, "no_pen", sizeof(dbg.block) - 1);
		else if (candidates.empty())
			std::strncpy(dbg.block, "no_candidates", sizeof(dbg.block) - 1);
		else
			std::strncpy(dbg.block, "no_target", sizeof(dbg.block) - 1);
	}

	const bool want_bt = g_cfg->ragebot.m_backtrack;
	const bool want_extrap = g_cfg->ragebot.m_extrapolation && g_cfg->ragebot.m_extrap_ticks > 0;
	const int extrap_ticks = std::clamp(g_cfg->ragebot.m_extrap_ticks, 0, 8);

	struct scan_job_t {
		aim_point_t point{};
		lag_record_t* record = nullptr;
		bool scanned = false;
	};
	std::vector<scan_job_t> jobs(candidates.size());

	// Scan while the weapon is ready OR within ~8 ticks of ready: aim keeps
	// tracking through the fire cycle so the shot leaves on the exact ready tick
	// instead of losing 1-2 ticks re-acquiring the target.
	bool weapon_ready_for_scan = weapon->m_clip1() > 0 && !weapon->m_in_reload()
		&& local->m_health() > 0 && !local->is_throwing();
	if (weapon_ready_for_scan) {
		const int tb = local_tick_base();
		const int next = weapon->m_next_primary_attack();
		if (next > 0 && tb > 0) {
			const int delta = next - tb;
			if (delta > 8 && delta <= 64)
				weapon_ready_for_scan = false; // deep in fire cycle — skip heavy AW/BT scan
		}
	}

	auto run_scan = [&](std::size_t i) {
		const auto& cand = candidates[i];
		jobs[i].scanned = scan_target(local, cand.pawn, cand.controller_index, wpn, data, eye,
			cand.min_dmg, cand.health, want_bt, want_extrap, extrap_ticks,
			jobs[i].point, jobs[i].record);
	};

	// Skip full AW/BT when weapon can't fire — was ~720 CreateTrace calls / tick.
	if (weapon_ready_for_scan) {
		for (std::size_t i = 0; i < candidates.size(); ++i)
			run_scan(i);
	}

	for (std::size_t i = 0; i < candidates.size(); ++i) {
		const auto& cand = candidates[i];
		const auto& job = jobs[i];
		const bool scanned = job.scanned;
		const aim_point_t& point = job.point;
		lag_record_t* record = job.record;

		debug_target_t row{};
		row.index = cand.controller_index;
		row.hp = cand.health;
		row.min_dmg = cand.min_dmg;
		row.dist = cand.distance;
		row.fov = cand.fov;
		row.scanned = scanned;
		row.can_hit = scanned && point.damage > 0.f;
		row.damage = scanned ? point.damage : 0.f;
		row.hitchance = -1.f;
		row.hitbox_id = scanned ? point.hitbox_id : -1;
		if (scanned) {
			row.aim_x = point.pos.x;
			row.aim_y = point.pos.y;
			row.aim_z = point.pos.z;
		}
		if (auto* ctrl = reinterpret_cast<c_cs_player_controller*>(entity_by_index(cand.controller_index)))
			copy_player_name(ctrl, row.name, static_cast<int>(sizeof(row.name)));
		if (row.name[0] == '\0')
			std::snprintf(row.name, sizeof(row.name), "idx %d", cand.controller_index);

		if (!scanned || point.damage <= 0.f) {
			if (dbg.target_count < c_ragebot::k_debug_max_targets)
				dbg.targets[dbg.target_count++] = row;
			continue;
		}

		target_t t{};
		t.pawn = cand.pawn;
		t.controller_index = cand.controller_index;
		t.record = record;
		t.best = point;
		t.health = cand.health;
		t.distance = (point.pos - eye).length();
		t.fov = angle_distance(view, calc_angle(eye, point.pos));

		if (!have_best || better_target(t, best, sel_mode, view)) {
			best = t;
			have_best = true;
		}

		if (dbg.target_count < c_ragebot::k_debug_max_targets)
			dbg.targets[dbg.target_count++] = row;
	}

	auto fill_best_dbg = [&]() {
		if (!have_best)
			return;
		dbg.have_target = true;
		dbg.best_index = best.controller_index;
		dbg.best_hp = best.health;
		dbg.best_damage = best.best.damage;
		dbg.best_fov = best.fov;
		dbg.best_dist = best.distance;
		dbg.hitbox_id = best.best.hitbox_id;
		dbg.hitbox_bit = best.best.hitbox_bit;
		dbg.aim_x = best.best.pos.x;
		dbg.aim_y = best.best.pos.y;
		dbg.aim_z = best.best.pos.z;
		dbg.min_damage = resolve_min_damage(wpn, best.health, override_active);
	};

	// Keep R8 cocked even without a target.
	if (auto_revolver && !have_best) {
		s_auto_held = false;
		run_auto_revolver(cmd, cs_weapon, false);
		std::strncpy(dbg.block, "no_target_r8", sizeof(dbg.block) - 1);
		publish_debug(dbg);
		return;
	}

	if (!have_best) {
		s_auto_held = false;
		publish_debug(dbg);
		return;
	}

	fill_best_dbg();
	for (int i = 0; i < dbg.target_count; ++i) {
		if (dbg.targets[i].index == best.controller_index)
			dbg.targets[i].is_best = true;
	}

	const vec3_t punch = get_aim_punch(local);
	const vec3_t aim_to_point = calc_angle(eye, best.best.pos);
	const vec3_t cmd_angles = normalize_angles(aim_to_point - punch);
	dbg.punch_x = punch.x; dbg.punch_y = punch.y;
	dbg.ang_x = cmd_angles.x; dbg.ang_y = cmd_angles.y;

	const bool reloading = weapon->m_in_reload() || weapon->m_clip1() <= 0;
	const bool on_ground = (local->m_flags() & FL_ONGROUND) != 0;
	vec3_t vel = local->m_vec_abs_velocity();
	if (g_prediction) {
		if (const auto* pred = g_prediction->get_local_data()) {
			if (pred->m_valid && pred->m_velocity.is_valid())
				vel = pred->m_velocity;
		}
	}
	const float speed2d = vel.length_2d();

	if (wpn.m_auto_scope && data->m_weapon_type() == WEAPONTYPE_SNIPER_RIFLE && cs_weapon->m_zoom_level() <= 0) {
		set_attack2(cmd, true);
	}

	bool ready = can_fire(local, weapon);
	if (auto_revolver) {
		ready = ready && revolver_fire_ready(cs_weapon, local_tick_base());
		if (!ready)
			run_auto_revolver(cmd, cs_weapon, false);
	}
	dbg.can_fire = ready;

	const bool any_stop = want_early_stop || want_between || want_full;
	const float acc_thresh = accuracy_speed_threshold(cs_weapon, data);
	float max_spd_wpn = cs_weapon->get_max_speed();
	{
		const int mode = weapon_mode_of(cs_weapon);
		const int mi = (mode == 1) ? 1 : 0;
		const firing_float_t& ms = data->m_max_speed();
		if (std::isfinite(ms[mi]) && ms[mi] > 1.f)
			max_spd_wpn = ms[mi];
	}
	if (!(max_spd_wpn > 1.f))
		max_spd_wpn = 250.f;

	// Fast Counter-Strafe while above weapon accuracy-speed threshold (no duck).
	const bool need_brake = !reloading && on_ground && any_stop && speed2d > acc_thresh;
	if (need_brake || (want_between && !ready && on_ground && speed2d > acc_thresh)) {
		apply_auto_stop(cmd, local, view, false);
		dbg.is_autostop = true;
	} else if (any_stop && on_ground && speed2d <= acc_thresh && speed2d > 1.f) {
		// Under threshold: zero wish only — already accuracy-valid for fire.
		if (auto* base = cmd->get_base_cmd()) {
			base->set_forwardmove(0.f);
			base->set_leftmove(0.f);
			base->set_upmove(0.f);
		}
		movement_input::set_button(cmd, IN_FORWARD, false);
		movement_input::set_button(cmd, IN_BACK, false);
		movement_input::set_button(cmd, IN_MOVELEFT, false);
		movement_input::set_button(cmd, IN_MOVERIGHT, false);
		movement_input::set_button(cmd, IN_DUCK, false);
		dbg.is_autostop = true;
	}

	if (!ready) {
		std::strncpy(dbg.block, "can_fire=0", sizeof(dbg.block) - 1);
		publish_debug(dbg);
		return;
	}

	// Scout/AWP: don't fire unscoped if autoscope is on — wait for zoom (standing miss).
	if (wpn.m_auto_scope && data->m_weapon_type() == WEAPONTYPE_SNIPER_RIFLE
		&& cs_weapon->m_zoom_level() <= 0) {
		std::strncpy(dbg.block, "scope", sizeof(dbg.block) - 1);
		publish_debug(dbg);
		return;
	}

	// Engine-accurate bloom at the FIRE subtick (~1.0): the brake at subtick 0
	// has already cut speed and the penalty has decayed one exp step toward the
	// new baseline. Always predicted — firing on the live (start-of-tick) read
	// costs 1-2 extra ticks on every peek.
	if (cs_weapon)
		cs_weapon->update_accuracy_penalty();

	float hc_spread = -1.f, hc_inac = -1.f;
	predict_fire_bloom(cs_weapon, data, vel, need_brake ? 1 : 0, &hc_spread, &hc_inac);

	float hc_pct = 0.f;
	const bool hc = hitchance_ok(
		local, best.pawn, best.record, cs_weapon, data, eye, best.best, cmd_angles,
		wpn.m_hitchance, hc_spread, hc_inac, &hc_pct);
	dbg.hitchance_pass = hc;
	for (int i = 0; i < dbg.target_count; ++i) {
		if (dbg.targets[i].is_best)
			dbg.targets[i].hitchance = hc_pct;
	}

	if (!hc) {
		if (any_stop && on_ground && speed2d > acc_thresh) {
			apply_auto_stop(cmd, local, view, false);
			dbg.is_autostop = true;
		}
		std::strncpy(dbg.block, "hitchance", sizeof(dbg.block) - 1);
		if (auto_revolver)
			run_auto_revolver(cmd, cs_weapon, false);
		publish_debug(dbg);
		return;
	}

	if (!g_cfg->ragebot.m_auto_fire) {
		std::strncpy(dbg.block, "auto_fire_off", sizeof(dbg.block) - 1);
		if (auto_revolver)
			run_auto_revolver(cmd, cs_weapon, false);
		publish_debug(dbg);
		return;
	}

	// Same-tick sequence: brake already on cmd (subtick when=0) → angles → +attack (when=0.99).
	write_aim_angles(input, cmd, cmd_angles, g_cfg->ragebot.m_silent, &eye);

	// Extrapolation is aim-point only — never rewrite input history as backtrack.
	if (best.record && !best.best.extrapolated)
		process_backtrack(cmd, best.record);

	if (auto_revolver)
		run_auto_revolver(cmd, cs_weapon, true);
	set_attack(cmd, true);
	stamp_attack_subtick(cmd, 0.99f);

	dbg.fired = true;
	std::strncpy(dbg.block, "fired", sizeof(dbg.block) - 1);
	publish_debug(dbg);
}
