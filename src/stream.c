#include "lauxlib.h"
#include <stdio.h>
#include <errno.h>
#include "lutil.h"
#include "stream.h"
#include <fcntl.h>

#ifdef _WIN32 
    #define fdopen _fdopen
    #define read _read
    #define write _write
    #define dup _dup
    #define close _close
    #define fileno _fileno
#endif

int stream_write(lua_State *L, int fd, const char * data, size_t datasize)
{
    size_t status = 1;
    status = status && (write(fd,data, datasize) == datasize);
    if (status) {   
        lua_pushboolean(L, status);
        return 1;
    }
    return luaL_fileresult(L, status, NULL); 
}

static int stream_read_line(lua_State *L, int fd, int chop, int nonblocking)
{
    luaL_Buffer b;
    char c = '\0';
    luaL_buffinit(L, &b);
    size_t res = 1;

    while (res == 1 && c != EOF && c != '\n')
    {
        char *buff = luaL_prepbuffer(&b);
        int i = 0;
        while (i < LUAL_BUFFERSIZE && (res = read(fd, &c, sizeof(char))) == 1 && c != EOF && c != '\n')
        {
            buff[i++] = c;
        }
        luaL_addsize(&b, i);
    }
    if (!chop && c == '\n')
        luaL_addchar(&b, c);
    luaL_pushresult(&b);

    return push_read_result(L, res, nonblocking);
}

static int stream_read_all(lua_State *L, int fd, int nonblocking)
{
    size_t res;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    do
    {
        char *p = luaL_prepbuffer(&b);
        res = read(fd, p, LUAL_BUFFERSIZE);
        luaL_addsize(&b, res);
    } while (res == LUAL_BUFFERSIZE);

    luaL_pushresult(&b); 
    return push_read_result(L, res, nonblocking);
}

int stream_read_bytes(lua_State *L, int fd, size_t length, int nonblocking) 
{
    size_t res;
    char *p;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    p = luaL_prepbuffsize(&b, length);
    size_t res = read(fd, p, length);
    luaL_addsize(&b, res);
    luaL_pushresult(&b); 
    return push_read_result(L, res, nonblocking);
}

static int push_read_result(lua_State *L, int res, int nonblocking)
{
    if (res == -1)
    {
        char *errmsg;
        size_t _errno;

#ifdef _WIN32
        if (!nonblocking || (_errno = GetLastError()) != ERROR_NO_DATA)
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK || !nonblocking)
#endif
        {
#ifdef _WIN32
            if (!nonblocking || _errno != ERROR_NO_DATA)
            { // nonblocking so np
                LPSTR messageBuffer = NULL;
                size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                             NULL, _errno, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
                errmsg = messageBuffer;
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK || !nonblocking)
            {
                errmsg = strerror(errno);
                _errno = errno;
            }
#endif
            if (lua_rawlen(L, -1) == 0)
            {
                lua_pushnil(L);
            }
            lua_pushstring(L, errmsg);
            lua_pushinteger(L, _errno);
            return 3;
        }
    }
    return 1;
}

int stream_read(lua_State *L, int fd, const char * opt, int nonblocking)
{
    size_t success;
    if (*opt == '*') opt++; /* skip optional '*' (for compatibility) */
    switch (*opt)
    {
    case 'l': /* line */
        return read_line(L, fd, 1, nonblocking);
    case 'L': /* line with end-of-line */
        return read_line(L, fd, 0, nonblocking);
    case 'a':
        return read_all(L, fd, nonblocking); /* read all data available */
    default:
        return luaL_argerror(L, 2, "invalid format");
    }
}

int stream_is_nonblocking(int fd)
{
#ifdef _WIN32
    HANDLE h = _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return -1;
    }
    if (GetFileType(h) == FILE_TYPE_PIPE)
    {
        DWORD state;
        if (!GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL, 0))
            return -1;

        return (state & PIPE_NOWAIT) != 0;
    }
    errno = ENOTSUP;
    return -1;
#else
    if (fd < 0)
    {
        errno = EBADF;
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return -1;
    }
    return (flags & O_NONBLOCK) != 0;
#endif 
}

int stream_set_nonblocking(int fd, int nonblocking)
{
#ifdef _WIN32
    HANDLE h = _get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        errno = EBADF;
        return 0;
    }
    if (GetFileType(h) == FILE_TYPE_PIPE)
    {
        DWORD state;
        if (GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL, 0))
        {
            if (((state & PIPE_NOWAIT) != 0) == nonblocking)
            {
                return 1;
            }

            if (nonblocking)
                state &= ~PIPE_NOWAIT;
            else
                state |= PIPE_NOWAIT;
            if (SetNamedPipeHandleState(h, &state, NULL, NULL))
            {
                return 1;
            }
            errno = EINVAL;
            return 0;
        }
    }
    errno = ENOTSUP;
    return 0;
#else
    if (fd < 0)
    {
        errno = EBADF;
        return 0;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return 0;
    }
    if (((flags & O_NONBLOCK) != 0) != nonblocking)
    {   
        if (nonblocking)
        {
            flags |= O_NONBLOCK;
        }
        else
        {
            flags &= ~O_NONBLOCK;
        }
        int res = fcntl(fd, F_SETFL, flags);
        if (res == -1) return 0;
    }
    return 1;
#endif
}

static int io_fclose(lua_State *L)
{
    luaL_Stream *p = ((luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE));
    int res = fclose(p->f);
    return luaL_fileresult(L, (res == 0), NULL);
}

int stream_as_filestream(lua_State *L, int fd, const char * mode)
{
    int res = fdopen(dup(fd), mode);
    if (res == NULL) return -1;
    luaL_Stream *p = (luaL_Stream *)lua_newuserdata(L, sizeof(luaL_Stream));
    luaL_setmetatable(L, LUA_FILEHANDLE);
    p->f = res;
    p->closef = &io_fclose;
    return 1;
}

int stream_close(int fd) {
    return close(fd) == 0;
}

/**
 This creates potential issues on windows
 I was not able to find proper way to determine filedescriptor opening flags
 windows does not provide F_GETFL so probably no go for now.
 May be revaluated if this function becames needed in future. 
 For now it is just nice to have 
*/
// int filestream_as_stream(lua_State *L, luaL_Stream* file) {
//     int fd = fileno(file->f);
//     if (fd == -1) return 0;
//     int mode = fcntl(fd, F_GETFL);
//     if (mode == -1) return 0;
//     ELI_STREAM * stream = (ELI_STREAM *)lua_newuserdata(L, sizeof(ELI_STREAM));
//     stream->fd = dup(fd);
//     if (stream->fd == -1)
//         return 0;
//     stream->closed = 0;
//     stream->nonblocking = 0; 
//     switch (mode)
//     {
//     case O_RDONLY:
//         luaL_getmetatable(L, ELI_STREAM_R_METATABLE);
//         lua_setmetatable(L, -2);
//         break;
//     case O_WRONLY:
//         luaL_getmetatable(L, ELI_STREAM_W_METATABLE);
//         lua_setmetatable(L, -2);
//         break;
//     case O_RDWR:
//     case O_RDONLY | O_WRONLY:
//         luaL_getmetatable(L, ELI_STREAM_RW_METATABLE);
//         lua_setmetatable(L, -2);
//         break;
//     }
//     return 1;
// }

ELI_STREAM * new_stream() {
    ELI_STREAM* stream = malloc(sizeof(ELI_STREAM));
    stream->closed = 0;
    stream->fd = -1;
    stream->nonblocking = 0;
}