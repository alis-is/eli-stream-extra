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

int stream_win_read_pipe(ELI_STREAM *stream, char *buffer, size_t size)
{
	DWORD bytes_available;
	// peek at the pipe to see if there is data
	if (!PeekNamedPipe(stream->fd, NULL, 0, NULL, &bytes_available, NULL)) {
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
	size_t to_read = bytes_available > size ? size : bytes_available;
	DWORD bytes_read = 0;
	if (!ReadFile(stream->fd, buffer, to_read, &bytes_read, NULL)) {
		int error_code = GetLastError();
		if (error_code != ERROR_BROKEN_PIPE) {
			return -1;
		}
	}
	return bytes_read;
}

int finish_overlap_read(ELI_STREAM *stream, char *buffer, size_t size)
{
	memcpy(buffer, stream->overlapped_buffer, size);
	stream->overlapped_pending = 0;
	offset_add(&stream->overlapped, size);
	return size;
}

int process_failed_overlap_result(ELI_STREAM *stream)
{
	DWORD error = GetLastError();
	switch (error) {
	case ERROR_IO_INCOMPLETE:
	case ERROR_IO_PENDING:
		SetLastError(ERROR_NO_DATA);
		return -1;
	case ERROR_HANDLE_EOF:
		return 0; // EOF
	case ERROR_INVALID_PARAMETER:
		if (is_offset_beyond_eof(stream->fd, &stream->overlapped)) {
			return 0; // EOF
		}
		return -1;
	default:
		return -1;
	}
}

int stream_win_read(ELI_STREAM *stream, char *buffer, size_t size)
{
	// if handle is a pipe
	if (stream->fd == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}

	if (GetFileType(stream->fd) == FILE_TYPE_PIPE) {
		return stream_win_read_pipe(stream, buffer, size);
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
			return finish_overlap_read(stream, buffer, bytes_read);
		} else {
			return process_failed_overlap_result(stream);
		}
	}

	size_t to_read = size > stream->overlapped_buffer_size ?
				 stream->overlapped_buffer_size :
				 size;
	BOOL read_result = ReadFile(stream->fd, stream->overlapped_buffer,
				    (DWORD)to_read, &bytes_read,
				    &stream->overlapped);
	if (read_result) { // ReadFile completed immediately
		return finish_overlap_read(stream, buffer, bytes_read);
	}

	DWORD error = GetLastError();
	switch (error) {
	case ERROR_IO_PENDING:
		stream->overlapped_pending = 1;
		if (GetOverlappedResult(stream->fd, &stream->overlapped,
					&bytes_read, FALSE)) {
			return finish_overlap_read(stream, buffer, bytes_read);
		} else {
			return process_failed_overlap_result(stream);
		}
	case ERROR_HANDLE_EOF:
		return 0; // EOF
	case ERROR_INVALID_PARAMETER:
		if (is_offset_beyond_eof(stream->fd, &stream->overlapped)) {
			return 0; // EOF
		}
		return -1;
	default:
		return -1;
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

		if (error != ERROR_IO_PENDING ||
		    !GetOverlappedResult(stream->fd, &overlapped, &bytesWritten,
					 TRUE)) {
			return -1;
		}
	}
	return bytesWritten;
}

#endif