"""Оригинальные учебные текстуры. PNG создаются стандартной библиотекой Python.
Во время работы C++ читает готовые файлы через стороннюю библиотеку stb_image.
Повторная генерация: python tools/generate_assets.py. Сторонних рисунков нет.
"""
from pathlib import Path
import math
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1] / 'assets'
SIZE = 512

def write_png(name, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data) & 0xffffffff)
    raw = b''.join(b'\x00' + bytes(pixels[y * width * 3:(y + 1) * width * 3]) for y in range(height))
    image = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', width, height, 8, 2, 0, 0, 0))
    (ROOT / name).write_bytes(image + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))

def byte(value):
    return max(0, min(255, round(value)))

def generate():
    ROOT.mkdir(exist_ok=True)
    maps = {key: bytearray() for key in ['planet_albedo', 'planet_specular', 'circuit_albedo',
            'circuit_specular', 'circuit_emissive', 'tiles_albedo', 'tiles_specular', 'fabric_albedo']}
    for y in range(SIZE):
        v = y / SIZE
        phi = v * math.pi
        for x in range(SIZE):
            u = x / SIZE
            theta = u * math.tau
            sx, sy, sz = math.sin(phi)*math.cos(theta), math.cos(phi), math.sin(phi)*math.sin(theta)
            noise = (math.sin(4*sx + 2*sz) + math.sin(6*sz - 3*sy) * .55 + math.cos(9*sy + 5*sx)*.3)
            land = noise > .28
            ice = abs(sy) > .94
            albedo = (195, 218, 225) if ice else ((65 + 30*noise, 115 + 30*noise, 56) if land else (13, 48 + 18*noise, 105 + 20*noise))
            spec = 30 if ice or land else 235
            maps['planet_albedo'].extend(byte(c) for c in albedo)
            maps['planet_specular'].extend((spec, spec, spec))
            # Платы: тёмные панели, медные дорожки и отдельная маска свечения.
            cx, cy = x % 64, y % 64
            border = cx < 3 or cy < 3
            trace = (abs(cx-16) < 2 and cy < 48) or (abs(cy-48) < 2 and cx >= 16)
            node = (cx-16)**2 + (cy-16)**2 < 24
            glow = trace or node
            maps['circuit_albedo'].extend((35, 190, 210) if glow else ((8, 12, 19) if border else (40, 56, 76)))
            s = 230 if glow else (15 if border else 100)
            maps['circuit_specular'].extend((s, s, s))
            maps['circuit_emissive'].extend((18, 180, 245) if glow else (0, 0, 0))
            tx, ty = x % 64, y % 64
            grout = tx < 3 or ty < 3
            checker = (x // 64 + y // 64) % 2
            maps['tiles_albedo'].extend((28, 33, 44) if grout else ((135, 145, 155) if checker else (65, 77, 95)))
            s = 0 if grout else (180 if checker else 60)
            maps['tiles_specular'].extend((s, s, s))
            weave = 0.8 + 0.2 * math.sin(x * math.pi/4) * math.sin(y * math.pi/4)
            stripe = 1.0 if (x//32 + y//32)%2 else 0.7
            maps['fabric_albedo'].extend(byte(c * weave * stripe) for c in (225, 110, 54))
    for name, pixels in maps.items():
        write_png(name + '.png', SIZE, SIZE, pixels)
    write_png('black.png', 1, 1, [0, 0, 0])
    print('Generated', len(maps) + 1, 'PNG textures in', ROOT)

if __name__ == '__main__':
    generate()
