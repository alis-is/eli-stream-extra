cmake_minimum_required (VERSION 2.6)
project (eli_stream_extra)

file(GLOB eli_stream_extra_sources ./src/**.c)
SET(eli_stream_extra ${eli_stream_extra_sources})

ADD_LIBRARY (eli_stream_extra ${eli_stream_extra})
TARGET_LINK_LIBRARIES (eli_stream_extra)