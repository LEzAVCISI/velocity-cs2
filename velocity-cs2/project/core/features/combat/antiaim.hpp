#pragma once
#include <memory>
#include "../../sdk/typedefs/vec_t.hpp"

class c_user_cmd;
class i_csgo_input;

// Anti-aim: writes fake view angles into the OUTGOING usercmd only (base
// viewangles + every input-history entry) — the local camera is never touched,
// so what you see stays camera-relative (cs2-internal style separation).
// Movement correction rotates the wish moves from camera frame into the sent
// (fake) frame so WASD keeps moving you where the CAMERA points.
class c_antiaim {
public:
	void run(i_csgo_input* input, c_user_cmd* cmd);

private:
	float m_spin_yaw = 0.f;
};

inline const auto g_antiaim = std::make_unique<c_antiaim>();
