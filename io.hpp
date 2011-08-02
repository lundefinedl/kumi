#pragma once

struct Io {
	virtual bool load_file(const char *filename, void **buf, size_t *len) = 0;
	virtual bool file_exists(const char *filename) = 0;
	virtual __time64_t mdate(const char *filename) = 0;
};

struct DiskIo : public Io {
	virtual bool load_file(const char *filename, void **buf, size_t *len);
	virtual bool file_exists(const char *filename);
	virtual __time64_t mdate(const char *filename);
};
