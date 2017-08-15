#include <stdint.h>
#include <math.h>
#include <string.h>
#include "gb.h"

#define likely(x)   __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

static const uint8_t duties[] = {
    0, 0, 0, 0, 0, 0, 1, 0,
    0, 0, 0, 0, 0, 0, 1, 1,
    0, 0, 0, 0, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 0, 0,
};

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
            right_volume = (gb->io_registers[GB_IO_NR50] >> 4) & 7;
        }
        GB_sample_t output = {(0xf - value) * left_volume, (0xf - value) * right_volume};
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
    


    GB_sample_t filtered_output = gb->apu_output.highpass_mode?
        (GB_sample_t) {output.left - gb->apu_output.highpass_diff.left,
                       output.right - gb->apu_output.highpass_diff.right} :
        output;
    
    switch (gb->apu_output.highpass_mode) {
        case GB_HIGHPASS_OFF:
            gb->apu_output.highpass_diff = (GB_double_sample_t) {0, 0};
            break;
        case GB_HIGHPASS_ACCURATE:
            gb->apu_output.highpass_diff = (GB_double_sample_t)
                {output.left - filtered_output.left * gb->apu_output.highpass_rate,
                    output.right - filtered_output.right * gb->apu_output.highpass_rate};
            break;
        case GB_HIGHPASS_REMOVE_DC_OFFSET: {
            unsigned mask = gb->io_registers[GB_IO_NR51];
            unsigned left_volume = 0;
            unsigned right_volume = 0;
            for (unsigned i = GB_N_CHANNELS; i--;) {
                if (mask & 1) {
                    left_volume += (gb->io_registers[GB_IO_NR50] & 7) * CH_STEP * 0xF;
                }
                if (mask & 0x10) {
                    right_volume += ((gb->io_registers[GB_IO_NR50] >> 4) & 7) * CH_STEP * 0xF;
                }
                mask >>= 1;
            }
            gb->apu_output.highpass_diff = (GB_double_sample_t)
            {left_volume * (1 - gb->apu_output.highpass_rate) + gb->apu_output.highpass_diff.left * gb->apu_output.highpass_rate,
                right_volume * (1 - gb->apu_output.highpass_rate) + gb->apu_output.highpass_diff.right * gb->apu_output.highpass_rate};

        }
            
    }

    while (gb->apu_output.copy_in_progress);
    while (!__sync_bool_compare_and_swap(&gb->apu_output.lock, false, true));
    if (gb->apu_output.buffer_position < gb->apu_output.buffer_size) {
        gb->apu_output.buffer[gb->apu_output.buffer_position++] = filtered_output;
    }
    gb->apu_output.lock = false;
}

static uint16_t new_sweep_sample_legnth(GB_gameboy_t *gb)
{
    uint16_t delta = gb->apu.square_channels[GB_SQUARE_1].sample_length >> (gb->io_registers[GB_IO_NR10] & 7);
    if (gb->io_registers[GB_IO_NR10] & 8) {
        return gb->apu.square_channels[GB_SQUARE_1].sample_length - delta;
    }
    return gb->apu.square_channels[GB_SQUARE_1].sample_length + delta;
}

void GB_apu_div_event(GB_gameboy_t *gb)
{
    if (!gb->apu.global_enable) return;
    gb->apu.div_divider++;

    if ((gb->apu.div_divider & 7) == 0) {
        for (unsigned i = GB_SQUARE_2 + 1; i--;) {
            uint8_t nrx2 = gb->io_registers[i == GB_SQUARE_1? GB_IO_NR12 : GB_IO_NR22];
            
            if (gb->apu.square_channels[i].volume_countdown) {
                if (!--gb->apu.square_channels[i].volume_countdown) {
                    if ((nrx2 & 8) && gb->apu.square_channels[i].current_volume < 0xF) {
                        gb->apu.square_channels[i].current_volume++;
                    }
                    
                    else if (!(nrx2 & 8) && gb->apu.square_channels[i].current_volume > 0) {
                        gb->apu.square_channels[i].current_volume--;
                    }
                    
                    gb->apu.square_channels[i].volume_countdown = nrx2 & 7;
                    
                    uint8_t duty = gb->io_registers[i == GB_SQUARE_1? GB_IO_NR11 :GB_IO_NR21] >> 6;
                    update_sample(gb, i,
                                  duties[gb->apu.square_channels[i].current_sample_index + duty * 8]?
                                  gb->apu.square_channels[i].current_volume : 0,
                                  0);
                }
            }
        }
        
        uint8_t nr42 = gb->io_registers[GB_IO_NR42];
        
        if (gb->apu.noise_channel.volume_countdown) {
            if (!--gb->apu.noise_channel.volume_countdown) {
                if ((nr42 & 8) && gb->apu.noise_channel.current_volume < 0xF) {
                    gb->apu.noise_channel.current_volume++;
                }
                
                else if (!(nr42 & 8) && gb->apu.noise_channel.current_volume > 0) {
                    gb->apu.noise_channel.current_volume--;
                }
                
                gb->apu.noise_channel.volume_countdown = nr42 & 7;
                
                update_sample(gb, GB_NOISE,
                              (gb->apu.noise_channel.lfsr & 1) ?
                              gb->apu.noise_channel.current_volume : 0,
                              0);
            }
        }
    }
    
    if ((gb->apu.div_divider & 1) == 1) {
        for (unsigned i = GB_SQUARE_2 + 1; i--;) {
            if (gb->apu.square_channels[i].length_enabled) {
                if (gb->apu.square_channels[i].pulse_length) {
                    if (!--gb->apu.square_channels[i].pulse_length) {
                        gb->apu.is_active[i] = false;
                        update_sample(gb, i, 0, 0);
                    }
                }
            }
        }
        
        if (gb->apu.wave_channel.length_enabled) {
            if (gb->apu.wave_channel.pulse_length) {
                if (!--gb->apu.wave_channel.pulse_length) {
                    gb->apu.is_active[GB_WAVE] = false;
                    gb->apu.wave_channel.current_sample = 0;
                    update_sample(gb, GB_WAVE, 0, 0);
                }
            }
        }
        
        if (gb->apu.noise_channel.length_enabled) {
            if (gb->apu.noise_channel.pulse_length) {
                if (!--gb->apu.noise_channel.pulse_length) {
                    gb->apu.is_active[GB_NOISE] = false;
                    update_sample(gb, GB_NOISE, 0, 0);
                }
            }
        }
    }
    
    if ((gb->apu.div_divider & 3) == 3) {
        if (!gb->apu.sweep_enabled) {
            return;
        }
        if (gb->apu.square_sweep_countdown) {
            if (!--gb->apu.square_sweep_countdown) {
                if ((gb->io_registers[GB_IO_NR10] & 0x70) && (gb->io_registers[GB_IO_NR10] & 0x07)) {
                    gb->apu.square_channels[GB_SQUARE_1].sample_length = gb->apu.new_sweep_sample_legnth;
                }
                /* Recalculation and overflow check only occurs after a delay */
                gb->apu.square_sweep_calculate_countdown = 0x13 - gb->apu.lf_div;
                
                gb->apu.square_channels[GB_SQUARE_1].sample_length &= 0x7FF;
                gb->apu.square_sweep_countdown = ((gb->io_registers[GB_IO_NR10] >> 4) & 7);
            }
        }
    }
}


void GB_apu_run(GB_gameboy_t *gb)
{
    /* Convert 4MHZ to 2MHz. apu_cycles is always even. */
    uint8_t cycles = gb->apu.apu_cycles >> 1;
    gb->apu.apu_cycles = 0;
    if (!cycles) return;
    
    /* To align the square signal to 1MHz */
    gb->apu.lf_div ^= cycles & 1;
    gb->apu.noise_channel.alignment += cycles;
    
    if (gb->apu.square_sweep_calculate_countdown) {
        if (gb->apu.square_sweep_calculate_countdown > cycles) {
            gb->apu.square_sweep_calculate_countdown -= cycles;
        }
        else {
            /* APU bug: sweep frequency is checked after adding the sweep delta twice */
            gb->apu.new_sweep_sample_legnth = new_sweep_sample_legnth(gb);
            if (gb->apu.new_sweep_sample_legnth > 0x7ff) {
                gb->apu.is_active[GB_SQUARE_1] = false;
                update_sample(gb, GB_SQUARE_1, 0, gb->apu.square_sweep_calculate_countdown - cycles);
                gb->apu.new_sweep_sample_legnth = gb->apu.square_channels[0].sample_length;
            }
            gb->apu.square_sweep_calculate_countdown = 0;
        }
    }
    
    for (unsigned i = GB_SQUARE_2 + 1; i--;) {
        if (gb->apu.is_active[i]) {
            uint8_t cycles_left = cycles;
            while (unlikely(cycles_left > gb->apu.square_channels[i].sample_countdown)) {
                cycles_left -= gb->apu.square_channels[i].sample_countdown + 1;
                gb->apu.square_channels[i].sample_countdown = (gb->apu.square_channels[i].sample_length ^ 0x7FF) * 2 + 1;
                gb->apu.square_channels[i].current_sample_index++;
                gb->apu.square_channels[i].current_sample_index &= 0x7;
                
                uint8_t duty = gb->io_registers[i == GB_SQUARE_1? GB_IO_NR11 :GB_IO_NR21] >> 6;

                update_sample(gb, i,
                              duties[gb->apu.square_channels[i].current_sample_index + duty * 8]?
                              gb->apu.square_channels[i].current_volume : 0,
                              cycles - cycles_left);
                gb->apu.square_channels[i].sample_emitted = true;
            }
            if (cycles_left) {
                gb->apu.square_channels[i].sample_countdown -= cycles_left;
            }
        }
    }
    
    gb->apu.wave_channel.wave_form_just_read = false;
    if (gb->apu.is_active[GB_WAVE]) {
        uint8_t cycles_left = cycles;
        while (unlikely(cycles_left > gb->apu.wave_channel.sample_countdown)) {
            cycles_left -= gb->apu.wave_channel.sample_countdown + 1;
            gb->apu.wave_channel.sample_countdown = gb->apu.wave_channel.sample_length ^ 0x7FF;
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
    
    if (gb->apu.is_active[GB_NOISE]) {
        uint8_t cycles_left = cycles;
        while (unlikely(cycles_left > gb->apu.noise_channel.sample_countdown)) {
            cycles_left -= gb->apu.noise_channel.sample_countdown + 1;
            gb->apu.noise_channel.sample_countdown = gb->apu.noise_channel.sample_length * 4 + 3;
            
            /* Step LFSR */
            unsigned high_bit_mask = gb->apu.noise_channel.narrow ? 0x4040 : 0x4000;
            // This formula is different on a GBA!
            bool new_high_bit = (gb->apu.noise_channel.lfsr ^ (gb->apu.noise_channel.lfsr >> 1) ^ 1) & 1;
            gb->apu.noise_channel.lfsr >>= 1;
            
            if (new_high_bit) {
                gb->apu.noise_channel.lfsr |= high_bit_mask;
            }
            else {
                /* This code is not redundent, it's relevant when switching LFSR widths */
                gb->apu.noise_channel.lfsr &= ~high_bit_mask;
            }
            
            update_sample(gb, GB_NOISE,
                          (gb->apu.noise_channel.lfsr & 1) ?
                          gb->apu.noise_channel.current_volume : 0,
                          0);
        }
        if (cycles_left) {
            gb->apu.noise_channel.sample_countdown -= cycles_left;
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
        GB_sample_t output = {-gb->apu_output.highpass_diff.left, -gb->apu_output.highpass_diff.right};
        for (unsigned i = GB_N_CHANNELS; i--;) {
            output.left += gb->apu_output.current_sample[i].left * CH_STEP;
            output.right += gb->apu_output.current_sample[i].right * CH_STEP;
        }
        
        for (unsigned i = 0; i < count - gb->apu_output.buffer_position; i++) {
            dest[gb->apu_output.buffer_position + i] = output;
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
    gb->apu.lf_div = 1;
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
    if (!gb->apu.global_enable && reg != GB_IO_NR52 && (gb->is_cgb ||
                                                        (
                                                        reg != GB_IO_NR11 &&
                                                        reg != GB_IO_NR21 &&
                                                        reg != GB_IO_NR31 &&
                                                        reg != GB_IO_NR41
                                                        )
                                                        )) {
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
            /* These registers affect the output of all 4 channels (but not the output of the PCM registers).*/
            /* We call update_samples with the current value so the APU output is updated with the new outputs */
            for (unsigned i = GB_N_CHANNELS; i--;) {
                update_sample(gb, i, gb->apu.samples[i], 0);
            }
            break;
        case GB_IO_NR52: {
            
            uint8_t old_nrx1[] = {
                gb->io_registers[GB_IO_NR11],
                gb->io_registers[GB_IO_NR21],
                gb->io_registers[GB_IO_NR31],
                gb->io_registers[GB_IO_NR41]
            };
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
                old_nrx1[0] &= 0x3F;
                old_nrx1[1] &= 0x3F;
                
                gb->apu.global_enable = false;
            }
            
            if (!gb->is_cgb && (value & 0x80)) {
                GB_apu_write(gb, GB_IO_NR11, old_nrx1[0]);
                GB_apu_write(gb, GB_IO_NR21, old_nrx1[1]);
                GB_apu_write(gb, GB_IO_NR31, old_nrx1[2]);
                GB_apu_write(gb, GB_IO_NR41, old_nrx1[3]);
            }
        }
        break;
            
        /* Square channels */
        case GB_IO_NR10:
            gb->apu.square_sweep_countdown = ((value >> 4) & 7);
            if (!gb->apu.square_sweep_countdown) gb->apu.square_sweep_countdown = 8;
            break;
        
        case GB_IO_NR11:
        case GB_IO_NR21: {
            unsigned index = reg == GB_IO_NR21? GB_SQUARE_2: GB_SQUARE_1;
            gb->apu.square_channels[index].pulse_length = (0x40 - (value & 0x3f));
            break;
        }
            
        case GB_IO_NR12:
        case GB_IO_NR22: {
            /* TODO: What happens when changing bits 0-2 after triggering? */
            if ((value & 0xF8) == 0) {
                /* According to Blargg's test ROM this should disable the channel instantly
                   TODO: verify how "instant" the change is using PCM12*/
                unsigned index = reg == GB_IO_NR22? GB_SQUARE_2: GB_SQUARE_1;
                update_sample(gb, index, 0, 0);
                gb->apu.is_active[index] = false;
            }
            break;
        }
            
        case GB_IO_NR13:
        case GB_IO_NR23: {
            unsigned index = reg == GB_IO_NR23? GB_SQUARE_2: GB_SQUARE_1;
            gb->apu.square_channels[index].sample_length &= ~0xFF;
            gb->apu.square_channels[index].sample_length |= value & 0xFF;
            if (index == GB_SQUARE_1) {
                gb->apu.new_sweep_sample_legnth = gb->apu.square_channels[0].sample_length;
            }
            break;
        }
        
        case GB_IO_NR14:
        case GB_IO_NR24: {
            unsigned index = reg == GB_IO_NR24? GB_SQUARE_2: GB_SQUARE_1;
            gb->apu.square_channels[index].sample_length &= 0xFF;
            gb->apu.square_channels[index].sample_length |= (value & 7) << 8;
            if (index == GB_SQUARE_1) {
                gb->apu.new_sweep_sample_legnth = gb->apu.square_channels[0].sample_length;
            }
            if (value & 0x80) {
                gb->apu.square_channels[index].current_sample_index = 7;

                if (!gb->apu.is_active[index]) {
                    gb->apu.square_channels[index].sample_countdown = (gb->apu.square_channels[index].sample_length ^ 0x7FF) * 2 + 6 - gb->apu.lf_div;
                }
                else {
                    /* Timing quirk: if already active, sound starts 2 (2MHz) ticks earlier.
                       if both active AND already emitted a sample, sound starts the next 1MHz tick, 
                       and one sample is skipped */
                    if (!gb->apu.square_channels[index].sample_emitted) {
                        gb->apu.square_channels[index].sample_countdown = (gb->apu.square_channels[index].sample_length ^ 0x7FF) * 2 + 4 - gb->apu.lf_div;
                    }
                    else {
                        gb->apu.square_channels[index].sample_countdown = gb->apu.lf_div;
                        gb->apu.square_channels[index].current_sample_index = 0;
                    }
                }
                gb->apu.square_channels[index].current_volume = gb->io_registers[index == GB_SQUARE_1 ? GB_IO_NR12 : GB_IO_NR22] >> 4;
                gb->apu.square_channels[index].volume_countdown = gb->io_registers[index == GB_SQUARE_1 ? GB_IO_NR12 : GB_IO_NR22] & 7;
                
                if ((gb->io_registers[index == GB_SQUARE_1 ? GB_IO_NR12 : GB_IO_NR22] & 0xF8) != 0) {
                    gb->apu.is_active[index] = true;
                }
                if (gb->apu.square_channels[index].pulse_length == 0) {
                    gb->apu.square_channels[index].pulse_length = 0x40;
                    gb->apu.square_channels[index].length_enabled = false;
                }
                
                if (index == GB_SQUARE_1 && gb->io_registers[GB_IO_NR10] & 7) {
                    /* APU bug: if shift is nonzero, overflow check also occurs on trigger */
                    /* Todo: check actual timing */
                    gb->apu.square_sweep_calculate_countdown = 0x3 - gb->apu.lf_div;
                }
                
                if (index == GB_SQUARE_1) {
                    gb->apu.sweep_enabled = gb->io_registers[GB_IO_NR10] & 0x77;
                }
                
                /* Note that we don't change the sample just yet! This was verified on hardware. */
            }
            
            /* APU glitch - if length is enabled while the DIV-divider's LSB is 1, tick the length once. */
            if ((value & 0x40) &&
                !gb->apu.square_channels[index].length_enabled &&
                (gb->apu.div_divider & 1) &&
                gb->apu.square_channels[index].pulse_length) {
                gb->apu.square_channels[index].pulse_length--;
                if (gb->apu.square_channels[index].pulse_length == 0) {
                    if (value & 0x80) {
                        gb->apu.square_channels[index].pulse_length = 0x3F;
                    }
                    else {
                        update_sample(gb, index, 0, 0);
                        gb->apu.is_active[index] = false;
                    }
                }
            }
            gb->apu.square_channels[index].length_enabled = value & 0x40;
            break;
        }
            
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
            gb->apu.wave_channel.pulse_length = (0x100 - value);
            break;
        case GB_IO_NR32:
            gb->apu.wave_channel.shift = (uint8_t[]){4, 0, 1, 2}[(value >> 5) & 3];
            update_sample(gb, GB_WAVE, gb->apu.wave_channel.current_sample >> gb->apu.wave_channel.shift, 0);
            break;
        case GB_IO_NR33:
            gb->apu.wave_channel.sample_length &= ~0xFF;
            gb->apu.wave_channel.sample_length |= value & 0xFF;
            break;
        case GB_IO_NR34:
            gb->apu.wave_channel.sample_length &= 0xFF;
            gb->apu.wave_channel.sample_length |= (value & 7) << 8;
            if ((value & 0x80)) {
                /* DMG bug: wave RAM gets corrupted if the channel is retriggerred 1 cycle before the APU
                            reads from it. */
                if (!gb->is_cgb &&
                    gb->apu.is_active[GB_WAVE] &&
                    gb->apu.wave_channel.sample_countdown == 0 &&
                    gb->apu.wave_channel.enable) {
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
                gb->apu.wave_channel.sample_countdown = (gb->apu.wave_channel.sample_length ^ 0x7FF) + 3;
                gb->apu.wave_channel.current_sample_index = 0;
                if (gb->apu.wave_channel.pulse_length == 0) {
                    gb->apu.wave_channel.pulse_length = 0x100;
                    gb->apu.wave_channel.length_enabled = false;
                }
                /* Note that we don't change the sample just yet! This was verified on hardware. */
            }
            
            /* APU glitch - if length is enabled while the DIV-divider's LSB is 1, tick the length once. */
            if ((value & 0x40) &&
                !gb->apu.wave_channel.length_enabled &&
                (gb->apu.div_divider & 1) &&
                gb->apu.wave_channel.pulse_length) {
                gb->apu.wave_channel.pulse_length--;
                if (gb->apu.wave_channel.pulse_length == 0) {
                    if (value & 0x80) {
                        gb->apu.wave_channel.pulse_length = 0xFF;
                    }
                    else {
                        update_sample(gb, GB_WAVE, 0, 0);
                        gb->apu.is_active[GB_WAVE] = false;
                    }
                }
            }
            gb->apu.wave_channel.length_enabled = value & 0x40;
            gb->apu.is_active[GB_WAVE] &= gb->apu.wave_channel.enable;

            break;
        
        /* Noise Channel */
            
        case GB_IO_NR41: {
            gb->apu.noise_channel.pulse_length = (0x40 - (value & 0x3f));
            break;
        }
            
        case GB_IO_NR42: {
            /* TODO: What happens when changing bits 0-2 after triggering? */
            if ((value & 0xF8) == 0) {
                /* According to Blargg's test ROM this should disable the channel instantly
                 TODO: verify how "instant" the change is using PCM34 */
                update_sample(gb, GB_NOISE, 0, 0);
                gb->apu.is_active[GB_NOISE] = false;
            }
            break;
        }
            
        case GB_IO_NR43: {
            gb->apu.noise_channel.narrow = value & 8;
            unsigned divisor = (value & 0x07) << 1;
            if (!divisor) divisor = 1;
            gb->apu.noise_channel.sample_length = (divisor << (value >> 4)) - 1;
            
            /* Todo: changing the frequency sometimes delays the next sample. This is probably
               due to how the frequency is actually calculated in the noise channel, which is probably
               not by calculating the effective sample length and counting simiarly to the other channels.
               This is not emulated correctly. */
            break;
        }
            
        case GB_IO_NR44: {
            if (value & 0x80) {
                gb->apu.noise_channel.lfsr = 0;

                gb->apu.noise_channel.sample_countdown = (gb->apu.noise_channel.sample_length) * 2 + 6 - gb->apu.lf_div;
                
                /* I'm COMPLETELY unsure about this logic, but it passes all relevant tests.
                   See comment in NR43. */
                if ((gb->io_registers[GB_IO_NR43] & 7) && (gb->apu.noise_channel.alignment & 2) == 0) {
                    if ((gb->io_registers[GB_IO_NR43] & 7) == 1) {
                        gb->apu.noise_channel.sample_countdown += 2;
                    }
                    else {
                        gb->apu.noise_channel.sample_countdown -= 2;
                    }
                }
                if (gb->apu.is_active[GB_NOISE]) {
                    gb->apu.noise_channel.sample_countdown += 2;
                }
                
                gb->apu.noise_channel.current_volume = gb->io_registers[GB_IO_NR42] >> 4;
                gb->apu.noise_channel.volume_countdown = gb->io_registers[GB_IO_NR42] & 7;
                
                if ((gb->io_registers[GB_IO_NR42] & 0xF8) != 0) {
                    gb->apu.is_active[GB_NOISE] = true;
                }
                
                if (gb->apu.noise_channel.pulse_length == 0) {
                    gb->apu.noise_channel.pulse_length = 0x40;
                    gb->apu.noise_channel.length_enabled = false;
                }
                
                /* Note that we don't change the sample just yet! This was verified on hardware. */
            }
            
            /* APU glitch - if length is enabled while the DIV-divider's LSB is 1, tick the length once. */
            if ((value & 0x40) &&
                !gb->apu.noise_channel.length_enabled &&
                (gb->apu.div_divider & 1) &&
                gb->apu.noise_channel.pulse_length) {
                gb->apu.noise_channel.pulse_length--;
                if (gb->apu.noise_channel.pulse_length == 0) {
                    if (value & 0x80) {
                        gb->apu.noise_channel.pulse_length = 0x3F;
                    }
                    else {
                        update_sample(gb, GB_NOISE, 0, 0);
                        gb->apu.is_active[GB_NOISE] = false;
                    }
                }
            }
            gb->apu.noise_channel.length_enabled = value & 0x40;
            break;
        }
            
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

void GB_set_sample_rate(GB_gameboy_t *gb, unsigned int sample_rate)
{
    if (gb->apu_output.buffer) {
        free(gb->apu_output.buffer);
    }
    gb->apu_output.buffer_size = sample_rate / 25; // 40ms delay
    gb->apu_output.buffer = malloc(gb->apu_output.buffer_size * sizeof(*gb->apu_output.buffer));
    gb->apu_output.sample_rate = sample_rate;
    gb->apu_output.buffer_position = 0;
    if (sample_rate) {
        gb->apu_output.highpass_rate = pow(0.999958,  CPU_FREQUENCY / (double)sample_rate);
    }
}

void GB_set_highpass_filter_mode(GB_gameboy_t *gb, GB_highpass_mode_t mode)
{
    gb->apu_output.highpass_mode = mode;
}
