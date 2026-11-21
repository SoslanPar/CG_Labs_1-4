"""Интеграционная проверка Vulkan-рендера, включая изображения, а не только сборку.
Запуск: python tools/check_render.py [build/lab3/lab3.exe]
Нужны рабочие Vulkan-драйвер/окно. Результаты сохраняются в build/.../checks.
"""
from pathlib import Path
import json
import os
import subprocess
import sys
import generate_assets as png

ROOT = Path(__file__).resolve().parents[1]
EXE = (ROOT / (sys.argv[1] if len(sys.argv) > 1 else 'build/lab3/lab3.exe')).resolve()
OUTPUT = EXE.parent/'checks'
OUTPUT.mkdir(exist_ok=True)
png.ROOT = OUTPUT

def read_ppm(path):
    with path.open('rb') as f:
        assert f.readline().strip() == b'P6'
        width, height = map(int, f.readline().split())
        assert f.readline().strip() == b'255'
        pixels = f.read()
    assert len(pixels) == width*height*3
    return width, height, pixels

def roi(image, x0, y0, x1, y1):
    w, h, pixels = image
    assert (w,h) == (1600,900), 'ROI coordinates require the default window dimensions'
    return b''.join(pixels[(y*w+x0)*3:(y*w+x1)*3] for y in range(y0,y1))

images = {}
for case in ['normal', 'dark', 'black', 'effect0', 'effect2', 'specular', 'stress']:
    env = os.environ.copy()
    env.update(LAB3_SMOKE_FRAMES='12', LAB3_CAPTURE=str(OUTPUT/(case+'.ppm')))
    env.pop('LAB3_TEST_CASE', None)
    if case != 'normal': env['LAB3_TEST_CASE'] = case
    # Запускаем из каталога tools, проверяя независимость от working directory.
    run = subprocess.run([str(EXE)], cwd=ROOT/'tools', env=env, capture_output=True, timeout=40)
    log = (run.stdout + run.stderr).decode('utf-8', errors='replace')
    (OUTPUT/(case+'.log')).write_text(log, encoding='utf-8')
    assert run.returncode == 0, (case, run.returncode, log)
    assert '[ERROR' not in log and 'VUID-' not in log, (case, log)
    assert log.count('Texture loaded:') == 9, (case, log)
    images[case] = read_ppm(OUTPUT/(case+'.ppm'))
    w,h,pixels=images[case]
    png.write_png(case+'.png',w,h,pixels)
    print('PASS',case,flush=True)

# Центр второй сферы: без ламп circuit должен светиться, fabric с fallback — нет.
dark = roi(images['dark'],715,390,825,500)
black = roi(images['black'],715,390,825,500)
assert sum(dark) > 100000, 'Emissive is not visible with lights off'
assert max(black) == 0, 'Black fallback emits light despite zero illumination'
# Одинаковые геометрия/камера/свет, отличается только время push constant эффекта.
frame0 = roi(images['effect0'],240,300,520,590)
frame2 = roi(images['effect2'],240,300,520,590)
changed = sum(a != b for a,b in zip(frame0,frame2))
assert changed > 5000, 'Animated sampling does not change the rendered sphere'
# При чёрном альбедо остаётся только блик; у ткани с black specular его быть не должно.
spec_planet = roi(images['specular'],300,380,460,520)
spec_fabric = roi(images['specular'],715,390,825,500)
assert max(spec_planet) > 50, 'Specular map does not produce a highlight'
assert max(spec_fabric) == 0, 'Missing specular map did not use black fallback'
report={'executable':str(EXE),'cases':list(images),'validation_errors':0,
        'dark_emission_sum':sum(dark),'black_emission_max':max(black),
        'animated_changed_channels':changed,'specular_peak':max(spec_planet),
        'black_specular_max':max(spec_fabric)}
(OUTPUT/'results.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('PASS pixel assertions; results:',OUTPUT/'results.json')
