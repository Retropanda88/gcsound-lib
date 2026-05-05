#include <gcsound.h>
#include <aesndlib.h>
#include <ogc/lwp.h>
#include <ogc/lwp_queue.h>
#include <ogc/irq.h>
#include <ogc/context.h>
#include <ogc/cache.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <malloc.h>

#ifndef VOICE_STATE_STOP
#define VOICE_STATE_STOP 0
#endif

#define MUSIC_BUFFER_SIZE (32*1024)
#define MUSIC_BUFFERS 16
#define MAX_SAMPLE_VOICES 16
#define USER_CB_BUFFER_SIZE 4096 

static u8 music_buffers[MUSIC_BUFFERS][MUSIC_BUFFER_SIZE] __attribute__((aligned(32)));
static FILE* musicFile = NULL;
static AESNDPB* music_voice = NULL;
static u32 music_dataOffset = 0;
static volatile bool music_keep_playing = false;
static volatile bool music_is_paused = false;
static int music_active_buffer = 0;
static volatile int music_fill_next = -1;
static lwp_t music_thread = LWP_THREAD_NULL;
static lwpq_t music_queue;
static u32 music_sampleRate = 44100;
static u8 music_vol_l = 128;
static u8 music_vol_r = 128;
static volatile int music_is_ready = 0; 
static volatile int buffer_locked[MUSIC_BUFFERS];

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
    float leftRatio, rightRatio;
    if (pan < -128) pan = -128;
    if (pan > 127) pan = 127;
    leftRatio = (127 - pan) / 255.0f;
    rightRatio = (pan + 128) / 255.0f;
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
        AESND_SetVoiceStop(user_voice, true);
        AESND_FreeVoice(user_voice);
        user_voice = NULL;
    }
}

/*static void _loadMusicChunk(int index) {
    u32 i;
    u16 *ptr;
    size_t read;
    if (!musicFile) return;
    memset(music_buffers[index], 0, MUSIC_BUFFER_SIZE);
    read = fread(music_buffers[index], 1, MUSIC_BUFFER_SIZE, musicFile);
    if (read < MUSIC_BUFFER_SIZE) {
        fseek(musicFile, music_dataOffset, SEEK_SET);
        fread(music_buffers[index] + read, 1, MUSIC_BUFFER_SIZE - read, musicFile);
    }
    ptr = (u16*)music_buffers[index];
    for (i = 0; i < MUSIC_BUFFER_SIZE / 2; i++) {
        ptr[i] = (ptr[i] << 8) | (ptr[i] >> 8);
    }
    DCFlushRange(music_buffers[index], MUSIC_BUFFER_SIZE);
}*/


static void _loadMusicChunk(int index) {
    if (!musicFile || index < 0 || index >= MUSIC_BUFFERS) return;

    size_t read = fread(music_buffers[index], 1, MUSIC_BUFFER_SIZE, musicFile);
    
    // Si llegamos al final del archivo, hacemos el loop
    if (read < MUSIC_BUFFER_SIZE) {
        fseek(musicFile, music_dataOffset, SEEK_SET);
        // Leemos lo que falta para completar el buffer
        fread((u8*)music_buffers[index] + read, 1, MUSIC_BUFFER_SIZE - read, musicFile);
    }

    // SWAP DE BYTES OPTIMIZADO
    // Procesamos de 32 bits en 32 bits (dos muestras a la vez) para ir más rápido
    u32 *ptr32 = (u32*)music_buffers[index];
    u32 i;
    for (i = 0; i < MUSIC_BUFFER_SIZE / 4; i++) {
        // Esto cambia los bytes de 0xAABBCCDD a 0xBBAADDCC en una sola operación
        u32 val = ptr32[i];
        ptr32[i] = ((val << 8) & 0xFF00FF00) | ((val >> 8) & 0x00FF00FF);
    }

    // ¡CRÍTICO! Aseguramos que la RAM tenga los datos finales antes de que el audio los pida
    DCFlushRange(music_buffers[index], MUSIC_BUFFER_SIZE);
}

/*static void _musicCallback(AESNDPB *pb, u32 state) {
    if (state == VOICE_STATE_STREAM && music_keep_playing && !music_is_paused) {
        music_active_buffer = (music_active_buffer + 1) % MUSIC_BUFFERS;
        AESND_SetVoiceBuffer(pb, music_buffers[music_active_buffer], MUSIC_BUFFER_SIZE);
        music_fill_next = (music_active_buffer + 1) % MUSIC_BUFFERS;
        LWP_ThreadSignal(music_queue);
    }
}*/

static void _musicCallback(AESNDPB *pb, u32 state) {
    if (state == VOICE_STATE_STREAM && music_keep_playing && !music_is_paused) {
        // 1. Guardamos el que acabamos de terminar
        int finished_buffer = music_active_buffer;
        
        // 2. Apuntamos AL SIGUIENTE para el hardware
        music_active_buffer = (music_active_buffer + 1) % MUSIC_BUFFERS;
        
        // 3. Le damos el nuevo buffer al hardware de inmediato
        AESND_SetVoiceBuffer(pb, music_buffers[music_active_buffer], MUSIC_BUFFER_SIZE);
        
        // 4. AHORA avisamos al hilo que rellene el que ya soltamos
        music_fill_next = finished_buffer;
        LWP_ThreadSignal(music_queue);
    }
}

/*static void* _music_thread_func(void* arg) {
    while (music_keep_playing) {
        LWP_ThreadSleep(music_queue);
        if (music_fill_next != -1 && music_keep_playing) {
            int b = music_fill_next;
            music_fill_next = -1;
            _loadMusicChunk(b);
        }
    }
    return NULL;
}*/



static void* _music_thread_func(void* arg) {
    int i;
    for(i = 0; i < MUSIC_BUFFERS; i++) {
        buffer_locked[i] = 0;
        _loadMusicChunk(i);
    }
    music_is_ready = 1;

    while (music_keep_playing) {
        LWP_ThreadSleep(music_queue);
        
        if (music_keep_playing && music_fill_next != -1) {
            int b = music_fill_next;
            music_fill_next = -1;
            
            // Solo cargamos si el hardware no está en este buffer
            if (b != music_active_buffer) {
                _loadMusicChunk(b);
            }
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

/*int GCSound_PlayMusic(const char* filename, u32 forceFrequency) {
    char id[4];
    u32 rate = 44100;
    u16 chans = 2;
    GCSound_StopMusic();
    musicFile = fopen(filename, "rb");
    if (!musicFile) return 0;
    fseek(musicFile, 12, SEEK_SET);
    while (fread(id, 1, 4, musicFile) == 4) {
        u32 chunkSize;
        fread(&chunkSize, 4, 1, musicFile);
        u32 realSize = _swap32(chunkSize);
        if (memcmp(id, "fmt ", 4) == 0) {
            fseek(musicFile, 2, SEEK_CUR);
            fread(&chans, 2, 1, musicFile);
            fread(&rate, 4, 1, musicFile);
            chans = _swap16(chans);
            rate = _swap32(rate);
            fseek(musicFile, realSize - 8, SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            music_dataOffset = ftell(musicFile);
            break;
        } else {
            fseek(musicFile, (realSize + (realSize % 2)), SEEK_CUR);
        }
    }
    music_sampleRate = (forceFrequency > 0) ? forceFrequency : rate;
    music_keep_playing = true;
    music_is_paused = false;
    int i=0;
    for(i=0; i<MUSIC_BUFFERS; i++) _loadMusicChunk(i);
    LWP_CreateThread(&music_thread, _music_thread_func, NULL, NULL, 16384, 80);
    music_voice = AESND_AllocateVoice(_musicCallback);
    AESND_SetVoiceStream(music_voice, true);
    AESND_SetVoiceFrequency(music_voice, music_sampleRate);
    AESND_SetVoiceFormat(music_voice, (chans==2 ? VOICE_STEREO16 : VOICE_MONO16));
    AESND_SetVoiceVolume(music_voice, music_vol_l, music_vol_r);
    AESND_SetVoiceBuffer(music_voice, music_buffers[0], MUSIC_BUFFER_SIZE);
    AESND_SetVoiceStop(music_voice, false);
    return 1;
}*/


int GCSound_PlayMusic(const char* filename, u32 forceFrequency) {
    char id[4];
    u32 rate = 44100;
    u16 chans = 2;
    
    GCSound_StopMusic();
    
    // Abrimos el archivo (recuerda: sin prefijo sd:/)
    musicFile = fopen(filename, "rb");
    if (!musicFile) return 0;
    
    // Leemos la cabecera WAV para obtener rate y chans
    fseek(musicFile, 12, SEEK_SET);
    while (fread(id, 1, 4, musicFile) == 4) {
        u32 chunkSize;
        fread(&chunkSize, 4, 1, musicFile);
        u32 realSize = _swap32(chunkSize);
        if (memcmp(id, "fmt ", 4) == 0) {
            fseek(musicFile, 2, SEEK_CUR);
            fread(&chans, 2, 1, musicFile);
            fread(&rate, 4, 1, musicFile);
            chans = _swap16(chans);
            rate = _swap32(rate);
            fseek(musicFile, realSize - 8, SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            music_dataOffset = ftell(musicFile);
            break;
        } else {
            fseek(musicFile, (realSize + (realSize % 2)), SEEK_CUR);
        }
    }

    // Configuración de estado
    music_sampleRate = (forceFrequency > 0) ? forceFrequency : rate;
    music_keep_playing = true;
    music_is_paused = false;
    music_active_buffer = 0;
    music_fill_next = -1;
    music_is_ready = 0; // El hilo la pondrá en 1 al terminar de cargar

    // 1. Iniciamos el hilo de carga
    LWP_CreateThread(&music_thread, _music_thread_func, NULL, NULL, 16384, 65);

    // 2. Espera de Sincronización: Máximo 100ms para llenar los 10 buffers
    int timeout = 0;
    while(!music_is_ready && timeout < 100) {
        usleep(1000); 
        timeout++;
    }

    // 3. Activamos el audio
    music_voice = AESND_AllocateVoice(_musicCallback);
    if (music_voice) {
        AESND_SetVoiceStream(music_voice, true);
        AESND_SetVoiceFrequency(music_voice, music_sampleRate);
        AESND_SetVoiceFormat(music_voice, (chans == 2 ? VOICE_STEREO16 : VOICE_MONO16));
        AESND_SetVoiceVolume(music_voice, music_vol_l, music_vol_r);
        
        // El hilo ya llenó el buffer 0, así que lo asignamos
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
    char buffer[4];
    u32 dsize = 0;
    u32 rate = 44100;
    u16 chans = 2;
    fseek(f, 12, SEEK_SET);
    while (fread(buffer, 1, 4, f) == 4) {
        u32 chunkSize;
        fread(&chunkSize, 4, 1, f);
        u32 realSize = _swap32(chunkSize);
        if (memcmp(buffer, "fmt ", 4) == 0) {
            fseek(f, 2, SEEK_CUR);
            fread(&chans, 2, 1, f);
            fread(&rate, 4, 1, f);
            chans = _swap16(chans);
            rate = _swap32(rate);
            fseek(f, realSize - 8, SEEK_CUR);
        } else if (memcmp(buffer, "data", 4) == 0) {
            dsize = realSize;
            break;
        } else { fseek(f, realSize, SEEK_CUR); }
    }
    if (dsize == 0) { fclose(f); return NULL; }
    GCSound_Sample* s = (GCSound_Sample*)malloc(sizeof(GCSound_Sample));
    s->size = dsize;
    s->frequency = rate;
    s->channels = (u8)chans;
    s->format = (chans == 2) ? VOICE_STEREO16 : VOICE_MONO16;
    u32 alignedSize = (dsize + 31) & ~31;
    s->data = (u8*)memalign(32, alignedSize);
    fread(s->data, 1, dsize, f);
    fclose(f);
    u16* p = (u16*)s->data;
    u32 i;
    for(i=0; i < dsize/2; i++) p[i] = (p[i] << 8) | (p[i] >> 8);
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
    GCSound_SetSampleVolume(channel, (u8)l, (u8)r);
}