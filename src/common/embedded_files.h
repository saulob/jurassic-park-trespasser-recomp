#ifndef JPT_EMBEDDED_FILES_H
#define JPT_EMBEDDED_FILES_H

typedef struct {
    const char* name;
    const unsigned char* data;
    unsigned long size;
} jpt_embedded_file_t;

#endif
