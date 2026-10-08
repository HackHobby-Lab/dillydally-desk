#pragma once

typedef enum {
    DD_MEDIA_RADIO,
    DD_MEDIA_SPEECH,
} dd_media_kind_t;

typedef struct {
    dd_media_kind_t kind;
    const char *name;
    const char *url;
} dd_media_t;

extern const dd_media_t dd_media[];
extern const int dd_media_count;
