#include <pch/pch.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

#include "../../rendering.hpp"

namespace rendering {

	namespace detail {

		constexpr const char* k_cham_material_names[ ]{
			"liquid", "metallic", "matte", "flat", "bloom", "outlines", "glow", "electric", "distortion", "hologram", "pearl",
			"liquid (iz)", "matte (iz)", "flat (iz)", "bloom (iz)", "outlines (iz)", "glow (iz)", "distortion (iz)", "hologram (iz)"
		};
		constexpr auto k_cham_material_count = static_cast< int >( settings::esp::cham_ids::count );

	} // namespace detail

	void menu::draw_player( float group_w )
	{
		auto& esp = settings::g_esp;
		auto& p = esp.m_player;

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = this->m_body_x;
		const auto body_y = this->m_body_y;
		const auto content_w = this->m_body_w;
		const auto col_w = ( content_w - tokens::gap ) * 0.5f;
		const auto right_x = content_x + col_w + tokens::gap;

		const auto target_idx = ( this->m_target_mode == 1 ) ? 1 : 0;
		auto& ov = p.m_overlay[ target_idx ];

		// ==========================================
		// LEFT COLUMN: Players Groupbox
		// ==========================================
		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( xui::begin_child( "Player ESP##vis_players", col_w, this->m_body_h ) )
		{
			constexpr const char* target_modes[ ]{ "Enemies", "Teammates", "Local" };
			xui::combo( "Target##vis_target", this->m_target_mode, target_modes, 3 );
			xui::layout::separator( );

			const char* master_label = ( this->m_target_mode == 0 ) ? "Enable Enemy ESP" : ( this->m_target_mode == 1 ? "Enable Team ESP" : "Enable Local ESP" );
			xui::checkbox( master_label, ov.enabled );

			// Bounding Box
			xui::checkbox( "Bounding box", ov.m_box.enabled );
			if ( xui::begin_popup( "##box_settings_popup", 220.0f ) )
			{
				constexpr const char* box_styles[ ]{ "Full", "Cornered" };
				xui::combo( "Style##box", ov.m_box.style.value, box_styles, 2 );
				xui::checkbox( "Fill", ov.m_box.fill );
				xui::checkbox( "Outline", ov.m_box.outline );
				xui::slider_float( "Corner length", ov.m_box.corner_length, 2.0f, 20.0f, "%.0f" );
				xui::color_picker( "Visible color##box", ov.m_box.visible_color );
				xui::color_picker( "Occluded color##box", ov.m_box.occluded_color );
				xui::end_popup( );
			}

			// Player Name
			xui::checkbox( "Player name", ov.m_name.enabled );
			if ( xui::begin_popup( "##name_popup", 220.0f ) )
			{
				xui::color_picker( "Name color", ov.m_name.color );
				xui::end_popup( );
			}

			// Health Bar
			xui::checkbox( "Health bar", ov.m_health_bar.enabled );
			if ( xui::begin_popup( "##hp_settings_popup", 220.0f ) )
			{
				constexpr const char* positions[ ]{ "Left", "Top", "Bottom" };
				xui::combo( "Position##hp", ov.m_health_bar.position.value, positions, 3 );
				xui::checkbox( "Gradient##hp", ov.m_health_bar.gradient );
				xui::checkbox( "Show number##hp", ov.m_health_bar.show_value );
				xui::checkbox( "Glow effect##hp", ov.m_health_bar.glow );
				if ( ov.m_health_bar.glow )
				{
					xui::slider_float( "Glow intensity##hp", ov.m_health_bar.glow_strength, 0.1f, 1.0f, "%.2f" );
					xui::color_picker( "Glow color##hp", ov.m_health_bar.glow_color );
				}
				xui::color_picker( "Full color##hp", ov.m_health_bar.full_color );
				xui::color_picker( "Low color##hp", ov.m_health_bar.low_color );
				xui::end_popup( );
			}

			// Ammo Bar
			xui::checkbox( "Ammo bar", ov.m_ammo_bar.enabled );
			if ( xui::begin_popup( "##ammo_settings_popup", 220.0f ) )
			{
				constexpr const char* positions[ ]{ "Bottom", "Left", "Top" };
				xui::combo( "Position##ammo", ov.m_ammo_bar.position.value, positions, 3 );
				xui::checkbox( "Show number##ammo", ov.m_ammo_bar.show_value );
				xui::color_picker( "Color##ammo", ov.m_ammo_bar.full_color );
				xui::end_popup( );
			}

			// Skeleton
			xui::checkbox( "Skeleton", ov.m_skeleton.enabled );
			if ( xui::begin_popup( "##skel_settings_popup", 220.0f ) )
			{
				constexpr const char* modes[ ]{ "Normal", "Backtrack" };
				xui::combo( "Mode##skel", ov.m_skeleton.type.value, modes, 2 );
				xui::slider_float( "Thickness##skel", ov.m_skeleton.thickness, 1.0f, 4.0f, "%.1fpx" );
				xui::color_picker( "Visible color##skel", ov.m_skeleton.visible_color );
				xui::color_picker( "Occluded color##skel", ov.m_skeleton.occluded_color );
				xui::end_popup( );
			}

			// Weapon
			xui::checkbox( "Weapon", ov.m_weapon.enabled );
			if ( xui::begin_popup( "##wep_settings_popup", 220.0f ) )
			{
				constexpr const char* wep_displays[ ]{ "Text", "Icon", "Text + Icon" };
				xui::combo( "Display##wep", ov.m_weapon.display.value, wep_displays, 3 );
				xui::color_picker( "Text color##wep", ov.m_weapon.text_color );
				xui::color_picker( "Icon color##wep", ov.m_weapon.icon_color );
				xui::end_popup( );
			}

			// Flags
			xui::checkbox( "Flags", ov.m_info_flags.enabled );
			if ( xui::begin_popup( "##flags_settings_popup", 220.0f ) )
			{
				constexpr const char* flag_names[ ]{ "Money", "Armor", "Kit", "Scoped", "Defusing", "Flashed", "Ping", "Distance" };
				xui::multicombo( "Enabled flags##vis_flags", ov.m_info_flags.flags, flag_names, settings::esp::player::overlay::info_flags::count );
				xui::color_picker( "Money color", ov.m_info_flags.money_color );
				xui::color_picker( "Armor color", ov.m_info_flags.armor_color );
				xui::color_picker( "Kit color", ov.m_info_flags.kit_color );
				xui::color_picker( "Distance color", ov.m_info_flags.distance_color );
				xui::color_picker( "Scoped color", ov.m_info_flags.scoped_color );
				xui::color_picker( "Defusing color", ov.m_info_flags.defusing_color );
				xui::color_picker( "Flashed color", ov.m_info_flags.flashed_color );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		// ==========================================
		// RIGHT COLUMN: Player ESP Configuration
		// ==========================================
		xui::layout::set_cursor( right_x - wx, body_y - wy );

		if ( xui::begin_child( "Player Configuration##vis_player_cfg", col_w, this->m_body_h ) )
		{
			// Offscreen arrows
			xui::checkbox( "Offscreen arrows", ov.m_oof_arrow.enabled );
			if ( xui::begin_popup( "##oof_settings_popup", 220.0f ) )
			{
				xui::slider_float( "Radius X##oof", ov.m_oof_arrow.radius_x, 50.0f, 500.0f, "%.0fpx" );
				xui::slider_float( "Radius Y##oof", ov.m_oof_arrow.radius_y, 50.0f, 500.0f, "%.0fpx" );
				xui::slider_float( "Width##oof", ov.m_oof_arrow.width, 6.0f, 30.0f, "%.0fpx" );
				xui::slider_float( "Height##oof", ov.m_oof_arrow.height, 6.0f, 30.0f, "%.0fpx" );
				xui::checkbox( "Glow effect##oof", ov.m_oof_arrow.glow );
				xui::color_picker( "Visible color##oof", ov.m_oof_arrow.visible_color );
				xui::color_picker( "Occluded color##oof", ov.m_oof_arrow.occluded_color );
				xui::end_popup( );
			}

			// Snaplines
			xui::checkbox( "Snaplines", ov.m_snapline.enabled );
			if ( xui::begin_popup( "##snap_settings_popup", 220.0f ) )
			{
				constexpr const char* positions[ ]{ "Bottom", "Center", "Top" };
				xui::combo( "Origin##snap", ov.m_snapline.position.value, positions, 3 );
				xui::color_picker( "Color##snap", ov.m_snapline.color );
				xui::end_popup( );
			}

			// Head circle
			xui::checkbox( "Head circle", ov.m_head_circle.enabled );
			if ( xui::begin_popup( "##hc_settings_popup", 220.0f ) )
			{
				xui::slider_float( "Radius##hc", ov.m_head_circle.radius, 2.0f, 20.0f, "%.1f" );
				xui::color_picker( "Color##hc", ov.m_head_circle.color );
				xui::end_popup( );
			}

			xui::layout::separator( );

			xui::slider_float( "Maximum render range", ov.max_range, 20.0f, 1000.0f, "%.0fm" );

			xui::end_child( );
		}
	}

	void menu::draw_colored_models( float group_w )
	{
		auto& esp = settings::g_esp;
		auto& p = esp.m_player;

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = this->m_body_x;
		const auto body_y = this->m_body_y;
		const auto content_w = this->m_body_w;
		const auto col_w = ( content_w - tokens::gap ) * 0.5f;
		const auto right_x = content_x + col_w + tokens::gap;

		auto& chams = ( this->m_target_mode == 1 ) ? p.m_chams.team : ( this->m_target_mode == 2 ? p.m_chams.local : p.m_chams.enemy );

		// ==========================================
		// LEFT COLUMN: Player Chams
		// ==========================================
		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( xui::begin_child( "Player Models##vis_chams_player", col_w, this->m_body_h ) )
		{
			constexpr const char* target_modes[ ]{ "Enemies", "Teammates", "Local" };
			xui::combo( "Target##chams_target", this->m_target_mode, target_modes, 3 );
			xui::layout::separator( );

			// Player chams
			xui::checkbox( "Player chams", chams.enabled );
			if ( xui::begin_popup( "##chams_settings_popup", 220.0f ) )
			{
				xui::text( "Visible model", tokens::col_text );
				xui::combo( "Material##ch_vis", chams.primary.material.value, detail::k_cham_material_names, detail::k_cham_material_count );
				xui::color_picker( "Visible color##ch", chams.primary.color );

				xui::layout::separator( );

				xui::checkbox( "Behind wall (occluded)", chams.secondary.enabled );
				if ( chams.secondary.enabled )
				{
					xui::combo( "Material##ch_occ", chams.secondary.material.value, detail::k_cham_material_names, detail::k_cham_material_count );
					xui::color_picker( "Occluded color##ch", chams.secondary.color );
				}

				xui::layout::separator( );

				xui::checkbox( "Overlay glow layer", chams.overlay.enabled );
				if ( chams.overlay.enabled )
				{
					xui::combo( "Material##ch_ovl", chams.overlay.material.value, detail::k_cham_material_names, detail::k_cham_material_count );
					xui::color_picker( "Overlay color##ch", chams.overlay.color );
				}
				xui::end_popup( );
			}

			// Backtrack Chams
			xui::checkbox( "Backtrack chams", p.m_chams.backtrack.enabled );
			if ( xui::begin_popup( "##bt_chams_popup", 220.0f ) )
			{
				xui::combo( "Material##bt_ch", p.m_chams.backtrack.primary.material.value, detail::k_cham_material_names, detail::k_cham_material_count );
				xui::color_picker( "Color##bt_ch", p.m_chams.backtrack.primary.color );
				xui::end_popup( );
			}

			// Ragdoll Chams
			xui::checkbox( "Ragdoll chams", p.m_chams.enemy_ragdoll.enabled );
			if ( xui::begin_popup( "##rag_chams_popup", 220.0f ) )
			{
				xui::combo( "Material##rag_ch", p.m_chams.enemy_ragdoll.primary.material.value, detail::k_cham_material_names, detail::k_cham_material_count );
				xui::color_picker( "Color##rag_ch", p.m_chams.enemy_ragdoll.primary.color );
				xui::end_popup( );
			}

			xui::end_child( );
		}

		// ==========================================
		// RIGHT COLUMN: Viewmodel & Hands Chams
		// ==========================================
		xui::layout::set_cursor( right_x - wx, body_y - wy );

		if ( xui::begin_child( "Viewmodel Models##vis_chams_vm", col_w, this->m_body_h ) )
		{
			// Viewmodel Arms Chams
			xui::checkbox( "Arms & hand chams", esp.m_viewmodel.arms.enabled );
			if ( xui::begin_popup( "##arms_chams_popup", 220.0f ) )
			{
				xui::combo( "Material##arms_ch", esp.m_viewmodel.arms.primary.material.value, detail::k_cham_material_names, detail::k_cham_material_count );
				xui::color_picker( "Color##arms_ch", esp.m_viewmodel.arms.primary.color );
				xui::end_popup( );
			}

			// Viewmodel Weapon Chams
			xui::checkbox( "Weapon model chams", esp.m_viewmodel.weapon.enabled );
			if ( xui::begin_popup( "##wep_chams_popup", 220.0f ) )
			{
				xui::combo( "Material##wep_ch", esp.m_viewmodel.weapon.primary.material.value, detail::k_cham_material_names, detail::k_cham_material_count );
				xui::color_picker( "Color##wep_ch", esp.m_viewmodel.weapon.primary.color );
				xui::end_popup( );
			}

			xui::end_child( );
		}
	}

} // namespace rendering