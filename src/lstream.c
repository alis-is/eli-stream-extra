#include "lua.h"
#include "lstream.h" 
#include "stream.h"
#include "lauxlib.h"
#include <errno.h>
#include "lutil.h"

static ELI_STREAM_KIND get_stream_kind(lua_State *L, int idx) {
    ELI_STREAM_KIND res = ELI_STREAM_INVALID_KIND;
    int top = lua_gettop(L);
    lua_getmetatable(L, idx);
    luaL_getmetatable(L, ELI_STREAM_R_METATABLE);
    luaL_getmetatable(L, ELI_STREAM_W_METATABLE);
    luaL_getmetatable(L, ELI_STREAM_RW_METATABLE);
    if (lua_rawequal(L, -3, -4)) {
        res = ELI_STREAM_R_KIND;
    }
    if (lua_rawequal(L, -2, -4)) {
        res = ELI_STREAM_W_KIND;
    }
    if (lua_rawequal(L, -1, -4)) {
        res = ELI_STREAM_RW_KIND;
    }

    lua_pop(L, lua_gettop(L) - top);
    return res;
}

static int is_stream(lua_State *L, int idx) {
    return get_stream_kind(L, idx) != ELI_STREAM_INVALID_KIND;
}

static int is_readable_stream(lua_State *L, int idx) {
    ELI_STREAM_KIND kind = get_stream_kind(L, idx);
    return kind == ELI_STREAM_R_KIND || kind == ELI_STREAM_RW_KIND;
}

static int is_writable_stream(lua_State *L, int idx) {
    ELI_STREAM_KIND kind = get_stream_kind(L, idx);
    return kind == ELI_STREAM_W_KIND || kind == ELI_STREAM_RW_KIND;
}

int lstream_read(lua_State *L) {
    if (!is_readable_stream(L, 1)) {
        errno = EBADF;
        return push_error(L, "Not valid readable stream!");
    }
    ELI_STREAM * stream = (ELI_STREAM *)lua_touserdata(L, 1);
    if (stream->closed) {
        errno = EBADF;
        return push_error(L, "Stream is not readable (closed)!");
    }
    int res;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        size_t l = (size_t)luaL_checkinteger(L, 2);
        res = stream_read_bytes(L, stream -> fd, l, stream->nonblocking);
    } else {
        const char *opt = luaL_checkstring(L, 2);
        res = stream_read(L, stream -> fd, opt, stream -> nonblocking);
    }

    if (res == -1) {
        return push_error(L, "Read failed!");
    }
    return res;
}

int lstream_write(lua_State *L) {
    if (!is_writable_stream(L, 1)) {
        errno = EBADF;
        return push_error(L, "Not valid writable stream!");
    }
    ELI_STREAM * stream = (ELI_STREAM *)lua_touserdata(L, 1);
    if (stream->closed) {
        errno = EBADF;
        return push_error(L, "Stream is not writable (closed)!");
    }
    size_t datasize;
    const char *data = luaL_checklstring(L, 2, &datasize);
    return stream_write(L, stream -> fd, data, datasize);
}

int lstream_close(lua_State *L) {
    if (!is_stream(L, 1)) {
        errno = EBADF;
        return push_error(L, "Not valid ELI_STREAM!");
    }
    ELI_STREAM * stream = (ELI_STREAM *)lua_touserdata(L, 1);
    if (stream -> closed == 1) return 0;
    if (stream -> notDisposable == 0) {
       // only non preowned file descriptors can be disposed here.
       int res = stream_close(stream -> fd);
       if (!res) return push_error(L, "Failed to close stream!");
    }
    stream->closed = 1;
    return 0;
}

int lstream_set_nonblocking(lua_State *L) 
{
    if (!is_stream(L, 1)) {
        errno = EBADF;
        return push_error(L, "Not valid ELI_STREAM!");
    }
    ELI_STREAM * stream = (ELI_STREAM *)lua_touserdata(L, 1);
    int nonblocking = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : 1;
    int res = stream_set_nonblocking(stream -> fd, nonblocking);
    if (!res) return push_error(L, "Failed set stream nonblocking!");
    stream-> nonblocking = nonblocking;
    lua_pushboolean(L, 1);
    return 1;
}

int lstream_is_nonblocking(lua_State *L) 
{
    if (!is_stream(L, 1)) {
        errno = EBADF;
        return push_error(L, "Not valid ELI_STREAM!");
    }
    ELI_STREAM * stream = (ELI_STREAM *)lua_touserdata(L, 1);
    int res = stream_is_nonblocking(stream -> fd);
    if (res == -1) return push_error(L, "Failed to check if stream is nonblocking!");
    lua_pushboolean(L, res);
    return 1;
}

int lstream_as_filestream(lua_State *L) 
{
    const char * mode = "";
    switch (get_stream_kind(L, 1))
    {
    case ELI_STREAM_R_KIND:
        mode = "r";
        break;
    case ELI_STREAM_W_KIND:
        mode = "w";
        break;
    case ELI_STREAM_RW_KIND:
        mode = luaL_optstring(L, 2, "r+");
    default:
        errno = EBADF;
        return push_error(L, "Not valid ELI_STREAM!");
    }
    ELI_STREAM * stream = (ELI_STREAM *)lua_touserdata(L, 1);

    int res = stream_as_filestream(L, stream -> fd, mode);
    if (res == -1) return push_error(L, "Failed to convert stream to FILE*!");
    return res;
}

static void clone_stream(lua_State *L, ELI_STREAM* stream) {
    ELI_STREAM* res = lua_newuserdata(L, sizeof(ELI_STREAM));
    res->closed = 0;
    res->fd = dup(stream->fd);
    res->nonblocking = stream->nonblocking;
}

int lstream_rw_as_r(lua_State *L) 
{
    ELI_STREAM* stream = (ELI_STREAM *)luaL_checkudata(L, 1, ELI_STREAM_RW_METATABLE);
    if (stream->closed) {
        errno = EBADF;
        return push_error(L, "Stream is not writable (closed)!");
    }
    clone_stream(L, stream);
    luaL_getmetatable(L, ELI_STREAM_R_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int lstream_rw_as_w(lua_State *L) 
{
    ELI_STREAM* stream = (ELI_STREAM *)luaL_checkudata(L, 1, ELI_STREAM_RW_METATABLE);
    if (stream->closed) {
        errno = EBADF;
        return push_error(L, "Stream is not writable (closed)!");
    }
    clone_stream(L, stream);
    luaL_getmetatable(L, ELI_STREAM_W_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static void push_stream_base_methods(lua_State *L) 
{
    lua_pushcfunction(L, lstream_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, lstream_set_nonblocking);
    lua_setfield(L, -2, "set_nonblocking");
    lua_pushcfunction(L, lstream_is_nonblocking);
    lua_setfield(L, -2, "is_nonblocking");
    lua_pushcfunction(L, lstream_as_filestream);
    lua_setfield(L, -2, "as_filestream");
}

int create_stream_r_meta(lua_State *L)
{
    luaL_newmetatable(L, ELI_STREAM_R_METATABLE);

    /* Method table */
    lua_newtable(L);
    lua_pushcfunction(L, lstream_read);
    lua_setfield(L, -2, "read");
    push_stream_base_methods(L);

    lua_pushstring(L, ELI_STREAM_R_METATABLE);
    lua_setfield(L, -2, "__type");

    /* Metamethods */
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lstream_close);
    lua_setfield(L, -2, "__gc");

    return 1;
}

int create_stream_w_meta(lua_State *L)
{
    luaL_newmetatable(L, ELI_STREAM_W_METATABLE);

    /* Method table */
    lua_newtable(L);
    lua_pushcfunction(L, lstream_write);
    lua_setfield(L, -2, "write");
    push_stream_base_methods(L);

    lua_pushstring(L, ELI_STREAM_W_METATABLE);
    lua_setfield(L, -2, "__type");

    /* Metamethods */
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lstream_close);
    lua_setfield(L, -2, "__gc");

    return 1;
}

int create_stream_rw_meta(lua_State *L)
{
    luaL_newmetatable(L, ELI_STREAM_RW_METATABLE);

    /* Method table */
    lua_newtable(L);
    lua_pushcfunction(L, lstream_write);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, lstream_read);
    lua_setfield(L, -2, "read");
    push_stream_base_methods(L);

    lua_pushcfunction(L, lstream_rw_as_r);
    lua_setfield(L, -2, "as_readable_stream");
    lua_pushcfunction(L, lstream_rw_as_w);
    lua_setfield(L, -2, "as_writable_stream");

    lua_pushstring(L, ELI_STREAM_RW_METATABLE);
    lua_setfield(L, -2, "__type");

    /* Metamethods */
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lstream_close);
    lua_setfield(L, -2, "__gc");

    return 1;
}

int luaopen_eli_stream_extra(lua_State *L)
{
    create_stream_r_meta(L);
    create_stream_w_meta(L);
    create_stream_rw_meta(L);
    return 0;
}
