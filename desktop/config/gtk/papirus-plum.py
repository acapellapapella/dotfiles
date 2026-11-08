#!/usr/bin/env python3
"""Build ~/.local/share/icons/Papirus-Dark-Plum: Papirus-Dark with violet folders.

Only the folder icons change (symlinks to Papirus' own violet variants), so the
system theme is untouched and package updates are picked up automatically.
Safe to re-run.
"""
import os, re, shutil

SRC = '/usr/share/icons/Papirus'
DARK_INDEX = '/usr/share/icons/Papirus-Dark/index.theme'
DST = os.path.expanduser('~/.local/share/icons/Papirus-Dark-Plum')

index = open(DARK_INDEX).read()
sections = dict(re.findall(r'^\[([^\]]+/places)\]\n(.*?)(?=^\[|\Z)', index, re.M | re.S))

shutil.rmtree(DST, ignore_errors=True)
dirs = []
for d in sorted(sections):
    places = os.path.join(SRC, d)
    if not os.path.isdir(places):
        continue
    made = 0
    for name in os.listdir(places):
        path = os.path.join(places, name)
        if not os.path.islink(path):
            continue
        target = os.readlink(path)
        if '-blue' not in target:
            continue
        violet = target.replace('-blue', '-violet')
        if os.path.exists(os.path.join(places, violet)):
            os.makedirs(os.path.join(DST, d), exist_ok=True)
            os.symlink(os.path.join(os.path.realpath(places), violet), os.path.join(DST, d, name))
            made += 1
    if made:
        dirs.append(d)

with open(os.path.join(DST, 'index.theme'), 'w') as f:
    f.write('[Icon Theme]\nName=Papirus-Dark-Plum\nComment=Papirus-Dark with violet folders\n')
    f.write('Inherits=Papirus-Dark,breeze-dark,hicolor\n')
    f.write('Directories=' + ','.join(dirs) + '\n\n')
    for d in dirs:
        f.write(f'[{d}]\n{sections[d].strip()}\n\n')
print(f'{DST}: {len(dirs)} directories')
