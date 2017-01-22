#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <assert.h>

#include "get_image_for_rom.h"
#include "gb.h"

#define LENGTH 60 * 10

struct local_data {
    unsigned long frames;
    bool running;
};

static char *async_input_callback(GB_gameboy_t *gb)
{
    return NULL;
}

static void log_callback(GB_gameboy_t *gb, const char *string, GB_log_attributes attributes)
{
    
}


static void vblank(GB_gameboy_t *gb)
{

    struct local_data *local_data = (struct local_data *)gb->user_data;

    if (local_data->frames == LENGTH) {
        local_data->running = false;
    }
    else if (local_data->frames == LENGTH - 1) {
        gb->disable_rendering = false;
    }
    
    local_data->frames++;
}

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
    return (b << 16) | (g << 8) | (r) | 0xFF000000;
}

int get_image_for_rom(const char *filename, const char *boot_path, uint32_t *output, uint8_t *cgb_flag)
{
    GB_gameboy_t gb;
    GB_init_cgb(&gb);
    if (GB_load_boot_rom(&gb, boot_path)) {
        GB_free(&gb);
        return 1;
    }
        
    GB_set_vblank_callback(&gb, (GB_vblank_callback_t) vblank);
    GB_set_pixels_output(&gb, output);
    GB_set_rgb_encode_callback(&gb, rgb_encode);
    GB_set_async_input_callback(&gb, async_input_callback);
    GB_set_log_callback(&gb, log_callback);
    
    if (GB_load_rom(&gb, filename)) {
        GB_free(&gb);
        return 1;
    }
        
    /* Run emulation */
    struct local_data local_data = {0,};
    gb.user_data = &local_data;
    local_data.running = true;
    local_data.frames = 0;
    gb.turbo = gb.turbo_dont_skip = gb.disable_rendering = true;
    
    while (local_data.running) {
        GB_run(&gb);
    }
    
    *cgb_flag = gb.rom[0x143] & 0xC0;
    
    GB_free(&gb);
    return 0;
}

