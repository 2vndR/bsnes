#include <stdint.h>
#include <math.h>
#include <string.h>
#include "gb.h"

#define likely(x)   __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

static void refresh_channel(GB_gameboy_t *gb, unsigned index, unsigned cycles_offset)
{
    unsigned multiplier = gb->apu_output.cycles_since_render + cycles_offset - gb->apu_output.last_update[index];
    gb->apu_output.summed_samples[index].left += gb->apu_output.current_sample[index].left * multiplier;
    gb->apu_output.summed_samples[index].right += gb->apu_output.current_sample[index].right * multiplier;
    gb->apu_output.last_update[index] = gb->apu_output.cycles_since_render + cycles_offset;
}

static void update_sample(GB_gameboy_t *gb, unsigned index, uint8_t value, unsigned cycles_offset)
{
    gb->apu.samples[index] = value;
    if (gb->apu_output.sample_rate) {
        unsigned left_volume = 0;
        if (gb->io_registers[GB_IO_NR51] & (1 << index)) {
            left_volume = gb->io_registers[GB_IO_NR50] & 7;
        }
        unsigned right_volume = 0;
        if (gb->io_registers[GB_IO_NR51] & (0x10 << index)) {
            right_volume = (gb->io_registers[GB_IO_NR50] >> 4) & 7;;
        }
        GB_sample_t output = {value * left_volume, value * right_volume};
        if (*(uint32_t *)&(gb->apu_output.current_sample[index]) != *(uint32_t *)&output) {
            refresh_channel(gb, index, cycles_offset);
            gb->apu_output.current_sample[index] = output;
        }
    }
}

static void render(GB_gameboy_t *gb)
{
    GB_sample_t output = {0,0};
    for (unsigned i = GB_N_CHANNELS; i--;) {
        if (likely(gb->apu_output.last_update[i] == 0)) {
            output.left += gb->apu_output.current_sample[i].left * CH_STEP;
            output.right += gb->apu_output.current_sample[i].right * CH_STEP;
        }
        else {
            refresh_channel(gb, i, 0);
            output.left += (unsigned) gb->apu_output.summed_samples[i].left * CH_STEP
                            / gb->apu_output.cycles_since_render;
            output.right += (unsigned) gb->apu_output.summed_samples[i].right * CH_STEP
                            / gb->apu_output.cycles_since_render;
            gb->apu_output.summed_samples[i] = (GB_sample_t){0, 0};
        }
        gb->apu_output.last_update[i] = 0;
    }
    gb->apu_output.cycles_since_render = 0;
    

    while (gb->apu_output.copy_in_progress);
    while (!__sync_bool_compare_and_swap(&gb->apu_output.lock, false, true));
    if (gb->apu_output.buffer_position < gb->apu_output.buffer_size) {
        gb->apu_output.buffer[gb->apu_output.buffer_position++] = output;
    }
    gb->apu_output.lock = false;
}

void GB_apu_div_event(GB_gameboy_t *gb)
{
    if (gb->apu.is_active[GB_WAVE] && gb->apu.wave_channel.length_enabled) {
        if (gb->apu.wave_channel.pulse_length) {
            gb->apu.wave_channel.pulse_length--;
        }
        else {
            gb->apu.is_active[GB_WAVE] = false;
            gb->apu.wave_channel.current_sample = 0;
            update_sample(gb, GB_WAVE, 0, 0);
        }
    }
}


void GB_apu_run(GB_gameboy_t *gb, uint8_t cycles)
{
    /* Convert 4MHZ to 2MHz. cycles is always even. */
    cycles >>= 1;
    
    gb->apu.wave_channel.wave_form_just_read = false;
    if (gb->apu.is_active[GB_WAVE]) {
        uint8_t cycles_left = cycles;
        while (unlikely(cycles_left > gb->apu.wave_channel.sample_countdown)) {
            cycles_left -= gb->apu.wave_channel.sample_countdown + 1;
            gb->apu.wave_channel.sample_countdown = gb->apu.wave_channel.sample_length;
            gb->apu.wave_channel.current_sample_index++;
            gb->apu.wave_channel.current_sample_index &= 0x1F;
            gb->apu.wave_channel.current_sample =
                gb->apu.wave_channel.wave_form[gb->apu.wave_channel.current_sample_index];
            update_sample(gb, GB_WAVE,
                          gb->apu.wave_channel.current_sample >> gb->apu.wave_channel.shift,
                          cycles - cycles_left);
            gb->apu.wave_channel.wave_form_just_read = true;
        }
        if (cycles_left) {
            gb->apu.wave_channel.sample_countdown -= cycles_left;
            gb->apu.wave_channel.wave_form_just_read = false;
        }
    }
    
    if (gb->apu_output.sample_rate) {
        gb->apu_output.cycles_since_render += cycles;
        double cycles_per_sample = CPU_FREQUENCY / (double)gb->apu_output.sample_rate; // TODO: this should be cached!
        
        if (gb->apu_output.sample_cycles > cycles_per_sample) {
            gb->apu_output.sample_cycles -= cycles_per_sample;
            render(gb);
        }
    }
}

void GB_apu_copy_buffer(GB_gameboy_t *gb, GB_sample_t *dest, size_t count)
{
    gb->apu_output.copy_in_progress = true;

    if (!gb->apu_output.stream_started) {
        // Intentionally fail the first copy to sync the stream with the Gameboy.
        gb->apu_output.stream_started = true;
        gb->apu_output.buffer_position = 0;
    }

    if (count > gb->apu_output.buffer_position) {
        // GB_log(gb, "Audio underflow: %d\n", count - gb->apu_output.buffer_position);
        if (gb->apu_output.buffer_position != 0) {
            for (unsigned i = 0; i < count - gb->apu_output.buffer_position; i++) {
                dest[gb->apu_output.buffer_position + i] = gb->apu_output.buffer[gb->apu_output.buffer_position - 1];
            }
        }
        else {
            memset(dest + gb->apu_output.buffer_position, 0, (count - gb->apu_output.buffer_position) * sizeof(*gb->apu_output.buffer));
        }
        count = gb->apu_output.buffer_position;
    }
    memcpy(dest, gb->apu_output.buffer, count * sizeof(*gb->apu_output.buffer));
    memmove(gb->apu_output.buffer, gb->apu_output.buffer + count, (gb->apu_output.buffer_position - count) * sizeof(*gb->apu_output.buffer));
    gb->apu_output.buffer_position -= count;

    gb->apu_output.copy_in_progress = false;
}

void GB_apu_init(GB_gameboy_t *gb)
{
    memset(&gb->apu, 0, sizeof(gb->apu));
    // gb->apu.wave_channels[0].duty = gb->apu.wave_channels[1].duty = 4;
    // gb->apu.lfsr = 0x7FFF;
    gb->io_registers[GB_IO_NR50] = 0x77;
    for (int i = 0; i < 4; i++) {
        gb->apu.left_enabled[i] = gb->apu.right_enabled[i] = true;
    }
    gb->apu.wave_channel.sample_length = 0x7FF;
}

uint8_t GB_apu_read(GB_gameboy_t *gb, uint8_t reg)
{
    if (reg == GB_IO_NR52) {
        uint8_t value = 0;
        for (int i = 0; i < GB_N_CHANNELS; i++) {
            value >>= 1;
            if (gb->apu.is_active[i]) {
                value |= 0x8;
            }
        }
        if (gb->apu.global_enable) {
            value |= 0x80;
        }
        value |= 0x70;
        return value;
    }

    static const char read_mask[GB_IO_WAV_END - GB_IO_NR10 + 1] = {
     /* NRX0  NRX1  NRX2  NRX3  NRX4 */
        0x80, 0x3F, 0x00, 0xFF, 0xBF, // NR1X
        0xFF, 0x3F, 0x00, 0xFF, 0xBF, // NR2X
        0x7F, 0xFF, 0x9F, 0xFF, 0xBF, // NR3X
        0xFF, 0xFF, 0x00, 0x00, 0xBF, // NR4X
        0x00, 0x00, 0x70, 0xFF, 0xFF, // NR5X

        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Unused
        // Wave RAM
        0, /* ... */
    };

    if (reg >= GB_IO_WAV_START && reg <= GB_IO_WAV_END && gb->apu.is_active[GB_WAVE]) {
        if (!gb->is_cgb && !gb->apu.wave_channel.wave_form_just_read) {
            return 0xFF;
        }
        reg = GB_IO_WAV_START + gb->apu.wave_channel.current_sample_index / 2;
    }

    return gb->io_registers[reg] | read_mask[reg - GB_IO_NR10];
}

void GB_apu_write(GB_gameboy_t *gb, uint8_t reg, uint8_t value)
{
    if (!gb->apu.global_enable && reg != GB_IO_NR52) {
        return;
    }

    if (reg >= GB_IO_WAV_START && reg <= GB_IO_WAV_END && gb->apu.is_active[GB_WAVE]) {
        if (!gb->is_cgb && !gb->apu.wave_channel.wave_form_just_read) {
            return;
        }
        reg = GB_IO_WAV_START + gb->apu.wave_channel.current_sample_index / 2;
    }
    
    gb->io_registers[reg] = value;


    switch (reg) {
        /* Globals */
        case GB_IO_NR50:
        case GB_IO_NR51:
            /* These registers affect the output of all 3 channels (but not the output of the PCM registers).*/
            /* We call update_samples with the current value so the APU output is updated with the new outputs */
            for (unsigned i = GB_N_CHANNELS; i--;) {
                update_sample(gb, i, gb->apu.samples[i], 0);
            }
            break;
        case GB_IO_NR52:
            if ((value & 0x80) && !gb->apu.global_enable) {
                GB_apu_init(gb);
                gb->apu.global_enable = true;
            }
            else if (!(value & 0x80) && gb->apu.global_enable)  {
                for (unsigned i = GB_N_CHANNELS; i--;) {
                    update_sample(gb, i, 0, 0);
                }
                memset(&gb->apu, 0, sizeof(gb->apu));
                memset(gb->io_registers + GB_IO_NR10, 0, GB_IO_WAV_START - GB_IO_NR10);
                gb->apu.global_enable = false;
            }
            break;
            
        /* Wave channel */
        case GB_IO_NR30:
            gb->apu.wave_channel.enable = value & 0x80;
            if (!gb->apu.wave_channel.enable) {
                gb->apu.is_active[GB_WAVE] = false;
                gb->apu.wave_channel.current_sample = 0;
                update_sample(gb, GB_WAVE, 0, 0);
            }
            break;
        case GB_IO_NR31:
            break;
        case GB_IO_NR32:
            gb->apu.wave_channel.shift = (uint8_t[]){4, 0, 1, 2}[(value >> 5) & 3];
            update_sample(gb, GB_WAVE, gb->apu.wave_channel.current_sample >> gb->apu.wave_channel.shift, 0);
            break;
        case GB_IO_NR33:
            gb->apu.wave_channel.sample_length &= ~0xFF;
            gb->apu.wave_channel.sample_length |= (~value) & 0xFF;
            break;
        case GB_IO_NR34:
            gb->apu.wave_channel.length_enabled = value & 0x40;
            gb->apu.wave_channel.sample_length &= 0xFF;
            gb->apu.wave_channel.sample_length |= ((~value) & 7) << 8;
            if ((value & 0x80) && gb->apu.wave_channel.enable) {
                /* DMG bug: wave RAM gets corrupted if the channel is retriggerred 1 cycle before the APU
                            reads from it. */
                if (!gb->is_cgb && gb->apu.is_active[GB_WAVE] && gb->apu.wave_channel.sample_countdown == 0) {
                    unsigned offset = ((gb->apu.wave_channel.current_sample_index + 1) >> 1) & 0xF;
                    
                    /* On SGB2 (and probably SGB1 and MGB as well) this behavior is not accurate,
                       however these systems are not currently emulated. */
                    if (offset < 4) {
                        gb->io_registers[GB_IO_WAV_START] = gb->io_registers[GB_IO_WAV_START + offset];
                        gb->apu.wave_channel.wave_form[0] = gb->apu.wave_channel.wave_form[offset / 2];
                        gb->apu.wave_channel.wave_form[1] = gb->apu.wave_channel.wave_form[offset / 2 + 1];
                    }
                    else {
                        memcpy(gb->io_registers + GB_IO_WAV_START,
                               gb->io_registers + GB_IO_WAV_START + (offset & ~3),
                               4);
                        memcpy(gb->apu.wave_channel.wave_form,
                               gb->apu.wave_channel.wave_form + (offset & ~3) * 2,
                               8);
                    }
                }
                gb->apu.is_active[GB_WAVE] = true;
                gb->apu.wave_channel.pulse_length = ~gb->io_registers[GB_IO_NR31];
                gb->apu.wave_channel.sample_countdown = gb->apu.wave_channel.sample_length + 3;
                gb->apu.wave_channel.current_sample_index = 0;
                /* Note that we don't change the sample just yet! This was verified on hardware. */
            }
            break;
        
        default:
            if (reg >= GB_IO_WAV_START && reg <= GB_IO_WAV_END) {
                gb->apu.wave_channel.wave_form[(reg - GB_IO_WAV_START) * 2]     = value >> 4;
                gb->apu.wave_channel.wave_form[(reg - GB_IO_WAV_START) * 2 + 1] = value & 0xF;
            }
    }


}

size_t GB_apu_get_current_buffer_length(GB_gameboy_t *gb)
{
    return  gb->apu_output.buffer_position;
}
