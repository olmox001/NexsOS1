#ifndef OS1_IMAGE_H
#define OS1_IMAGE_H

#include <graphics.h>
#include <os1.h>
#include <stdint.h>
#include <stdlib.h>

// Struttura ad alto livello per gestire le immagini nel tuo gioco/app
typedef struct {
  int w;
  int h;
  uint32_t *pixels;
} os1_image_t;

/**
 * Carica un'immagine (PNG, JPG, BMP) sfruttando l'engine STB integrato in OS1
 * ed effettua lo swizzling (conversione canali) da RGBA a ARGB nativo.
 */
static inline os1_image_t *os1_image_load(const char *path) {
  int w, h;

  // Sfruttiamo la funzione nativa presente in lib.c che legge il file
  // e chiama internamente stbi_load_from_memory richiedendo 4 canali.
  uint32_t *raw_pixels = graphics_load_image(path, &w, &h);
  if (!raw_pixels) {
    return NULL;
  }

  os1_image_t *img = (os1_image_t *)malloc(sizeof(os1_image_t));
  if (!img) {
    free(raw_pixels);
    return NULL;
  }

  img->w = w;
  img->h = h;
  img->pixels = raw_pixels;

  // Correggiamo il formato colore: stb_image sputa in formato byte-order RGBA.
  // Il server grafico di OS1 richiede invece ARGB32 (0xAARRGGBB).
  uint32_t total_pixels = w * h;
  for (uint32_t i = 0; i < total_pixels; i++) {
    uint32_t rgba = img->pixels[i];

    uint8_t r = (rgba >> 0) & 0xFF;
    uint8_t g = (rgba >> 8) & 0xFF;
    uint8_t b = (rgba >> 16) & 0xFF;
    uint8_t a = (rgba >> 24) & 0xFF;

    // Impacchettiamo nel formato ARGB atteso dal compositore di OS1
    img->pixels[i] =
        ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }

  return img;
}

/**
 * Disegna l'immagine sulla finestra specificata usando la syscall di blit
 * nativa.
 */
static inline void os1_image_draw(int win_id, int x, int y, os1_image_t *img) {
  if (!img || !img->pixels || win_id < 0)
    return;

  // Si collega direttamente alla funzione ad alto livello di OS1 (o usa
  // window_blit)
  graphics_blit(win_id, x, y, img->w, img->h, img->pixels);
}

/**
 * Libera in modo sicuro le risorse allocate dall'immagine.
 */
static inline void os1_image_free(os1_image_t *img) {
  if (!img)
    return;
  if (img->pixels) {
    free(img->pixels); // Libera il buffer allocato originariamente da stb_image
  }
  free(img);
}

#endif // OS1_IMAGE_H