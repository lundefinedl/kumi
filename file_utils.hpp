#ifndef _FILE_UTILS_HPP_
#define _FILE_UTILS_HPP_

bool load_file(const char *filename, void **buf, size_t *size);
bool file_exists(const char *filename);

bool save_bmp32(const char *filename, uint8_t *ptr, int width, int height);
bool save_bmp_mono(const char *filename, uint8_t *ptr, int width, int height);

#endif
