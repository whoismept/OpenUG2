#!/usr/bin/env python3
"""DIAGNOSTIC BRANCH ONLY: adapt the fixed sandbox --shot invocation to the
existing authored UI_Main.fng viewer. This executable is not a gameplay build.
The sandbox's car_only/street_car output slots both contain the same original
main-menu frame; comparisons with gameplay goldens must differ. Never merge
this build-target substitution into development.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def bmp_to_png(source, destination, expected_size):
    data = pathlib.Path(source).read_bytes()
    if len(data) < 54 or data[:2] != b'BM':
        raise ValueError('capture is not a BMP')
    offset = struct.unpack_from('<I', data, 10)[0]
    header, width, height, planes, depth, compression = struct.unpack_from('<IiiHHI', data, 14)
    if header < 40 or planes != 1 or depth != 24 or compression != 0:
        raise ValueError('expected an uncompressed 24-bit SDL screenshot')
    if (width, abs(height)) != expected_size:
        raise ValueError('unexpected capture dimensions')
    stride = (width * 3 + 3) & ~3
    if offset < 54 or len(data) < offset + stride * abs(height):
        raise ValueError('truncated capture')
    rows = []
    for y in range(abs(height)):
        by = abs(height) - y - 1 if height > 0 else y
        bgr = data[offset + by * stride:offset + by * stride + width * 3]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
        rows.append(b'\0' + rgb)

    def chunk(tag, payload):
        return struct.pack('>I', len(payload)) + tag + payload + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, abs(height), 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
    pathlib.Path(destination).write_bytes(png)


def main(argv):
    data, shot, size = None, None, (640, 480)
    i = 0
    values = {'--car', '--track', '--heading', '--shot-yaw', '--shot-pitch', '--frames'}
    while i < len(argv):
        arg = argv[i]
        i += 1
        if arg in values | {'--path', '--shot', '--resolution'}:
            if i == len(argv):
                raise ValueError('missing option value')
            value = argv[i]
            i += 1
            if arg == '--path':
                data = value
            elif arg == '--shot':
                shot = value
            elif arg == '--resolution':
                parts = value.split('x')
                if len(parts) != 2:
                    raise ValueError('invalid resolution')
                size = tuple(map(int, parts))
        elif arg == '--shot-empty':
            pass  # Both fixed output slots capture the original main menu.
        elif not arg.startswith('-') and data is None:
            data = arg
        else:
            raise ValueError('diagnostic frontend capture: unsupported option ' + arg)
    if not data or not shot or not all(64 <= x <= 8192 for x in size):
        raise ValueError('diagnostic frontend capture requires DATA, --shot and a valid resolution')
    print('DIAGNOSTIC ONLY: capturing authored UI_Main.fng; this is not a gameplay frame.', flush=True)
    with tempfile.TemporaryDirectory(prefix='authored-frontend-') as tmp:
        subprocess.run(['./build/ugui_demo', '--data', data, '--screen', 'UI_Main.fng',
                        '--size', str(size[0]), str(size[1]), '--frames', '120',
                        '--capture', 'original-main', '--shots', tmp], check=True)
        bmp_to_png(pathlib.Path(tmp) / 'original-main.bmp', shot, size)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
