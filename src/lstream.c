#include "lua.h"
#include "lstream.h"
#include "stream.h"
#include "lauxlib.h"
#include <errno.h>
#include "lutil.h"

static ELI_STREAM_KIND get_stream_kind(lua_State *L, int idx)
{
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

static int is_stream(lua_State *L, int idx)
{
	return get_stream_kind(L, idx) != ELI_STREAM_INVALID_KIND;
}

static int is_readable_stream(lua_State *L, int idx)
{
	ELI_STREAM_KIND kind = get_stream_kind(L, idx);
	return kind == ELI_STREAM_R_KIND || kind == ELI_STREAM_RW_KIND;
}

static int is_writable_stream(lua_State *L, int idx)
{
	ELI_STREAM_KIND kind = get_stream_kind(L, idx);
	return kind == ELI_STREAM_W_KIND || kind == ELI_STREAM_RW_KIND;
}

int lstream_read(lua_State *L)
{
	if (!is_readable_stream(L, 1)) {
		errno = EBADF;
		return push_error(L, "Not valid readable stream!");
	}
	int stream_index = 1;
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, stream_index);
	if (stream->closed) {
		errno = EBADF;
		return push_error(L, "Stream is not readable (closed)!");
	}

	int timeout = (int)luaL_optnumber(L, 3, -1);
	if (timeout < -1) {
		return luaL_argerror(L, 3, "timeout must be >= 0 or nil");
	}

	int divider = get_sleep_divider_from_state(L, 4, 1);
	int timeout_ms = sleep_duration_to_ms(timeout, divider);
	if (timeout == -1) {
		timeout_ms = stream->nonblocking ? 0 : -1;
	}

	switch (lua_type(L, 2)) {
	case LUA_TNUMBER: {
		size_t l = (size_t)luaL_checkinteger(L, 2);
		return stream_read_bytes(L, stream_index, l,
					 timeout < 0 ? -1 : timeout_ms);
	}
	case LUA_TSTRING: {
		return stream_read(L, stream_index, luaL_checkstring(L, 2),
				   timeout_ms);
	}
	default:
		return luaL_argerror(L, 2, "number or string expected");
	}

	return luaL_error(L, "Internal error");
}

int lstream_write(lua_State *L)
{
	if (!is_writable_stream(L, 1)) {
		errno = EBADF;
		return push_error(L, "Not valid writable stream!");
	}
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, 1);
	if (stream->closed) {
		errno = EBADF;
		return push_error(L, "Stream is not writable (closed)!");
	}
	size_t datasize;
	const char *data = luaL_checklstring(L, 2, &datasize);
	return stream_write(L, stream->fd, data, datasize);
}

int lstream_close(lua_State *L)
{
	if (!is_stream(L, 1)) {
		errno = EBADF;
		return push_error(L, "Not valid ELI_STREAM!");
	}
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, 1);
	if (stream->closed == 1)
		return 0;
	if (stream->notDisposable == 0) {
		// only non preowned file descriptors can be disposed here.
		int res = stream_close(stream->fd);
		if (!res)
			return push_error(L, "Failed to close stream!");
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
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, 1);
	int nonblocking = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : 1;
	int res = stream_set_nonblocking(stream->fd, nonblocking);
	if (!res)
		return push_error(L, "Failed set stream nonblocking!");
	stream->nonblocking = nonblocking;
	lua_pushboolean(L, 1);
	return 1;
}

int lstream_is_nonblocking(lua_State *L)
{
	if (!is_stream(L, 1)) {
		errno = EBADF;
		return push_error(L, "Not valid ELI_STREAM!");
	}
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, 1);
	int res = stream_is_nonblocking(stream->fd);
	if (res == -1)
		return push_error(L,
				  "Failed to check if stream is nonblocking!");
	lua_pushboolean(L, res);
	return 1;
}

int lstream_as_file(lua_State *L)
{
	const char *mode = "";
	switch (get_stream_kind(L, 1)) {
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
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, 1);

	int res = stream_as_filestream(L, stream->fd, mode);
	if (res == -1)
		return push_error(L, "Failed to convert stream to FILE*!");
	return res;
}

static void clone_stream(lua_State *L, ELI_STREAM *stream)
{
	ELI_STREAM *res = eli_new_stream(L);
	res->closed = 0;
	res->fd = dup(stream->fd);
	res->nonblocking = stream->nonblocking;
}

int lfile_as_stream(lua_State *L)
{
	luaL_Stream *file =
		(luaL_Stream *)luaL_checkudata(L, 1, LUA_FILEHANDLE);

	int fd = fileno(file->f);
	if (fd == -1)
		return push_error(L,
				  "Failed to get file descriptor from FILE*");

	int mode = 0;
	mode |= is_fd_readable(fd) ? 1 : 0;
	mode |= is_fd_writable(fd) ? 2 : 0;

	ELI_STREAM *stream = eli_new_stream(L);
	switch (mode) {
	case 1:
		luaL_getmetatable(L, ELI_STREAM_R_METATABLE);
		break;
	case 2:
		luaL_getmetatable(L, ELI_STREAM_W_METATABLE);
		break;
	case 3:
		luaL_getmetatable(L, ELI_STREAM_RW_METATABLE);
		break;
	default:
		return push_error(L, "Failed to determine stream mode");
	}
	lua_setmetatable(L, -2);

	stream->fd = dup(fd);
	return 1;
}

int lstream_rw_as_r(lua_State *L)
{
	ELI_STREAM *stream =
		(ELI_STREAM *)luaL_checkudata(L, 1, ELI_STREAM_RW_METATABLE);
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
	ELI_STREAM *stream =
		(ELI_STREAM *)luaL_checkudata(L, 1, ELI_STREAM_RW_METATABLE);
	if (stream->closed) {
		errno = EBADF;
		return push_error(L, "Stream is not writable (closed)!");
	}
	clone_stream(L, stream);
	luaL_getmetatable(L, ELI_STREAM_W_METATABLE);
	lua_setmetatable(L, -2);
	return 1;
}

int lextend_file_metatable(lua_State *L)
{
	luaL_getmetatable(L, LUA_FILEHANDLE);
	lua_getfield(L, -1, "__index");

	lua_pushcfunction(L, lfile_as_stream);
	lua_setfield(L, -2, "as_stream");

	return 0;
}

static void push_stream_base_methods(lua_State *L)
{
	lua_pushcfunction(L, lstream_close);
	lua_setfield(L, -2, "close");
	lua_pushcfunction(L, lstream_set_nonblocking);
	lua_setfield(L, -2, "set_nonblocking");
	lua_pushcfunction(L, lstream_is_nonblocking);
	lua_setfield(L, -2, "is_nonblocking");
	lua_pushcfunction(L, lstream_as_file);
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

static const struct luaL_Reg eli_stream_extra[] = {
	{ "file_as_stream", lfile_as_stream },
	{ "stream_as_filestream", lstream_as_file },
	{ "extend_file_metatable", lextend_file_metatable },
	{ NULL, NULL },
};

int luaopen_eli_stream_extra(lua_State *L)
{
	create_stream_r_meta(L);
	create_stream_w_meta(L);
	create_stream_rw_meta(L);

	lua_newtable(L);
	luaL_setfuncs(L, eli_stream_extra, 0);
	return 1;
}
