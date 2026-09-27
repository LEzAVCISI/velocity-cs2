#pragma once

namespace features::movement {

	class bhop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class airstrafe
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void store_angles( );

	private:
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		void rotate_movement( proto::base_usercmd_pb* base, float target_yaw, float view_yaw ) const;
		void rotate_to_stop( proto::base_usercmd_pb* base, const math::vector3& velocity ) const;

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		bool m_side_switch{};
		math::vector3 m_angles{};
	};

	class jumpbug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }
		[[nodiscard]] float landing_fraction( ) const { return this->m_landing_fraction; }

	private:
		[[nodiscard]] float get_impulse_mul( std::uintptr_t local_pawn ) const;

		float m_landing_fraction{ 1.0f };
		bool m_active_this_tick{ false };
	};

	class fastladder
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgejump
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgestop
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	class edgebug
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		void on_render( xdraw::draw_list& draw_list );

		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }
		[[nodiscard]] bool assist_active( ) const { return this->m_assist_active; }
		[[nodiscard]] int assist_ticks( ) const { return this->m_assist_ticks; }

	private:
		bool m_active_this_tick{ false };
		bool m_assist_active{ false };
		int m_assist_ticks{ 0 };
	};

	class slowwalk
	{
	public:
		void on_create_move( systems::input::usercmd* cmd ) const;
	};

	// ================================================================
	// pixelsurf — pixel-walk surface detection and angle correction
	//
	// detects pixel-walk surfaces by tracing downward with tiny offsets
	// and applies corrective view angles / button flags to maintain
	// the pixel walk. reversed from sub_1A29A0, sub_1A2180, sub_3A24B0.
	// ================================================================
	class pixelsurf
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );

		[[nodiscard]] bool active_this_tick( ) const { return this->m_active_this_tick; }
		[[nodiscard]] bool is_slipping( ) const { return this->m_is_slipping; }

	private:
		[[nodiscard]] bool detect_pixel_surface(
			const math::vector3& origin,
			const math::vector3& velocity,
			const systems::tracing::bbox_collision& bbox,
			const systems::tracing::player_movement_filter& filter,
			std::uintptr_t movement_services,
			float sv_standable_normal );

		void apply_angle_correction(
			systems::input::usercmd* cmd,
			const math::vector3& velocity,
			float target_yaw );

		bool m_active_this_tick{ false };
		bool m_is_slipping{ false };
		std::uint8_t m_edge_type{ 0 };     // 0 = jump-based, 1 = attack2-based
		std::uint8_t m_edge_direction{ 0 }; // 0 = left, 1 = right
		float m_target_yaw{ 0.0f };
		float m_target_pitch{ 0.0f };
		float m_last_vel_z{ 0.0f };
	};

	class subtick_strafer
	{
	public:
		void on_create_move( systems::input::usercmd* cmd );
		[[nodiscard]] bool is_active( ) const;
		[[nodiscard]] bool handled_this_tick( ) const { return this->m_handled_this_tick; }

	private:
		void quantized_path( systems::input::usercmd* cmd );
		[[nodiscard]] bool apply_yaw_subtick( proto::base_usercmd_pb* base, float when, float yaw_delta ) const;
		void check_button( std::uintptr_t current_buttons, std::uintptr_t button );
		[[nodiscard]] static math::vector2 movement_from_buttons( std::uintptr_t pressed );

		std::uintptr_t m_last_buttons{};
		std::uintptr_t m_last_pressed{};
		int m_substep_counter{};
		bool m_handled_this_tick{};
	};

} // namespace features::movement