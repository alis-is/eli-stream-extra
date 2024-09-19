#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lauxlib.h"
#include "lsleep.h"
#include "lerror.h"
#include "stream.h"

#ifdef _WIN32
#include <errno.h>
#include "stream_win.h"

#define fdopen _fdopen
#define read _read
#define write _write
#define dup _dup
#define close _close
#define fileno _fileno
#define lseek _lseek
#define errno GetLastError()

#define STREAM_FD_DEFAULT INVALID_HANDLE_VALUE
#define WOULD_BLOCK (GetLastError() == ERROR_NO_DATA)
#define read_stream(stream, buffer, size) stream_win_read(stream, buffer, size)
#define write_stream(stream, data, size) stream_win_write(stream, data, size)
#else
#define STREAM_FD_DEFAULT -1
#define WOULD_BLOCK (errno == EWOULDBLOCK || errno == EAGAIN)
#define read_stream(stream, buffer, size) read(stream->fd, buffer, size)
#define write_stream(stream, data, size) write(stream->fd, data, size)
#endif

int stream_write(lua_State *L, ELI_STREAM *stream, const char *data,
		 size_t size)
{
	size_t status = write_stream(stream, data, size) == size;
	if (status) {
		lua_pushboolean(L, status);
		return 1;
	}
	return luaL_fileresult(L, status, NULL);
}

static int push_read_result(lua_State *L, int res, int timed_out)
{
	if (timed_out) {
		// data we read so far
		lua_pushliteral(L, "timeout");
		return 2;
	}

	switch (res) {
	case -1:
		if (!WOULD_BLOCK) {
			// we push nil only if it's not WOULD_BLOCK
			// because if it is WOULD_BLOCK, there may be some data later
			lua_pushnil(L);
		}
		push_error_string(L, NULL);
		lua_pushinteger(L, errno);
		return 3;
	case 0:
		lua_pushnil(L); // EOF
	default:
		return 1;
	}
}

static int get_sleep_per_iteration(int timeout_ms)
{
	int sleep_per_iteration = timeout_ms / 10;
	if (sleep_per_iteration <= 0) {
		sleep_per_iteration = 1; // at least 1 ms
	}
	return sleep_per_iteration;
}

static int stream_is_nonblocking(ELI_STREAM *stream)
{
#ifndef _WIN32
	if (stream->fd < 0) {
		errno = EBADF;
		return -1;
	}
	int flags = fcntl(stream->fd, F_GETFL, 0);
	if (flags < 0) {
		return -1;
	}
	return (flags & O_NONBLOCK) != 0;
#endif
	return 1;
}

static int stream_set_nonblocking(ELI_STREAM *stream, int nonblocking)
{
#ifndef _WIN32
	if (stream->fd < 0) {
		errno = EBADF;
		return 0;
	}
	int flags = fcntl(stream->fd, F_GETFL, 0);
	if (flags < 0) {
		return 0;
	}
	if (((flags & O_NONBLOCK) != 0) != nonblocking) {
		if (nonblocking) {
			flags |= O_NONBLOCK;
		} else {
			flags &= ~O_NONBLOCK;
		}
		int res = fcntl(stream->fd, F_SETFL, flags);
		if (res == -1) {
			return 0;
		}
	}
#endif
	return 1;
}

// return 1 if mode was changed, 0 if it was already nonblocking
static int set_nonblocking(lua_State *L, ELI_STREAM *stream)
{
	if (stream_is_nonblocking(stream)) {
		return 0;
	}

	if (!stream_set_nonblocking(stream, 1)) {
		return luaL_error(L, "failed to set nonblocking mode");
	}
	return 1;
}

static int restore_blocking_mode(lua_State *L, ELI_STREAM *stream)
{
	// NOTE: throwing error here is tricky, it would discard the data
	// so we just ignore the error here right now
	stream_set_nonblocking(stream, stream->nonblocking);
	return 1;
}

static int add_pending_data(lua_State *L, int stream_index, const char *data,
			    size_t data_len)
{
	if (!lua_getiuservalue(L, stream_index, 1)) {
		lua_pushlstring(L, data, data_len);
		lua_setiuservalue(L, stream_index, 1);
		return 1;
	}

	size_t pending_length = 0;
	const char *pending_data = lua_tolstring(L, -1, &pending_length);
	if (pending_data == NULL || pending_length == 0) {
		lua_pushlstring(L, data, data_len);
		lua_setiuservalue(L, stream_index, 1);
		return 1;
	}

	luaL_Buffer b;
	luaL_buffinit(L, &b);
	luaL_addlstring(&b, pending_data, pending_length);
	luaL_addlstring(&b, data, data_len);
	luaL_pushresult(&b);
	lua_setiuservalue(L, stream_index, 1);
	return 1;
}

static int read_pending_line(lua_State *L, int stream_index, luaL_Buffer *b,
			     int chop)
{
	if (!lua_getiuservalue(L, stream_index, 1)) {
		return -1;
	}

	size_t pending_length = 0;
	const char *pending = lua_tolstring(L, -1, &pending_length);
	if (pending == NULL || pending_length == 0) {
		return -1;
	}

	const char *newline = memchr(pending, '\n', pending_length);
	if (newline != NULL) {
		size_t line_length = newline - pending + (chop ? 0 : 1);
		size_t remaining =
			pending_length - line_length - (chop ? 1 : 0);
		memcpy(luaL_prepbuffsize(b, line_length), pending, line_length);
		lua_remove(L, -1);
		lua_pushlstring(L, newline + 1, remaining);
		lua_setiuservalue(L, stream_index, 1); // store remaining data
		luaL_addsize(b, line_length);
		return line_length;
	}

	memcpy(luaL_prepbuffsize(b, pending_length), pending, pending_length);
	lua_remove(L, -1);
	lua_pushnil(L);
	lua_setiuservalue(L, stream_index, 1); // remove pending data
	luaL_addsize(b, pending_length);
	return -1; // no newline found
}

static int read_all_pending_data(lua_State *L, int stream_index, luaL_Buffer *b)
{
	if (!lua_getiuservalue(L, stream_index, 1)) {
		return 0;
	}

	size_t pending_length = 0;
	const char *pending_data = lua_tolstring(L, -1, &pending_length);
	if (pending_data == NULL || pending_length == 0) {
		return 0;
	}

	memcpy(luaL_prepbuffsize(b, pending_length), pending_data,
	       pending_length);
	lua_remove(L, -1);
	lua_pushnil(L);
	lua_setiuservalue(L, stream_index, 1); // store pending data
	luaL_addsize(b, pending_length);
	return pending_length;
}

static int stream_read_line(lua_State *L, int stream_index, int chop,
			    int timeout_ms)
{
	size_t res;
	luaL_Buffer b;
	luaL_buffinit(L, &b);

	long long start_time = get_time_in_ms();
	int sleep_per_iteration =
		timeout_ms == -1 ? 100 : get_sleep_per_iteration(timeout_ms);

	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, stream_index);
	set_nonblocking(L, stream);

	size_t total_read = 0;
	int timed_out = 0;
	do {
		int line_length = read_pending_line(L, stream_index, &b, chop);
		if (line_length >= 0) {
			total_read += line_length;
			break;
		}

		char *buff = luaL_prepbuffsize(&b, LUAL_BUFFERSIZE);
		res = read_stream(stream, buff, LUAL_BUFFERSIZE);
		if (res == -1) {
			if (WOULD_BLOCK) {
				sleep_ms(sleep_per_iteration);
				goto TIMEOUT_CHECK;
			}
			return push_read_result(L, res, 0);
		}
		if (res == 0) {
			// EOF, add pending data if any
			res = read_all_pending_data(L, stream_index, &b);
			break;
		}

		char *new_line = memchr(buff, '\n', res);
		if (new_line != NULL) {
			size_t line_length = new_line - buff + (chop ? 0 : 1);
			size_t remaining = res - line_length - (chop ? 1 : 0);
			luaL_addsize(&b, line_length);
			total_read += line_length;
			// the rest of the data without '\n'
			add_pending_data(L, stream_index, new_line + 1,
					 remaining);
			break;
		}
		luaL_addsize(&b, res);
		total_read += res;
TIMEOUT_CHECK:
		if (timeout_ms != -1 &&
		    start_time + timeout_ms < get_time_in_ms()) {
			timed_out = 1;
			break;
		}
	} while (res != 0);

	luaL_pushresult(&b);
	restore_blocking_mode(L, stream);
	return push_read_result(L, total_read > 0 ? total_read : res,
				timed_out);
}

static int stream_read_all(lua_State *L, int stream_index, int timeout_ms)
{
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	read_all_pending_data(L, stream_index, &b);

	size_t res;
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, stream_index);
	set_nonblocking(L, stream);

	long long start_time = get_time_in_ms();
	int sleep_per_iteration =
		timeout_ms == -1 ? 100 : get_sleep_per_iteration(timeout_ms);
	size_t total_read = 0;
	int timed_out = 0;
	do {
		char *p = luaL_prepbuffsize(&b, LUAL_BUFFERSIZE);
		res = read_stream(stream, p, LUAL_BUFFERSIZE);
		if (res == -1) { // read some data
			if (WOULD_BLOCK) {
				sleep_ms(sleep_per_iteration);
				goto TIMEOUT_CHECK;
			}
			break;
		}
		luaL_addsize(&b, res);
		total_read += res;
TIMEOUT_CHECK:
		if (timeout_ms != -1 &&
		    start_time + timeout_ms < get_time_in_ms()) {
			timed_out = 1;
			break;
		}
	} while (res != 0);
	luaL_pushresult(&b);
	restore_blocking_mode(L, stream);
	return push_read_result(L, total_read > 0 ? total_read : res,
				timed_out);
}

static int read_pending_bytes(lua_State *L, int stream_index, size_t lengh,
			      luaL_Buffer *b)
{
	if (!lua_getiuservalue(L, stream_index, 1)) {
		return 0;
	}

	size_t pending_length = 0;
	const char *pending_data = lua_tolstring(L, -1, &pending_length);
	if (pending_data == NULL || pending_length == 0) {
		return 0;
	}

	size_t copy_length = lengh < pending_length ? lengh : pending_length;
	memcpy(luaL_prepbuffsize(b, copy_length), pending_data, copy_length);
	lua_remove(L, -1);
	lua_pushlstring(L, pending_data + copy_length,
			pending_length - copy_length);
	lua_setiuservalue(L, stream_index, 1); // store pending data
	luaL_addsize(b, copy_length);
	return copy_length;
}

int stream_read_bytes(lua_State *L, int stream_index, size_t length,
		      int timeout_ms)
{
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	int cached = read_pending_bytes(L, stream_index, length, &b);
	if (length == cached) {
		luaL_pushresult(&b);
		return push_read_result(L, cached, 0);
	}
	length -= cached;

	size_t res;
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, stream_index);
	set_nonblocking(L, stream);

	long long start_time = get_time_in_ms();
	int sleep_per_iteration =
		timeout_ms == -1 ? 100 : get_sleep_per_iteration(timeout_ms);

	char *p = luaL_prepbuffsize(&b, length);
	size_t total_read = 0;
	int timed_out = 0;
	do {
		res = read_stream(stream, p + total_read, length - total_read);
		if (res == -1) { // read some data
			if (WOULD_BLOCK) {
				sleep_ms(sleep_per_iteration);
				goto TIMEOUT_CHECK;
			}
			break;
		}
		luaL_addsize(&b, res);
		total_read += res;
		if (total_read == length) {
			break;
		}
TIMEOUT_CHECK:
		if (timeout_ms != -1 &&
		    start_time + timeout_ms < get_time_in_ms()) {
			timed_out = 1;
			break;
		}
	} while (res != 0);
	luaL_pushresult(&b);
	restore_blocking_mode(L, stream);
	return push_read_result(L, total_read > 0 ? total_read : res,
				timed_out);
}

int stream_read(lua_State *L, int stream_index, const char *opt, int timeout_ms)
{
	size_t success;
	if (*opt == '*') {
		opt++; /* skip optional '*' (for compatibility) */
	}
	switch (*opt) {
	case 'l': /* line */
		return stream_read_line(L, stream_index, 1, timeout_ms);
	case 'L': /* line with end-of-line */
		return stream_read_line(L, stream_index, 0, timeout_ms);
	case 'a':
		return stream_read_all(
			L, stream_index,
			timeout_ms); /* read all data available */
	default:
		return luaL_argerror(L, 2, "invalid format");
	}
}

ELI_STREAM *eli_new_stream(lua_State *L)
{
	ELI_STREAM *stream;
	if (L == NULL) {
		stream = calloc(1, sizeof(ELI_STREAM));
	} else {
		stream = lua_newuserdatauv(L, sizeof(ELI_STREAM), 1);
		memset(stream, 0, sizeof(ELI_STREAM));
	}
	stream->fd = STREAM_FD_DEFAULT;
	return stream;
}

int eli_stream_close(ELI_STREAM *stream)
{
	if (stream->closed) {
		return 1;
	}
	stream->closed = 1;
	if (!stream->not_disposable) {
#ifdef _WIN32
		if (stream->overlapped_buffer != NULL) {
			free(stream->overlapped_buffer);
			stream->overlapped_buffer = NULL;
		}
		if (stream->fd != STREAM_FD_DEFAULT) {
			if (stream->overlapped_pending) {
				CancelIo(stream->fd);
			}
			BOOL ok = CloseHandle(stream->fd);
			stream->fd = STREAM_FD_DEFAULT;
			if (!ok) {
				return 0;
			}
		}
#else
		if (stream->fd != STREAM_FD_DEFAULT) {
			int result = close(stream->fd);
			stream->fd = STREAM_FD_DEFAULT;
			if (result == -1) {
				return 0;
			}
		}
#endif
	}
	return 1;
}