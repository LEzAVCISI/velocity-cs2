#pragma once
#include <core/systems/systems.hpp>

namespace features::combat {

	class resolver
	{
	public:
		enum class side : std::uint8_t { zero = 0, left = 1, right = 2 };

		struct player_state
		{
			std::uintptr_t pawn{};
			float resolved_yaw{ 0.0f };
			float eye_yaw{ 0.0f };
			float last_eye_yaw{ 0.0f };
			side current_side{ side::zero };
			int miss_count{ 0 };
			int brute_index{ 0 };
			float confidence{ 1.0f };
			float last_update_time{ 0.0f };
			bool is_moving{ false };
			bool is_valid{ false };
		};

		void on_create_move( systems::input::usercmd* cmd );
		void update( );

		[[nodiscard]] float get_resolved_yaw( std::uintptr_t pawn ) const;
		[[nodiscard]] side get_side( std::uintptr_t pawn ) const;
		[[nodiscard]] float get_confidence( std::uintptr_t pawn ) const;
		[[nodiscard]] bool is_low_confidence( std::uintptr_t pawn ) const;
		[[nodiscard]] bool should_prefer_body( std::uintptr_t pawn ) const;

		void on_shot_miss( std::uintptr_t pawn );
		void on_shot_hit( std::uintptr_t pawn );
		void on_player_hurt( std::uintptr_t pawn, bool is_hit );

		// Used by rage to apply correction to records if needed
		[[nodiscard]] float get_yaw_correction( std::uintptr_t pawn ) const;

		void reset( );

	private:
		void update_player( std::uintptr_t pawn, float cur_time );
		[[nodiscard]] side detect_desync_side( std::uintptr_t pawn, float eye_yaw, const math::vector3& velocity, float& out_confidence ) const;
		[[nodiscard]] bool try_read_anim_layers( std::uintptr_t pawn, float& out_body_yaw_delta ) const;

		mutable std::shared_mutex m_mtx{};
		std::unordered_map<std::uintptr_t, player_state> m_states{};
	};

} // namespace features::combat
