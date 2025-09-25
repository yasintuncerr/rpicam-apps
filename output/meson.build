rpicam_app_src += files([
    'circular_output.cpp',
    'file_output.cpp',
    'net_output.cpp',
    'uvc_output.cpp,
    'output.cpp',
])

output_headers = [
    'circular_output.hpp',
    'uvc_output.hpp',
    'file_output.hpp',
    'net_output.hpp',
    'output.hpp',
]

# FFmpeg bağımlılıkları için (UVCOutput transcoding için gerekli)
avcodec_dep = dependency('libavcodec', required: false)
avformat_dep = dependency('libavformat', required: false) 
avutil_dep = dependency('libavutil', required: false)
swscale_dep = dependency('libswscale', required: false)

rpicam_app_dep += [exif_dep, jpeg_dep, tiff_dep, png_dep, avcodec_dep, avformat_dep, avutil_dep, swscale_dep]

install_headers(files(output_headers), subdir: meson.project_name() / 'output')
