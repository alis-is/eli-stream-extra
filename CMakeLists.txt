project (eli_stream_extra)

file(GLOB eli_stream_extra_sources ./src/**.c)
set(eli_stream_extra ${eli_stream_extra_sources})

add_library(eli_stream_extra ${eli_stream_extra})
target_link_libraries(eli_stream_extra)