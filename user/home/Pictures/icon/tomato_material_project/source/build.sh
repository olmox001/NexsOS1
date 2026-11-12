#!/bin/sh
# Rigenera frame, GIF e font. Richiede python3 (numpy, scipy, pillow, opencv, fontTools) e ffmpeg.
set -e
cd "$(dirname "$0")"
python3 -I render.py tomato.obj fcol 72 576 3
GRAY=1 python3 -I render.py tomato.obj fgray 72 576 3
OPX=0.016 python3 -I render.py tomato.obj t128c 72 128 6
OPX=0.016 GRAY=1 python3 -I render.py tomato.obj t128g 72 128 6
OPX=0.022 python3 -I render.py tomato.obj s_col 72 64 8
OPX=0.022 GRAY=1 python3 -I render.py tomato.obj s_gray 72 64 8
OPX=0.026 python3 -I render.py tomato.obj t32c 72 32 12
OPX=0.026 GRAY=1 python3 -I render.py tomato.obj t32g 72 32 12
OPX=0.03 python3 -I render.py tomato.obj t16c 72 16 16
OPX=0.03 GRAY=1 python3 -I render.py tomato.obj t16g 72 16 16
python3 -I encode_all.py     # palette ottimizzata per dimensione, 24 fps (ritardi 4/5 cs, 3,00 s a giro)
mkdir -p ../font && python3 -I mkfont.py fcol/f018.png ../font
