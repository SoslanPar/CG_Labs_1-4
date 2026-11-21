"""Проверка ЛР4 по реальным кадрам, включая отдельные тени солнца и прожектора.
Запуск: python3 tools/check_lab4.py [build/lab4/lab4 или build/lab4/lab4.exe]
Для validation установите Vulkan validation layers (VK_LAYER_KHRONOS_validation).
"""
from pathlib import Path
import json
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EXE = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'build/lab4' / ('lab4.exe' if os.name == 'nt' else 'lab4')
OUTPUT = EXE.parent / 'checks'
OUTPUT.mkdir(exist_ok=True)

def read_ppm(path):
    with path.open('rb') as f:
        assert f.readline().strip() == b'P6'
        width, height = map(int, f.readline().split())
        assert f.readline().strip() == b'255'
        pixels = f.read()
    assert (width, height) == (1600, 900)
    assert len(pixels) == width * height * 3
    # Исключаем ImGui: сравниваем только изображение сцены.
    return b''.join(pixels[(y*width)*3:(y*width+1150)*3] for y in range(300, height))

cases = ['normal', 'sun-on', 'sun-off', 'sun-wide', 'sun-direction', 'sun-bias',
         'spot-on', 'spot-off', 'spot-move', 'spot-angle', 'spot-selected',
         'empty', 'vertical', 'stress']
images = {}
for case in cases:
    env = os.environ.copy()
    env.pop('LAB3_TEST_CASE', None)
    env.update(LAB3_SMOKE_FRAMES='16', LAB3_CAPTURE=str(OUTPUT / (case + '.ppm')),
               LAB4_TEST_CASE=case, VK_LOADER_DEBUG='layer')
    run = subprocess.run([str(EXE)], cwd=ROOT / 'tools', env=env, capture_output=True, timeout=45)
    log = (run.stdout + run.stderr).decode('utf-8', errors='replace')
    (OUTPUT / (case + '.log')).write_text(log, encoding='utf-8')
    assert run.returncode == 0, (case, run.returncode, log[-4000:])
    assert '[ERROR' not in log and 'VUID-' not in log, (case, log[-4000:])
    assert log.count('Texture loaded:') == 9, case
    # Loader debug подтверждает реальную загрузку слоя, а не просто отсутствие ошибок.
    validated = 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log
    if case == 'normal': validation_active = validated
    assert validated == validation_active
    images[case] = read_ppm(OUTPUT / (case + '.ppm'))
    print('PASS', case, flush=True)

metrics = {}
for source in ['sun', 'spot']:
    on, off = images[source+'-on'], images[source+'-off']
    darkened = sum(b > a + 8 for a, b in zip(on, off))
    assert darkened > 1000, (source, 'No visible shadow', darkened)
    metrics[source + '_shadow_darkened_channels'] = darkened
for variant, base in [('sun-wide','sun-on'), ('sun-direction','sun-on'), ('sun-bias','sun-on'),
                      ('spot-move','spot-on'), ('spot-angle','spot-on')]:
    changed = sum(abs(a-b) > 2 for a,b in zip(images[variant], images[base]))
    assert changed > 100, (variant, 'Parameter does not affect rendering', changed)
    metrics[variant + '_changed_channels'] = changed
# Один и тот же прожектор перенесён в индекс 1; индекс 0 имеет нулевую интенсивность.
assert images['spot-selected'] == images['spot-on'], 'Selected spotlight uses wrong shadow map'
report = dict(executable=str(EXE), cases=cases, validation_active=validation_active,
              validation_errors=0 if validation_active else None, metrics=metrics)
(OUTPUT / 'results.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
