"""Обновить единый файл для нового чата без сторонних Python-зависимостей.
Ручное объяснение до GENERATED APPENDIX сохраняется; снимки после маркера заменяются.
"""
from pathlib import Path
import hashlib
import re

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'LAB3_CONTEXT_FOR_NEW_CHAT.md'
MARKER = '<!-- GENERATED APPENDIX -->'
base = OUT.read_text(encoding='utf-8').split(MARKER)[0].rstrip()
parts = [base, MARKER, '# Приложения: текущая документация и полный код']
for name in ['README.md', 'docs/LAB2_DEFENSE.md', 'docs/LAB3_DEFENSE.md', 'docs/LAB2_REQUIREMENTS.md', 'docs/REQUIREMENTS.txt', 'third_party/README.md']:
    parts += ['## DOCUMENT: '+name, (ROOT/name).read_text(encoding='utf-8').rstrip()]
parts += ['## Зафиксированные результаты проверок: не новая проверка при экспорте']
lab2_log = ROOT/'build/lab2/smoke.log'
if lab2_log.exists():
    log = lab2_log.read_text(encoding='utf-8-sig', errors='replace')
    # PowerShell мог сохранить UTF-16: распознаём BOM вместо искажения журнала.
    if lab2_log.read_bytes().startswith((b'\xff\xfe', b'\xfe\xff')):
        log = lab2_log.read_text(encoding='utf-16')
    parts += ['### ЛР2: сохранённый журнал запуска',
              'Журнал build/lab2/smoke.log: ' + ('обнаружены сообщения ERROR/VUID.' if '[ERROR' in log or 'VUID-' in log else 'сообщений ERROR/VUID не найдено.') +
              ' Это журнал ранее выполненного запуска, а не проверка всех действий интерфейса при экспорте.']
for variant in ['lab3']:
    report = ROOT/'build'/variant/'checks/results.json'
    if report.exists():
        parts += ['### '+variant, '```json\n'+report.read_text(encoding='utf-8').strip()+'\n```']
    else:
        parts += ['Отчёт '+variant+' отсутствует в момент экспорта; успешный тест не утверждается.']

files = sorted([p.relative_to(ROOT).as_posix() for folder in ['source','shaders','tools']
                for p in (ROOT/folder).iterdir() if p.suffix in ['.cpp','.hpp','.vert','.frag','.py']])
files += ['CMakeLists.txt','CMakePresets.json','.gitignore','.gitattributes']
parts += ['## Карта файлов и контрольные суммы']
table=['| Файл | Строк | SHA-256 |','|---|---:|---|']
for name in files:
    p=ROOT/name
    table.append(f'| {name} | {len(p.read_text(encoding="utf-8-sig").splitlines())} | {hashlib.sha256(p.read_bytes()).hexdigest()} |')
parts += ['\n'.join(table)]
parts += ['## Индекс функций и структур с исходными номерами строк']
table=['| Файл | Строка | Объявление |','|---|---:|---|']
for name in files:
    if not name.endswith(('.cpp','.hpp','.vert','.frag','.py')): continue
    for i,line in enumerate((ROOT/name).read_text(encoding='utf-8-sig').splitlines(),1):
        if re.match(r'^(?:bool|void|int|Mat4|FrameData|VkFormat|VkShaderModule|float|vec3|std::vector<[^>]+>)\s+\w+\s*\(',line) or re.match(r'^(struct |def )',line):
            table.append(f'| {name} | {i} | {line.strip().replace("|", "&#124;")} |')
parts += ['\n'.join(table)]
parts += ['## Файлы текстур и сторонний декодер','PNG не встроены бинарными данными. Для их воспроизведения в полном виде ниже включён tools/generate_assets.py.']
table=['| Ресурс | Байт | SHA-256 |','|---|---:|---|']
for p in sorted((ROOT/'assets').glob('*.png')) + [ROOT/'third_party/stb_image.h']:
    table.append(f'| {p.relative_to(ROOT).as_posix()} | {p.stat().st_size} | {hashlib.sha256(p.read_bytes()).hexdigest()} |')
parts += ['\n'.join(table), '## Полные собственные исходники']
for name in files:
    lines=(ROOT/name).read_text(encoding='utf-8-sig').splitlines()
    parts += ['### SNAPSHOT: '+name, '````text\n'+'\n'.join(f'{i:04d} |'+(' '+line if line else '') for i,line in enumerate(lines,1))+'\n````']
result='\n\n'.join(parts)+'\n\nКонец самостоятельного контекста ЛР2 и ЛР3.\n'
for name in files:
    block=result.split('### SNAPSHOT: '+name+'\n',1)[1].split('````text\n',1)[1].split('\n````',1)[0]
    assert [line.split(' |',1)[1][1:] for line in block.splitlines()] == (ROOT/name).read_text(encoding='utf-8-sig').splitlines(), name
assert '\ufffd' not in result
OUT.write_text(result,encoding='utf-8',newline='\n')
print('Exported',len(files),'source/configuration files;',len(result.splitlines()),'lines;',OUT.stat().st_size,'bytes')
print('Verified: source blocks match current files line by line')
