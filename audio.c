#include "audio.h"
#include "hw.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "tjpgd.h"

#define SAMPLE_RATE        44100
#define BUF_SAMPLES        256
#define AUDIO_TASK_PRIO    6
#define AUDIO_STACK        16384
#define SINE_LUT_SIZE      512
#define MP3_BUF_SIZE       16384
#define MAX_MP3_SAMPLES    (1152 * 2)
#define MP3_PCM_BUF_SIZE   MAX_MP3_SAMPLES

static const char *TAG = "pb_audio";

typedef enum {
    AS_IDLE,
    AS_TONE,
    AS_SAMPLE,
    AS_MP3_FILE,
    AS_WAV_FILE,
} audio_state_t;

static i2s_chan_handle_t tx_handle = NULL;
static TaskHandle_t audio_task = NULL;
static int16_t sine_lut[SINE_LUT_SIZE];
static volatile audio_state_t state = AS_IDLE;
static volatile uint32_t tone_freq_val = 440;
static volatile TickType_t tone_end_tick = 0;
static volatile const int16_t *sample_data_val = NULL;
static volatile size_t sample_pos_val = 0;
static volatile size_t sample_total_val = 0;
static uint8_t audio_volume = 192;

static FILE *mp3_fp = NULL;
static mp3dec_t mp3_decoder;
static uint8_t *mp3_buf = NULL;
static int mp3_buf_filled = 0;
static int mp3_buf_pos = 0;
static int16_t mp3_pcm_buf[MP3_PCM_BUF_SIZE];
static int mp3_pcm_pos = 0;
static int mp3_pcm_filled = 0;

static SemaphoreHandle_t audio_lock = NULL;
static volatile int64_t decode_start = 0;
static volatile bool mp3_paused = false;
static volatile bool mp3_finished = false;
static volatile bool audio_idle_ack = false;
static uint32_t mp3_file_size = 0;
static uint32_t mp3_id3_size = 0;
static int mp3_bitrate = 0;

#define WAV_BUF_SAMPLES  2048
static FILE *wav_fp = NULL;
static int16_t wav_buf[WAV_BUF_SAMPLES];
static volatile int wav_buf_pos = 0;
static volatile int wav_buf_filled = 0;
static volatile int wav_nch = 2;
static uint32_t wav_data_start = 0;
static uint32_t wav_data_size = 0;

static void init_sine_lut(void)
{
    for (int i = 0; i < SINE_LUT_SIZE; i++) {
        sine_lut[i] = (int16_t)(sinf(2.0f * M_PI * i / SINE_LUT_SIZE) * 32767.0f);
    }
}

static int mp3_refill(void)
{
    if (!mp3_fp) {
        return -1;
    }

    if (mp3_buf_filled < 0 || mp3_buf_pos < 0 || mp3_buf_pos > mp3_buf_filled) {
        mp3_buf_filled = 0;
        mp3_buf_pos = 0;
    }

    int remain = mp3_buf_filled - mp3_buf_pos;
    if (remain > 0) {
        memmove(mp3_buf, mp3_buf + mp3_buf_pos, remain);
    }

    int free_space = MP3_BUF_SIZE - remain;
    if (free_space <= 0) {
        mp3_buf_filled = 0;
        mp3_buf_pos = 0;
        free_space = MP3_BUF_SIZE;
    }

    mp3_buf_filled = remain;
    mp3_buf_pos = 0;
    int nr = fread(mp3_buf + mp3_buf_filled, 1, free_space, mp3_fp);
    if (nr > 0) {
        mp3_buf_filled += nr;
    }
    return mp3_buf_filled;
}

static void write_silence(i2s_chan_handle_t h, int16_t *buf)
{
    size_t n;
    memset(buf, 0, BUF_SAMPLES * 2);
    i2s_channel_write(h, buf, BUF_SAMPLES * 2, &n, portMAX_DELAY);
}

static void audio_task_func(void *arg)
{
    size_t written;
    int16_t buf[BUF_SAMPLES];
    uint32_t phase = 0;

    while (1) {
        xSemaphoreTake(audio_lock, portMAX_DELAY);
        audio_state_t cur_state = state;
        xSemaphoreGive(audio_lock);

        if (cur_state == AS_IDLE) {
            audio_idle_ack = true;
            write_silence(tx_handle, buf);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (cur_state == AS_TONE) {
            if (xTaskGetTickCount() >= tone_end_tick) {
                state = AS_IDLE;
                continue;
            }
            uint32_t freq = tone_freq_val;
            uint32_t step = ((uint64_t)freq << 16) * SINE_LUT_SIZE / SAMPLE_RATE;
            for (int i = 0; i < BUF_SAMPLES; i++) {
                uint32_t idx = (phase >> 16) & (SINE_LUT_SIZE - 1);
                int32_t s = ((int32_t)sine_lut[idx] * audio_volume) >> 8;
                buf[i] = (int16_t)s;
                phase += step;
            }
            i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
            continue;
        }

        if (cur_state == AS_SAMPLE) {
            size_t pos = sample_pos_val;
            size_t total = sample_total_val;
            const int16_t *data = (const int16_t *)sample_data_val;
            uint32_t n = BUF_SAMPLES;
            if (pos + n > total) {
                n = total - pos;
            }
            for (uint32_t i = 0; i < n; i++) {
                buf[i] = (int16_t)(((int32_t)data[pos + i] * audio_volume) >> 8);
            }
            pos += n;
            if (n < BUF_SAMPLES) {
                memset(&buf[n], 0, (BUF_SAMPLES - n) * sizeof(int16_t));
            }
            sample_pos_val = pos;
            if (pos >= total) {
                state = AS_IDLE;
            }
            i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
            continue;
        }

        if (cur_state == AS_MP3_FILE) {
            if (mp3_paused) {
                write_silence(tx_handle, buf);
                continue;
            }

            if (mp3_pcm_pos < mp3_pcm_filled) {
                uint32_t n = BUF_SAMPLES;
                if (mp3_pcm_pos + n > mp3_pcm_filled) {
                    n = mp3_pcm_filled - mp3_pcm_pos;
                }
                for (uint32_t i = 0; i < n; i++) {
                    buf[i] = (int16_t)(((int32_t)mp3_pcm_buf[mp3_pcm_pos + i] * audio_volume) >> 8);
                }
                mp3_pcm_pos += n;
                if (n < BUF_SAMPLES) {
                    memset(&buf[n], 0, (BUF_SAMPLES - n) * sizeof(int16_t));
                }
                i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
                continue;
            }

            if (!mp3_fp) {
                state = AS_IDLE;
                write_silence(tx_handle, buf);
                continue;
            }

            if (mp3_buf_filled - mp3_buf_pos < MP3_BUF_SIZE / 4) {
                int ret = mp3_refill();
                if (ret <= 0) {
                    if (mp3_fp) {
                        fclose(mp3_fp);
                        mp3_fp = NULL;
                    }
                    mp3_finished = true;
                    state = AS_IDLE;
                    write_silence(tx_handle, buf);
                    continue;
                }
            }

            int buf_remain = mp3_buf_filled - mp3_buf_pos;
            if (buf_remain < 4) {
                continue;
            }

            int64_t t0 = esp_timer_get_time();
            decode_start = t0;
            mp3dec_frame_info_t minfo;
            int samples = mp3dec_decode_frame(&mp3_decoder, mp3_buf + mp3_buf_pos,
                                              buf_remain, mp3_pcm_buf, &minfo);
            if (samples > MAX_MP3_SAMPLES / 2) {
                samples = MAX_MP3_SAMPLES / 2;
            }
            decode_start = 0;
            int64_t t1 = esp_timer_get_time();
            int64_t decode_us = t1 - t0;
            if (decode_us > 100000) {
                ESP_LOGW(TAG, "mp3dec took %lld ms", decode_us / 1000);
            }

            if (samples > 0) {
                if (mp3_bitrate == 0) {
                    mp3_bitrate = minfo.bitrate_kbps;
                }
                mp3_buf_pos += minfo.frame_bytes;
                int total = samples * minfo.channels;
                if (total > MAX_MP3_SAMPLES) {
                    total = MAX_MP3_SAMPLES;
                }

                if (minfo.channels == 1) {
                    for (int i = samples - 1; i >= 0; i--) {
                        mp3_pcm_buf[i * 2] = mp3_pcm_buf[i];
                        mp3_pcm_buf[i * 2 + 1] = mp3_pcm_buf[i];
                    }
                    mp3_pcm_filled = samples * 2;
                } else {
                    mp3_pcm_filled = total;
                }
                mp3_pcm_pos = 0;

                uint32_t n = BUF_SAMPLES;
                if (mp3_pcm_filled < (int)n) {
                    n = mp3_pcm_filled;
                }
                for (uint32_t i = 0; i < n; i++) {
                    buf[i] = (int16_t)(((int32_t)mp3_pcm_buf[i] * audio_volume) >> 8);
                }
                mp3_pcm_pos = n;
                if (n < BUF_SAMPLES) {
                    memset(&buf[n], 0, (BUF_SAMPLES - n) * sizeof(int16_t));
                }
                i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
                continue;
            }

            if (minfo.frame_bytes > 0) {
                mp3_buf_pos += minfo.frame_bytes;
            } else {
                mp3_buf_pos++;
            }
            continue;
        }

        if (cur_state == AS_WAV_FILE) {
            if (wav_buf_pos < wav_buf_filled) {
                uint32_t n = BUF_SAMPLES;
                if (wav_buf_pos + n > wav_buf_filled) {
                    n = wav_buf_filled - wav_buf_pos;
                }
                if (wav_nch == 1) {
                    for (uint32_t i = 0; i < n / 2; i++) {
                        int16_t s = (int16_t)(((int32_t)wav_buf[wav_buf_pos + i] * audio_volume) >> 8);
                        buf[i * 2] = s;
                        buf[i * 2 + 1] = s;
                    }
                    wav_buf_pos += n / 2;
                    if (n % 2) {
                        int16_t s = (int16_t)(((int32_t)wav_buf[wav_buf_pos] * audio_volume) >> 8);
                        buf[n - 1] = s;
                        buf[n] = s;
                        wav_buf_pos++;
                        n++;
                    }
                } else {
                    for (uint32_t i = 0; i < n; i++) {
                        buf[i] = (int16_t)(((int32_t)wav_buf[wav_buf_pos + i] * audio_volume) >> 8);
                    }
                    wav_buf_pos += n;
                }
                if (n < BUF_SAMPLES) {
                    memset(&buf[n], 0, (BUF_SAMPLES - n) * sizeof(int16_t));
                }
                i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
                continue;
            }

            if (!wav_fp) {
                state = AS_IDLE;
                write_silence(tx_handle, buf);
                continue;
            }

            int nr = fread(wav_buf, 1, sizeof(wav_buf), wav_fp);
            if (nr > 0) {
                wav_buf_filled = nr / 2;
                wav_buf_pos = 0;
                uint32_t n = BUF_SAMPLES;
                if (wav_buf_filled < (int)n) {
                    n = wav_buf_filled;
                }
                if (wav_nch == 1) {
                    for (uint32_t i = 0; i < n / 2; i++) {
                        int16_t s = (int16_t)(((int32_t)wav_buf[i] * audio_volume) >> 8);
                        buf[i * 2] = s;
                        buf[i * 2 + 1] = s;
                    }
                    wav_buf_pos = n / 2;
                    if (n % 2) {
                        wav_buf_pos++;
                    }
                } else {
                    for (uint32_t i = 0; i < n; i++) {
                        buf[i] = (int16_t)(((int32_t)wav_buf[i] * audio_volume) >> 8);
                    }
                    wav_buf_pos = n;
                }
                if (n < BUF_SAMPLES) {
                    memset(&buf[n], 0, (BUF_SAMPLES - n) * sizeof(int16_t));
                }
                i2s_channel_write(tx_handle, buf, sizeof(buf), &written, portMAX_DELAY);
                continue;
            }

            fclose(wav_fp);
            wav_fp = NULL;
            state = AS_IDLE;
            write_silence(tx_handle, buf);
            continue;
        }
    }
}

void pb_audio_init(void)
{
    init_sine_lut();
    audio_lock = xSemaphoreCreateMutex();

    i2s_chan_config_t cc = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 16,
        .dma_frame_num = 240,
        .auto_clear = true,
    };
    if (i2s_new_channel(&cc, &tx_handle, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel failed");
        return;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_LRCLK_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(tx_handle, &sc) != ESP_OK) {
        ESP_LOGE(TAG, "I2S std mode failed");
        return;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    if (xTaskCreatePinnedToCore(audio_task_func, "audio", AUDIO_STACK, NULL,
                                AUDIO_TASK_PRIO, &audio_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "audio task creation failed");
    }
    ESP_LOGI(TAG, "I2S audio initialized (%d Hz)", SAMPLE_RATE);
}

void pb_audio_play_tone(uint32_t freq, uint32_t dur_ms)
{
    tone_freq_val = freq;
    tone_end_tick = xTaskGetTickCount() + pdMS_TO_TICKS(dur_ms);
    state = AS_TONE;
}

void pb_audio_no_tone(void)
{
    state = AS_IDLE;
}

void pb_audio_set_volume(uint8_t vol)
{
    audio_volume = vol;
}

bool pb_audio_is_playing(void)
{
    return state != AS_IDLE;
}

void pb_audio_play_sample(const int16_t *data, size_t count, uint32_t sample_rate)
{
    (void)sample_rate;
    sample_data_val = data;
    sample_pos_val = 0;
    sample_total_val = count;
    state = AS_SAMPLE;
}

static bool mp3_check_header(FILE *fp)
{
    unsigned char buf[10];
    long offset = 0;

    if (fread(buf, 1, 10, fp) != 10) {
        return false;
    }
    if (buf[0] == 'I' && buf[1] == 'D' && buf[2] == '3') {
        int sz = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14) | ((buf[8] & 0x7F) << 7)  | (buf[9] & 0x7F);
        mp3_id3_size = sz + 10;
        offset = sz + 10;
        if (fseek(fp, offset, SEEK_SET) != 0) {
            return false;
        }
        if (fread(buf, 1, 4, fp) != 4) {
            return false;
        }
    }

    if ((buf[0] & 0xFF) != 0xFF || (buf[1] & 0xE0) != 0xE0) {
        return false;
    }

    mp3_bitrate = hdr_bitrate_kbps(buf);

    int verIdx = (buf[1] >> 3) & 0x03;
    int srIdx = (buf[2] >> 2) & 0x03;
    if (verIdx > 3 || srIdx == 3) {
        return false;
    }

    // Note: 44100 Hz is the only frequency that will sound perfect because that's what I2S is
    // set up for. You can still play MP3 files with different properties, but this is the
    // golden standard.
    static const int rates[4][3] = {
        {11025, 12000, 8000},
        {0, 0, 0},
        {22050, 24000, 16000},
        {44100, 48000, 32000},
    };
    int sr = rates[verIdx][srIdx];
    if (sr == 0) {
        return false;
    }
    if (sr != SAMPLE_RATE) {
        ESP_LOGW(TAG, "MP3 sample rate %d Hz != I2S %d Hz (audio will play at wrong speed)",
                 sr, SAMPLE_RATE);
    }
    fseek(fp, offset, SEEK_SET);
    return true;
}

void pb_audio_play_mp3(const char *path)
{
    pb_audio_stop();

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return;
    }

    fseek(fp, 0, SEEK_END);
    mp3_file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (!mp3_check_header(fp)) {
        fclose(fp);
        return;
    }

    mp3dec_init(&mp3_decoder);

    uint8_t *rbuf = (uint8_t *)heap_caps_malloc(MP3_BUF_SIZE, MALLOC_CAP_INTERNAL);
    if (!rbuf) {
        fclose(fp);
        ESP_LOGE(TAG, "malloc %d failed", MP3_BUF_SIZE);
        return;
    }

    xSemaphoreTake(audio_lock, portMAX_DELAY);
    mp3_fp = fp;
    mp3_buf = rbuf;
    mp3_buf_filled = 0;
    mp3_buf_pos = 0;
    mp3_pcm_pos = 0;
    mp3_pcm_filled = 0;
    mp3_paused = false;
    mp3_finished = false;
    state = AS_MP3_FILE;
    xSemaphoreGive(audio_lock);

    ESP_LOGI(TAG, "Playing MP3: %s", path);
}

void pb_audio_play_wav(const char *path)
{
    pb_audio_stop();

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s", path);
        return;
    }

    char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12
        || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not WAV");
        fclose(fp);
        return;
    }
    uint32_t data_size = 0;
    while (1) {
        char cid[4];
        uint32_t cs;
        if (fread(cid, 1, 4, fp) != 4) {
            break;
        }
        if (fread(&cs, 4, 1, fp) != 1) {
            break;
        }
        if (memcmp(cid, "fmt ", 4) == 0) {
            char fmt[32];
            uint32_t rs = cs > 32 ? 32 : cs;
            if (fread(fmt, 1, rs, fp) != rs) {
                break;
            }
            uint16_t af = *(uint16_t *)fmt;
            uint16_t nc = *(uint16_t *)(fmt + 2);
            uint32_t sr = *(uint32_t *)(fmt + 4);
            uint16_t bps = *(uint16_t *)(fmt + 14);
            if (af != 1) {
                ESP_LOGE(TAG, "not PCM");
                fclose(fp);
                return;
            }
            if (bps != 16) {
                ESP_LOGE(TAG, "not 16-bit");
                fclose(fp);
                return;
            }
            if (sr != SAMPLE_RATE) {
                ESP_LOGW(TAG, "WAV %d Hz vs I2S %d Hz", sr, SAMPLE_RATE);
            }
            wav_nch = nc;
            if (cs > rs) {
                fseek(fp, cs - rs, SEEK_CUR);
            }
            if (cs & 1) {
                fseek(fp, 1, SEEK_CUR);
            }
        } else if (memcmp(cid, "data", 4) == 0) {
            data_size = cs;
            break;
        } else {
            fseek(fp, cs, SEEK_CUR);
            if (cs & 1) {
                fseek(fp, 1, SEEK_CUR);
            }
        }
    }
    if (data_size == 0) {
        ESP_LOGE(TAG, "no data");
        fclose(fp);
        return;
    }
    wav_data_start = ftell(fp);
    wav_data_size = data_size;
    xSemaphoreTake(audio_lock, portMAX_DELAY);
    wav_fp = fp;
    wav_buf_filled = 0;
    wav_buf_pos = 0;
    state = AS_WAV_FILE;
    xSemaphoreGive(audio_lock);
    ESP_LOGI(TAG, "Playing WAV: %s (%d ch)", path, wav_nch);
}

void pb_audio_watchdog(void)
{
    if (decode_start == 0) {
        return;
    }

    int64_t elapsed = esp_timer_get_time() - decode_start;
    if (elapsed <= 2000000) {
        return;
    }

    ESP_LOGW(TAG, "MP3 decoder hung for %lld ms, recovering", elapsed / 1000);
    vTaskSuspend(audio_task);

    if (state == AS_MP3_FILE && mp3_fp && mp3_buf) {
        mp3_buf_pos = mp3_buf_filled;
        mp3_buf_filled = 0;
        mp3_pcm_pos = 0;
        mp3_pcm_filled = 0;
        mp3dec_init(&mp3_decoder);
        state = AS_MP3_FILE;
    } else {
        state = AS_IDLE;
        if (mp3_fp) {
            fclose(mp3_fp);
            mp3_fp = NULL;
        }
        mp3dec_init(&mp3_decoder);
        if (mp3_buf) {
            free(mp3_buf);
            mp3_buf = NULL;
        }
        mp3_buf_filled = 0;
        mp3_buf_pos = 0;
        mp3_pcm_pos = 0;
        mp3_pcm_filled = 0;
        mp3_paused = false;
        mp3_finished = false;
    }
    decode_start = 0;
    vTaskResume(audio_task);
}

const char *pb_audio_format_time(int64_t ms)
{
    static char buf[16];
    if (ms < 0) {
        snprintf(buf, sizeof(buf), "--:--");
        return buf;
    }
    int total_sec = (int)(ms / 1000);
    int m = total_sec / 60;
    int s = total_sec % 60;
    if (m >= 60) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", m / 60, m % 60, s);
    } else {
        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    }
    return buf;
}

void pb_audio_toggle_pause(void)
{
    mp3_paused = !mp3_paused;
}

bool pb_audio_is_paused(void)
{
    return mp3_paused;
}

int64_t pb_audio_get_duration_ms(void)
{
    if (wav_data_size > 0) {
        int denom = wav_nch * 2;
        if (denom == 0) {
            denom = 2;
        }
        return (int64_t)wav_data_size / denom * 1000 / SAMPLE_RATE;
    }
    if (mp3_bitrate == 0) {
        return 0;
    }
    return (int64_t)(mp3_file_size - mp3_id3_size) * 8 / mp3_bitrate;
}

int64_t pb_audio_get_position_ms(void)
{
    xSemaphoreTake(audio_lock, portMAX_DELAY);
    if (wav_fp && wav_data_size > 0) {
        long raw_pos = ftell(wav_fp);
        xSemaphoreGive(audio_lock);
        if (raw_pos < 0) {
            raw_pos = 0;
        }
        long consumed = raw_pos - wav_data_start - (wav_buf_filled - wav_buf_pos) * 2;
        if (consumed < 0) {
            consumed = 0;
        }
        int denom = wav_nch * 2;
        if (denom == 0) {
            denom = 2;
        }
        return (int64_t)consumed / denom * 1000 / SAMPLE_RATE;
    }
    if (mp3_bitrate == 0 || !mp3_fp) {
        xSemaphoreGive(audio_lock);
        return 0;
    }
    long raw_pos = ftell(mp3_fp);
    if (raw_pos < 0) {
        xSemaphoreGive(audio_lock);
        return 0;
    }
    long consumed = raw_pos - (mp3_buf_filled - mp3_buf_pos) - mp3_id3_size;
    xSemaphoreGive(audio_lock);
    if (consumed < 0) {
        consumed = 0;
    }
    return (int64_t)consumed * 8 / mp3_bitrate;
}

bool pb_audio_is_finished(void)
{
    return mp3_finished;
}

void pb_audio_seek_relative(int32_t delta_ms)
{
    xSemaphoreTake(audio_lock, portMAX_DELAY);
    audio_state_t cur = state;
    if (cur == AS_IDLE) {
        xSemaphoreGive(audio_lock);
        return;
    }
    state = AS_IDLE;
    audio_idle_ack = false;
    xSemaphoreGive(audio_lock);

    TickType_t seek_start = xTaskGetTickCount();
    while (!audio_idle_ack) {
        vTaskDelay(pdMS_TO_TICKS(2));
        if ((xTaskGetTickCount() - seek_start) > pdMS_TO_TICKS(200)) {
            break;
        }
    }

    xSemaphoreTake(audio_lock, portMAX_DELAY);

    bool paused = mp3_paused;

    if (cur == AS_WAV_FILE && wav_fp) {
        int denom = wav_nch * 2;
        if (denom == 0) {
            denom = 2;
        }
        int64_t dur_ms = (int64_t)wav_data_size / denom * 1000 / SAMPLE_RATE;
        long raw_pos = ftell(wav_fp);
        if (raw_pos < 0) {
            raw_pos = 0;
        }
        long consumed = raw_pos - wav_data_start;
        if (consumed < 0) {
            consumed = 0;
        }
        int64_t cur_ms = (int64_t)consumed / denom * 1000 / SAMPLE_RATE;

        int64_t target_ms = cur_ms + delta_ms;
        if (target_ms < 0) {
            target_ms = 0;
        }
        if (target_ms > dur_ms) {
            target_ms = dur_ms;
        }

        int64_t target_byte = target_ms * SAMPLE_RATE * denom / 1000;
        target_byte = (target_byte / denom) * denom;
        fseek(wav_fp, wav_data_start + target_byte, SEEK_SET);
        wav_buf_filled = 0;
        wav_buf_pos = 0;
        state = AS_WAV_FILE;
    } else if (cur == AS_MP3_FILE && mp3_fp && mp3_bitrate > 0) {
        int64_t dur_ms = (int64_t)(mp3_file_size - mp3_id3_size) * 8 / mp3_bitrate;
        long raw_pos = ftell(mp3_fp);
        if (raw_pos < 0) {
            raw_pos = 0;
        }
        long consumed = raw_pos - (mp3_buf_filled - mp3_buf_pos) - mp3_id3_size;
        if (consumed < 0) {
            consumed = 0;
        }
        int64_t cur_ms = (int64_t)consumed * 8 / mp3_bitrate;

        int64_t target_ms = cur_ms + delta_ms;
        if (target_ms < 0) {
            target_ms = 0;
        }
        if (target_ms > dur_ms) {
            target_ms = dur_ms;
        }

        int64_t target_byte = target_ms * mp3_bitrate / 8;
        long seek_pos = mp3_id3_size + target_byte;
        if (seek_pos > mp3_id3_size + 4096) {
            seek_pos -= 4096;
        }
        fseek(mp3_fp, seek_pos, SEEK_SET);

        mp3dec_init(&mp3_decoder);
        mp3_buf_filled = 0;
        mp3_buf_pos = 0;
        mp3_pcm_pos = 0;
        mp3_pcm_filled = 0;
        state = AS_MP3_FILE;
    } else {
        state = cur;
    }

    mp3_paused = paused;
    xSemaphoreGive(audio_lock);
}

void pb_audio_stop(void)
{
    xSemaphoreTake(audio_lock, portMAX_DELAY);
    state = AS_IDLE;
    audio_idle_ack = false;
    xSemaphoreGive(audio_lock);

    TickType_t stop_start = xTaskGetTickCount();
    while (!audio_idle_ack) {
        vTaskDelay(pdMS_TO_TICKS(2));
        if ((xTaskGetTickCount() - stop_start) > pdMS_TO_TICKS(200)) {
            break;
        }
    }

    xSemaphoreTake(audio_lock, portMAX_DELAY);

    if (mp3_fp) {
        fclose(mp3_fp);
        mp3_fp = NULL;
    }
    mp3dec_init(&mp3_decoder);
    if (mp3_buf) {
        free(mp3_buf);
        mp3_buf = NULL;
    }
    mp3_buf_filled = 0;
    mp3_buf_pos = 0;
    mp3_pcm_pos = 0;
    mp3_pcm_filled = 0;
    mp3_paused = false;
    mp3_finished = false;
    mp3_bitrate = 0;
    mp3_file_size = 0;
    mp3_id3_size = 0;
    decode_start = 0;

    if (wav_fp) {
        fclose(wav_fp);
        wav_fp = NULL;
    }
    wav_buf_pos = 0;
    wav_buf_filled = 0;
    wav_data_start = 0;
    wav_data_size = 0;
    sample_pos_val = 0;
    sample_total_val = 0;

    xSemaphoreGive(audio_lock);
}

static uint32_t read_id3_syncsafe(const uint8_t *buf, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 7) | (buf[i] & 0x7F);
    }
    return v;
}

static int read_id3_text(const uint8_t *data, int size, char *out, int out_size)
{
    if (size < 1) {
        return 0;
    }
    int enc = data[0];
    const uint8_t *str_data = data + 1;
    int slen = size - 1;
    if (slen <= 0) {
        return 0;
    }

    if (enc == 0 || enc == 3) {
        int len = 0;
        while (len < slen && len < out_size - 1 && str_data[len]) {
            out[len] = (char)str_data[len];
            len++;
        }
        out[len] = '\0';
        return 1;
    }

    if (enc == 1 || enc == 2) {
        int bo;
        int si;
        if (enc == 1) {
            if (slen >= 2 && str_data[0] == 0xFF && str_data[1] == 0xFE) {
                bo = 0;
                si = 2;
            } else if (slen >= 2 && str_data[0] == 0xFE && str_data[1] == 0xFF) {
                bo = 1;
                si = 2;
            } else {
                bo = 0;
                si = 0;
            }
        } else {
            bo = 1;
            si = 0;
        }
        int len = 0;
        while (si + 1 < slen && len < out_size - 1) {
            uint16_t ch = bo ? (str_data[si] << 8) | str_data[si + 1]
                             : str_data[si] | (str_data[si + 1] << 8);
            if (ch == 0) {
                break;
            }
            out[len] = (ch < 128) ? (char)ch : '?';
            len++;
            si += 2;
        }
        out[len] = '\0';
        return 1;
    }
    return 0;
}

static void id3v1_trim_copy(const uint8_t *src, int len, char *dst, int dst_size)
{
    int end = len - 1;
    while (end >= 0 && src[end] == ' ') {
        end--;
    }
    int copy_len = end + 1;
    if (copy_len >= dst_size) {
        copy_len = dst_size - 1;
    }
    memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static void parse_id3v2(FILE *fp, pb_audio_metadata_t *meta)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, fp) != 10) {
        return;
    }
    if (memcmp(hdr, "ID3", 3) != 0) {
        return;
    }

    int major = hdr[3];
    uint32_t tag_size = read_id3_syncsafe(hdr + 6, 4);
    if (tag_size == 0) {
        return;
    }

    uint8_t *buf = (uint8_t *)malloc(tag_size);
    if (!buf) {
        return;
    }
    if (fread(buf, 1, tag_size, fp) != tag_size) {
        free(buf);
        return;
    }

    uint32_t pos = 0;

    if (hdr[5] & 0x40) {
        if (pos + 4 > tag_size) {
            free(buf);
            return;
        }
        if (major == 3) {
            uint32_t ext_size = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16)
                              | ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
            pos += 4 + ext_size;
        } else if (major == 4) {
            uint32_t ext_size = read_id3_syncsafe(buf + pos, 4);
            pos += ext_size;
        }
    }

    while (pos + 8 <= tag_size) {
        uint8_t frame_id[5];
        uint32_t frame_size;
        int skip;

        if (major == 2) {
            if (pos + 6 > tag_size) {
                break;
            }
            memcpy(frame_id, buf + pos, 3);
            frame_id[3] = '\0';
            frame_size = ((uint32_t)buf[pos + 3] << 16)
                       | ((uint32_t)buf[pos + 4] << 8) | buf[pos + 5];
            skip = 6;
        } else {
            memcpy(frame_id, buf + pos, 4);
            frame_id[4] = '\0';
            if (major == 4) {
                frame_size = read_id3_syncsafe(buf + pos + 4, 4);
            } else {
                frame_size = ((uint32_t)buf[pos + 4] << 24)
                           | ((uint32_t)buf[pos + 5] << 16)
                           | ((uint32_t)buf[pos + 6] << 8) | buf[pos + 7];
            }
            skip = 10;
        }

        if (frame_id[0] == '\0') {
            break;
        }
        if (frame_size > tag_size - pos - skip) {
            break;
        }
        if (frame_size < 2) {
            pos += skip + frame_size;
            continue;
        }

        const uint8_t *fdata = buf + pos + skip;
        int fsize = frame_size;

        if (major == 2) {
            if (memcmp(frame_id, "TT2", 3) == 0 && !meta->title[0]) {
                read_id3_text(fdata, fsize, meta->title, sizeof(meta->title));
            } else if (memcmp(frame_id, "TP1", 3) == 0 && !meta->artist[0]) {
                read_id3_text(fdata, fsize, meta->artist, sizeof(meta->artist));
            } else if (memcmp(frame_id, "TAL", 3) == 0 && !meta->album[0]) {
                read_id3_text(fdata, fsize, meta->album, sizeof(meta->album));
            } else if (memcmp(frame_id, "TYE", 3) == 0 && !meta->year[0]) {
                read_id3_text(fdata, fsize, meta->year, sizeof(meta->year));
            } else if (memcmp(frame_id, "TCO", 3) == 0 && !meta->genre[0]) {
                read_id3_text(fdata, fsize, meta->genre, sizeof(meta->genre));
            } else if (memcmp(frame_id, "PIC", 3) == 0 && !meta->cover_art && fsize > 6) {
                char mime3[4] = {fdata[1], fdata[2], fdata[3], 0};
                if (strcmp(mime3, "JPG") == 0 || strcmp(mime3, "JPEG") == 0) {
                    strcpy(meta->cover_mime, "image/jpeg");
                } else if (strcmp(mime3, "PNG") == 0) {
                    strcpy(meta->cover_mime, "image/png");
                }
                int idx = 5;
                if (fdata[0] == 1 || fdata[0] == 2) {
                    while (idx + 1 < fsize && (fdata[idx] || fdata[idx + 1])) {
                        idx += 2;
                    }
                    idx += 2;
                } else {
                    while (idx < fsize && fdata[idx]) {
                        idx++;
                    }
                    idx++;
                }
                if (idx < fsize) {
                    meta->cover_art_size = fsize - idx;
                    meta->cover_art = malloc(meta->cover_art_size);
                    if (meta->cover_art) {
                        memcpy(meta->cover_art, fdata + idx, meta->cover_art_size);
                    }
                }
            }
        } else {
            if (memcmp(frame_id, "TIT2", 4) == 0 && !meta->title[0]) {
                read_id3_text(fdata, fsize, meta->title, sizeof(meta->title));
            } else if (memcmp(frame_id, "TPE1", 4) == 0 && !meta->artist[0]) {
                read_id3_text(fdata, fsize, meta->artist, sizeof(meta->artist));
            } else if (memcmp(frame_id, "TALB", 4) == 0 && !meta->album[0]) {
                read_id3_text(fdata, fsize, meta->album, sizeof(meta->album));
            } else if ((memcmp(frame_id, "TYER", 4) == 0 || memcmp(frame_id, "TDRC", 4) == 0) && !meta->year[0]) {
                read_id3_text(fdata, fsize, meta->year, sizeof(meta->year));
            } else if (memcmp(frame_id, "TCON", 4) == 0 && !meta->genre[0]) {
                read_id3_text(fdata, fsize, meta->genre, sizeof(meta->genre));
            } else if (memcmp(frame_id, "APIC", 4) == 0 && !meta->cover_art && fsize > 6) {
                int idx = 1;
                int mime_len = 0;
                while (idx + mime_len < fsize && fdata[idx + mime_len]) {
                    mime_len++;
                }
                if (idx + mime_len < fsize && mime_len > 0
                    && mime_len < (int)sizeof(meta->cover_mime)) {
                    memcpy(meta->cover_mime, fdata + idx, mime_len);
                    meta->cover_mime[mime_len] = '\0';
                }
                idx += mime_len + 1;
                if (idx >= fsize) {
                    pos += skip + frame_size;
                    continue;
                }
                idx++;
                if (fdata[0] == 1 || fdata[0] == 2) {
                    while (idx + 1 < fsize && (fdata[idx] || fdata[idx + 1])) {
                        idx += 2;
                    }
                    idx += 2;
                } else {
                    while (idx < fsize && fdata[idx]) {
                        idx++;
                    }
                    idx++;
                }
                if (idx < fsize) {
                    meta->cover_art_size = fsize - idx;
                    meta->cover_art = malloc(meta->cover_art_size);
                    if (meta->cover_art) {
                        memcpy(meta->cover_art, fdata + idx, meta->cover_art_size);
                    }
                }
            }
        }

        pos += skip + frame_size;
    }

    free(buf);
}

static void parse_id3v1(FILE *fp, long file_size, pb_audio_metadata_t *meta)
{
    if (file_size < 128) {
        return;
    }

    fseek(fp, file_size - 128, SEEK_SET);
    uint8_t tag[128];
    if (fread(tag, 1, 128, fp) != 128) {
        return;
    }
    if (memcmp(tag, "TAG", 3) != 0) {
        return;
    }

    if (!meta->title[0]) {
        id3v1_trim_copy(tag + 3, 30, meta->title, sizeof(meta->title));
    }
    if (!meta->artist[0]) {
        id3v1_trim_copy(tag + 33, 30, meta->artist, sizeof(meta->artist));
    }
    if (!meta->album[0]) {
        id3v1_trim_copy(tag + 63, 30, meta->album, sizeof(meta->album));
    }
    if (!meta->year[0]) {
        id3v1_trim_copy(tag + 93, 4, meta->year, sizeof(meta->year));
    }

    if (!meta->genre[0]) {
        uint8_t genre_idx = tag[127];
        if (genre_idx <= 79) {
            static const char *genres[80] = {
                "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk",
                "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies",
                "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno",
                "Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
                "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal",
                "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental",
                "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
                "Alt. Rock", "Bass", "Soul", "Punk", "Space", "Meditative",
                "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
                "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk",
                "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult",
                "Gangsta Rap", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
                "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave",
                "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk",
                "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll",
                "Hard Rock"
            };
            const char *g = genres[genre_idx];
            size_t glen = strlen(g);
            if (glen >= sizeof(meta->genre)) {
                glen = sizeof(meta->genre) - 1;
            }
            memcpy(meta->genre, g, glen);
            meta->genre[glen] = '\0';
        } else {
            snprintf(meta->genre, sizeof(meta->genre), "Genre %d", genre_idx);
        }
    }
}

static void parse_wav_metadata(FILE *fp, pb_audio_metadata_t *meta)
{
    char hdr[12];
    if (fread(hdr, 1, 12, fp) != 12
        || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return;
    }

    while (1) {
        char cid[4];
        uint32_t cs;
        if (fread(cid, 1, 4, fp) != 4) {
            return;
        }
        if (fread(&cs, 4, 1, fp) != 1) {
            return;
        }

        if (memcmp(cid, "LIST", 4) == 0) {
            char type[4];
            if (cs < 4) {
                return;
            }
            if (fread(type, 1, 4, fp) != 4) {
                return;
            }
            uint32_t remaining = cs - 4;

            if (memcmp(type, "INFO", 4) == 0) {
                while (remaining >= 8) {
                    char scid[4];
                    uint32_t scs;
                    if (fread(scid, 1, 4, fp) != 4) {
                        return;
                    }
                    if (fread(&scs, 4, 1, fp) != 1) {
                        return;
                    }
                    remaining -= 8;
                    if (scs > remaining) {
                        return;
                    }

                    char *target = NULL;
                    int target_size = 0;
                    if (memcmp(scid, "INAM", 4) == 0 && !meta->title[0]) {
                        target = meta->title;
                        target_size = sizeof(meta->title);
                    } else if (memcmp(scid, "IART", 4) == 0 && !meta->artist[0]) {
                        target = meta->artist;
                        target_size = sizeof(meta->artist);
                    } else if (memcmp(scid, "IPRD", 4) == 0 && !meta->album[0]) {
                        target = meta->album;
                        target_size = sizeof(meta->album);
                    } else if (memcmp(scid, "ICRD", 4) == 0 && !meta->year[0]) {
                        target = meta->year;
                        target_size = sizeof(meta->year);
                    } else if (memcmp(scid, "IGNR", 4) == 0 && !meta->genre[0]) {
                        target = meta->genre;
                        target_size = sizeof(meta->genre);
                    }

                    if (target && scs > 0) {
                        int rlen = (scs < target_size - 1) ? scs : (target_size - 1);
                        if (fread(target, 1, rlen, fp) == rlen) {
                            target[rlen] = '\0';
                            int slen = strlen(target);
                            while (slen > 0 && (target[slen - 1] == ' ' || target[slen - 1] == '\0')) {
                                target[--slen] = '\0';
                            }
                        }
                    } else {
                        fseek(fp, scs, SEEK_CUR);
                    }
                    remaining -= scs;
                    if (scs & 1) {
                        fseek(fp, 1, SEEK_CUR);
                        remaining--;
                    }
                }
            } else {
                fseek(fp, remaining, SEEK_CUR);
            }
            return;
        } else if (memcmp(cid, "data", 4) == 0) {
            return;
        } else if (memcmp(cid, "ID3 ", 4) == 0) {
            long data_start = ftell(fp);
            parse_id3v2(fp, meta);
            long data_end = ftell(fp);
            long consumed = data_end - data_start;
            if (consumed < (long)cs) {
                fseek(fp, cs - consumed, SEEK_CUR);
            }
            if (cs & 1) {
                fseek(fp, 1, SEEK_CUR);
            }
        } else {
            fseek(fp, cs, SEEK_CUR);
            if (cs & 1) {
                fseek(fp, 1, SEEK_CUR);
            }
        }
    }
}

bool pb_audio_get_metadata(const char *path, pb_audio_metadata_t *meta)
{
    if (!meta) {
        return false;
    }
    memset(meta, 0, sizeof(*meta));

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    unsigned char magic[4];
    if (fread(magic, 1, 4, fp) != 4) {
        fclose(fp);
        return false;
    }

    bool found = false;

    if (magic[0] == 'I' && magic[1] == 'D' && magic[2] == '3') {
        fseek(fp, 0, SEEK_SET);
        parse_id3v2(fp, meta);
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        parse_id3v1(fp, file_size, meta);
        found = true;
    } else if ((magic[0] & 0xFF) == 0xFF && (magic[1] & 0xE0) == 0xE0) {
        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        parse_id3v1(fp, file_size, meta);
        found = true;
    } else if (memcmp(magic, "RIFF", 4) == 0) {
        fseek(fp, 0, SEEK_SET);
        parse_wav_metadata(fp, meta);
        found = meta->title[0] || meta->artist[0] || meta->album[0]
                || meta->year[0] || meta->genre[0];
    }

    fclose(fp);
    return found;
}

void pb_audio_metadata_free(pb_audio_metadata_t *meta)
{
    if (meta) {
        if (meta->cover_art) {
            free(meta->cover_art);
            meta->cover_art = NULL;
        }
        meta->cover_art_size = 0;
    }
}

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t pos;
    uint16_t *pixels;
    int stride;
    int width;
    int height;
} jpeg_dec_t;

static size_t jpeg_read(JDEC *jd, uint8_t *buf, size_t n)
{
    jpeg_dec_t *d = (jpeg_dec_t *)jd->device;
    size_t avail = d->size - d->pos;
    if (n > avail) {
        n = avail;
    }
    if (buf) {
        memcpy(buf, d->data + d->pos, n);
    }
    d->pos += n;
    return n;
}

static int jpeg_write(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_dec_t *d = (jpeg_dec_t *)jd->device;
    uint16_t *src = (uint16_t *)bitmap;
    int pw = rect->right - rect->left + 1;
    int ph = rect->bottom - rect->top + 1;
    for (int y = 0; y < ph; y++) {
        int dy = rect->top + y;
        if (dy >= d->height) {
            break;
        }
        for (int x = 0; x < pw; x++) {
            int dx = rect->left + x;
            if (dx >= d->width) {
                break;
            }
            d->pixels[dy * d->stride + dx] = src[y * pw + x];
        }
    }
    return 1;
}

uint8_t *pb_audio_decode_cover_art(const pb_audio_metadata_t *meta,
                                   int *out_w, int *out_h, int max_w, int max_h)
{
    if (!meta || !meta->cover_art || meta->cover_art_size == 0 || !out_w || !out_h) {
        if (out_w) {
            *out_w = 0;
        }
        if (out_h) {
            *out_h = 0;
        }
        return NULL;
    }
    if (strcmp(meta->cover_mime, "image/jpeg") != 0) {
        *out_w = 0;
        *out_h = 0;
        return NULL;
    }

    JDEC jd;
    jpeg_dec_t dec;
    dec.data = meta->cover_art;
    dec.size = meta->cover_art_size;
    dec.pos = 0;
    dec.pixels = NULL;

    uint8_t *pool = (uint8_t *)malloc(3100);
    if (!pool) {
        *out_w = 0;
        *out_h = 0;
        return NULL;
    }
    JRESULT res = jd_prepare(&jd, jpeg_read, pool, 3100, &dec);
    if (res != JDR_OK) {
        free(pool);
        *out_w = 0;
        *out_h = 0;
        return NULL;
    }

    int w = jd.width;
    int h = jd.height;

    uint8_t scale = 0;
    if (max_w > 0 && max_h > 0) {
        while ((w >> (scale + 1)) >= (uint32_t)max_w
               && (h >> (scale + 1)) >= (uint32_t)max_h && scale < 3) {
            scale++;
        }
    }
    int dw = w >> scale;
    int dh = h >> scale;
    size_t pitch = (size_t)dw * 2;
    uint16_t *pixels = (uint16_t *)malloc(pitch * dh);
    if (!pixels) {
        free(pool);
        *out_w = 0;
        *out_h = 0;
        return NULL;
    }

    dec.pixels = pixels;
    dec.stride = dw;
    dec.width = dw;
    dec.height = dh;

    res = jd_decomp(&jd, jpeg_write, scale);
    free(pool);
    if (res != JDR_OK) {
        free(pixels);
        *out_w = 0;
        *out_h = 0;
        return NULL;
    }

    *out_w = dw;
    *out_h = dh;
    return (uint8_t *)pixels;
}
