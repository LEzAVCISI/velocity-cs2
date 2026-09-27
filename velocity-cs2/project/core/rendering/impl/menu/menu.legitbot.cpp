#include <pch/pch.hpp>
#include <core/settings.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* hitbox_names_legit[ ]{ "head", "chest", "stomach", "arms", "legs" };

	} // namespace detail

	void menu::draw_legitbot( float group_w ) const
	{
		auto& s = settings::g_combat;
		auto& lb = s.m_legitbot;

		const auto current_group = std::clamp( this->m_subtab, 0, 5 );
		auto& wg = lb.groups[ current_group ];

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = this->m_body_x;
		const auto body_y = this->m_body_y;
		const auto content_w = this->m_body_w;
		const auto col_w = ( content_w - tokens::gap ) * 0.5f;
		const auto right_x = content_x + col_w + tokens::gap;

		const auto aimbot_h = 285.0f;
		const auto rcs_h = this->m_body_h - aimbot_h - tokens::gap;

		const auto trigger_h = 240.0f;
		const auto other_h = this->m_body_h - trigger_h - tokens::gap;

		// ==========================================
		// LEFT COLUMN: Aimbot + Recoil System
		// ==========================================
		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( xui::begin_child( "Aimbot##lb_aimbot", col_w, aimbot_h ) )
		{
			xui::checkbox( "Master enable", lb.enabled );
			xui::checkbox( "Enabled", wg.aimbot );

			xui::slider_float( "Field of view", wg.fov, 0.5f, 30.0f, "%.1f°" );
			xui::slider_int( "Smooth", wg.smooth, 0, 100, "%d" );
			xui::multicombo( "Target hitboxes", wg.hitboxes, detail::hitbox_names_legit, 5 );

			xui::checkbox( "Visualize FOV", wg.visualize_fov );
			if ( xui::begin_popup( "##fov_color_popup", 220.0f ) )
			{
				xui::color_picker( "FOV color", wg.fov_color );
				xui::end_popup( );
			}

			xui::checkbox( "Force sniper crosshair", lb.force_sniper_crosshair );
			xui::slider_int( "Target switch delay", lb.target_switch_delay, 0, 500, "%d ms" );

			xui::end_child( );
		}

		const auto rcs_y = body_y + aimbot_h + tokens::gap;
		xui::layout::set_cursor( content_x - wx, rcs_y - wy );

		if ( xui::begin_child( "Recoil Control##lb_rcs", col_w, rcs_h ) )
		{
			xui::checkbox( "Recoil control", wg.rcs );
			if ( xui::begin_popup( "##rcs_popup", 220.0f ) )
			{
				xui::slider_int( "Min pitch scale", wg.rcs_min, 50, 150, "%d%%" );
				xui::slider_int( "Max yaw scale", wg.rcs_max, 50, 150, "%d%%" );
				xui::end_popup( );
			}

			xui::checkbox( "Standalone RCS", wg.standalone_rcs );
			if ( xui::begin_popup( "##srcs_popup", 220.0f ) )
			{
				xui::slider_int( "Strength", wg.standalone_rcs_strength, 0, 100, "%d%%" );
				xui::slider_int( "Min pitch scale", wg.standalone_rcs_min, 50, 150, "%d%%" );
				xui::slider_int( "Max yaw scale", wg.standalone_rcs_max, 50, 150, "%d%%" );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		// ==========================================
		// RIGHT COLUMN: Triggerbot + Other / Backtrack
		// ==========================================
		xui::layout::set_cursor( right_x - wx, body_y - wy );

		if ( xui::begin_child( "Triggerbot##lb_trigger", col_w, trigger_h ) )
		{
			xui::checkbox( "Triggerbot", wg.triggerbot );
			xui::slider_int( "Reaction delay", wg.trigger_delay, 0, 250, "%d ms" );
			xui::slider_int( "Hit chance", wg.trigger_hitchance, 0, 100, "%d%%" );
			xui::checkbox( "Head only", wg.trigger_head_only );
			xui::checkbox( "Seed mode", wg.give_me_your_seed );

			xui::end_child( );
		}

		const auto other_y = body_y + trigger_h + tokens::gap;
		xui::layout::set_cursor( right_x - wx, other_y - wy );

		if ( xui::begin_child( "Other##lb_other", col_w, other_h ) )
		{
			xui::checkbox( "Backtrack", wg.backtrack );
			if ( xui::begin_popup( "##bt_popup", 220.0f ) )
			{
				xui::slider_int( "Ticks", wg.backtrack_ticks, 1, 16, "%d tick(s)" );
				xui::slider_float( "Max FOV", wg.backtrack_fov, 0.5f, 30.0f, "%.1f°" );
				xui::checkbox( "Standalone backtrack", wg.standalone_backtrack );
				xui::end_popup( );
			}

			xui::checkbox( "Automatic wall penetration", wg.autowall );
			if ( xui::begin_popup( "##aw_popup", 220.0f ) )
			{
				xui::slider_int( "Minimum damage", wg.min_damage, 1, 125, "%d" );
				xui::end_popup( );
			}

			xui::end_child( );
		}
	}

} // namespace rendering