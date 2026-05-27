#include <gcsound.h>
#include <aesndlib.h>
#include <ogc/lwp.h>
#include <ogc/lwp_queue.h>
#include <ogc/mutex.h>
#include <ogc/irq.h>
#include <ogc/context.h>
#include <ogc/cache.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>

#ifndef VOICE_STATE_STOP
#define VOICE_STATE_STOP 0
#endif

//#define MUSIC_BUFFER_SIZE (1024*64)
//#define MUSIC_BUFFERS 16 

// ARQUITECTURA DE BUFFERING: 4 bloques de 128 KB (Colchón de 512 KB totales)
#define MUSIC_BUFFER_SIZE (1024*64)
#define MUSIC_BUFFERS 8 
#define MAX_SAMPLE_VOICES 16
#define USER_CB_BUFFER_SIZE 512 

static u8 music_buffers[MUSIC_BUFFERS][MUSIC_BUFFER_SIZE] __attribute__((aligned(32)));

static FILE* musicFile = NULL;
static AESNDPB* music_voice = NULL;
static u32 music_dataOffset = 0;
static u32 music_dataSize = 0;
static u32 music_dataRead = 0;
static volatile bool music_keep_playing = false;
static volatile bool music_is_paused = false;
static volatile int music_active_buffer = 0;

static lwp_t music_thread = LWP_THREAD_NULL;
static lwpq_t music_queue;
static u32 music_sampleRate = 44100;
static u8 music_vol_l = 255; 
static u8 music_vol_r = 255; 
static volatile int music_is_ready = 0; 

// Control asíncrono de estado por cada bloque del anillo
static volatile int buffer_ready_flag[MUSIC_BUFFERS]; 

static AESNDPB* sample_voices[MAX_SAMPLE_VOICES];
static int next_voice_index = 0;

static GCSound_UserCallback user_callback = NULL;
static void *user_callback_data = NULL;
static AESNDPB* user_voice = NULL;
static s16 user_buffer[USER_CB_BUFFER_SIZE / 2] __attribute__((aligned(32)));
static GCSound_RingBuffer st_ring_buffer = {NULL, 0, 0, 0, 0};
static bool rb_initialized = false;

static inline u32 _swap32(u32 val) {
    return ((val << 24) | ((val << 8) & 0x00FF0000) | ((val >> 8) & 0x0000FF00) | (val >> 24));
}

static inline u16 _swap16(u16 val) {
    return (val << 8) | (val >> 8);
}

static void _calculatePan(int pan, u8 volume, u16 *out_l, u16 *out_r) {
    if (pan < -128) pan = -128;
    if (pan > 127) pan = 127;
    float leftRatio = (pan < 0) ? 1.0f : (127 - pan) / 127.0f;
    float rightRatio = (pan > 0) ? 1.0f : (pan + 128) / 128.0f;
    *out_l = (u16)(volume * leftRatio);
    *out_r = (u16)(volume * rightRatio);
}

static u32 _ring_buffer_read(s16 *dest, u32 len) {
    u32 read_count = 0;
    u32 i;
    for (i = 0; i < len; i++) {
        if (st_ring_buffer.count > 0) {
            dest[i] = st_ring_buffer.buffer[st_ring_buffer.read_ptr];
            st_ring_buffer.read_ptr = (st_ring_buffer.read_ptr + 1) % st_ring_buffer.size;
            st_ring_buffer.count--;
            read_count++;
        } else {
            dest[i] = 0; 
        }
    }
    return read_count;
}

static void _user_voice_callback(AESNDPB *pb, u32 state) {
    if (state == VOICE_STATE_STREAM) {
        u32 len = USER_CB_BUFFER_SIZE / 2; 
        if (user_callback != NULL) {
            user_callback(user_buffer, len, user_callback_data);
        } else if (rb_initialized) {
            _ring_buffer_read(user_buffer, len);
        }
        u32 i;
        for (i = 0; i < len; i++) {
            u16 val = (u16)user_buffer[i];
            user_buffer[i] = (s16)((val << 8) | (val >> 8));
        }
        DCFlushRange(user_buffer, USER_CB_BUFFER_SIZE);
        AESND_SetVoiceBuffer(pb, user_buffer, USER_CB_BUFFER_SIZE);
    }
}

void GCSound_InitStreaming(u32 num_samples) {
    if (rb_initialized && st_ring_buffer.buffer) {
        free(st_ring_buffer.buffer);
        rb_initialized = false;
    }
    st_ring_buffer.size = num_samples;
    st_ring_buffer.buffer = (s16*)memalign(32, num_samples * sizeof(s16));
    if (st_ring_buffer.buffer) {
        memset(st_ring_buffer.buffer, 0, num_samples * sizeof(s16));
        DCFlushRange(st_ring_buffer.buffer, num_samples * sizeof(s16));
        st_ring_buffer.write_ptr = 0;
        st_ring_buffer.read_ptr = 0;
        st_ring_buffer.count = 0;
        rb_initialized = true;
    } else {
        return;
    }
    if (user_voice == NULL) {
        user_voice = AESND_AllocateVoice(_user_voice_callback);
        if (user_voice != NULL) {
            AESND_SetVoiceStream(user_voice, true);
            AESND_SetVoiceFrequency(user_voice, 32000); 
            AESND_SetVoiceFormat(user_voice, VOICE_STEREO16);
            AESND_SetVoiceVolume(user_voice, 255, 255);
            memset(user_buffer, 0, USER_CB_BUFFER_SIZE);
            DCFlushRange(user_buffer, USER_CB_BUFFER_SIZE);
            AESND_SetVoiceBuffer(user_voice, user_buffer, USER_CB_BUFFER_SIZE);
            AESND_SetVoiceStop(user_voice, false); 
            AESND_SetVoiceMute(user_voice, false);
        }
    }
}

void GCSound_PushAudio(s16 *data, u32 len) {
    if (!rb_initialized) return;
    u32 i;
    for (i = 0; i < len; i++) {
        if (st_ring_buffer.count < st_ring_buffer.size) {
            st_ring_buffer.buffer[st_ring_buffer.write_ptr] = data[i];
            st_ring_buffer.write_ptr = (st_ring_buffer.write_ptr + 1) % st_ring_buffer.size;
            st_ring_buffer.count++; 
        }
    }
    DCFlushRange(st_ring_buffer.buffer, st_ring_buffer.size * sizeof(s16));
}

u32 GCSound_GetFreeSpace() {
    if (!rb_initialized) return 0;
    return (st_ring_buffer.size - st_ring_buffer.count);
}

void GCSound_ClearBuffer() {
    if (!rb_initialized) return;
    u32 level = IRQ_Disable();
    st_ring_buffer.write_ptr = 0;
    st_ring_buffer.read_ptr = 0;
    st_ring_buffer.count = 0;
    memset(st_ring_buffer.buffer, 0, st_ring_buffer.size * sizeof(s16));
    DCFlushRange(st_ring_buffer.buffer, st_ring_buffer.size * sizeof(s16));
    IRQ_Restore(level);
}

void GCSound_RegisterUserCallback(GCSound_UserCallback cb, void *userdata) {
    user_callback = cb;
    user_callback_data = userdata;
    if (user_voice == NULL) {
        user_voice = AESND_AllocateVoice(_user_voice_callback);
        AESND_SetVoiceStream(user_voice, true);
        AESND_SetVoiceFrequency(user_voice, 32000); 
        AESND_SetVoiceFormat(user_voice, VOICE_STEREO16);
        AESND_SetVoiceVolume(user_voice, 255, 255);
        memset(user_buffer, 0, USER_CB_BUFFER_SIZE);
        DCFlushRange(user_buffer, USER_CB_BUFFER_SIZE);
        AESND_SetVoiceBuffer(user_voice, user_buffer, USER_CB_BUFFER_SIZE);
        AESND_SetVoiceStop(user_voice, false);
    }
}

void GCSound_UnregisterUserCallback() {
    user_callback = NULL;
    if (user_voice) {
        AESND_FreeVoice(user_voice);
        user_voice = NULL;
    }
}

static void _loadMusicChunk(int index) {
    if (!musicFile || index < 0 || index >= MUSIC_BUFFERS) return;

    // Bloqueamos la bandera explícitamente antes de escribir en la memoria
    buffer_ready_flag[index] = 0; 

    size_t to_read = MUSIC_BUFFER_SIZE;
    size_t total_read = 0;

    while (total_read < to_read) {
        size_t rem = music_dataSize - music_dataRead;
        size_t chunk = (to_read - total_read > rem) ? rem : (to_read - total_read);
        
        size_t r = fread(music_buffers[index] + total_read, 1, chunk, musicFile);
        music_dataRead += r;
        total_read += r;

        if (music_dataRead >= music_dataSize) {
            fseek(musicFile, music_dataOffset, SEEK_SET);
            music_dataRead = 0;
        }
        if (r == 0) break;
    }

    if (total_read < to_read) {
        memset(music_buffers[index] + total_read, 0, to_read - total_read);
    }

    s16 *ptr16 = (s16*)music_buffers[index];
    u32 num_samples = MUSIC_BUFFER_SIZE / 2;
    u32 i;
    
    for (i = 0; i < num_samples; i++) {
        u16 val = (u16)ptr16[i];
        ptr16[i] = (s16)((val << 8) | (val >> 8));
    }

    u32 ramp_start = num_samples - 64;
    for (i = ramp_start; i < num_samples; i++) {
        float gain = (float)(num_samples - i) / 64.0f;
        ptr16[i] = (s16)(ptr16[i] * gain);
    }

    // Aseguramos la coherencia de la caché L1/L2 de la GameCube
    DCFlushRange(music_buffers[index], MUSIC_BUFFER_SIZE);
    
    // El bloque está listo en memoria RAM: Levantamos la bandera
    buffer_ready_flag[index] = 1; 
}

// ========================================================================
// REFACTORIZADO: Callback desacoplado sin variables cuello de botella
// ========================================================================
static void _musicCallback(AESNDPB *pb, u32 state) {
    if (state == VOICE_STATE_STREAM && music_keep_playing && !music_is_paused) {
        
        // 1. El buffer actual terminó su reproducción: se libera la bandera de inmediato
        buffer_ready_flag[music_active_buffer] = 0;
        
        // 2. Avanzamos de manera estrictamente circular al siguiente buffer
        int next_buffer = (music_active_buffer + 1) % MUSIC_BUFFERS;

        // 3. Mecanismo anticaídas: si la SD se atrasó, mantiene el timing reproduciendo el bloque
        if (buffer_ready_flag[next_buffer] == 1) {
            music_active_buffer = next_buffer;
        } else {
            music_active_buffer = next_buffer; 
        }
        
        // 4. Encolar buffer al hardware mezclador
        AESND_SetVoiceBuffer(pb, music_buffers[music_active_buffer], MUSIC_BUFFER_SIZE);

        // 5. Señal asíncrona: El hilo de la SD sabrá que hay bloques libres en el anillo
        LWP_ThreadSignal(music_queue);
    }
}

// ========================================================================
// REFACTORIZADO: Hilo Productor Autónomo (Saturación dinámica del anillo)
// ========================================================================
static void* _music_thread_func(void* arg) {
    (void)arg;
    int i;
    
    // Precarga inicial completa del anillo físico de memoria
    for(i = 0; i < MUSIC_BUFFERS; i++) {
        _loadMusicChunk(i);
    }
    music_is_ready = 1;

    int search_index = 0;

    while (music_keep_playing) {
        // El hilo duerme eficientemente hasta que el callback procese un bloque
        LWP_ThreadSleep(music_queue);
        
        if (!music_keep_playing) break;

        // Escaneo circular inteligente: llena CUALQUIER bloque libre del anillo
        int checked;
        for (checked = 0; checked < MUSIC_BUFFERS; checked++) {
            // No tocamos el buffer que el hardware está leyendo en este microsegundo
            if (search_index != music_active_buffer && buffer_ready_flag[search_index] == 0) {
                _loadMusicChunk(search_index); 
            }
            search_index = (search_index + 1) % MUSIC_BUFFERS;
        }
    }
    return NULL;
}

void GCSound_Init() {
    AESND_Init();
    LWP_InitQueue(&music_queue);

    int i;
    for(i=0; i<MAX_SAMPLE_VOICES; i++) sample_voices[i] = NULL;
    next_voice_index = 0;
}

void GCSound_Close() {
    GCSound_StopMusic();
    GCSound_StopAllSamples();
    GCSound_UnregisterUserCallback();
    
    if (rb_initialized && st_ring_buffer.buffer) {
        free(st_ring_buffer.buffer);
        rb_initialized = false;
    }
}

int GCSound_PlayMusic(const char* filename, u32 forceFrequency) {
    char id[4];
    u32 rate = 44100;
    u16 chans = 2;
    u16 bits = 16;
    
    GCSound_StopMusic();
    
    musicFile = fopen(filename, "rb");
    if (!musicFile) return 0;
    
    char header[12];
    if (fread(header, 1, 12, musicFile) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(musicFile);
        musicFile = NULL;
        return 0;
    }
    
    music_dataOffset = 0;
    music_dataSize = 0;

    while (fread(id, 1, 4, musicFile) == 4) {
        u32 chunkSize;
        if (fread(&chunkSize, 4, 1, musicFile) != 1) break;
        
        u32 realSize = (chunkSize & 0xFF000000) ? _swap32(chunkSize) : chunkSize;
        if (realSize == 0) realSize = _swap32(chunkSize); 

        if (memcmp(id, "fmt ", 4) == 0) {
            u16 audioFormat;
            fread(&audioFormat, 2, 1, musicFile);
            fread(&chans, 2, 1, musicFile);
            fread(&rate, 4, 1, musicFile);
            fseek(musicFile, 6, SEEK_CUR); 
            fread(&bits, 2, 1, musicFile);
            
            chans = (chans > 2) ? _swap16(chans) : chans;
            rate = (rate > 192000) ? _swap32(rate) : rate;
            bits = (bits > 32) ? _swap16(bits) : bits;

            if (realSize > 16) {
                fseek(musicFile, realSize - 16, SEEK_CUR);
            }
        } else if (memcmp(id, "data", 4) == 0) {
            music_dataSize = realSize;
            music_dataOffset = ftell(musicFile);
            break;
        } else {
            fseek(musicFile, realSize + (realSize % 2), SEEK_CUR); 
        }
    }

    if (music_dataOffset == 0 || music_dataSize == 0) {
        fclose(musicFile);
        musicFile = NULL;
        return 0;
    }

    music_dataRead = 0;
    music_sampleRate = (forceFrequency > 0) ? forceFrequency : rate;
    music_keep_playing = true;
    music_is_paused = false;
    music_active_buffer = 0;
    music_is_ready = 0; 

    // Hilo prioritario en tiempo real
    LWP_CreateThread(&music_thread, _music_thread_func, NULL, NULL, 32768, 100);

    // Esperar de forma segura la precarga de los primeros 2 buffers (256 KB)
    int timeout = 0;
    while (timeout < 400) {
        int filled_count = 0;
        int i;
        for (i = 0; i < 2; i++) {
            if (buffer_ready_flag[i] == 1) filled_count++;
        }
        if (filled_count >= 2) break; 
        
        usleep(500); 
        timeout++;
    }

    music_voice = AESND_AllocateVoice(_musicCallback);
    if (music_voice) {
        AESND_SetVoiceStream(music_voice, true);
        AESND_SetVoiceFrequency(music_voice, music_sampleRate);
        AESND_SetVoiceFormat(music_voice, (chans == 2 ? VOICE_STEREO16 : VOICE_MONO16));
        AESND_SetVoiceVolume(music_voice, music_vol_l, music_vol_r);
        
        // Inicializamos el flujo pasando el primer bloque limpio cargado de la SD
        AESND_SetVoiceBuffer(music_voice, music_buffers[0], MUSIC_BUFFER_SIZE);
        AESND_SetVoiceStop(music_voice, false);
    }

    return 1;
}

void GCSound_PauseMusic(int paused) {
    if (!music_voice) return;
    music_is_paused = (bool)paused;
    if (music_is_paused) AESND_SetVoiceStop(music_voice, true);
    else {
        DCFlushRange(music_buffers[music_active_buffer], MUSIC_BUFFER_SIZE);
        AESND_SetVoiceStop(music_voice, false);
        LWP_ThreadSignal(music_queue);
    }
}

void GCSound_StopMusic() {
    music_keep_playing = false;
    music_is_paused = false;
    LWP_ThreadSignal(music_queue);
    if (music_thread != LWP_THREAD_NULL) {
        LWP_JoinThread(music_thread, NULL);
        music_thread = LWP_THREAD_NULL;
    }
    if (music_voice) {
        AESND_SetVoiceStop(music_voice, true);
        AESND_FreeVoice(music_voice);
        music_voice = NULL;
    }
    if (musicFile) {
        fclose(musicFile);
        musicFile = NULL;
    }
}

void GCSound_SetMusicVolume(u8 vol_l, u8 vol_r) {
    music_vol_l = vol_l; music_vol_r = vol_r;
    if (music_voice) AESND_SetVoiceVolume(music_voice, vol_l, vol_r);
}

GCSound_Sample* GCSound_LoadSample(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if(!f) return NULL;
    
    char header[12];
    if (fread(header, 1, 12, f) != 12 || memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(f);
        return NULL;
    }

    char buffer[4];
    u32 dsize = 0;
    u32 rate = 44100;
    u16 chans = 2;
    
    while (fread(buffer, 1, 4, f) == 4) {
        u32 chunkSize;
        fread(&chunkSize, 4, 1, f);
        u32 realSize = (chunkSize & 0xFF000000) ? _swap32(chunkSize) : chunkSize;
        if (realSize == 0) realSize = _swap32(chunkSize);

        if (memcmp(buffer, "fmt ", 4) == 0) {
            fseek(f, 2, SEEK_CUR);
            fread(&chans, 2, 1, f);
            fread(&rate, 4, 1, f);
            chans = (chans > 2) ? _swap16(chans) : chans;
            rate = (rate > 192000) ? _swap32(rate) : rate;
            fseek(f, realSize - 8, SEEK_CUR);
        } else if (memcmp(buffer, "data", 4) == 0) {
            dsize = realSize;
            break;
        } else { fseek(f, realSize + (realSize % 2), SEEK_CUR); }
    }
    
    if (dsize == 0) { fclose(f); return NULL; }
    
    GCSound_Sample* s = (GCSound_Sample*)malloc(sizeof(GCSound_Sample));
    s->size = dsize;
    s->frequency = rate;
    s->channels = (u8)chans;
    s->format = (chans == 2) ? VOICE_STEREO16 : VOICE_MONO16;
    
    u32 alignedSize = (dsize + 31) & ~31;
    s->data = memalign(32, alignedSize);
    memset(s->data, 0, alignedSize);
    fread(s->data, 1, dsize, f);
    fclose(f);
    
    s16* p = (s16*)s->data;
    u32 i;
    for(i=0; i < dsize/2; i++) {
        u16 val = (u16)p[i];
        p[i] = (s16)((val << 8) | (val >> 8));
    }
    
    DCFlushRange(s->data, alignedSize);
    return s;
}

void GCSound_FreeSample(GCSound_Sample* sample) {
    if(sample) {
        if(sample->data) free(sample->data);
        free(sample);
    }
}

int GCSound_PlaySample(GCSound_Sample* sample, u8 vol_l, u8 vol_r) {
    if (!sample || !sample->data) return -1;
    int v_idx = next_voice_index;
    next_voice_index = (next_voice_index + 1) % MAX_SAMPLE_VOICES;
    if (sample_voices[v_idx] == NULL) sample_voices[v_idx] = AESND_AllocateVoice(NULL);
    AESND_SetVoiceStop(sample_voices[v_idx], true);
    AESND_SetVoiceFrequency(sample_voices[v_idx], sample->frequency);
    AESND_SetVoiceFormat(sample_voices[v_idx], sample->format);
    AESND_SetVoiceVolume(sample_voices[v_idx], vol_l, vol_r);
    AESND_SetVoiceLoop(sample_voices[v_idx], false);
    AESND_SetVoiceBuffer(sample_voices[v_idx], sample->data, sample->size);
    AESND_SetVoiceStop(sample_voices[v_idx], false);
    return v_idx + 1;
}

void GCSound_StopAllSamples() {
    int i=0;
    for(i=0; i<MAX_SAMPLE_VOICES; i++) {
        if(sample_voices[i]) AESND_SetVoiceStop(sample_voices[i], true);
    }
}

void GCSound_SetMusicPan(int pan, u8 volume) {
    u16 l, r;
    _calculatePan(pan, volume, &l, &r);
    GCSound_SetMusicVolume((u8)l, (u8)r);
}

void GCSound_SetSamplePan(int channel, int pan, u8 volume) {
    u16 l, r;
    _calculatePan(pan, volume, &l, &r);
    if(channel > 0 && channel <= MAX_SAMPLE_VOICES) {
        int idx = channel - 1;
        if(sample_voices[idx]) AESND_SetVoiceVolume(sample_voices[idx], (u8)l, (u8)r);
    }
}

// ========================================================================
// NUEVA FUNCIÓN DE TELEMETRÍA ASÍNCRONA PARA EL MAIN.CPP
// ========================================================================
void GCSound_GetStatus(int *activeBuffer, int *buffersFilled, u32 *bytesRead, u32 *totalBytes) {
    if (activeBuffer) *activeBuffer = music_active_buffer;
    if (totalBytes)   *totalBytes   = music_dataSize;
    if (bytesRead)    *bytesRead    = music_dataRead;

    if (buffersFilled) {
        int count = 0;
        int i;
        for (i = 0; i < MUSIC_BUFFERS; i++) {
            if (buffer_ready_flag[i] == 1) {
                count++;
            }
        }
        *buffersFilled = count;
    }
}