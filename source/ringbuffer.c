#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <gcsound.h>

// Crea el buffer circular
void RingBuffer_Init(GCSound_RingBuffer *rb, u32 size_in_samples) {
    rb->size = size_in_samples;
    rb->buffer = (s16*)memalign(32, size_in_samples * sizeof(s16));
    rb->write_ptr = 0;
    rb->read_ptr = 0;
    rb->count = 0;
    memset(rb->buffer, 0, size_in_samples * sizeof(s16));
}

// El Mixer "empuja" datos aquí
void RingBuffer_Write(GCSound_RingBuffer *rb, s16 *data, u32 len) {
    u32 i;
    for (i = 0; i < len; i++) {
        // Si el buffer está lleno, dejamos de escribir (o sobreescribimos, según prefieras)
        if (rb->count < rb->size) {
            rb->buffer[rb->write_ptr] = data[i];
            rb->write_ptr = (rb->write_ptr + 1) % rb->size;
            rb->count++;
        }
    }
}

// La Callback de Hardware "saca" datos de aquí
u32 RingBuffer_Read(GCSound_RingBuffer *rb, s16 *data, u32 len) {
    u32 muestras_leidas = 0;
    u32 i;
    for (i = 0; i < len; i++) {
        if (rb->count > 0) {
            data[i] = rb->buffer[rb->read_ptr];
            rb->read_ptr = (rb->read_ptr + 1) % rb->size;
            rb->count--;
            muestras_leidas++;
        } else {
            data[i] = 0; // Silencio si no hay datos
        }
    }
    return muestras_leidas;
}