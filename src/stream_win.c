#ifdef _WIN32

#include <windows.h>
#include <errno.h>
#include <string.h>
#include "stream.h"

int stream_win_read(ELI_STREAM *stream, char *buffer, size_t size)
{
	// if handle is a pipe
	if (stream->fd == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}

	if (GetFileType(stream->fd) == FILE_TYPE_PIPE) {
		DWORD bytes_read;
		// peek at the pipe to see if there is data
		if (!PeekNamedPipe(stream->fd, NULL, 0, NULL, &bytes_read,
				   NULL)) {
			return -1;
		}
		bytes_read = bytes_read > size ? size : bytes_read;
		if (!ReadFile(stream->fd, buffer, bytes_read, &bytes_read,
			      NULL)) {
			return -1;
		}
		return bytes_read;
	}

	if (!stream->use_overlapped) {
		DWORD bytes_read;
		if (!ReadFile(stream->fd, buffer, size, &bytes_read, NULL)) {
			return -1;
		}
		return bytes_read;
	}

	DWORD bytes_read = 0;

	if (GetOverlappedResult(stream->fd, &stream->overlapped, &bytes_read,
				FALSE)) {
		memcpy(buffer, stream->overlapped_buffer, bytes_read);
		return bytes_read;
	}

	switch (GetLastError()) {
	case ERROR_IO_INCOMPLETE:
		errno = EAGAIN;
		return -1;
	case ERROR_HANDLE_EOF:
		memcpy(buffer, stream->overlapped_buffer, bytes_read);
		return bytes_read;
	case ERROR_INVALID_HANDLE:
		errno = EBADF;
		return -1;
	case ERROR_IO_PENDING:
		errno = EAGAIN;
		return -1;
	default:
		break;
	}

	size_t to_read = size > stream->overlapped_buffer_size ?
				 stream->overlapped_buffer_size :
				 size;
	BOOL read_result = ReadFile(stream->fd, stream->overlapped_buffer,
				    (DWORD)to_read, &bytes_read,
				    &stream->overlapped);

	if (read_result) { // ReadFile completed immediately
		memset(&stream->overlapped, 0,
		       sizeof(OVERLAPPED)); // Reset overlapped structure
		memcpy(buffer, stream->overlapped_buffer, bytes_read);
		return (ssize_t)bytes_read;
	} else {
		DWORD error = GetLastError();
		if (error == ERROR_IO_PENDING) {
			// The operation is pending; check its status without waiting
			BOOL overlapped_result = GetOverlappedResult(
				stream->fd, &stream->overlapped, &bytes_read,
				FALSE);
			if (overlapped_result) {
				memcpy(buffer, stream->overlapped_buffer,
				       bytes_read);
				return (ssize_t)bytes_read;
			} else {
				DWORD overlapped_error = GetLastError();
				if (overlapped_error == ERROR_IO_INCOMPLETE) {
					// The read operation is still pending; no data available right now
					errno = EAGAIN;
					return -1;
				} else {
					return -1;
				}
			}
		} else if (error == ERROR_HANDLE_EOF) {
			// Reached end of file
			return 0;
		} else {
			return -1;
		}
	}
}

int stream_win_write(ELI_STREAM *stream, const char *data, size_t size)
{
	if (GetFileType(stream->fd) == FILE_TYPE_PIPE) {
		DWORD bytes_written;
		if (!WriteFile(stream->fd, data, size, &bytes_written, NULL)) {
			return -1;
		}
		return bytes_written;
	}

	if (!stream->use_overlapped) {
		DWORD bytes_written;
		if (!WriteFile(stream->fd, data, size, &bytes_written, NULL)) {
			return -1;
		}
		return bytes_written;
	}

	OVERLAPPED overlapped = { 0 };

	DWORD bytesWritten = 0;
	BOOL writeResult =
		WriteFile(stream->fd, data, size, &bytesWritten, &overlapped);

	if (!writeResult) {
		DWORD error = GetLastError();
		if (error == ERROR_IO_PENDING) {
			// The write is pending, wait for the operation to complete
			if (!GetOverlappedResult(stream->fd, &overlapped,
						 &bytesWritten, TRUE)) {
				return -1;
			}
		} else {
			return -1;
		}
	} else {
		if (!GetOverlappedResult(stream->fd, &overlapped, &bytesWritten,
					 TRUE)) {
			return -1;
		}
	}

	return bytesWritten;
}

#endif