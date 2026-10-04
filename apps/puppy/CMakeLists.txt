# The skeleton `fw new-app` copies. A display app and its main companion are
# ONE deliverable -- the main UF2 carries the display image inside it -- so
# they live in one folder and are declared by one file, display first.
add_executable(puppy_display display/main.c)
target_link_libraries(puppy_display fwog_display_bsp)
# stdio off UART0, linked at the bootloader's 0x10021000 app offset, and
# UF2/bin/map outputs. Every display app must call this.
fwog_display_app(puppy_display
    VERSION 001
    DESCRIPTION "Pixel puppy: bob, and jump plus beep plus LED ripple on BLUE")

add_executable(puppy_main main/main.c)
target_link_libraries(puppy_main fwog_main_bsp)
# Carries the display firmware inside this UF2. With no argument it embeds
# this folder's own display app. Must come after target_link_libraries: the
# generated source needs fwog_main_bsp's include path.
fwog_embed_display_image(puppy_main)
fwog_main_app(puppy_main
    VERSION 001
    DESCRIPTION "Carries the puppy display app. Radio proximity is not in this build.")
