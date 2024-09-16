#ifdef _WIN32

#include <windows.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "stream.h"

BOOL is_offset_beyond_eof(HANDLE hFile, OVERLAPPED *overlapped)
{
	LARGE_INTEGER fileSize;

	// Get the file size (64-bit value)
	if (!GetFileSizeEx(hFile, &fileSize)) {
		// Handle error here
		return FALSE;
	}

	// Combine Offset and OffsetHigh into a 64-bit value
	ULONGLONG offset = ((ULONGLONG)overlapped->OffsetHigh << 32) |
			   overlapped->Offset;

	// Compare the offset with the file size
	if (offset >= (ULONGLONG)fileSize.QuadPart) {
		// Offset is beyond EOF
		return TRUE;
	}

	// Offset is valid and within the file
	return FALSE;
}

void offset_add(OVERLAPPED *overlapped, DWORD length)
{
	ULONGLONG offset = ((ULONGLONG)overlapped->OffsetHigh << 32) |
			   overlapped->Offset;
	offset += length;
	overlapped->Offset = (DWORD)offset;
	overlapped->OffsetHigh = (DWORD)(offset >> 32);
}
int stream_win_read(ELI_STREAM *stream, char *buffer, size_t size)
{
	// if handle is a pipe
	if (stream->fd == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}

	if (GetFileType(stream->fd) == FILE_TYPE_PIPE) {
		DWORD bytes_available;
		// peek at the pipe to see if there is data
		if (!PeekNamedPipe(stream->fd, NULL, 0, NULL, &bytes_available,
				   NULL)) {
			int error_code = GetLastError();
			if (error_code != ERROR_BROKEN_PIPE) {
				return -1;
			}
			if (bytes_available == 0) {
				return 0;
			}
		}
		if (bytes_available == 0) {
			SetLastError(ERROR_NO_DATA);
			return -1;
		}

		size_t to_read = bytes_available > size ? size :
							  bytes_available;
		DWORD bytes_read = 0;
		if (!ReadFile(stream->fd, buffer, to_read, &bytes_read, NULL)) {
			int error_code = GetLastError();
			if (error_code != ERROR_BROKEN_PIPE) {
				return -1;
			}
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

	if (stream->overlapped_pending) {
		if (GetOverlappedResult(stream->fd, &stream->overlapped,
					&bytes_read, FALSE)) {
			memcpy(buffer, stream->overlapped_buffer, bytes_read);
			stream->overlapped_pending = 0;
			offset_add(&stream->overlapped, bytes_read);
			return bytes_read;
		} else {
			DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING) {
				SetLastError(ERROR_NO_DATA);
				return -1;
			} else if (error == ERROR_INVALID_PARAMETER &&
				   is_offset_beyond_eof(stream->fd,
							&stream->overlapped)) {
				return 0; // EOF
			} else {
				return -1;
			}
		}
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
		offset_add(&stream->overlapped, bytes_read);
		return (ssize_t)bytes_read;
	} else {
		stream->overlapped_pending = 1;
		DWORD error = GetLastError();
		if (error == ERROR_IO_PENDING) {
			// The operation is pending; check its status without waiting
			BOOL overlapped_result = GetOverlappedResult(
				stream->fd, &stream->overlapped, &bytes_read,
				FALSE);
			if (overlapped_result) {
				memcpy(buffer, stream->overlapped_buffer,
				       bytes_read);
				stream->overlapped_pending = 0;
				offset_add(&stream->overlapped, bytes_read);
				return (ssize_t)bytes_read;
			} else {
				DWORD overlapped_error = GetLastError();
				if (overlapped_error == ERROR_IO_INCOMPLETE) {
					// The read operation is still pending; no data available right now
					SetLastError(ERROR_NO_DATA);
					return -1;
				} else if (overlapped_error ==
						   ERROR_INVALID_PARAMETER &&
					   is_offset_beyond_eof(
						   stream->fd,
						   &stream->overlapped)) {
					return 0; // EOF
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