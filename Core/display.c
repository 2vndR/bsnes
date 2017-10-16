#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "gb.h"

/*
 Each line is 456 cycles, approximately:
 Mode 2 - 80  cycles / OAM Transfer
 Mode 3 - 172 cycles / Rendering
 Mode 0 - 204 cycles / HBlank
 
 Mode 1 is VBlank
 
 Todo: Mode lengths are not constants, see http://blog.kevtris.org/blogfiles/Nitty%20Gritty%20Gameboy%20VRAM%20Timing.txt
 */

#define MODE2_LENGTH (80)
#define MODE3_LENGTH (172)
#define MODE0_LENGTH (204)
#define LINE_LENGTH (MODE2_LENGTH + MODE3_LENGTH + MODE0_LENGTH) // = 456
#define LINES (144)
#define WIDTH (160)
#define VIRTUAL_LINES (LCDC_PERIOD / LINE_LENGTH) // = 154

typedef struct __attribute__((packed)) {
    uint8_t y;
    uint8_t x;
    uint8_t tile;
    uint8_t flags;
} GB_sprite_t;

static bool window_enabled(GB_gameboy_t *gb)
{
    if ((gb->io_registers[GB_IO_LCDC] & 0x1) == 0) {
        if (!gb->cgb_mode && gb->is_cgb) {
            return false;
        }
    }
    return (gb->io_registers[GB_IO_LCDC] & 0x20) && gb->io_registers[GB_IO_WX] < 167;
}

static uint32_t get_pixel(GB_gameboy_t *gb, uint8_t x, uint8_t y)
{
    /*
     Bit 7 - LCD Display Enable             (0=Off, 1=On)
     Bit 6 - Window Tile Map Display Select (0=9800-9BFF, 1=9C00-9FFF)
     Bit 5 - Window Display Enable          (0=Off, 1=On)
     Bit 4 - BG & Window Tile Data Select   (0=8800-97FF, 1=8000-8FFF)
     Bit 3 - BG Tile Map Display Select     (0=9800-9BFF, 1=9C00-9FFF)
     Bit 2 - OBJ (Sprite) Size              (0=8x8, 1=8x16)
     Bit 1 - OBJ (Sprite) Display Enable    (0=Off, 1=On)
     Bit 0 - BG Display (for CGB see below) (0=Off, 1=On)
     */
    uint16_t map = 0x1800;
    uint8_t tile = 0;
    uint8_t attributes = 0;
    uint8_t sprite_palette = 0;
    uint16_t tile_address = 0;
    uint8_t background_pixel = 0, sprite_pixel = 0;
    GB_sprite_t *sprite = (GB_sprite_t *) &gb->oam;
    uint8_t sprites_in_line = 0;
    bool lcd_8_16_mode = (gb->io_registers[GB_IO_LCDC] & 4) != 0;
    bool sprites_enabled = (gb->io_registers[GB_IO_LCDC] & 2) != 0;
    uint8_t lowest_sprite_x = 0xFF;
    bool use_obp1 = false, priority = false;
    bool in_window = false;
    bool bg_enabled = true;
    bool bg_behind = false;
    if ((gb->io_registers[GB_IO_LCDC] & 0x1) == 0) {
        if (gb->cgb_mode) {
            bg_behind = true;
        }
        else {
            bg_enabled = false;
        }
    }

    if (window_enabled(gb) && y >= gb->io_registers[GB_IO_WY] + gb->wy_diff && x + 7 >= gb->io_registers[GB_IO_WX]) {
        in_window = true;
    }

    if (sprites_enabled) {
        // Loop all sprites
        for (uint8_t i = 40; i--; sprite++) {
            int sprite_y = sprite->y - 16;
            int sprite_x = sprite->x - 8;
            // Is sprite in our line?
            if (sprite_y <= y && sprite_y + (lcd_8_16_mode? 16:8) > y) {
                uint8_t tile_x, tile_y, current_sprite_pixel;
                uint16_t line_address;
                // Limit to 10 sprites in one scan line.
                if (++sprites_in_line == 11) break;
                // Does not overlap our pixel.
                if (sprite_x > x || sprite_x + 8 <= x) continue;
                tile_x = x - sprite_x;
                tile_y = y - sprite_y;
                if (sprite->flags & 0x20) tile_x = 7 - tile_x;
                if (sprite->flags & 0x40) tile_y = (lcd_8_16_mode? 15:7) - tile_y;
                line_address = (lcd_8_16_mode? sprite->tile & 0xFE : sprite->tile) * 0x10 + tile_y * 2;
                if (gb->cgb_mode && (sprite->flags & 0x8)) {
                    line_address += 0x2000;
                }
                current_sprite_pixel = (((gb->vram[line_address    ] >> ((~tile_x)&7)) & 1 ) |
                                        ((gb->vram[line_address + 1] >> ((~tile_x)&7)) & 1) << 1 );
                /* From Pandocs:
                     When sprites with different x coordinate values overlap, the one with the smaller x coordinate
                     (closer to the left) will have priority and appear above any others. This applies in Non CGB Mode
                     only. When sprites with the same x coordinate values overlap, they have priority according to table
                     ordering. (i.e. $FE00 - highest, $FE04 - next highest, etc.) In CGB Mode priorities are always
                     assigned like this.
                 */
                if (current_sprite_pixel != 0) {
                    if (!gb->cgb_mode && sprite->x >= lowest_sprite_x) {
                        break;
                    }
                    sprite_pixel = current_sprite_pixel;
                    lowest_sprite_x = sprite->x;
                    use_obp1 = (sprite->flags & 0x10) != 0;
                    sprite_palette = sprite->flags & 7;
                    priority = (sprite->flags & 0x80) != 0;
                    if (gb->cgb_mode) {
                        break;
                    }
                }
            }
        }
    }

    if (in_window) {
        x -= gb->io_registers[GB_IO_WX] - 7; // Todo: This value is probably latched
        y -= gb->io_registers[GB_IO_WY] + gb->wy_diff;
    }
    else {
        x += gb->effective_scx;
        y += gb->effective_scy;
    }
    if (gb->io_registers[GB_IO_LCDC] & 0x08 && !in_window) {
        map = 0x1C00;
    }
    else if (gb->io_registers[GB_IO_LCDC] & 0x40 && in_window) {
        map = 0x1C00;
    }
    tile = gb->vram[map + x/8 + y/8 * 32];
    if (gb->cgb_mode) {
        attributes = gb->vram[map + x/8 + y/8 * 32 + 0x2000];
    }

    if (attributes & 0x80) {
        priority = !bg_behind && bg_enabled;
    }

    if (!priority && sprite_pixel) {
        if (!gb->cgb_mode) {
            sprite_pixel = (gb->io_registers[use_obp1? GB_IO_OBP1:GB_IO_OBP0] >> (sprite_pixel << 1)) & 3;
            sprite_palette = use_obp1;
        }
        return gb->sprite_palettes_rgb[sprite_palette * 4 + sprite_pixel];
    }

    if (bg_enabled) {
        if (gb->io_registers[GB_IO_LCDC] & 0x10) {
            tile_address = tile * 0x10;
        }
        else {
            tile_address = (int8_t) tile * 0x10 + 0x1000;
        }
        if (attributes & 0x8) {
            tile_address += 0x2000;
        }

        if (attributes & 0x20) {
            x = ~x;
        }

        if (attributes & 0x40) {
            y = ~y;
        }

        background_pixel = (((gb->vram[tile_address + (y & 7) * 2    ] >> ((~x)&7)) & 1 ) |
                            ((gb->vram[tile_address + (y & 7) * 2 + 1] >> ((~x)&7)) & 1) << 1 );
    }

    if (priority && sprite_pixel && !background_pixel) {
        if (!gb->cgb_mode) {
            sprite_pixel = (gb->io_registers[use_obp1? GB_IO_OBP1:GB_IO_OBP0] >> (sprite_pixel << 1)) & 3;
            sprite_palette = use_obp1;
        }
        return gb->sprite_palettes_rgb[sprite_palette * 4 + sprite_pixel];
    }

    if (!gb->cgb_mode) {
        background_pixel = ((gb->io_registers[GB_IO_BGP] >> (background_pixel << 1)) & 3);
    }

    return gb->background_palettes_rgb[(attributes & 7) * 4 + background_pixel];
}

static void display_vblank(GB_gameboy_t *gb)
{
    if (gb->turbo) {
        if (GB_timing_sync_turbo(gb)) {
            return;
        }
    }
    
    if (!gb->disable_rendering && ((!(gb->io_registers[GB_IO_LCDC] & 0x80) || gb->stopped) || gb->frame_skip_state == GB_FRAMESKIP_LCD_TURNED_ON)) {
        /* LCD is off, set screen to white or black (if LCD is on in stop mode) */
        uint32_t color = (gb->io_registers[GB_IO_LCDC] & 0x80)  && gb->stopped ?
                            gb->rgb_encode_callback(gb, 0, 0, 0) :
                            gb->rgb_encode_callback(gb, 0xFF, 0xFF, 0xFF);
        for (unsigned i = 0; i < WIDTH * LINES; i++) {
            gb ->screen[i] = color;
        }
    }

    gb->vblank_callback(gb);
    GB_timing_sync(gb);

    gb->vblank_just_occured = true;
}

static inline uint8_t scale_channel(uint8_t x)
{
    return (x << 3) | (x >> 2);
}

static inline uint8_t scale_channel_with_curve(uint8_t x)
{
    return (uint8_t[]){0,2,4,7,12,18,25,34,42,52,62,73,85,97,109,121,134,146,158,170,182,193,203,213,221,230,237,243,248,251,253,255,}[x];
}

uint32_t GB_convert_rgb15(GB_gameboy_t *gb, uint16_t color)
{
    uint8_t r = (color) & 0x1F;
    uint8_t g = (color >> 5) & 0x1F;
    uint8_t b = (color >> 10) & 0x1F;
    
    if (gb->color_correction_mode == GB_COLOR_CORRECTION_DISABLED) {
        r = scale_channel(r);
        g = scale_channel(g);
        b = scale_channel(b);
    }
    else {
        r = scale_channel_with_curve(r);
        g = scale_channel_with_curve(g);
        b = scale_channel_with_curve(b);
        
        if (gb->color_correction_mode != GB_COLOR_CORRECTION_CORRECT_CURVES) {
            uint8_t new_g = (g * 3 + b) / 4;
            uint8_t new_r = r, new_b = b;
            if (gb->color_correction_mode == GB_COLOR_CORRECTION_PRESERVE_BRIGHTNESS) {
                uint8_t old_max = MAX(r, MAX(g, b));
                uint8_t new_max = MAX(new_r, MAX(new_g, new_b));
                
                if (new_max != 0) {
                    new_r = new_r * old_max / new_max;
                    new_g = new_g * old_max / new_max;
                    new_b = new_b * old_max / new_max;
                }
                
                uint8_t old_min = MIN(r, MIN(g, b));
                uint8_t new_min = MIN(new_r, MIN(new_g, new_b));
                
                if (new_min != 0xff) {
                    new_r = 0xff - (0xff - new_r) * (0xff - old_min) / (0xff - new_min);
                    new_g = 0xff - (0xff - new_g) * (0xff - old_min) / (0xff - new_min);
                    new_b = 0xff - (0xff - new_b) * (0xff - old_min) / (0xff - new_min);;
                }
            }
            r = new_r;
            g = new_g;
            b = new_b;
        }
    }
    
    return gb->rgb_encode_callback(gb, r, g, b);
}

void GB_palette_changed(GB_gameboy_t *gb, bool background_palette, uint8_t index)
{
    if (!gb->rgb_encode_callback) return;
    uint8_t *palette_data = background_palette? gb->background_palettes_data : gb->sprite_palettes_data;
    uint16_t color = palette_data[index & ~1] | (palette_data[index | 1] << 8);

    (background_palette? gb->background_palettes_rgb : gb->sprite_palettes_rgb)[index / 2] = GB_convert_rgb15(gb, color);
}

void GB_set_color_correction_mode(GB_gameboy_t *gb, GB_color_correction_mode_t mode)
{
    gb->color_correction_mode = mode;
    if (gb->is_cgb) {
        for (unsigned i = 0; i < 32; i++) {
            GB_palette_changed(gb, false, i * 2);
            GB_palette_changed(gb, true, i * 2);
        }
    }
}

/*
 STAT interrupt is implemented based on this finding:
 http://board.byuu.org/phpbb3/viewtopic.php?p=25527#p25531
 
 General timing is based on GiiBiiAdvance's documents:
 https://github.com/AntonioND/giibiiadvance
 
 */

static void update_display_state(GB_gameboy_t *gb, uint8_t cycles)
{
    if (!(gb->io_registers[GB_IO_LCDC] & 0x80)) {
        /* LCD is disabled, state is constant */
        
        /* When the LCD is off, LY is 0 and STAT mode is 0.
           Todo: Verify the LY=LYC flag should be on. */
        gb->io_registers[GB_IO_LY] = 0;
        gb->io_registers[GB_IO_STAT] &= ~3;
        gb->io_registers[GB_IO_STAT] |= 4;
        gb->effective_scx = gb->io_registers[GB_IO_SCX];
        if (gb->hdma_on_hblank) {
            gb->hdma_on_hblank = false;
            gb->hdma_on = false;
            
            /* Todo: is this correct? */
            gb->hdma_steps_left = 0xff;
        }
        
        gb->oam_read_blocked = false;
        gb->vram_read_blocked = false;
        gb->oam_write_blocked = false;
        gb->vram_write_blocked = false;
        
        /* Keep sending vblanks to user even if the screen is off */
        gb->display_cycles += cycles;
        if (gb->display_cycles >= LCDC_PERIOD) {
            /* VBlank! */
            gb->display_cycles -= LCDC_PERIOD;
            display_vblank(gb);
        }

        /* Reset window rendering state */
        gb->wy_diff = 0;
        gb->window_disabled_while_active = false;
        return;
    }
    
    uint8_t atomic_increase = gb->cgb_double_speed? 2 : 4;
    /* According to AntonioND's docs this value should be 0 in CGB mode, but tests I ran on my CGB seem to contradict
       these findings.
       Todo: Investigate what causes the difference between our findings */
    uint8_t stat_delay = gb->cgb_double_speed? 2 : 4; // (gb->cgb_mode? 0 : 4);
    /* Todo: Is this correct for DMG mode CGB? */
    uint8_t scx_delay = gb->effective_scx & 7;
    if (gb->cgb_double_speed) {
        scx_delay = (scx_delay + 1) & ~1;
    }
    else {
        scx_delay = (scx_delay + (gb->first_scanline ? 2 : 0)) & ~3;
    }
    
    /* Todo: These are correct for DMG, DMG-mode CGB, and single speed CGB. Is is correct for double speed CGB? */
    uint8_t oam_blocking_rush = gb->cgb_double_speed? 2 : 4;
    uint8_t vram_blocking_rush = gb->is_cgb? 0 : 4;
    
    for (; cycles; cycles -= atomic_increase) {
        bool dmg_future_stat = false;
        gb->io_registers[GB_IO_IF] |= gb->future_interrupts & 3;
        gb->future_interrupts &= ~3;
        
        bool previous_stat_interrupt_line = gb->stat_interrupt_line;
        gb->stat_interrupt_line = false;
        
        gb->display_cycles += atomic_increase;
        /* The very first line is 4 clocks shorter when the LCD turns on. Verified on SGB2, CGB in CGB mode and
         CGB in double speed mode. */
        if (gb->first_scanline && gb->display_cycles >= LINE_LENGTH - 8) {
            gb->first_scanline = false;
            gb->display_cycles += 4;
        }
        bool should_compare_ly = true;
        uint8_t ly_for_comparison = gb->io_registers[GB_IO_LY] = gb->display_cycles / LINE_LENGTH;
        bool just_entered_hblank = false;
        
        
        /* Handle cycle completion. STAT's initial value depends on model and mode */
        if (gb->display_cycles == LCDC_PERIOD) {
            /* VBlank! */
            gb->display_cycles = 0;
            gb->io_registers[GB_IO_STAT] &= ~3;
            if (gb->is_cgb) {
                if (stat_delay) {
                    gb->io_registers[GB_IO_STAT] |= 1;
                }
                else {
                    gb->io_registers[GB_IO_STAT] |= 2;
                }
            }
            ly_for_comparison = gb->io_registers[GB_IO_LY] = 0;
            
            /* Todo: verify timing */
            gb->oam_read_blocked = true;
            gb->vram_read_blocked = false;
            gb->oam_write_blocked = true;
            gb->vram_write_blocked = false;

            
            /* Reset window rendering state */
            gb->wy_diff = 0;
            gb->window_disabled_while_active = false;
        }
        
        /* Entered VBlank state, update STAT and IF */
        else if (gb->display_cycles == LINES * LINE_LENGTH + stat_delay) {
            gb->io_registers[GB_IO_STAT] &= ~3;
            gb->io_registers[GB_IO_STAT] |= 1;
            if (gb->is_cgb) {
                gb->future_interrupts |= 1;
            }
            else {
                gb->io_registers[GB_IO_IF] |= 1;
            }
            
            /* Entering VBlank state triggers the OAM interrupt. In CGB, it happens 4 cycles earlier */
            if (gb->io_registers[GB_IO_STAT] & 0x20 && !gb->is_cgb) {
                gb->stat_interrupt_line = true;
            }
            if (gb->frame_skip_state == GB_FRAMESKIP_LCD_TURNED_ON) {
                if (!gb->is_cgb) {
                    display_vblank(gb);
                    gb->frame_skip_state = GB_FRAMESKIP_SECOND_FRAME_RENDERED;
                }
                else {
                    gb->frame_skip_state = GB_FRAMESKIP_FIRST_FRAME_SKIPPED;
                }
            }
            else {
                gb->frame_skip_state = GB_FRAMESKIP_SECOND_FRAME_RENDERED;
                display_vblank(gb);
            }
        }

        
        /* Handle line 0 right after turning the LCD on  */
        else if (gb->first_scanline) {
            /* OAM and VRAM blocking is not rushed in the very first scanline */
            if (gb->display_cycles == atomic_increase) {
                gb->io_registers[GB_IO_STAT] &= ~3;
                gb->oam_read_blocked = false;
                gb->vram_read_blocked = false;
                gb->oam_write_blocked = false;
                gb->vram_write_blocked = false;
            }
            else if (gb->display_cycles == MODE2_LENGTH) {
                gb->io_registers[GB_IO_STAT] &= ~3;
                gb->io_registers[GB_IO_STAT] |= 3;
                gb->effective_scx = gb->io_registers[GB_IO_SCX];
                gb->oam_read_blocked = true;
                gb->vram_read_blocked = true;
                gb->oam_write_blocked = true;
                gb->vram_write_blocked = true;
            }
            else if (gb->display_cycles == MODE2_LENGTH + MODE3_LENGTH + scx_delay) {
                gb->io_registers[GB_IO_STAT] &= ~3;
                gb->oam_read_blocked = false;
                gb->vram_read_blocked = false;
                gb->oam_write_blocked = false;
                gb->vram_write_blocked = false;
                just_entered_hblank = true;
            }
        }
        
        /* Handle STAT changes for lines 0-143 */
        else if (gb->display_cycles < LINES * LINE_LENGTH) {
            unsigned position_in_line = gb->display_cycles % LINE_LENGTH;
            
            /* Handle OAM and VRAM blocking */
            /* Todo: verify CGB timing for write blocking */
            if (position_in_line == stat_delay - oam_blocking_rush ||
                 // In case stat_delay is 0
                (position_in_line == LINE_LENGTH + stat_delay - oam_blocking_rush && gb->io_registers[GB_IO_LY] != 143)) {
                gb->oam_read_blocked = true;
                gb->oam_write_blocked = gb->is_cgb;
            }
            else if (position_in_line == MODE2_LENGTH + stat_delay - vram_blocking_rush) {
                gb->vram_read_blocked = true;
                gb->vram_write_blocked = gb->is_cgb;
            }
            
            if (position_in_line == stat_delay) {
                gb->oam_write_blocked = true;
            }
            else if (!gb->is_cgb && position_in_line == MODE2_LENGTH + stat_delay - oam_blocking_rush) {
                gb->oam_write_blocked = false;
            }
            else if (position_in_line == MODE2_LENGTH + stat_delay) {
                gb->vram_write_blocked = true;
                gb->oam_write_blocked = true;
            }
            
            /* Handle everything else */
            /* OAM interrupt happens slightly before STAT is actually updated. (About 1-3 T-cycles)
               Todo: Test double speed CGB */
            if (position_in_line == 0 && stat_delay) {
                if (gb->io_registers[GB_IO_STAT] & 0x20) {
                    gb->stat_interrupt_line = true;
                    dmg_future_stat = true;
                }
            }
            if (position_in_line == stat_delay) {
                gb->io_registers[GB_IO_STAT] &= ~3;
                gb->io_registers[GB_IO_STAT] |= 2;
            }
            else if (position_in_line == 0 && gb->display_cycles != 0) {
                should_compare_ly = gb->is_cgb;
                ly_for_comparison--;
            }
            else if (position_in_line == MODE2_LENGTH + stat_delay) {
                gb->io_registers[GB_IO_STAT] &= ~3;
                gb->io_registers[GB_IO_STAT] |= 3;
                gb->effective_scx = gb->io_registers[GB_IO_SCX];
                gb->previous_lcdc_x = - (gb->effective_scx & 0x7);
            }
            else if (position_in_line == MODE2_LENGTH + MODE3_LENGTH + stat_delay + scx_delay) {
                just_entered_hblank = true;
                gb->io_registers[GB_IO_STAT] &= ~3;
                gb->oam_read_blocked = false;
                gb->vram_read_blocked = false;
                gb->oam_write_blocked = false;
                gb->vram_write_blocked = false;
                if (gb->hdma_on_hblank) {
                    gb->hdma_on = true;
                    gb->hdma_cycles = 0;
                }
            }
        }
        
        /* Line 153 is special */
        else if (gb->display_cycles >= (VIRTUAL_LINES - 1) * LINE_LENGTH) {
            /* DMG */
            if (!gb->is_cgb) {
                switch (gb->display_cycles - (VIRTUAL_LINES - 1) * LINE_LENGTH) {
                    case 0:
                        should_compare_ly = false;
                        break;
                    case 4:
                        gb->io_registers[GB_IO_LY] = 0;
                        ly_for_comparison = VIRTUAL_LINES - 1;
                        break;
                    case 8:
                        gb->io_registers[GB_IO_LY] = 0;
                        should_compare_ly = false;
                        break;
                    default:
                        gb->io_registers[GB_IO_LY] = 0;
                        ly_for_comparison = 0;
                }
            }
            /* CGB in DMG mode */
            else if (!gb->cgb_mode) {
                switch (gb->display_cycles - (VIRTUAL_LINES - 1) * LINE_LENGTH) {
                    case 0:
                        ly_for_comparison = VIRTUAL_LINES - 2;
                        break;
                    case 4:
                        break;
                    case 8:
                        gb->io_registers[GB_IO_LY] = 0;
                        break;
                    default:
                        gb->io_registers[GB_IO_LY] = 0;
                        ly_for_comparison = 0;
                }
            }
            /* Single speed CGB */
            else if (!gb->cgb_double_speed) {
                switch (gb->display_cycles - (VIRTUAL_LINES - 1) * LINE_LENGTH) {
                    case 0:
                        break;
                    case 4:
                        gb->io_registers[GB_IO_LY] = 0;
                        break;
                    default:
                        gb->io_registers[GB_IO_LY] = 0;
                        ly_for_comparison = 0;
                }
            }
            
            /* Double speed CGB */
            else {
                switch (gb->display_cycles - (VIRTUAL_LINES - 1) * LINE_LENGTH) {
                    case 0:
                        ly_for_comparison = VIRTUAL_LINES - 2;
                        break;
                    case 2:
                    case 4:
                        break;
                    case 6:
                    case 8:
                        gb->io_registers[GB_IO_LY] = 0;
                        break;
                    default:
                        gb->io_registers[GB_IO_LY] = 0;
                        ly_for_comparison = 0;
                }
            }
        }
        
        /* Lines 144 - 152 */
        else {
            if (stat_delay && gb->display_cycles % LINE_LENGTH == 0) {
                should_compare_ly = gb->is_cgb;
                ly_for_comparison--;
            }
        }
        
        /* Set LY=LYC bit */
        if (should_compare_ly && (ly_for_comparison == gb->io_registers[GB_IO_LYC])) {
            gb->io_registers[GB_IO_STAT] |= 4;
        }
        else {
            gb->io_registers[GB_IO_STAT] &= ~4;
        }
        
        if (!gb->stat_interrupt_line) {
            switch (gb->io_registers[GB_IO_STAT] & 3) {
                case 0:
                    gb->stat_interrupt_line = (gb->io_registers[GB_IO_STAT] & 8);
                    if (!gb->cgb_double_speed && just_entered_hblank && ((gb->effective_scx + (gb->first_scanline ? 2 : 0)) & 3) == 3) {
                        gb->stat_interrupt_line = false;
                    }
                    else if (just_entered_hblank && ((gb->effective_scx + (gb->first_scanline ? 2 : 0)) & 3) != 0) {
                        dmg_future_stat = true;
                    }
                    break;
                case 1: gb->stat_interrupt_line = gb->io_registers[GB_IO_STAT] & 0x10; break;
                case 2: gb->stat_interrupt_line = gb->io_registers[GB_IO_STAT] & 0x20; break;
            }
            
            /* User requested a LY=LYC interrupt and the LY=LYC bit is on */
            if ((gb->io_registers[GB_IO_STAT] & 0x44) == 0x44) {
                gb->stat_interrupt_line = true;
                dmg_future_stat = false;
            }
        }
        
        /* On the CGB, the last cycle of line 144 triggers an OAM interrupt
         Todo: Verify timing for CGB in CGB mode and double speed CGB */
        if (gb->is_cgb &&
            gb->display_cycles == LINES * LINE_LENGTH + stat_delay - atomic_increase &&
            (gb->io_registers[GB_IO_STAT] & 0x20)) {
            gb->stat_interrupt_line = true;
        }
        
        if (gb->stat_interrupt_line && !previous_stat_interrupt_line) {
            if (gb->is_cgb || dmg_future_stat) {
                gb->future_interrupts |= 2;
            }
            else {
                gb->io_registers[GB_IO_IF] |= 2;
            }
        }
    };
    
#if 0
    /* The value of LY is glitched in the last cycle of every line in CGB mode CGB in single speed
       This is based on AntonioND's docs, however I could not reproduce these findings on my CGB.
       Todo: Find out why my tests contradict these docs */
    if (gb->cgb_mode && !gb->cgb_double_speed &&
        gb->display_cycles % LINE_LENGTH == LINE_LENGTH - 4) {
        uint8_t glitch_pattern[] = {0, 0, 2, 0, 4, 4, 6, 0, 8};
        if ((gb->io_registers[GB_IO_LY] & 0xF) == 0xF) {
            gb->io_registers[GB_IO_LY] = glitch_pattern[gb->io_registers[GB_IO_LY] >> 4] << 4;
        }
        else {
            gb->io_registers[GB_IO_LY] = glitch_pattern[gb->io_registers[GB_IO_LY] & 7] | (gb->io_registers[GB_IO_LY] & 0xF8);
        }
    }
#endif
}

void GB_display_run(GB_gameboy_t *gb, uint8_t cycles)
{
    update_display_state(gb, cycles);
    if (gb->disable_rendering) {
        return;
    }
    
    /*
       Display controller bug: For some reason, the OAM STAT interrupt is called, as expected, for LY = 0..143.
       However, it is also called from LY = 144.

       See http://forums.nesdev.com/viewtopic.php?f=20&t=13727
    */

    if (!(gb->io_registers[GB_IO_LCDC] & 0x80)) {
        /* LCD is disabled, do nothing */
        return;
    }
    if (gb->display_cycles >= LINE_LENGTH * 144) { /* VBlank */
        return;
    }
    
    uint8_t effective_ly = gb->display_cycles / LINE_LENGTH;


    if (gb->display_cycles % LINE_LENGTH < MODE2_LENGTH) { /* Mode 2 */
        return;
    }


    /* Render */
    int16_t current_lcdc_x = gb->display_cycles % LINE_LENGTH - MODE2_LENGTH - (gb->effective_scx & 0x7) - 7;
    
    for (;gb->previous_lcdc_x < current_lcdc_x; gb->previous_lcdc_x++) {
        if (gb->previous_lcdc_x >= WIDTH) {
            continue;
        }
        
        if (((gb->previous_lcdc_x + gb->effective_scx) & 7) == 0) {
            gb->effective_scy = gb->io_registers[GB_IO_SCY];
        }
        
        if (gb->previous_lcdc_x < 0) {
            continue;
        }
        gb->screen[effective_ly * WIDTH + gb->previous_lcdc_x] =
        get_pixel(gb, gb->previous_lcdc_x, effective_ly);
    }
}

void GB_draw_tileset(GB_gameboy_t *gb, uint32_t *dest, GB_palette_type_t palette_type, uint8_t palette_index)
{
    uint32_t none_palette[4];
    uint32_t *palette = NULL;
    
    switch (gb->is_cgb? palette_type : GB_PALETTE_NONE) {
        default:
        case GB_PALETTE_NONE:
            none_palette[0] = gb->rgb_encode_callback(gb, 0xFF, 0xFF, 0xFF);
            none_palette[1] = gb->rgb_encode_callback(gb, 0xAA, 0xAA, 0xAA);
            none_palette[2] = gb->rgb_encode_callback(gb, 0x55, 0x55, 0x55);
            none_palette[3] = gb->rgb_encode_callback(gb, 0,    0,    0   );
            palette = none_palette;
            break;
        case GB_PALETTE_BACKGROUND:
            palette = gb->background_palettes_rgb + (4 * (palette_index & 7));
            break;
        case GB_PALETTE_OAM:
            palette = gb->sprite_palettes_rgb + (4 * (palette_index & 7));
            break;
    }
    
    for (unsigned y = 0; y < 192; y++) {
        for (unsigned x = 0; x < 256; x++) {
            if (x >= 128 && !gb->is_cgb) {
                *(dest++) = gb->background_palettes_rgb[0];
                continue;
            }
            uint16_t tile = (x % 128) / 8 + y / 8 * 16;
            uint16_t tile_address = tile * 0x10 + (x >= 128? 0x2000 : 0);
            uint8_t pixel = (((gb->vram[tile_address + (y & 7) * 2    ] >> ((~x)&7)) & 1 ) |
                             ((gb->vram[tile_address + (y & 7) * 2 + 1] >> ((~x)&7)) & 1) << 1);
            
            if (!gb->cgb_mode) {
                if (palette_type == GB_PALETTE_BACKGROUND) {
                    pixel = ((gb->io_registers[GB_IO_BGP] >> (pixel << 1)) & 3);
                }
                else if (!gb->cgb_mode) {
                    if (palette_type == GB_PALETTE_OAM) {
                        pixel = ((gb->io_registers[palette_index == 0? GB_IO_OBP0 : GB_IO_OBP1] >> (pixel << 1)) & 3);
                    }
                }
            }
            
            
            *(dest++) = palette[pixel];
        }
    }
}

void GB_draw_tilemap(GB_gameboy_t *gb, uint32_t *dest, GB_palette_type_t palette_type, uint8_t palette_index, GB_map_type_t map_type, GB_tileset_type_t tileset_type)
{
    uint32_t none_palette[4];
    uint32_t *palette = NULL;
    uint16_t map = 0x1800;
    
    switch (gb->is_cgb? palette_type : GB_PALETTE_NONE) {
        case GB_PALETTE_NONE:
            none_palette[0] = gb->rgb_encode_callback(gb, 0xFF, 0xFF, 0xFF);
            none_palette[1] = gb->rgb_encode_callback(gb, 0xAA, 0xAA, 0xAA);
            none_palette[2] = gb->rgb_encode_callback(gb, 0x55, 0x55, 0x55);
            none_palette[3] = gb->rgb_encode_callback(gb, 0,    0,    0   );
            palette = none_palette;
            break;
        case GB_PALETTE_BACKGROUND:
            palette = gb->background_palettes_rgb + (4 * (palette_index & 7));
            break;
        case GB_PALETTE_OAM:
            palette = gb->sprite_palettes_rgb + (4 * (palette_index & 7));
            break;
        case GB_PALETTE_AUTO:
            break;
    }
    
    if (map_type == GB_MAP_9C00 || (map_type == GB_MAP_AUTO && gb->io_registers[GB_IO_LCDC] & 0x08)) {
        map = 0x1c00;
    }
    
    if (tileset_type == GB_TILESET_AUTO) {
        tileset_type = (gb->io_registers[GB_IO_LCDC] & 0x10)? GB_TILESET_8800 : GB_TILESET_8000;
    }
    
    for (unsigned y = 0; y < 256; y++) {
        for (unsigned x = 0; x < 256; x++) {
            uint8_t tile = gb->vram[map + x/8 + y/8 * 32];
            uint16_t tile_address;
            uint8_t attributes = 0;
            
            if (tileset_type == GB_TILESET_8800) {
                tile_address = tile * 0x10;
            }
            else {
                tile_address = (int8_t) tile * 0x10 + 0x1000;
            }
            
            if (gb->cgb_mode) {
                attributes = gb->vram[map + x/8 + y/8 * 32 + 0x2000];
            }
            
            if (attributes & 0x8) {
                tile_address += 0x2000;
            }
            
            uint8_t pixel = (((gb->vram[tile_address + (((attributes & 0x40)? ~y : y) & 7) * 2    ] >> (((attributes & 0x20)? x : ~x)&7)) & 1 ) |
                             ((gb->vram[tile_address + (((attributes & 0x40)? ~y : y) & 7) * 2 + 1] >> (((attributes & 0x20)? x : ~x)&7)) & 1) << 1);
            
            if (!gb->cgb_mode && (palette_type == GB_PALETTE_BACKGROUND || palette_type == GB_PALETTE_AUTO)) {
                pixel = ((gb->io_registers[GB_IO_BGP] >> (pixel << 1)) & 3);
            }
            
            if (palette) {
                *(dest++) = palette[pixel];
            }
            else {
                *(dest++) = gb->background_palettes_rgb[(attributes & 7) * 4 + pixel];
            }
        }
    }
}

uint8_t GB_get_oam_info(GB_gameboy_t *gb, GB_oam_info_t *dest, uint8_t *sprite_height)
{
    uint8_t count = 0;
    *sprite_height = (gb->io_registers[GB_IO_LCDC] & 4) ? 16:8;
    uint8_t oam_to_dest_index[40] = {0,};
    for (unsigned y = 0; y < LINES; y++) {
        GB_sprite_t *sprite = (GB_sprite_t *) &gb->oam;
        uint8_t sprites_in_line = 0;
        for (uint8_t i = 0; i < 40; i++, sprite++) {
            int sprite_y = sprite->y - 16;
            bool obscured = false;
            // Is sprite not in this line?
            if (sprite_y > y || sprite_y + *sprite_height <= y) continue;
            if (++sprites_in_line == 11) obscured = true;
            
            GB_oam_info_t *info = NULL;
            if (!oam_to_dest_index[i]) {
                info = dest + count;
                oam_to_dest_index[i] = ++count;
                info->x = sprite->x;
                info->y = sprite->y;
                info->tile = *sprite_height == 16? sprite->tile & 0xFE : sprite->tile;
                info->flags = sprite->flags;
                info->obscured_by_line_limit = false;
                info->oam_addr = 0xFE00 + i * sizeof(*sprite);
            }
            else {
                info = dest + oam_to_dest_index[i] - 1;
            }
            info->obscured_by_line_limit |= obscured;
        }
    }
    
    
    for (unsigned i = 0; i < count; i++) {
        uint16_t vram_address = dest[i].tile * 0x10;
        uint8_t flags = dest[i].flags;
        uint8_t palette = gb->cgb_mode? (flags & 7) : ((flags & 0x10)? 1 : 0);
        if (gb->is_cgb && (flags & 0x8)) {
            vram_address += 0x2000;
        }

        for (unsigned y = 0; y < *sprite_height; y++) {
            for (unsigned x = 0; x < 8; x++) {
                uint8_t color = (((gb->vram[vram_address    ] >> ((~x)&7)) & 1 ) |
                                 ((gb->vram[vram_address + 1] >> ((~x)&7)) & 1) << 1 );
                
                if (!gb->cgb_mode) {
                    color = (gb->io_registers[palette? GB_IO_OBP1:GB_IO_OBP0] >> (color << 1)) & 3;
                }
                dest[i].image[((flags & 0x20)?7-x:x) + ((flags & 0x40)?*sprite_height - 1 -y:y) * 8] = gb->sprite_palettes_rgb[palette * 4 + color];
            }
            vram_address += 2;
        }
    }
    return count;
}

/* Called when a write might enable or disable the window */
void GB_window_related_write(GB_gameboy_t *gb, uint8_t addr, uint8_t value)
{
    bool before = window_enabled(gb);
    gb->io_registers[addr] = value;
    bool after = window_enabled(gb);
    
    if (before != after && gb->display_cycles < LINES * LINE_LENGTH) {
        /* Window was disabled or enabled outside of vblank */
        uint8_t current_line = gb->display_cycles / LINE_LENGTH;
        if (current_line >= gb->io_registers[GB_IO_WY]) {
            if (after) {
                if (!gb->window_disabled_while_active) {
                    /* Window was turned on for the first time this frame while LY > WY,
                       should start window in the next line */
                    gb->wy_diff = current_line + 1 - gb->io_registers[GB_IO_WY];
                }
                else {
                    gb->wy_diff += current_line;
                }
            }
            else {
                gb->wy_diff -= current_line;
                gb->window_disabled_while_active = true;
            }
        }
    }
}
