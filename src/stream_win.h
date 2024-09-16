#ifndef ELI_STREAM_WIN_EXTRA_H__
#define ELI_STREAM_WIN_EXTRA_H__

#ifdef _WIN32
#include "lua.h"
#include <windows.h>

int stream_win_read(ELI_STREAM *stream, char *buffer, size_t size);
int stream_win_write(ELI_STREAM *stream, const char *data, size_t size);

#endif
#endif // ELI_STREAM_WIN_EXTRA_H__