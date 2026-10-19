# Emscripten supplies zlib as a port, including its headers at compile time.
set(ZLIB_FOUND TRUE)
set(ZLIB_VERSION_STRING "1.3.1")
set(ZLIB_LIBRARIES ZLIB::ZLIB)
if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
    set_property(TARGET ZLIB::ZLIB PROPERTY INTERFACE_COMPILE_OPTIONS "-sUSE_ZLIB=1")
    set_property(TARGET ZLIB::ZLIB PROPERTY INTERFACE_LINK_OPTIONS "-sUSE_ZLIB=1")
endif()
