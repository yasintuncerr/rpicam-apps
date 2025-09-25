rpicam_app_src += files([
    'circular_output.cpp',
    'file_output.cpp',
    'net_output.cpp',
    'uvc_output.cpp',
    'output.cpp',
])

output_headers = [
    'circular_output.hpp',
    'uvc_output.hpp',
    'file_output.hpp',
    'net_output.hpp',
    'output.hpp',
]

# Sadece temel bağımlılıklar - FFmpeg kaldırıldı
rpicam_app_dep += [exif_dep, jpeg_dep, tiff_dep, png_dep]

install_headers(files(output_headers), subdir: meson.project_name() / 'output')