#include "lua.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define READABLE_STREAM_METATABLE "READABLE_STREAM_METATABLE"
#define WRITABLE_STREAM_METATABLE "WRITABLE_STREAM_METATABLE"

typedef struct ELI_STREAM
{
    int fd;
    int closed;
    int nonblocking;
} ELI_STREAM;

typedef enum ELI_STREAM_KIND {
    ELI_STREAM_READABLE_KIND,
    ELI_STREAM_WRITABLE_KIND,
    ELI_STREAM_INVALID_KIND
} ELI_STREAM_KIND;

int stream_read(lua_State *L, int fd, const char * opt, int nonblocking);
int stream_read_bytes(lua_State *L, int fd, size_t length, int nonblocking);
int stream_write(lua_State *L, int fd, const char * data, size_t datasize);
int stream_is_nonblocking(int fd);
int stream_set_nonblocking(int fd, int nonblocking);
int stream_as_filestream(lua_State *L, int fd, const char * mode);
int stream_close(int fd);
ELI_STREAM * new_stream();