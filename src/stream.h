#include "lua.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define ELI_STREAM_R_METATABLE "ELI_STREAM_R_METATABLE"
#define ELI_STREAM_W_METATABLE "ELI_STREAM_W_METATABLE"
#define ELI_STREAM_RW_METATABLE "ELI_STREAM_RW_METATABLE"

typedef struct ELI_STREAM
{
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

int stream_read(lua_State *L, int fd, const char * opt, int nonblocking);
int stream_read_bytes(lua_State *L, int fd, size_t length, int nonblocking);
int stream_write(lua_State *L, int fd, const char * data, size_t datasize);
int stream_is_nonblocking(int fd);
int stream_set_nonblocking(int fd, int nonblocking);
int stream_as_filestream(lua_State *L, int fd, const char * mode);
int stream_close(int fd);
ELI_STREAM * new_stream();
