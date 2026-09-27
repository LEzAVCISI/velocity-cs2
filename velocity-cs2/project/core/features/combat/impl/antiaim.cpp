#include "antiaim.hpp"

#include "../../core/main.hpp"
#include "../../sdk/memsafe.hpp"
#include "../../sdk/valve/interfaces/interfaces.hpp"
#include "../../sdk/valve/classes/c_cs_player_pawn.hpp"
#include "../../sdk/valve/interfaces/vtables/i_csgo_input.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr float k_pi = 3.14159265358979323846f;

float normalize_yaw(float yaw) {
	while (yaw > 180.f) yaw -= 360.f;
	while (yaw < -180.f) yaw += 360.f;
	return yaw;
}
} // namespace

void c_antiaim::run(i_csgo_input* input, c_user_cmd* cmd) {
	(void)input; // camera is intentionally never written
	if (!g_cfg || !g_cfg->antiaim.m_enabled || !cmd)
		return;
	if (!g_ctx || !g_ctx->m_local_pawn)
		return;

	auto* local = reinterpret_cast<c_cs_player_pawn*>(g_ctx->m_local_pawn);
	if (!memsafe::valid_entity(local) || !local->is_alive() || local->m_health() <= 0)
		return;

	// Never fight a shot: with +attack in this cmd the server fires at the cmd
	// angles — those must stay on the ragebot/manual aim, not the anti-aim.
	if (cmd->m_button_state.m_button_state & IN_ATTACK)
		return;
	if (local->is_throwing())
		return;

	auto* base = cmd->get_base_cmd();
	if (!base)
		return;

	// Camera yaw — the frame every movement feature (and the user) reasoned in.
	float cam_pitch = 0.f;
	float cam_yaw = 0.f;
	if (base->has_viewangles()) {
		cam_pitch = base->viewangles().x();
		cam_yaw = base->viewangles().y();
	} else if (g_interfaces && g_interfaces->m_csgo_input) {
		const vec3_t va = g_interfaces->m_csgo_input->get_view_angles();
		cam_pitch = va.x;
		cam_yaw = va.y;
	}

	const auto& aa = g_cfg->antiaim;

	float yaw = cam_yaw;
	switch (aa.m_yaw) {
	case 0: break;                       // forward
	case 1: yaw += 180.f; break;         // backward
	case 2:                              // spin
		m_spin_yaw = normalize_yaw(m_spin_yaw + static_cast<float>(std::clamp(aa.m_spin_speed, 1, 180)));
		yaw = m_spin_yaw;
		break;
	case 3: {                            // jitter (alternate sides per cmd)
		const float half = static_cast<float>(std::clamp(aa.m_jitter_range, 0, 180)) * 0.5f;
		yaw += 180.f + ((cmd->m_command_number & 1) ? half : -half);
		break;
	}
	default: break;
	}
	yaw = normalize_yaw(yaw + static_cast<float>(std::clamp(aa.m_yaw_add, -180, 180)));

	const float pitch = (aa.m_pitch == 1) ? 89.f : cam_pitch;

	// Write the fake angles into the outgoing cmd ONLY.
	if (auto* va = base->mutable_viewangles()) {
		va->set_x(pitch);
		va->set_y(yaw);
		va->set_z(0.f);
	}
	for (int i = 0; i < cmd->pb.input_history_size(); ++i) {
		auto* hist = cmd->pb.mutable_input_history(i);
		if (!hist)
			continue;
		if (auto* va = hist->mutable_view_angles()) {
			va->set_x(pitch);
			va->set_y(yaw);
			va->set_z(0.f);
		}
	}

	if (!aa.m_movement_correction)
		return;

	// Movement correction: moves were authored in the CAMERA frame; the server
	// interprets them in the SENT (fake) frame. Rotate by (cam - fake) so the
	// world-space wish direction is unchanged.
	const float rot = normalize_yaw(cam_yaw - yaw) * (k_pi / 180.f);
	const float cr = std::cosf(rot);
	const float sr = std::sinf(rot);

	auto rotate_pair = [&](float f, float s, float& of, float& os) {
		of = f * cr - s * sr;
		os = f * sr + s * cr;
	};

	const float f0 = base->has_forwardmove() ? base->forwardmove() : 0.f;
	const float s0 = base->has_leftmove() ? base->leftmove() : 0.f;
	if (std::fabs(f0) > 1e-4f || std::fabs(s0) > 1e-4f) {
		float nf = 0.f, ns = 0.f;
		rotate_pair(f0, s0, nf, ns);
		base->set_forwardmove(nf);
		base->set_leftmove(ns);
	}

	// Subtick analog steps (autostop / fast counter-strafe) live in the same
	// frame — rotate them too or braking drifts sideways under anti-aim.
	for (int i = 0; i < base->subtick_moves_size(); ++i) {
		auto* step = base->mutable_subtick_moves(i);
		if (!step)
			continue;
		const float sf = step->has_analog_forward_delta() ? step->analog_forward_delta() : 0.f;
		const float sl = step->has_analog_left_delta() ? step->analog_left_delta() : 0.f;
		if (std::fabs(sf) < 1e-4f && std::fabs(sl) < 1e-4f)
			continue;
		float nf = 0.f, ns = 0.f;
		rotate_pair(sf, sl, nf, ns);
		step->set_analog_forward_delta(nf);
		step->set_analog_left_delta(ns);
	}
}
