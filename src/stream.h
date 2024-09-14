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
	int fd;
	int closed;
	int nonblocking;
	int notDisposable;
} ELI_STREAM;

typedef enum ELI_STREAM_KIND {
	ELI_STREAM_R_KIND,
	ELI_STREAM_W_KIND,
	ELI_STREAM_RW_KIND,
	ELI_STREAM_INVALID_KIND
} ELI_STREAM_KIND;

int is_fd_readable(int fd);
int is_fd_writable(int fd);

int stream_read(lua_State *L, int stream_index, const char *opt,
		int timeout_ms);
int stream_read_bytes(lua_State *L, int stream_index, size_t length,
		      int timeout_ms);
int stream_write(lua_State *L, int fd, const char *data, size_t datasize);
int stream_is_nonblocking(int fd);
int stream_set_nonblocking(int fd, int nonblocking);
int stream_as_filestream(lua_State *L, int fd, const char *mode);
int stream_close(int fd);
ELI_STREAM *eli_new_stream(lua_State *L);
#endif