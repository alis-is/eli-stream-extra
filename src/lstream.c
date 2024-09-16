#include "lua.h"
#include "lstream.h"
#include "stream.h"
#include "lauxlib.h"
#include <errno.h>
#include <string.h>
#include "lerror.h"
#include "lsleep.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

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
	size_t size;
	const char *data = luaL_checklstring(L, 2, &size);
	return stream_write(L, stream, data, size);
}

int lstream_close(lua_State *L)
{
	if (!is_stream(L, 1)) {
		errno = EBADF;
		return push_error(L, "Not valid ELI_STREAM!");
	}
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, 1);
	if (!eli_stream_close(stream)) {
		return push_error(L, "Failed to close stream!");
	}

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
	lua_pushboolean(L, stream->nonblocking);
	return 1;
}

static void clone_stream(lua_State *L, ELI_STREAM *stream)
{
	ELI_STREAM *res = eli_new_stream(L);
	res->closed = 0;
#ifdef _WIN32
	DuplicateHandle(GetCurrentProcess(), stream->fd, GetCurrentProcess(),
			&res->fd, 0, FALSE, DUPLICATE_SAME_ACCESS);
#else
	res->fd = dup(stream->fd);
#endif
	res->nonblocking = stream->nonblocking;
}

int lopen_fstream(lua_State *L)
{
	ELI_STREAM *stream = eli_new_stream(L);
	const char *path = luaL_checkstring(L, 1);
	size_t mode_length;
	const char *mode = luaL_optlstring(L, 2, "r", &mode_length);
	if (mode_length == 0) {
		return push_error(L, "Invalid mode!");
	}

	int mode_num = 0; // 1 - read, 2 - write, 4 - append
	// switch (mode[0]) {
	// case 'r':
	// 	mode_num |= 1;
	// 	if (mode_length == 2) {
	// 		if (mode[1] != '+') {
	// 			return push_error(L, "Invalid mode!");
	// 		}
	// 		mode_num |= 2;
	// 	}
	// 	break;
	// case 'w':
	// 	mode_num |= 2;
	// 	if (mode_length == 2) {
	// 		if (mode[1] != '+') {
	// 			return push_error(L, "Invalid mode!");
	// 		}
	// 		mode_num |= 1;
	// 	}
	// 	break;
	// case 'a':
	// 	mode_num |= 2;
	// 	mode_num |= 4;
	// 	if (mode_length == 2) {
	// 		if (mode[1] != '+') {
	// 			return push_error(L, "Invalid mode!");
	// 		}
	// 		mode_num |= 1;
	// 	}
	// 	break;

	// default:
	// 	return push_error(L, "Invalid mode!");
	// }

	if (strchr(mode, '+') != NULL) {
		mode_num |= 1;
		mode_num |= 2;
	} else {
		if (strchr(mode, 'r') != NULL) {
			mode_num |= 1;
		}
		if (strchr(mode, 'w') != NULL) {
			mode_num |= 2;
		}
		if (strchr(mode, 'a') != NULL) {
			mode_num |= 4;
		}
	}

	switch (mode_num) {
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
		return push_error(L, "Invalid mode!");
	}
	lua_setmetatable(L, -2);
#ifdef _WIN32
	DWORD desired_access = 0;
	DWORD creation_disposition = 0;

	if (mode_num & 1) {
		desired_access |= GENERIC_READ;
	}
	if ((mode_num & 2) || (mode_num & 4)) {
		desired_access |= GENERIC_WRITE;
	}

	if (mode_num & 1) {
		creation_disposition = OPEN_EXISTING;
	} else if (mode_num & 2) {
		creation_disposition = CREATE_ALWAYS;
	} else if (mode_num & 4) {
		creation_disposition = OPEN_ALWAYS;
	}

	HANDLE fd = CreateFile(path, desired_access,
			       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
			       creation_disposition,
			       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
				       FILE_FLAG_NO_BUFFERING,
			       NULL);

	if (fd == INVALID_HANDLE_VALUE) {
		return push_error(L, "Failed to open file!");
	}
	stream->use_overlapped = 1;
	stream->overlapped_buffer = malloc(LUAL_BUFFERSIZE);
	stream->overlapped_buffer_size = LUAL_BUFFERSIZE;
#else
	int oflag = 0;
	if (mode_num & 1) {
		oflag |= O_RDONLY;
	} else if (mode_num & 2) {
		oflag |= O_WRONLY | O_CREAT | O_TRUNC;
	} else if (mode_num & 4) {
		oflag |= O_WRONLY | O_CREAT | O_APPEND;
	}

	int fd = open(path, oflag);
	if (fd == -1) {
		return push_error(L, "Failed to open file!");
	}
#endif
	stream->fd = fd;
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

static void push_stream_base_methods(lua_State *L)
{
	lua_pushcfunction(L, lstream_close);
	lua_setfield(L, -2, "close");
	lua_pushcfunction(L, lstream_set_nonblocking);
	lua_setfield(L, -2, "set_nonblocking");
	lua_pushcfunction(L, lstream_is_nonblocking);
	lua_setfield(L, -2, "is_nonblocking");
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
	{ "open_fstream", lopen_fstream },
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
