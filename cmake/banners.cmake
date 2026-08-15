########################################################################################
# banners.cmake
#
# ASCII art banner for PPE

macro(print_header)
    message("")
    message("  ____  ____  _____ ")
    message(" |  _ \\|  _ \\| ____|")
    message(" | |_) | |_) |  _|  ")
    message(" |  __/|  __/| |___ ")
    message(" |_|   |_|   |_____|")
    message("")
    message(" Platform Performance Engineering -- tools and studies")
    message("")
endmacro()

macro(print_footer)
    print_header()
endmacro()
