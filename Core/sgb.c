#include "gb.h"

enum {
    PAL01    = 0x00,
    PAL23    = 0x01,
    PAL03    = 0x02,
    PAL12    = 0x03,
    PAL_SET  = 0x0A,
    PAL_TRN  = 0x0B,
    DATA_SND = 0x0f,
    MLT_REQ  = 0x11,
    CHR_TRN  = 0x13,
    PCT_TRN  = 0x14,
    MASK_EN  = 0x17,
};

typedef enum {
    MASK_DISABLED,
    MASK_FREEZE,
    MASK_COLOR_3,
    MASK_COLOR_0,
} mask_mode_t;

#define SGB_PACKET_SIZE 16
static inline void pal_command(GB_gameboy_t *gb, unsigned first, unsigned second)
{
    gb->sgb->effective_palettes[0] = gb->sgb->effective_palettes[4] =
        gb->sgb->effective_palettes[8] = gb->sgb->effective_palettes[12] =
        gb->sgb->command[1] | (gb->sgb->command[2] << 8);
    
    for (unsigned i = 0; i < 3; i++) {
        gb->sgb->effective_palettes[first * 4 + i + 1] = gb->sgb->command[3 + i * 2] | (gb->sgb->command[4 + i * 2] << 8);
    }
    
    for (unsigned i = 0; i < 3; i++) {
        gb->sgb->effective_palettes[second * 4 + i + 1] = gb->sgb->command[9 + i * 2] | (gb->sgb->command[10 + i * 2] << 8);
    }
}

static void command_ready(GB_gameboy_t *gb)
{
    /* SGB header commands are used to send the contents of the header to the SNES CPU.
       A header command looks like this:
       Command ID: 0b1111xxx1, where xxx is the packet index. (e.g. F1 for [0x104, 0x112), F3 for [0x112, 0x120))
       Checksum: Simple one byte sum for the following content bytes
       0xE content bytes. The last command, FB, is padded with zeros, so information past the header is not sent. */
    
    
    if ((gb->sgb->command[0] & 0xF1) == 0xF1) {
        uint8_t checksum = 0;
        for (unsigned i = 2; i < 0x10; i++) {
            checksum += gb->sgb->command[i];
        }
        if (checksum != gb->sgb->command[1]) {
            GB_log(gb, "Failed checksum for SGB header command, disabling SGB features\n");
            gb->sgb->disable_commands = true;
            return;
        }
        if (gb->sgb->command[0] == 0xf9) {
            if (gb->sgb->command[0xc] != 3) { // SGB Flag
                GB_log(gb, "SGB flag is not 0x03, disabling SGB features\n");
                gb->sgb->disable_commands = true;
            }
        }
        else if (gb->sgb->command[0] == 0xfb) {
            if (gb->sgb->command[0x3] != 0x33) { // Old licensee code
                GB_log(gb, "Old licensee code is not 0x33, disabling SGB features\n");
                gb->sgb->disable_commands = true;
            }
        }
        return;
    }
    
    switch (gb->sgb->command[0] >> 3) {
        case PAL01:
            pal_command(gb, 0, 1);
            break;
        case PAL23:
            pal_command(gb, 2, 3);
            break;
        case PAL03:
            pal_command(gb, 0, 3);
            break;
        case PAL12:
            pal_command(gb, 1, 2);
            break;
        case PAL_SET:
            memcpy(&gb->sgb->effective_palettes[0],
                   &gb->sgb->ram_palettes[4 * (gb->sgb->command[1] + (gb->sgb->command[2] & 1) * 0x100)],
                   8);
            memcpy(&gb->sgb->effective_palettes[4],
                   &gb->sgb->ram_palettes[4 * (gb->sgb->command[3] + (gb->sgb->command[4] & 1) * 0x100)],
                   8);
            memcpy(&gb->sgb->effective_palettes[8],
                   &gb->sgb->ram_palettes[4 * (gb->sgb->command[5] + (gb->sgb->command[6] & 1) * 0x100)],
                   8);
            memcpy(&gb->sgb->effective_palettes[12],
                   &gb->sgb->ram_palettes[4 * (gb->sgb->command[7] + (gb->sgb->command[8] & 1) * 0x100)],
                   8);
            
            if (gb->sgb->command[9] & 0x40) {
                gb->sgb->mask_mode = MASK_DISABLED;
            }
            break;
        case PAL_TRN:
            gb->sgb->vram_transfer_countdown = 2;
            gb->sgb->tile_transfer = false;
            gb->sgb->data_transfer = true;
            gb->sgb->palette_transfer = true;
            break;
        case DATA_SND:
            // Not supported, but used by almost all SGB games for hot patching, so let's mute the warning for this
            break;
        case MLT_REQ:
            gb->sgb->player_count = (uint8_t[]){1, 2, 1, 4}[gb->sgb->command[1] & 3];
            gb->sgb->current_player = gb->sgb->player_count - 1;
            break;
        case MASK_EN:
            gb->sgb->mask_mode = gb->sgb->command[1] & 3;
            break;
        case CHR_TRN:
            gb->sgb->vram_transfer_countdown = 2;
            gb->sgb->tile_transfer = true;
            gb->sgb->data_transfer = false;
            gb->sgb->tile_transfer_high = gb->sgb->command[1] & 1;
            break;
        case PCT_TRN:
            gb->sgb->vram_transfer_countdown = 2;
            gb->sgb->tile_transfer = false;
            gb->sgb->data_transfer = true;
            gb->sgb->palette_transfer = false;
            break;
        default:
            GB_log(gb, "Unimplemented SGB command %x: ", gb->sgb->command[0] >> 3);
            for (unsigned i = 0; i < gb->sgb->command_write_index / 8; i++) {
                GB_log(gb, "%02x ", gb->sgb->command[i]);
            }
            GB_log(gb, "\n");
            ;
    }
}

void GB_sgb_write(GB_gameboy_t *gb, uint8_t value)
{
    if (!GB_is_sgb(gb)) return;
    if (gb->sgb->disable_commands) return;
    if (gb->sgb->command_write_index >= sizeof(gb->sgb->command) * 8) return;
    
    uint16_t command_size = (gb->sgb->command[0] & 7 ?: 1) * SGB_PACKET_SIZE * 8;
    if ((gb->sgb->command[0] & 0xF1) == 0xF1) {
        command_size = SGB_PACKET_SIZE * 8;
    }
    
    switch ((value >> 4) & 3) {
        case 3:
            gb->sgb->ready_for_pulse = true;
            break;
            
        case 2: // Zero
            if (!gb->sgb->ready_for_pulse || !gb->sgb->ready_for_write) return;
            if (gb->sgb->ready_for_stop) {
                if (gb->sgb->command_write_index == command_size) {
                    command_ready(gb);
                    gb->sgb->command_write_index = 0;
                    memset(gb->sgb->command, 0, sizeof(gb->sgb->command));
                }
                gb->sgb->ready_for_pulse = false;
                gb->sgb->ready_for_write = false;
                gb->sgb->ready_for_stop = false;
            }
            else {
                gb->sgb->command_write_index++;
                gb->sgb->ready_for_pulse = false;
                if (((gb->sgb->command_write_index) & (SGB_PACKET_SIZE * 8 - 1)) == 0) {
                    gb->sgb->ready_for_stop = true;
                }
            }
            break;
        case 1: // One
            if (!gb->sgb->ready_for_pulse || !gb->sgb->ready_for_write) return;
            if (gb->sgb->ready_for_stop) {
                GB_log(gb, "Corrupt SGB command.\n");
                gb->sgb->ready_for_pulse = false;
                gb->sgb->ready_for_write = false;
                gb->sgb->command_write_index = 0;
                memset(gb->sgb->command, 0, sizeof(gb->sgb->command));
            }
            else {
                gb->sgb->command[gb->sgb->command_write_index / 8] |= 1 << (gb->sgb->command_write_index & 7);
                gb->sgb->command_write_index++;
                gb->sgb->ready_for_pulse = false;
                if (((gb->sgb->command_write_index) & (SGB_PACKET_SIZE * 8 - 1)) == 0) {
                    gb->sgb->ready_for_stop = true;
                }
            }
            break;
        
        case 0:
            if (!gb->sgb->ready_for_pulse) return;
            gb->sgb->ready_for_pulse = false;
            gb->sgb->ready_for_write = true;
            gb->sgb->ready_for_pulse = false;
            if (gb->sgb->player_count > 1 && (value & 0x30) != (gb->io_registers[GB_IO_JOYP] & 0x30)) {
                gb->sgb->current_player++;
                gb->sgb->current_player &= gb->sgb->player_count - 1;
            }
            break;
            
        default:
            break;
    }
}

static inline uint8_t scale_channel(uint8_t x)
{
    return (x << 3) | (x >> 2);
}

static uint32_t convert_rgb15(GB_gameboy_t *gb, uint16_t color)
{
    uint8_t r = (color) & 0x1F;
    uint8_t g = (color >> 5) & 0x1F;
    uint8_t b = (color >> 10) & 0x1F;
    
    r = scale_channel(r);
    g = scale_channel(g);
    b = scale_channel(b);
    
    return gb->rgb_encode_callback(gb, r, g, b);
}

static uint32_t convert_rgb15_with_fade(GB_gameboy_t *gb, uint16_t color, uint8_t fade)
{
    uint8_t r = ((color) & 0x1F) - fade;
    uint8_t g = ((color >> 5) & 0x1F) - fade;
    uint8_t b = ((color >> 10) & 0x1F) - fade;
    
    if (r >= 0x20) r = 0;
    if (g >= 0x20) g = 0;
    if (b >= 0x20) b = 0;
    
    r = scale_channel(r);
    g = scale_channel(g);
    b = scale_channel(b);
    
    return gb->rgb_encode_callback(gb, r, g, b);
}

void GB_sgb_render(GB_gameboy_t *gb)
{
    if (!gb->screen || !gb->rgb_encode_callback) return;
    
    uint32_t colors[] = {
        convert_rgb15(gb, gb->sgb->effective_palettes[0]),
        convert_rgb15(gb, gb->sgb->effective_palettes[1]),
        convert_rgb15(gb, gb->sgb->effective_palettes[2]),
        convert_rgb15(gb, gb->sgb->effective_palettes[3]),
    };
    
    switch ((mask_mode_t) gb->sgb->mask_mode) {
        case MASK_DISABLED:
            memcpy(gb->sgb->effective_screen_buffer,
                   gb->sgb->screen_buffer,
                   sizeof(gb->sgb->effective_screen_buffer));
            break;
        case MASK_FREEZE:
            break;
        
        case MASK_COLOR_3:
            memset(gb->sgb->effective_screen_buffer,
                   3,
                   sizeof(gb->sgb->effective_screen_buffer));
            break;
        case MASK_COLOR_0:
            memset(gb->sgb->effective_screen_buffer,
                   0,
                   sizeof(gb->sgb->effective_screen_buffer));
    }
    
    if (gb->sgb->vram_transfer_countdown) {
        if (--gb->sgb->vram_transfer_countdown == 0) {
            if (gb->sgb->tile_transfer) {
                uint8_t *base = &gb->sgb->pending_border.tiles[gb->sgb->tile_transfer_high? 0x80 * 8 * 8 : 0];
                for (unsigned tile = 0; tile < 0x80; tile++) {
                    unsigned tile_x = (tile % 10) * 16;
                    unsigned tile_y = (tile / 10) * 8;
                    for (unsigned y = 0; y < 0x8; y++) {
                        for (unsigned x = 0; x < 0x8; x++) {
                            base[tile * 8 * 8 + y * 8 + x] = gb->sgb->screen_buffer[(tile_x + x) + (tile_y + y) * 160] +
                                                             gb->sgb->screen_buffer[(tile_x + x + 8) + (tile_y + y) * 160] * 4;
                        }
                    }
                }
                
            }
            else if (gb->sgb->data_transfer) {
                unsigned size = gb->sgb->palette_transfer? 0x100 : 0x88;
                uint16_t *data = gb->sgb->palette_transfer? gb->sgb->ram_palettes : gb->sgb->pending_border.raw_data;
                for (unsigned tile = 0; tile < size; tile++) {
                    unsigned tile_x = (tile % 20) * 8;
                    unsigned tile_y = (tile / 20) * 8;
                    for (unsigned y = 0; y < 0x8; y++) {
                        static const uint16_t pixel_to_bits[4] = {0x0000, 0x0080, 0x8000, 0x8080};
                        *data = 0;
                        for (unsigned x = 0; x < 8; x++) {
                            *data |= pixel_to_bits[gb->sgb->screen_buffer[(tile_x + x) + (tile_y + y) * 160] & 3] >> x;
                        }
                        data++;
                    }
                }
                if (!gb->sgb->palette_transfer) {
                    gb->sgb->border_animation = 64;
                }
            }
        }
    }
    
    uint32_t *output = &gb->screen[48 + 40 * 256];
    uint8_t *input = gb->sgb->effective_screen_buffer;
    for (unsigned y = 0; y < 144; y++) {
        for (unsigned x = 0; x < 160; x++) {
            *(output++) = colors[*(input++) & 3];
        }
        output += 256 - 160;
    }
    
    uint32_t border_colors[16 * 4];
    if (gb->sgb->border_animation == 0) {
        for (unsigned i = 0; i < 16 * 4; i++) {
            border_colors[i] = convert_rgb15(gb, gb->sgb->border.palette[i]);
        }
    }
    else if (gb->sgb->border_animation > 32) {
        gb->sgb->border_animation--;
        for (unsigned i = 0; i < 16 * 4; i++) {
            border_colors[i] = convert_rgb15_with_fade(gb, gb->sgb->border.palette[i], 64 - gb->sgb->border_animation);
        }
    }
    else {
        gb->sgb->border_animation--;
        for (unsigned i = 0; i < 16 * 4; i++) {
            border_colors[i] = convert_rgb15_with_fade(gb, gb->sgb->border.palette[i], gb->sgb->border_animation);
        }
    }
    
    if (gb->sgb->border_animation == 32) {
        memcpy(&gb->sgb->border, &gb->sgb->pending_border, sizeof(gb->sgb->border));
    }
    
    for (unsigned tile_y = 0; tile_y < 28; tile_y++) {
        for (unsigned tile_x = 0; tile_x < 32; tile_x++) {
            bool gb_area = false;
            if (tile_x >= 6 && tile_x < 26 && tile_y >= 5 && tile_y < 23) {
                gb_area = true;
            }
            uint16_t tile = gb->sgb->border.map[tile_x + tile_y * 32];
            uint8_t flip_x = (tile & 0x4000)? 0x7 : 0;
            uint8_t flip_y = (tile & 0x8000)? 0x7 : 0;
            uint8_t palette = (tile >> 10) & 3;
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    uint8_t color = gb->sgb->border.tiles[(tile & 0xFF) * 64 + (x ^ flip_x) + (y ^ flip_y) * 8] & 0xF;
                    if (color == 0 && gb_area) continue;
                    gb->screen[tile_x * 8 + x + (tile_y * 8 + y) * 0x100] = border_colors[palette * 16 + color];
                }
            }
        }
    }
}

void GB_sgb_load_default_data(GB_gameboy_t *gb)
{
    
#include "sgb_border.inc"
    
    memcpy(gb->sgb->border.map, tilemap, sizeof(tilemap));
    memcpy(gb->sgb->border.palette, palette, sizeof(palette));
    
    /* Expend tileset */
    for (unsigned tile = 0; tile < sizeof(tiles) / 32; tile++) {
        for (unsigned y = 0; y < 8; y++) {
            for (unsigned x = 0; x < 8; x++) {
                gb->sgb->border.tiles[tile * 8 * 8 + y * 8 + x] =
                    (tiles[tile * 32 + y * 2 +  0] & (1 << (7 ^ x)) ? 1 : 0) |
                    (tiles[tile * 32 + y * 2 +  1] & (1 << (7 ^ x)) ? 2 : 0) |
                    (tiles[tile * 32 + y * 2 + 16] & (1 << (7 ^ x)) ? 4 : 0) |
                    (tiles[tile * 32 + y * 2 + 17] & (1 << (7 ^ x)) ? 8 : 0);
            }
        }
    }
    
    gb->sgb->effective_palettes[0] = 0x639E;
    gb->sgb->effective_palettes[1] = 0x263A;
    gb->sgb->effective_palettes[2] = 0x10D4;
    gb->sgb->effective_palettes[3] = 0x2866;
}
