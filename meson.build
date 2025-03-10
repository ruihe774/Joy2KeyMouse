project('Joy2KeyMouse', 'c',
        version : '0.1.0',
        default_options : ['warning_level=3', 'c_std=gnu23'])

executable('joy2keymouse', 'joy2keymouse.c', install : true, dependencies : [
           dependency('libevdev', version : '>=1.0.0'),
           meson.get_compiler('c').find_library('m', required : false)])
