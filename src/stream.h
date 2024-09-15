#ifndef ELI_STREAM_EXTRA_H__
#define ELI_STREAM_EXTRA_H__

#include "lua.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define ELI_STREAM_R_METATABLE "ELI_STREAM_R"
#define ELI_STREAM_W_METATABLE "ELI_STREAM_W"
#define ELI_STREAM_RW_METATABLE "ELI_STREAM_RW"

typedef struct ELI_STREAM {
#ifdef _WIN32
	HANDLE fd;
	int use_overlapped;
	OVERLAPPED overlapped;
	char *overlapped_buffer;
	size_t overlapped_buffer_size;
#else
	int fd;
#endif
	int closed;
	int nonblocking;
	int not_disposable;
} ELI_STREAM;

typedef enum ELI_STREAM_KIND {
	ELI_STREAM_R_KIND,
	ELI_STREAM_W_KIND,
	ELI_STREAM_RW_KIND,
	ELI_STREAM_INVALID_KIND
} ELI_STREAM_KIND;

int stream_read(lua_State *L, int stream_index, const char *opt,
		int timeout_ms);
int stream_read_bytes(lua_State *L, int stream_index, size_t length,
		      int timeout_ms);
int stream_write(lua_State *L, ELI_STREAM *stream, const char *data,
		 size_t size);
int stream_is_nonblocking(ELI_STREAM *stream);
int stream_set_nonblocking(ELI_STREAM *stream, int nonblocking);
ELI_STREAM *eli_new_stream(lua_State *L);
int eli_stream_close(ELI_STREAM *stream);
#endif