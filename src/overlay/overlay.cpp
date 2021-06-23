#include "overlay.h"

#include <SDL.h>

#include <functional>
#include <nfd.h>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl.h"

#include "disasm.h"
#include "ram_dump.h"
#include "util.h"
#include "vram_dump.h"

#include "audio.h"
#include "cpu/fake6502.h"
#include "debugger.h"
#include "display.h"
#include "glue.h"
#include "joystick.h"
#include "keyboard.h"
#include "smc.h"
#include "symbols.h"
#include "timing.h"
#include "vera/sdcard.h"
#include "vera/vera_psg.h"
#include "vera/vera_video.h"

bool Show_imgui_demo       = false;
bool Show_memory_dump_1    = false;
bool Show_memory_dump_2    = false;
bool Show_monitor          = false;
bool Show_VERA_monitor     = false;
bool Show_VERA_palette     = false;
bool Show_VERA_layers      = false;
bool Show_VERA_sprites     = false;
bool Show_VERA_PSG_monitor = false;

imgui_vram_dump vram_dump;

static void draw_debugger_cpu_status()
{
	ImGui::BeginGroup();
	{
		ImGui::TextDisabled("Status");
		ImGui::SameLine();
		ImGui::Dummy(ImVec2(0.0f, 19.0f));
		ImGui::Separator();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 3));

		char const *names[] = { "N", "V", "-", "B", "D", "I", "Z", "C" };
		uint8_t     mask    = 0x80;
		int         n       = 0;
		while (mask > 0) {
			ImGui::BeginGroup();
			ImGui::Text(names[n]);
			if (ImGui::SmallButton(status & mask ? "1" : "0")) {
				status ^= mask;
			}
			mask >>= 1;
			++n;
			ImGui::EndGroup();
			ImGui::SameLine();
		}

		ImGui::NewLine();
		ImGui::NewLine();

		ImGui::PopStyleVar();

		ImGui::BeginGroup();
		{
			ImGui::InputHexLabel("A", a);
			ImGui::InputHexLabel("X", x);
			ImGui::InputHexLabel("Y", y);
		}
		ImGui::EndGroup();

		ImGui::SameLine();

		ImGui::BeginGroup();
		{
			ImGui::InputHexLabel("PC", pc);
			ImGui::InputHexLabel("SP", sp);
		}
		ImGui::EndGroup();

		ImGui::NewLine();

		auto registers = [&](int start, int end) {
			ImGui::PushItemWidth(width_uint16);

			char label[4] = "r0";
			for (int i = start; i <= end; ++i) {
				sprintf(label, i < 10 ? " r%d" : "r%d", i);
				ImGui::Text(label);
				ImGui::SameLine();
				uint16_t value = (int)get_mem16(2 + (i << 1), 0);
				if (ImGui::InputHex(i, value)) {
					debug_write6502(2 + (i << 1), 0, value & 0xff);
					debug_write6502(3 + (i << 1), 0, value >> 8);
				}
			}

			ImGui::PopItemWidth();
		};

		ImGui::TextDisabled("API Registers");
		ImGui::Separator();

		ImGui::BeginGroup();
		registers(0, 5);
		ImGui::EndGroup();
		ImGui::SameLine();

		ImGui::BeginGroup();
		registers(6, 10);
		ImGui::NewLine();
		registers(11, 15);
		ImGui::EndGroup();
	}
	ImGui::EndGroup();
}

static void draw_debugger_vera_status()
{
	ImGui::BeginGroup();
	{
		ImGui::TextDisabled("VERA Settings");
		ImGui::SameLine();
		ImGui::Dummy(ImVec2(0.0f, 19.0f));
		ImGui::Separator();

		// ImGuiInputTextFlags_ReadOnly

		char hex[7];

		{
			uint32_t value;

			value = vera_video_get_data_addr(0);
			if (ImGui::InputHexLabel<uint32_t, 24>("Data0 Address", value)) {
				vera_video_set_data_addr(0, value);
			}

			value = vera_video_get_data_addr(1);
			if (ImGui::InputHexLabel<uint32_t, 24>("Data1 Address", value)) {
				vera_video_set_data_addr(1, value);
			}

			ImGui::NewLine();

			value = vera_debug_video_read(3);
			if (ImGui::InputHexLabel<uint32_t, 24>("Data0", value)) {
				vera_video_space_write(vera_video_get_data_addr(0), value);
			}

			value = vera_debug_video_read(4);
			if (ImGui::InputHexLabel<uint32_t, 24>("Data1", value)) {
				vera_video_space_write(vera_video_get_data_addr(1), value);
			}
		}

		ImGui::NewLine();

		ImGui::PushItemWidth(width_uint8);
		{
			uint8_t dc_video       = vera_video_get_dc_video();
			uint8_t dc_video_start = dc_video;

			static constexpr const char *modes[] = { "Disabled", "VGA", "NTSC", "RGB interlaced, composite, via VGA connector" };

			ImGui::Text("Output Mode");
			ImGui::SameLine();

			if (ImGui::BeginCombo(modes[dc_video & 3], modes[dc_video & 3])) {
				for (uint8_t i = 0; i < 4; ++i) {
					const bool selected = ((dc_video & 3) == i);
					if (ImGui::Selectable(modes[i], selected)) {
						dc_video = (dc_video & ~3) | i;
					}
				}
				ImGui::EndCombo();
			}

			// Other dc_video flags
			{
				static constexpr struct {
					const char *name;
					uint8_t     flag;
				} video_options[] = { { "No Chroma", 0x04 }, { "Layer 0", 0x10 }, { "Layer 1", 0x20 }, { "Sprites", 0x40 } };

				for (auto &option : video_options) {
					bool selected = dc_video & option.flag;
					if (ImGui::Checkbox(option.name, &selected)) {
						dc_video ^= option.flag;
					}
				}
			}

			if (dc_video_start != dc_video) {
				vera_video_set_dc_video(dc_video);
			}
		}
		ImGui::NewLine();
		{
			ImGui::Text("Scale");
			ImGui::SameLine();

			sprintf(hex, "%02X", (int)vera_video_get_dc_hscale());
			if (ImGui::InputText("H", hex, 5, hex_flags)) {
				vera_video_set_dc_hscale(parse<8>(hex));
			}

			ImGui::SameLine();

			sprintf(hex, "%02X", (int)vera_video_get_dc_vscale());
			if (ImGui::InputText("V", hex, 3, hex_flags)) {
				vera_video_set_dc_vscale(parse<8>(hex));
			}
		}

		ImGui::Text("DC Borders");
		ImGui::Dummy(ImVec2(width_uint8, 0));
		ImGui::SameLine();
		ImGui::PushID("vstart");
		sprintf(hex, "%02X", (int)vera_video_get_dc_vstart());
		if (ImGui::InputText("", hex, 3, hex_flags)) {
			vera_video_set_dc_vstart(parse<8>(hex));
		}
		ImGui::PopID();
		ImGui::PushID("hstart");
		sprintf(hex, "%02X", (int)vera_video_get_dc_hstart());
		if (ImGui::InputText("", hex, 3, hex_flags)) {
			vera_video_set_dc_hstart(parse<8>(hex));
		}
		ImGui::PopID();
		ImGui::SameLine();
		ImGui::Dummy(ImVec2(width_uint8, 0));
		ImGui::SameLine();
		ImGui::PushID("hstop");
		sprintf(hex, "%02X", (int)vera_video_get_dc_hstop());
		if (ImGui::InputText("", hex, 3, hex_flags)) {
			vera_video_set_dc_hstop(parse<8>(hex));
		}
		ImGui::PopID();
		ImGui::Dummy(ImVec2(width_uint8, 0));
		ImGui::SameLine();
		ImGui::PushID("vstop");
		sprintf(hex, "%02X", (int)vera_video_get_dc_vstop());
		if (ImGui::InputText("", hex, 3, hex_flags)) {
			vera_video_set_dc_vstop(parse<8>(hex));
		}
		ImGui::PopID();

		ImGui::PopItemWidth();
	}
	ImGui::EndGroup();
}

static void draw_debugger_vera_palette()
{
	ImGui::BeginGroup();
	{
		ImGui::TextDisabled("Palette");
		ImGui::SameLine();
		ImGui::Dummy(ImVec2(0.0f, 19.0f));
		ImGui::Separator();

		const uint32_t *palette = vera_video_get_palette_argb32();
		for (int i = 0; i < 256; ++i) {
			const uint8_t *p = reinterpret_cast<const uint8_t *>(&palette[i]);
			ImVec4         c{ (float)(p[2]) / 255.0f, (float)(p[1]) / 255.0f, (float)(p[0]) / 255.0f, 1.0f };
			ImGui::ColorButton("", c, ImGuiColorEditFlags_NoBorder, ImVec2(16, 16));
			if (i % 16 != 15) {
				ImGui::SameLine();
			}
		}
	}
	ImGui::EndGroup();
}

static void draw_debugger_vera_sprite()
{
	static icon_set sprite_preview;
	static uint8_t  uncompressed_vera_memory[64 * 64];
	static uint32_t sprite_pixels[64 * 64];

	static ImU8     sprite_id  = 0;
	static uint64_t sprite_sig = 0;

	static const ImU8  incr_one8  = 1;
	static const ImU8  incr_hex8  = 16;
	static const ImU16 incr_one16 = 1;
	static const ImU16 incr_ten16 = 10;
	static const ImU16 incr_hex16 = 16;
	static const ImU16 incr_addr  = 32;
	static const ImU16 fast_addr  = 32 * 16;

	static bool reload = true;

	ImGui::BeginGroup();
	{
		if (ImGui::InputScalar("Sprite", ImGuiDataType_U8, &sprite_id, &incr_one8, nullptr, "%d")) {
			reload = true;
		}

		uint8_t sprite_data[8];
		memcpy(sprite_data, vera_video_get_sprite_data(sprite_id), 8);

		vera_video_sprite_properties sprite_props;
		memcpy(&sprite_props, vera_video_get_sprite_properties(sprite_id), sizeof(vera_video_sprite_properties));

		if (sprite_sig != *reinterpret_cast<const uint64_t *>(sprite_data)) {
			sprite_sig = *reinterpret_cast<const uint64_t *>(sprite_data);
			reload     = true;
		}

		const uint32_t num_dots = 1 << (sprite_props.sprite_width_log2 + sprite_props.sprite_height_log2);
		vera_video_get_expanded_vram(sprite_props.sprite_address, 4 << sprite_props.color_mode, uncompressed_vera_memory, num_dots);
		const uint32_t *palette = vera_video_get_palette_argb32();
		for (uint32_t i = 0; i < num_dots; ++i) {
			sprite_pixels[i] = (palette[uncompressed_vera_memory[i] + sprite_props.palette_offset] << 8) | 0xff;
		}
		if (reload) {
			sprite_preview.load_memory(sprite_pixels, sprite_props.sprite_width, sprite_props.sprite_height, sprite_props.sprite_width, sprite_props.sprite_height);
			reload = false;
		} else {
			sprite_preview.update_memory(sprite_pixels);
		}


		ImGui::BeginGroup();
		{
			ImGui::TextDisabled("Sprite Preview");
			ImGui::Image((void *)(intptr_t)sprite_preview.get_texture_id(), ImVec2(128.0f, 128.0f), sprite_preview.get_top_left(0), sprite_preview.get_bottom_right(0));
		}
		ImGui::EndGroup();
		ImGui::SameLine();
		ImGui::BeginGroup();
		{
			ImGui::TextDisabled("Raw Bytes");

			for (int i = 0; i < 8; ++i) {
				if (i) {
					ImGui::SameLine();
				}
				if (ImGui::InputHex(i, sprite_data[i])) {
					vera_video_space_write(0x1FC00 + 8 * sprite_id + i, sprite_data[i]);
				}
				//if ((i & 1) != 1) {
				//	ImGui::SameLine();
				//}
			}
		}
		//ImGui::EndGroup();
		//ImGui::SameLine();
		//ImGui::BeginGroup();
		ImGui::NewLine();
		{
			ImGui::PushItemWidth(128.0f);

			ImGui::TextDisabled("Sprite Properties");

			if (ImGui::InputScalar("VRAM Addr", ImGuiDataType_U16, &sprite_props.sprite_address, &incr_addr, &fast_addr, "%d")) {
				vera_video_space_write(0x1FC00 + 8 * sprite_id, (uint8_t)(sprite_props.sprite_address >> 5));
				vera_video_space_write(0x1FC01 + 8 * sprite_id, (uint8_t)(sprite_props.sprite_address >> 13) | (sprite_props.color_mode << 7));
			}
			bool eight_bit = sprite_props.color_mode;
			if (ImGui::Checkbox("8bit Color", &eight_bit)) {
				vera_video_space_write(0x1FC01 + 8 * sprite_id, (uint8_t)(sprite_props.sprite_address >> 13) | (sprite_props.color_mode << 7));
			}
			if (ImGui::InputScalar("Pos X", ImGuiDataType_U16, &sprite_props.sprite_x, &incr_one16, &incr_ten16, "%d")) {
				vera_video_space_write(0x1FC02 + 8 * sprite_id, (uint8_t)(sprite_props.sprite_x));
				vera_video_space_write(0x1FC03 + 8 * sprite_id, (uint8_t)(sprite_props.sprite_x >> 8));
			}
			if (ImGui::InputScalar("Pos Y", ImGuiDataType_U16, &sprite_props.sprite_y, &incr_one16, &incr_ten16, "%d")) {
				vera_video_space_write(0x1FC04 + 8 * sprite_id, (uint8_t)(sprite_props.sprite_y));
				vera_video_space_write(0x1FC05 + 8 * sprite_id, (uint8_t)(sprite_props.sprite_y >> 8));
			}
			if (ImGui::Checkbox("h-flip", &sprite_props.hflip)) {
				vera_video_space_write(0x1FC06 + 8 * sprite_id, (uint8_t)(sprite_props.hflip ? 0x1 : 0) | (uint8_t)(sprite_props.vflip ? 0x2 : 0) | (uint8_t)((sprite_props.sprite_zdepth & 3) << 2) | (uint8_t)((sprite_props.sprite_collision_mask & 0xf) << 4));
			}
			if (ImGui::Checkbox("v-flip", &sprite_props.vflip)) {
				vera_video_space_write(0x1FC06 + 8 * sprite_id, (uint8_t)(sprite_props.hflip ? 0x1 : 0) | (uint8_t)(sprite_props.vflip ? 0x2 : 0) | (uint8_t)((sprite_props.sprite_zdepth & 3) << 2) | (uint8_t)((sprite_props.sprite_collision_mask & 0xf) << 4));
			}
			if (ImGui::InputScalar("Z-depth", ImGuiDataType_U8, &sprite_props.sprite_zdepth, &incr_one8, nullptr, "%d")) {
				vera_video_space_write(0x1FC06 + 8 * sprite_id, (uint8_t)(sprite_props.hflip ? 0x1 : 0) | (uint8_t)(sprite_props.vflip ? 0x2 : 0) | (uint8_t)((sprite_props.sprite_zdepth & 3) << 2) | (uint8_t)((sprite_props.sprite_collision_mask & 0xf) << 4));
			}
			if (ImGui::InputScalar("Collision", ImGuiDataType_U8, &sprite_props.sprite_collision_mask, &incr_hex8, nullptr, "%1x")) {
				vera_video_space_write(0x1FC06 + 8 * sprite_id, (uint8_t)(sprite_props.hflip ? 0x1 : 0) | (uint8_t)(sprite_props.vflip ? 0x2 : 0) | (uint8_t)((sprite_props.sprite_zdepth & 3) << 2) | (uint8_t)((sprite_props.sprite_collision_mask & 0xf0)));
			}
			if (ImGui::InputScalar("Palette Offset", ImGuiDataType_U16, &sprite_props.palette_offset, &incr_hex16, nullptr, "%d")) {
				vera_video_space_write(0x1FC07 + 8 * sprite_id, (uint8_t)((sprite_props.palette_offset >> 4) & 0xf) | (uint8_t)(((sprite_props.sprite_width_log2 - 3) & 0x3) << 4) | (uint8_t)(((sprite_props.sprite_height_log2 - 3) & 0x3) << 6));
			}
			if (ImGui::InputScalar("Width", ImGuiDataType_U8, &sprite_props.sprite_width_log2, &incr_one8, nullptr, "%d")) {
				vera_video_space_write(0x1FC07 + 8 * sprite_id, (uint8_t)((sprite_props.palette_offset >> 4) & 0xf) | (uint8_t)(((sprite_props.sprite_width_log2 - 3) & 0x3) << 4) | (uint8_t)(((sprite_props.sprite_height_log2 - 3) & 0x3) << 6));
			}
			if (ImGui::InputScalar("Height", ImGuiDataType_U8, &sprite_props.sprite_height_log2, &incr_one8, nullptr, "%d")) {
				vera_video_space_write(0x1FC07 + 8 * sprite_id, (uint8_t)((sprite_props.palette_offset >> 4) & 0xf) | (uint8_t)(((sprite_props.sprite_width_log2 - 3) & 0x3) << 4) | (uint8_t)(((sprite_props.sprite_height_log2 - 3) & 0x3) << 6));
			}
			ImGui::PopItemWidth();
		}
		ImGui::EndGroup();
	}
	ImGui::EndGroup();
}

static void draw_debugger_vera_layer()
{
	static icon_set tiles_preview;
	static uint8_t  uncompressed_vera_memory[16 * 16 * 1024];
	static uint32_t tile_pixels[16 * 16 * 1024];

	static uint16_t tile_palette_offset = 0;

	static ImU8     layer_id  = 0;
	static uint64_t layer_sig = 0;

	static const ImU8  incr_one8  = 1;
	static const ImU8  incr_hex8  = 16;
	static const ImU16 incr_one16 = 1;
	static const ImU16 incr_ten16 = 10;
	static const ImU16 incr_hex16 = 16;
	static const ImU32 incr_map  = 1 << 9;
	static const ImU32 fast_map   = incr_map << 4;
	static const ImU32 incr_tile  = 1 << 11;
	static const ImU32 fast_tile  = incr_tile << 4;

	static bool reload = true;

	ImGui::BeginGroup();
	{
		if (ImGui::InputScalar("layer", ImGuiDataType_U8, &layer_id, &incr_one8, nullptr, "%d")) {
			reload = true;
			layer_id &= 1;
		}

		uint8_t layer_data[8];
		memcpy(layer_data, vera_video_get_layer_data(layer_id), 7);
		layer_data[7] = 0;

		vera_video_layer_properties layer_props;
		memcpy(&layer_props, vera_video_get_layer_properties(layer_id), sizeof(vera_video_layer_properties));

		if (layer_sig != *reinterpret_cast<const uint64_t *>(layer_data)) {
			layer_sig = *reinterpret_cast<const uint64_t *>(layer_data);
			reload    = true;
		}

		if (layer_props.bitmap_mode) {
			const uint32_t num_dots = layer_props.tilew * 128;
			vera_video_get_expanded_vram(layer_props.tile_base, 1 << layer_props.color_depth, uncompressed_vera_memory, num_dots);

			const uint32_t *palette = vera_video_get_palette_argb32();
			for (uint32_t i = 0; i < num_dots; ++i) {
				tile_pixels[i] = (palette[uncompressed_vera_memory[i] + tile_palette_offset] << 8) | 0xff;
			}

			if (reload) {
				tiles_preview.load_memory(tile_pixels, layer_props.tilew, 128, layer_props.tilew, 128);
				reload = false;
			} else {
				tiles_preview.update_memory(tile_pixels);
			}
		} else {
			const uint32_t num_dots = 1024 << (layer_props.tilew_log2 + layer_props.tileh_log2);
			vera_video_get_expanded_vram(layer_props.tile_base, 1 << layer_props.color_depth, uncompressed_vera_memory, num_dots);

			const uint32_t *palette = vera_video_get_palette_argb32();
			for (uint32_t i = 0; i < num_dots; ++i) {
				tile_pixels[i] = (palette[uncompressed_vera_memory[i] + tile_palette_offset] << 8) | 0xff;
			}

			if (reload) {
				tiles_preview.load_memory(tile_pixels, layer_props.tilew, 1024 << layer_props.tileh_log2, layer_props.tilew, layer_props.tileh);
				reload = false;
			} else {
				tiles_preview.update_memory(tile_pixels);
			}
		}

		ImGui::BeginGroup();
		{
			ImGui::TextDisabled("Tile Preview");

			ImVec2 custom_spacing(5, 5);
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, custom_spacing);
			ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));

			static const int tiles_per_row = 128 >> layer_props.tilew_log2;
			ImGui::BeginChild("tiles", ImVec2(256.0f + custom_spacing.x * tiles_per_row + 10, 256.0f + custom_spacing.y * tiles_per_row));
			{
				ImVec2 tile_imsize(layer_props.tilew << 1, layer_props.tileh << 1);

				ImGuiListClipper clipper;
				clipper.Begin(1024 / tiles_per_row, layer_props.tileh + custom_spacing.y);

				while (clipper.Step()) {
					uint16_t start_tile = clipper.DisplayStart * tiles_per_row;
					uint16_t end_tile   = clipper.DisplayEnd * tiles_per_row;
					if (end_tile > 1024) {
						end_tile = 1024;
					}
					for (int i = start_tile; i < end_tile; ++i) {
						if (i % tiles_per_row) {
							ImGui::SameLine();
						}
						ImGui::Image((void *)(intptr_t)tiles_preview.get_texture_id(), tile_imsize, tiles_preview.get_top_left(i), tiles_preview.get_bottom_right(i));
					}
				}
				clipper.End();

				ImGui::EndChild();
			}
			ImGui::PopStyleVar();
			ImGui::PopStyleVar();
			ImGui::PushItemWidth(128.0f);
			ImGui::InputScalar("Preview Palette Offset", ImGuiDataType_U16, &tile_palette_offset, &incr_hex16, nullptr, "%d");
			ImGui::PopItemWidth();
		}
		ImGui::EndGroup();
		ImGui::SameLine();
		ImGui::BeginGroup();
		{
			ImGui::TextDisabled("Raw Bytes");

			for (int i = 0; i < 7; ++i) {
				if (i) {
					ImGui::SameLine();
				}
				if (ImGui::InputHex(i, layer_data[i])) {
					vera_video_write(0x0D + 7 * layer_id + i, layer_data[i]);
				}
				//if ((i & 1) != 1) {
				//	ImGui::SameLine();
				//}
			}
		}
		//ImGui::EndGroup();
		//ImGui::SameLine();
		ImGui::NewLine();
		//ImGui::BeginGroup();
		{
			ImGui::TextDisabled("Layer Properties");

			ImGui::PushItemWidth(128.0f);
			auto get_byte = [&layer_props](int b) -> uint8_t {
				switch (b) {
					case 0:
						return ((layer_props.maph_log2 - 5) << 6) | ((layer_props.mapw_log2 - 5) << 4) | (layer_props.text_mode_256c ? 0x8 : 0) | (layer_props.bitmap_mode ? 0x4 : 0) | layer_props.color_depth;
					case 1:
						return layer_props.map_base >> 9;
					case 2:
						return ((layer_props.tile_base >> 11) << 2) | (layer_props.tileh_log2 == 4 ? 0x2 : 0) | (layer_props.tilew_log2 == 4 ? 0x1 : 0);
					case 3:
						return layer_props.hscroll >> 8;
					case 4:
						return layer_props.hscroll & 0xff;
					case 5:
						return layer_props.vscroll >> 8;
					case 6:
						return layer_props.vscroll & 0xff;
					default:
						return 0;
				}
			};

			if (ImGui::InputScalar("Color Depth", ImGuiDataType_U8, &layer_props.color_depth, &incr_one8, "%d")) {
				vera_video_write(0x0D + 7 * layer_id, get_byte(0));
			}
			if (ImGui::Checkbox("Bitmap Layer", &layer_props.bitmap_mode)) {
				vera_video_write(0x0D + 7 * layer_id, get_byte(0));
			}
			if (ImGui::Checkbox("256-color text", &layer_props.text_mode_256c)) {
				vera_video_write(0x0D + 7 * layer_id, get_byte(0));
			}
			uint8_t mapw_log2 = layer_props.mapw_log2 - 5;
			if (ImGui::InputScalar("Map Width (log 2)", ImGuiDataType_U8, &mapw_log2, &incr_one8, "%d")) {
				layer_props.mapw_log2 = mapw_log2 + 5;
				vera_video_write(0x0D + 7 * layer_id, get_byte(0));
			}
			uint8_t maph_log2 = layer_props.maph_log2 - 5;
			if (ImGui::InputScalar("Map Height (log 2)", ImGuiDataType_U8, &maph_log2, &incr_one8, "%d")) {
				layer_props.maph_log2 = maph_log2 + 5;
				vera_video_write(0x0D + 7 * layer_id, get_byte(0));
			}
			if (ImGui::InputScalar("Map Base", ImGuiDataType_U32, &layer_props.map_base, &incr_map, &fast_map, "%03X")) {
				vera_video_write(0x0E + 7 * layer_id, get_byte(1));
			}
			bool tile16h = layer_props.tileh_log2 > 3;
			if (ImGui::Checkbox("16-pixel tile height", &tile16h)) {
				layer_props.tileh_log2 = tile16h ? 4 : 3;
				vera_video_write(0x0F + 7 * layer_id, get_byte(2));
			}
			bool tile16w = layer_props.tilew_log2 > 3;
			if (ImGui::Checkbox("16-pixel tile width", &tile16w)) {
				layer_props.tilew_log2 = tile16w ? 4 : 3;
				vera_video_write(0x0F + 7 * layer_id, get_byte(2));
			}

			if (ImGui::InputScalar("H-Scroll", ImGuiDataType_U16, &layer_props.hscroll, &incr_hex16, &incr_hex16, "%03X")) {
				vera_video_write(0x10 + 7 * layer_id, get_byte(3));
				vera_video_write(0x10 + 7 * layer_id, get_byte(4));
			}

			if (ImGui::InputScalar("V-Scroll", ImGuiDataType_U16, &layer_props.vscroll, &incr_hex16, &incr_hex16, "%03X")) {
				vera_video_write(0x10 + 7 * layer_id, get_byte(5));
				vera_video_write(0x10 + 7 * layer_id, get_byte(6));
			}

			ImGui::PopItemWidth();
		}
		ImGui::EndGroup();
	}
	ImGui::EndGroup();
}

static void draw_breakpoints()
{
	ImGui::BeginGroup();
	{
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f);
		if (ImGui::TreeNodeEx("Breakpoints", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Columns(5);
			ImGui::SetColumnWidth(0, 27);
			ImGui::SetColumnWidth(1, 27);
			ImGui::SetColumnWidth(2, ImGui::CalcTextSize("Address  ").x);
			ImGui::SetColumnWidth(3, ImGui::CalcTextSize("Bank      ").x);

			ImGui::Dummy(ImVec2(10, 10));
			ImGui::NextColumn();

			ImGui::Dummy(ImVec2(10, 10));
			ImGui::NextColumn();

			ImGui::Text("Address");
			ImGui::NextColumn();

			ImGui::Text("Bank");
			ImGui::NextColumn();

			ImGui::Text("Symbol");
			ImGui::NextColumn();

			ImGui::Separator();

			const auto &breakpoints = debugger_get_breakpoints();
			for (auto [address, bank] : breakpoints) {
				if (ImGui::TileButton(ICON_REMOVE)) {
					debugger_remove_breakpoint(address, bank);
					break;
				}
				ImGui::NextColumn();

				if (debugger_breakpoint_is_active(address, bank)) {
					if (ImGui::TileButton(ICON_CHECKED)) {
						debugger_deactivate_breakpoint(address, bank);
					}
				} else {
					if (ImGui::TileButton(ICON_UNCHECKED)) {
						debugger_activate_breakpoint(address, bank);
					}
				}
				ImGui::NextColumn();

				char addr_text[5];
				sprintf(addr_text, "%04X", address);
				if (ImGui::Selectable(addr_text, false, ImGuiSelectableFlags_AllowDoubleClick)) {
					disasm.set_dump_start(address);
					if (address >= 0xc000) {
						disasm.set_rom_bank(bank);
					} else if (address >= 0xa000) {
						disasm.set_ram_bank(bank);
					}
				}

				ImGui::NextColumn();

				if (address < 0xa000) {
					ImGui::Text("--");
				} else {
					ImGui::Text("%s %02X", address < 0xc000 ? "RAM" : "ROM", bank);
				}
				ImGui::NextColumn();

				for (auto &sym : symbols_find(address)) {
					if (ImGui::Selectable(sym.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
						disasm.set_dump_start(address);
						if (address >= 0xc000) {
							disasm.set_rom_bank(bank);
						} else if (address >= 0xa000) {
							disasm.set_ram_bank(bank);
						}
					}
				}
				ImGui::NextColumn();
			}

			ImGui::Columns(1);
			ImGui::Separator();

			static uint16_t new_address = 0;
			static uint8_t  new_bank    = 0;
			ImGui::InputHexLabel("New Address", new_address);
			ImGui::SameLine();
			ImGui::InputHexLabel("Bank", new_bank);
			ImGui::SameLine();
			if (ImGui::Button("Add")) {
				debugger_add_breakpoint(new_address, new_bank);
			}

			ImGui::Dummy(ImVec2(0, 5));
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();
	}
	ImGui::EndGroup();
}

static void draw_symbols_list()
{
	ImGui::BeginGroup();
	{
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f);
		if (ImGui::TreeNodeEx("Loaded Symbols", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
			static char symbol_filter[64] = "";
			ImGui::InputText("Filter", symbol_filter, 64);

			static bool     selected      = false;
			static uint16_t selected_addr = 0;
			static uint8_t  selected_bank = 0;
			if (ImGui::ListBoxHeader("Filtered Symbols")) {
				int  id                   = 0;
				bool any_selected_visible = false;

				auto search_filter_contains = [&](const char *value) -> bool {
					char filter[64];
					strcpy(filter, symbol_filter);
					char *token    = strtok(filter, " ");
					bool  included = true;
					while (token != nullptr) {
						if (strstr(value, token) == nullptr) {
							included = false;
							break;
						}
						token = strtok(nullptr, " ");
					}
					return included;
				};

				symbols_for_each([&](uint16_t address, symbol_bank_type bank, const std::string &name) {
					if (search_filter_contains(name.c_str())) {
						ImGui::PushID(id++);
						bool is_selected = selected && (selected_addr == address) && (selected_bank == bank);
						char display_name[128];
						sprintf(display_name, "%04x %s", address, name.c_str());
						if (ImGui::Selectable(display_name, is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
							selected      = true;
							selected_addr = address;
							selected_bank = bank;
							is_selected   = true;

							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
								disasm.set_dump_start(address);
							}
						}
						any_selected_visible = any_selected_visible || is_selected;
						ImGui::PopID();
					}
				});
				selected = any_selected_visible;
				ImGui::ListBoxFooter();
			}

			if (ImGui::Button("Add Breakpoint at Symbol") && selected) {
				debugger_add_breakpoint(selected_addr, selected_bank);
			}

			ImGui::Dummy(ImVec2(0, 5));
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();
	}
	ImGui::EndGroup();
}

static void draw_symbols_files()
{
	ImGui::BeginGroup();
	{
		ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 0.0f);
		if (ImGui::TreeNodeEx("Loaded Symbol Files", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Columns(3);
			ImGui::SetColumnWidth(0, 27);
			ImGui::SetColumnWidth(1, 27);

			ImGui::Dummy(ImVec2(16, 16));
			ImGui::NextColumn();

			const auto &files = symbols_get_loaded_files();

			if (symbols_file_all_are_visible()) {
				if (ImGui::TileButton(ICON_CHECKED)) {
					for (auto file : files) {
						symbols_hide_file(file);
					}
				}
			} else if (symbols_file_any_is_visible()) {
				if (ImGui::TileButton(ICON_CHECK_UNCERTAIN)) {
					for (auto file : files) {
						symbols_hide_file(file);
					}
				}
			} else {
				if (ImGui::TileButton(ICON_UNCHECKED)) {
					for (auto file : files) {
						symbols_show_file(file);
					}
				}
			}
			ImGui::NextColumn();

			ImGui::Text("Path");
			ImGui::NextColumn();

			ImGui::Separator();

			for (auto file : files) {
				ImGui::PushID(file.c_str());
				if (ImGui::TileButton(ICON_REMOVE)) {
					symbols_unload_file(file);
					ImGui::PopID();
					break;
				}
				ImGui::NextColumn();

				if (symbols_file_is_visible(file)) {
					if (ImGui::TileButton(ICON_CHECKED)) {
						symbols_hide_file(file);
					}
				} else {
					if (ImGui::TileButton(ICON_UNCHECKED)) {
						symbols_show_file(file);
					}
				}
				ImGui::PopID();
				ImGui::NextColumn();

				ImGui::Text("%s", file.c_str());
				ImGui::NextColumn();
			}

			ImGui::Columns(1);
			static uint8_t ram_bank = 0;
			if (ImGui::Button("Load Symbols")) {
				char *open_path = nullptr;
				if (NFD_OpenDialog("sym", nullptr, &open_path) == NFD_OKAY && open_path != nullptr) {
					symbols_load_file(open_path, ram_bank);
				}
			}

			ImGui::InputHexLabel("Bank", ram_bank);

			ImGui::Dummy(ImVec2(0, 5));
			ImGui::TreePop();
		}
		ImGui::PopStyleVar();
	}
	ImGui::EndGroup();
}

static void draw_debugger_controls()
{
	bool paused  = debugger_is_paused();
	bool shifted = ImGui::IsKeyDown(SDL_SCANCODE_LSHIFT) || ImGui::IsKeyDown(SDL_SCANCODE_RSHIFT);

	static bool stop_hovered = false;
	if (ImGui::TileButton(paused ? ICON_STOP_DISABLED : ICON_STOP, !paused, &stop_hovered) || (shifted && ImGui::IsKeyPressed(SDL_SCANCODE_F5))) {
		debugger_pause_execution();
		disasm.follow_pc();
	}
	if (!paused && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Pause execution (Shift+F5)");
	}

	ImGui::SameLine();

	static bool run_hovered = false;
	if (ImGui::TileButton(paused ? ICON_RUN : ICON_RUN_DISABLED, paused, &run_hovered) || (!shifted && ImGui::IsKeyPressed(SDL_SCANCODE_F5))) {
		debugger_continue_execution();
		disasm.follow_pc();
	}
	if (paused && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Run (F5)");
	}
	ImGui::SameLine();

	static bool step_over_hovered = false;
	if (ImGui::TileButton(paused ? ICON_STEP_OVER : ICON_STEP_OVER_DISABLED, paused, &step_over_hovered) || (!shifted && ImGui::IsKeyPressed(SDL_SCANCODE_F10))) {
		debugger_step_over_execution();
		disasm.follow_pc();
	}
	if (paused && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Step Over (F10)");
	}
	ImGui::SameLine();

	static bool step_into_hovered = false;
	if (ImGui::TileButton(paused ? ICON_STEP_INTO : ICON_STEP_INTO_DISABLED, paused, &step_into_hovered) || (!shifted && ImGui::IsKeyPressed(SDL_SCANCODE_F11))) {
		debugger_step_execution();
		disasm.follow_pc();
	}
	if (paused && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Step Into (F11)");
	}
	ImGui::SameLine();

	static bool step_out_hovered = false;
	if (ImGui::TileButton(paused ? ICON_STEP_OUT : ICON_STEP_OUT_DISABLED, paused, &step_out_hovered) || (shifted && ImGui::IsKeyPressed(SDL_SCANCODE_F11))) {
		debugger_step_out_execution();
		disasm.follow_pc();
	}
	if (paused && ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Step Out (Shift+F11)");
	}
	ImGui::SameLine();

	char cycles_raw[32];
	int  digits = sprintf(cycles_raw, "%" SDL_PRIu64, debugger_step_clocks());

	char  cycles_formatted[32];
	char *r = cycles_raw;
	char *f = cycles_formatted;
	while (*r != '\0') {
		*f = *r;
		++r;
		++f;
		--digits;
		if ((digits > 0) && (digits % 3 == 0)) {
			*f = ',';
			++f;
		}
	}
	*f = '\0';

	if (paused) {
		ImGui::Text("%s cycles%s", cycles_formatted, debugger_step_interrupted() ? " (Interrupted)" : "");
	} else {
		ImGui::TextDisabled("%s cycles%s", cycles_formatted, debugger_step_interrupted() ? " (Interrupted)" : "");
	}
}

static void draw_debugger_vera_psg()
{
	ImGui::Columns(6);
	static const char *labels[] = {
		"Freq",
		"Left",
		"Right",
		"Vol",
		"Wave",
		"Width"
	};
	for (int i = 0; i < 6; ++i) {
		ImGui::Text("%s", labels[i]);
		ImGui::NextColumn();
	}

	for (unsigned int i = 0; i < 16; ++i) {
		ImGui::PushID(i);
		const psg_channel *channel = psg_get_channel(i);

		int freq = channel->freq;
		ImGui::PushID("freq");
		if (ImGui::InputInt("", &freq)) {
			psg_set_channel_frequency(i, freq);
		}
		ImGui::PopID();
		ImGui::NextColumn();

		bool left = channel->left;
		ImGui::PushID("left");
		if (ImGui::Checkbox("", &left)) {
			psg_set_channel_left(i, left);
		}
		ImGui::PopID();
		ImGui::NextColumn();

		bool right = channel->right;
		ImGui::PushID("right");
		if (ImGui::Checkbox("", &right)) {
			psg_set_channel_right(i, right);
		}
		ImGui::PopID();
		ImGui::NextColumn();

		int volume = channel->volume;
		ImGui::PushID("volume");
		if (ImGui::InputInt("", &volume)) {
			psg_set_channel_volume(i, volume);
		}
		ImGui::PopID();
		ImGui::NextColumn();

		static const char *waveforms[] = {
			"Pulse",
			"Sawtooth",
			"Triangle",
			"Noise"
		};
		int wf = channel->waveform;
		ImGui::PushID("waveforms");
		if (ImGui::Combo("", &wf, waveforms, IM_ARRAYSIZE(waveforms))) {
			psg_set_channel_waveform(i, wf);
		}
		ImGui::PopID();
		ImGui::NextColumn();

		int pulse_width = channel->pw;
		ImGui::PushID("pulse_width");
		if (ImGui::InputInt("", &pulse_width)) {
			psg_set_channel_pulse_width(i, pulse_width);
		}
		ImGui::PopID();
		ImGui::NextColumn();

		ImGui::PopID();
	}
	ImGui::Columns(1);

	const int16_t *psg_buffer = audio_get_psg_buffer();
	{
		float left_samples[SAMPLES_PER_BUFFER];
		float right_samples[SAMPLES_PER_BUFFER];

		float *l = left_samples;
		float *r = right_samples;

		const int16_t *b = psg_buffer;
		for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) {
			*l = *b;
			++l;
			++b;
			*r = *b;
			++r;
			++b;
		}

		ImGui::PlotLines("Left", left_samples, SAMPLES_PER_BUFFER, 0, nullptr, INT16_MIN, INT16_MAX, ImVec2(0, 80.0f));
		ImGui::PlotLines("Right", right_samples, SAMPLES_PER_BUFFER, 0, nullptr, INT16_MIN, INT16_MAX, ImVec2(0, 80.0f));
	}
}

static void draw_menu_bar()
{
	if (ImGui::BeginMainMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem("Open TXT file")) {
				char *open_path = nullptr;
				if (NFD_OpenDialog("txt", nullptr, &open_path) == NFD_OKAY && open_path != nullptr) {
					keyboard_add_file(open_path);
				}
			}

			if (ImGui::MenuItem("Save Options")) {
				save_options(true);
			}

			if (ImGui::MenuItem("Exit")) {
				SDL_Event evt;
				evt.type = SDL_QUIT;
				SDL_PushEvent(&evt);
			}
			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Machine")) {
			if (ImGui::MenuItem("Reset", "Ctrl-R")) {
				machine_reset();
			}
			if (ImGui::MenuItem("NMI")) {
				nmi6502();
				debugger_interrupt();
			}
			if (ImGui::MenuItem("Save Dump", "Ctrl-S")) {
				machine_dump();
			}
			if (ImGui::BeginMenu("Controller Ports")) {
				joystick_for_each_slot([](int slot, int instance_id, SDL_GameController *controller) {
					const char *name = nullptr;
					if (controller != nullptr) {
						name = SDL_GameControllerName(controller);
					}
					if (name == nullptr) {
						name = "(No Controller)";
					}

					char label[256];
					snprintf(label, 256, "%d: %s", slot, name);
					label[255] = '\0';

					if (ImGui::BeginMenu(label)) {
						if (ImGui::RadioButton("(No Controller)", instance_id == -1)) {
							if (instance_id >= 0) {
								joystick_slot_remap(slot, -1);
							}
						}

						joystick_for_each([slot](int instance_id, SDL_GameController *controller, int current_slot) {
							const char *name = nullptr;
							if (controller != nullptr) {
								name = SDL_GameControllerName(controller);
							}
							if (name == nullptr) {
								name = "(No Controller)";
							}

							char label[256];
							snprintf(label, 256, "%s (%d)", name, instance_id);
							label[255] = '\0';

							if (ImGui::RadioButton(label, slot == current_slot)) {
								if (slot != current_slot) {
									joystick_slot_remap(slot, instance_id);
								}
							}
						});
						ImGui::EndMenu();
					}
				});
				ImGui::EndMenu();
			}

			if (ImGui::BeginMenu("SD Card")) {
				if (ImGui::MenuItem("Open")) {
					char *open_path = nullptr;
					if (NFD_OpenDialog("bin,img,sdcard", nullptr, &open_path) == NFD_OKAY && open_path != nullptr) {
						sdcard_set_file(open_path);
					}
				}

				bool sdcard_attached = sdcard_is_attached();
				if (ImGui::Checkbox("Attach card", &sdcard_attached)) {
					if (sdcard_attached) {
						sdcard_attach();
					} else {
						sdcard_detach();
					}
				}
				ImGui::EndMenu();
			}

			if (ImGui::MenuItem("Change CWD")) {
				char *open_path = nullptr;
				if (NFD_PickFolder("", &open_path) == NFD_OKAY && open_path != nullptr) {
					strcpy(Options.hyper_path, open_path);
				}
			}

			ImGui::Separator();

			bool warp_mode = Options.warp_factor > 0;
			if (ImGui::Checkbox("Enable Warp Mode", &warp_mode)) {
				if (Options.warp_factor > 0) {
					Options.warp_factor = 0;
					vera_video_set_cheat_mask(0);
				} else {
					Options.warp_factor = 1;
					vera_video_set_cheat_mask(0x3f);
				}
			}

			ImGui::EndMenu();
		}

		if (ImGui::BeginMenu("Windows")) {
			if (ImGui::BeginMenu("Debugging")) {
				ImGui::Checkbox("Memory Dump 1", &Show_memory_dump_1);
				ImGui::Checkbox("Memory Dump 2", &Show_memory_dump_2);
				ImGui::Checkbox("CPU Monitor", &Show_monitor);
				ImGui::Separator();
				ImGui::Checkbox("VERA Monitor", &Show_VERA_monitor);
				ImGui::Checkbox("VERA Palette", &Show_VERA_palette);
				ImGui::Checkbox("VERA Layers", &Show_VERA_layers);
				ImGui::Checkbox("VERA Sprites", &Show_VERA_sprites);
				ImGui::Checkbox("PSG Monitor", &Show_VERA_PSG_monitor);
				ImGui::EndMenu();
			}
			ImGui::Separator();

			bool safety_frame = vera_video_safety_frame_is_enabled();
			if (ImGui::Checkbox("Show Safety Frame", &safety_frame)) {
				vera_video_enable_safety_frame(safety_frame);
			}

			if (ImGui::Checkbox("Show ImGui Demo", &Show_imgui_demo)) {
				// Nothing to do.
			}
			ImGui::EndMenu();
		}

		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 116.0f);
		ImGui::Tile(ICON_POWER_LED_OFF);
		if (power_led > 0) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 116.0f);
			ImGui::Tile(ICON_POWER_LED_ON, (float)power_led / 255.0f);
		}
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 96.0f);
		ImGui::Tile(ICON_ACTIVITY_LED_OFF);
		if (activity_led > 0) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 96.0f);
			ImGui::Tile(ICON_ACTIVITY_LED_ON, (float)activity_led / 255.0f);
		}
		ImGui::Text("Speed: %d%%", Timing_perf);
		ImGui::EndMainMenuBar();
	}
}

void overlay_draw()
{
	draw_menu_bar();

	if (Show_memory_dump_1) {
		if (ImGui::Begin("Memory 1", &Show_memory_dump_1)) {
			memory_dump_1.draw();
		}
		ImGui::End();
	}

	if (Show_memory_dump_2) {
		if (ImGui::Begin("Memory 2", &Show_memory_dump_2)) {
			memory_dump_2.draw();
		}
		ImGui::End();
	}

	if (Show_monitor) {
		if (ImGui::Begin("CPU Monitor", &Show_monitor)) {
			draw_debugger_controls();
			disasm.draw();
			ImGui::SameLine();
			draw_debugger_cpu_status();
			draw_breakpoints();
			draw_symbols_list();
			draw_symbols_files();
		}
		ImGui::End();
	}

	if (Show_VERA_monitor) {
		if (ImGui::Begin("VERA Monitor", &Show_VERA_monitor)) {
			vram_dump.draw();
			ImGui::SameLine();
			draw_debugger_vera_status();
		}
		ImGui::End();
	}

	if (Show_VERA_palette) {
		if (ImGui::Begin("VERA Palette", &Show_VERA_palette)) {
			draw_debugger_vera_palette();
		}
		ImGui::End();
	}

	if (Show_VERA_layers) {
		if (ImGui::Begin("VERA Layers", &Show_VERA_layers)) {
			draw_debugger_vera_layer();
		}
		ImGui::End();
	}

	if (Show_VERA_sprites) {
		if (ImGui::Begin("VERA Sprites", &Show_VERA_sprites)) {
			draw_debugger_vera_sprite();
		}
		ImGui::End();
	}

	if (Show_imgui_demo) {
		ImGui::ShowDemoWindow();
	}
	
	if (Show_VERA_PSG_monitor) {
		if (ImGui::Begin("VERA PSG", &Show_VERA_PSG_monitor)) {
			draw_debugger_vera_psg();
		}
		ImGui::End();
	}
}

bool imgui_overlay_has_focus()
{
	return ImGui::IsAnyItemFocused();
}