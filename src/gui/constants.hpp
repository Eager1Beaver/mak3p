#pragma once

namespace audio_converter::gui::metrics {
inline constexpr int minimum_width = 500;
inline constexpr int minimum_height = 640;
inline constexpr int margin = 12;
inline constexpr int gap = 8;
inline constexpr int menu_height = 28;
inline constexpr int control_height = 30;
inline constexpr int label_height = 18;
inline constexpr int row_height = 78;
inline constexpr int scrollbar_width = 12;
inline constexpr int font_size = 13;
inline constexpr int small_font_size = 11;
inline constexpr int title_font_size = 20;
inline constexpr double status_width_ratio = 0.30;
inline constexpr double refresh_seconds = 0.10;
inline constexpr int about_width = 460;
inline constexpr int about_height = 310;
inline constexpr int about_header_height = 50;
inline constexpr int help_width = 480;
inline constexpr int help_height = 490;
inline constexpr int settings_panel_height = 338;
inline constexpr int menu_width = 112;
inline constexpr int hint_offset = 5;
inline constexpr int hint_height = 18;
inline constexpr int input_buttons_offset = 31;
inline constexpr int audio_settings_offset = 89;
inline constexpr int output_settings_offset = 145;
inline constexpr int import_options_offset = 184;
inline constexpr int log_option_offset = 209;
inline constexpr int metadata_offset = 236;
inline constexpr int summary_offset = 259;
inline constexpr int notice_offset = 280;
inline constexpr int engine_action_offset = 276;
inline constexpr int option_height = 22;
inline constexpr int primary_height = 34;
inline constexpr int count_width = 64;
inline constexpr int row_name_baseline = 21;
inline constexpr int row_properties_baseline = 41;
inline constexpr int row_progress_offset = 57;
inline constexpr int row_remaining_baseline = 63;
inline constexpr int row_padding = 9;
inline constexpr int font_face = 0; // FLTK Helvetica
inline constexpr int resize_save_delay_ms = 500;
} // namespace audio_converter::gui::metrics

namespace audio_converter::gui::colors {
inline constexpr unsigned background = 0xf8f9fb00;
inline constexpr unsigned control = 0xebeef300;
inline constexpr unsigned queue = 0xf8f9fa00;
inline constexpr unsigned row_odd = 0xe8ebf000;
inline constexpr unsigned row_even = 0xf6f7f900;
inline constexpr unsigned text = 0x1e263000;
inline constexpr unsigned muted = 0x4d556000;
inline constexpr unsigned accent = 0x2c64aa00;
inline constexpr unsigned error = 0xa0232300;
inline constexpr unsigned warning = 0x855b0a00;
inline constexpr unsigned border = 0xc4cbd500;
inline constexpr unsigned progress_track = 0xcfd6df00;
} // namespace audio_converter::gui::colors
