#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lauxlib.h"
#include "lutil.h"
#include "stream.h"

#ifdef _WIN32
#define fdopen _fdopen
#define read _read
#define write _write
#define dup _dup
#define close _close
#define fileno _fileno
#define lseek _lseek
#endif

int stream_write(lua_State *L, int fd, const char *data, size_t datasize)
{
	size_t status = 1;
	status = status && (write(fd, data, datasize) == datasize);
	if (status) {
		lua_pushboolean(L, status);
		return 1;
	}
	return luaL_fileresult(L, status, NULL);
}

static int push_read_result(lua_State *L, int res, int nonblocking)
{
	if (res == -1) {
		char *errmsg = NULL;
		size_t _errno;

#ifdef _WIN32
		if (!nonblocking || (_errno = GetLastError()) != ERROR_NO_DATA)
#else
		if ((errno != EAGAIN && errno != EWOULDBLOCK) || !nonblocking)
#endif
		{
#ifdef _WIN32
			LPSTR messageBuffer = NULL;
			size_t size = FormatMessageA(
				FORMAT_MESSAGE_ALLOCATE_BUFFER |
					FORMAT_MESSAGE_FROM_SYSTEM |
					FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, _errno,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				(LPSTR)&messageBuffer, 0, NULL);
			errmsg = messageBuffer;
#else
			errmsg = strerror(errno);
			_errno = errno;
#endif
			if (lua_rawlen(L, -1) == 0) { // no data read
				lua_pushnil(L);
			}
			lua_pushstring(L, errmsg);
			lua_pushinteger(L, _errno);
			return 3;
		}
	}
	if (res == 0) {
		lua_pushnil(L); // EOF
	}
	return 1;
}

static int get_sleep_per_iteration(int timeout_ms)
{
	int sleep_per_iteration = timeout_ms / 10;
	if (sleep_per_iteration <= 0) {
		sleep_per_iteration = 1; // at least 1 ms
	}
	return sleep_per_iteration;
}

// return 1 if mode was changed, 0 if it was already nonblocking
static int as_nonblocking(lua_State *L, ELI_STREAM *stream)
{
	if (stream_is_nonblocking(stream->fd)) {
		return 0;
	}

	if (!stream_set_nonblocking(stream->fd, 1)) {
		return luaL_error(L, "failed to set nonblocking mode");
	}
	return 1;
}

static int restore_blocking_mode(lua_State *L, ELI_STREAM *stream)
{
	// NOTE: throwing error here is tricky, it would discard the data
	// so we just ignore the error here right now
	stream_set_nonblocking(stream->fd, stream->nonblocking);
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

static int read_pending_line(lua_State *L, int stream_index, luaL_Buffer *b)
{
	if (!lua_getiuservalue(L, stream_index, 1)) {
		return -1;
	}

	size_t pending_length = 0;
	const char *pending_data = lua_tolstring(L, -1, &pending_length);
	if (pending_data == NULL || pending_length == 0) {
		return -1;
	}

	const char *newline = memchr(pending_data, '\n', pending_length);
	if (newline != NULL) {
		size_t line_length = newline - pending_data;
		memcpy(luaL_prepbuffsize(b, line_length), pending_data,
		       line_length);
		lua_remove(L, -1);
		lua_pushlstring(L, newline + 1,
				pending_length - line_length - 1);
		lua_setiuservalue(L, stream_index, 1); // store remaining data
		luaL_addsize(b, line_length);
		return line_length;
	}

	memcpy(luaL_prepbuffsize(b, pending_length), pending_data,
	       pending_length);
	lua_remove(L, -1);
	lua_pushnil(L);
	lua_setiuservalue(L, stream_index, 1); // remove pending data
	luaL_addsize(b, pending_length);
	return -1; // no newline found
}

static int stream_read_line(lua_State *L, int stream_index, int chop,
			    int timeout_ms)
{
	size_t res;
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	int line_length = read_pending_line(L, stream_index, &b);
	if (line_length >= 0) {
		if (!chop) {
			luaL_addchar(&b, '\n');
		}
		luaL_pushresult(&b);
		return push_read_result(L, line_length + 1, 1);
	}

	int sleep_counter = 0;
	int sleep_per_iteration =
		timeout_ms == -1 ? 100 : get_sleep_per_iteration(timeout_ms);

	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, stream_index);
	as_nonblocking(L, stream);

	size_t total_read = 0;
	do {
		char *buff = luaL_prepbuffsize(&b, LUAL_BUFFERSIZE);
		res = read(stream->fd, buff, LUAL_BUFFERSIZE);
		if (res == -1) {
#ifdef _WIN32
			int isWouldBlockError = GetLastError() != ERROR_NO_DATA;
#else
			int isWouldBlockError = errno == EWOULDBLOCK ||
						errno == EAGAIN;
#endif
			if (isWouldBlockError) {
				sleep_counter += sleep_per_iteration;
				sleep_ms(sleep_per_iteration);
				continue;
			}
			return push_read_result(L, res, 1);
		}
		char *new_line = memchr(buff, '\n', res);
		if (new_line != NULL) {
			// line_length without '\n'
			size_t line_length = new_line - buff;
			luaL_addsize(&b, line_length);
			total_read += line_length;
			// the rest of the data without '\n'
			add_pending_data(L, stream_index, new_line + 1,
					 res - line_length - 1);
			break;
		}
		luaL_addsize(&b, res);
		total_read += res;
		// if not end of stream and not timeout
	} while (res != 0 && (sleep_counter <= timeout_ms || timeout_ms == -1));

	if (!chop) {
		luaL_addchar(&b, '\n');
	}
	luaL_pushresult(&b);
	restore_blocking_mode(L, stream);
	return push_read_result(L, total_read > 0 ? total_read : res,
				timeout_ms >= 0);
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

static int stream_read_all(lua_State *L, int stream_index, int timeout_ms)
{
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	read_all_pending_data(L, stream_index, &b);

	size_t res;
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, stream_index);
	as_nonblocking(L, stream);

	int sleep_counter = 0;
	int sleep_per_iteration =
		timeout_ms == -1 ? 100 : get_sleep_per_iteration(timeout_ms);
	size_t total_read = 0;
	do {
		char *p = luaL_prepbuffsize(&b, LUAL_BUFFERSIZE);
		res = read(stream->fd, p, LUAL_BUFFERSIZE);
		if (res == -1) { // read some data
#ifdef _WIN32
			int isWouldBlockError = GetLastError() != ERROR_NO_DATA;
#else
			int isWouldBlockError = errno == EWOULDBLOCK ||
						errno == EAGAIN;
#endif
			if (isWouldBlockError) {
				sleep_counter += sleep_per_iteration;
				sleep_ms(sleep_per_iteration);
				continue;
			}
			break;
		}
		luaL_addsize(&b, res);
		total_read += res;
		// if not end of stream and not timeout
	} while (res != 0 && (sleep_counter <= timeout_ms || timeout_ms == -1));

	luaL_pushresult(&b);
	restore_blocking_mode(L, stream);
	return push_read_result(L, total_read > 0 ? total_read : res,
				timeout_ms >= 0);
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
		return push_read_result(L, cached, 1);
	}
	length -= cached;

	size_t res;
	ELI_STREAM *stream = (ELI_STREAM *)lua_touserdata(L, stream_index);
	as_nonblocking(L, stream);

	int sleep_counter = 0;
	int sleep_per_iteration =
		timeout_ms == -1 ? 100 : get_sleep_per_iteration(timeout_ms);

	char *p = luaL_prepbuffsize(&b, length);
	size_t total_read = 0;
	do {
		res = read(stream->fd, p + total_read, length - total_read);
		if (res == -1) { // read some data
#ifdef _WIN32
			int isWouldBlockError = GetLastError() != ERROR_NO_DATA;
#else
			int isWouldBlockError = errno == EWOULDBLOCK ||
						errno == EAGAIN;
#endif
			if (isWouldBlockError) {
				sleep_counter += sleep_per_iteration;
				sleep_ms(sleep_per_iteration);
				continue;
			}
			break;
		}
		luaL_addsize(&b, res);
		total_read += res;
		if (total_read == length) {
			break;
		}
		// if not end of stream and not timeout
	} while (res != 0 && (sleep_counter <= timeout_ms || timeout_ms == -1));
	luaL_pushresult(&b);
	restore_blocking_mode(L, stream);
	return push_read_result(L, total_read > 0 ? total_read : res,
				timeout_ms >= 0);
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

int stream_is_nonblocking(int fd)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}
	if (GetFileType(h) == FILE_TYPE_PIPE) {
		DWORD state;
		if (!GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL,
					     0)) {
			return -1;
		}

		return (state & PIPE_NOWAIT) != 0;
	}
	errno = ENOTSUP;
	return -1;
#else
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return -1;
	}
	return (flags & O_NONBLOCK) != 0;
#endif
}

int stream_set_nonblocking(int fd, int nonblocking)
{
#ifdef _WIN32
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return 0;
	}
	if (GetFileType(h) == FILE_TYPE_PIPE) {
		DWORD state;
		if (GetNamedPipeHandleState(h, &state, NULL, NULL, NULL, NULL,
					    0)) {
			if (((state & PIPE_NOWAIT) != 0) == nonblocking) {
				return 1;
			}

			if (nonblocking) {
				state &= ~PIPE_NOWAIT;
			} else {
				state |= PIPE_NOWAIT;
			}
			if (SetNamedPipeHandleState(h, &state, NULL, NULL)) {
				return 1;
			}
			errno = EINVAL;
			return 0;
		}
	}
	errno = ENOTSUP;
	return 0;
#else
	if (fd < 0) {
		errno = EBADF;
		return 0;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		return 0;
	}
	if (((flags & O_NONBLOCK) != 0) != nonblocking) {
		if (nonblocking) {
			flags |= O_NONBLOCK;
		} else {
			flags &= ~O_NONBLOCK;
		}
		int res = fcntl(fd, F_SETFL, flags);
		if (res == -1) {
			return 0;
		}
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

int stream_as_filestream(lua_State *L, int fd, const char *mode)
{
	FILE *res = fdopen(dup(fd), mode);
	if (res == NULL) {
		return -1;
	}
	luaL_Stream *p =
		(luaL_Stream *)lua_newuserdatauv(L, sizeof(luaL_Stream), 1);
	luaL_setmetatable(L, LUA_FILEHANDLE);
	p->f = res;
	p->closef = &io_fclose;
	return 1;
}

int stream_close(int fd)
{
	return close(fd) == 0;
}

int is_fd_readable(int fd)
{
	char buffer[1]; // Buffer is not used, since we're reading 0 bytes
	// Attempt to read 0 bytes from the file descriptor
	ssize_t result = read(fd, buffer, 0);
	if (result == -1) {
		if (errno == EBADF || errno == EACCES) {
			// File descriptor is not valid for reading or permission denied
			return 0;
		}
	}
	return 1;
}

int is_fd_writable(int fd)
{
	char buffer[1]; // Buffer is not used, since we're writing 0 bytes
	// Attempt to write 0 bytes to the file descriptor
	ssize_t result = write(fd, buffer, 0);
	if (result == -1) {
		if (errno == EBADF || errno == EACCES) {
			// File descriptor is not valid for writing or permission denied
			return 0;
		}
	}
	return 1;
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
	stream->fd = -1;
	return stream;
}