/**
 * @file gcsound.h
 * @author retropanda88 / libgcsound
 * @brief Librería estándar de audio para GameCube (Streaming, SFX y Mixer).
 * @version 2.7
 */

#ifndef _GCSOUND_H_
#define _GCSOUND_H_

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
   CONSTANTES Y DEFINICIONES
   ============================================================================ */

#define GCSOUND_MIN_VOLUME  0
#define GCSOUND_MAX_VOLUME  127
#define GCSOUND_DEFAULT_BUFFER_SIZE 16384

typedef enum {
    GCSOUND_STOPPED = 0,
    GCSOUND_PLAYING,     
    GCSOUND_PAUSED       
} GCSoundStatus;

/* ============================================================================
   ESTRUCTURAS
   ============================================================================ */

/**
 * @brief Estructura de Buffer Circular (Ring Buffer).
 * Gestiona el flujo de datos entre el productor (Mixer) y el consumidor (DMA/Hardware).
 */
typedef struct {
    s16 *buffer;        
    u32 size;           
    volatile u32 write_ptr; 
    volatile u32 read_ptr;  
    volatile u32 count;     
} GCSound_RingBuffer;

/**
 * @brief Estructura para Efectos de Sonido (SFX) precargados en RAM.
 */
typedef struct {
    s16* data;        /**< Puntero a muestras PCM 16-bit */
    u32  size;        /**< Tamaño total en bytes */
    u32  samples;     /**< Cantidad total de muestras */
    u32  frequency;   /**< Frecuencia original (ej. 22050, 44100) */
    u32 format;   // <--- ESTO DEBE ESTAR AQUÍ
    u8   channels;    /**< 1 = Mono, 2 = Estéreo */
} GCSound_Sample;

/**
 * @brief Definición del Callback de Usuario para síntesis o procesamiento externo.
 */
typedef void (*GCSound_UserCallback)(s16 *buffer, u32 len, void *userdata);

/* ============================================================================
   CONTROL GLOBAL DEL SISTEMA
   ============================================================================ */

/** @brief Inicializa el hardware de audio de la GameCube y la librería. */
void GCSound_Init();

/** @brief Cierra el sistema de audio y libera recursos. */
void GCSound_Close();

/* ============================================================================
   MÚSICA (Streaming continuo)
   ============================================================================ */

/** @brief Reproduce un archivo WAV desde el dispositivo de almacenamiento. */
int  GCSound_PlayMusic(const char* filename, u32 forceFrequency);
void GCSound_StopMusic();
void GCSound_PauseMusic(int paused);

/** @brief Ajusta el volumen maestro de la música (0-127). */
void GCSound_SetMusicVolume(u8 vol_l, u8 vol_r);

/** @brief Realiza un desvanecimiento gradual de la música. */
void GCSound_FadeMusicOut(u32 duration_ms);

/** @brief Estado actual de la reproducción de música. */
GCSoundStatus GCSound_GetMusicStatus();

/* ============================================================================
   SAMPLES / EFECTOS DE SONIDO (SFX)
   ============================================================================ */

/** @brief Carga un archivo WAV pequeño en la memoria RAM. */
GCSound_Sample* GCSound_LoadSample(const char* filename);

/** @brief Libera la memoria ocupada por un Sample. */
void GCSound_FreeSample(GCSound_Sample* sample);

/** @brief Reproduce un Sample en un canal libre. */
int  GCSound_PlaySample(GCSound_Sample* sample, u8 vol_l, u8 vol_r);

/** @brief Detiene todos los efectos de sonido activos. */
void GCSound_StopAllSamples();

/* ============================================================================
   INTERFACE DE BAJO NIVEL Y STREAMING
   ============================================================================ */

/** @brief Inicializa el buffer de streaming con un tamaño específico. */
void GCSound_InitStreaming(u32 num_samples);

/** @brief Inyecta audio PCM procesado al Ring Buffer. */
void GCSound_PushAudio(s16 *data, u32 len);

/** @brief Limpia el Ring Buffer de forma inmediata (Elimina latencia). */
void GCSound_ClearBuffer();

/** @brief Retorna el espacio disponible en muestras (s16) dentro del buffer. */
u32  GCSound_GetFreeSpace();

/* ============================================================================
   CALLBACKS DE USUARIO
   ============================================================================ */

/** @brief Registra una función para generar audio dinámicamente. */
void GCSound_RegisterUserCallback(GCSound_UserCallback cb, void *userdata);

/** @brief Elimina el callback de usuario activo. */
void GCSound_UnregisterUserCallback();

#ifdef __cplusplus
}
#endif

#endif // _GCSOUND_H_