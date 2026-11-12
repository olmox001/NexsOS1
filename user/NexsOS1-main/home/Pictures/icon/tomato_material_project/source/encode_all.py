import os, shutil, subprocess, sys, struct
import numpy as np
from PIL import Image
SETS = {  # nome: (cartella frame, lato)
 'tomato_material_v2':('fcol',576),'tomato_material_gray':('fgray',576),
 'tomato_material_v2_128':('t128c',128),'tomato_material_gray_128':('t128g',128),
 'tomato_material_v2_64':('s_col',64),'tomato_material_gray_64':('s_gray',64),
 'tomato_material_v2_32':('t32c',32),'tomato_material_gray_32':('t32g',32),
 'tomato_material_v2_16':('t16c',16),'tomato_material_gray_16':('t16g',16)}
def enc(d, out, colors, dither):
    subprocess.run(['ffmpeg','-y','-v','error','-framerate','24','-i',f'{d}/f%03d.png','-vf',f'palettegen=reserve_transparent=1:stats_mode=full:max_colors={colors}','./pz.png'],check=True)
    subprocess.run(['ffmpeg','-y','-v','error','-framerate','24','-i',f'{d}/f%03d.png','-i','./pz.png','-lavfi',f'paletteuse=dither={dither}:alpha_threshold=96:diff_mode=rectangle','-loop','0',out],check=True)
def comp(im, bg=(128,128,128)):
    im = im.convert('RGBA'); b = Image.new('RGBA', im.size, bg+(255,)); b.alpha_composite(im); return np.array(b.convert('RGB')).astype(float)
def psnr(path, d):
    g = Image.open(path); ps = []
    for k in range(g.n_frames):
        g.seek(k); a = comp(g.copy()); s = comp(Image.open(f'{d}/f{k:03d}.png'))
        ps.append(10*np.log10(255**2/max(((a-s)**2).mean(),1e-9)))
    return float(np.mean(ps))
def patch_delays(path, n=72):
    """24 fps esatti in media: ritardi 4/5 centesimi (72 frame = 3.00 s)."""
    b = bytearray(open(path,'rb').read()); p = 13
    if b[10] & 0x80: p += 3 * (2 << (b[10] & 7))
    k = 0; extra = 12  # 12 frame da 5 cs + 60 da 4 cs = 300 cs
    pattern = [5 if (i*72//12) != ((i+1)*72//12 if False else -1) else 4 for i in range(n)]
    cs5 = set(int(i*n/extra) for i in range(extra))
    while p < len(b):
        t = b[p]
        if t == 0x3B: break
        if t == 0x21:
            lab = b[p+1]
            if lab == 0xF9:
                d = 5 if k in cs5 else 4; b[p+4:p+6] = struct.pack('<H', d); k += 1
            p += 2
            while b[p]: p += 1 + b[p]
            p += 1
        elif t == 0x2C:
            flags = b[p+9]; p += 10
            if flags & 0x80: p += 3 * (2 << (flags & 7))
            p += 1
            while b[p]: p += 1 + b[p]
            p += 1
        else: raise SystemExit('parse error')
    open(path,'wb').write(b); return k
os.makedirs("../output", exist_ok=True)
CANDS = [192,128,96,64,48,32,24,16]
for n,(d,side) in SETS.items():
    out = f'../output/{n}.gif'
    enc(d, './base.gif', 255, 'sierra2_4a'); base_sz = os.path.getsize('./base.gif'); base_ps = psnr('./base.gif', d)
    best = (base_sz, './base.gif', 255, 'sierra2_4a', base_ps)
    if side != 576:
        for c in CANDS:
            for dith in ('sierra2_4a','none'):
                t = f'./t_{c}_{dith}.gif'; enc(d, t, c, dith); sz = os.path.getsize(t)
                if sz < best[0]:
                    ps = psnr(t, d)
                    if ps >= base_ps - 0.35: best = (sz, t, c, dith, ps)
    shutil.copy(best[1], out); frames = patch_delays(out)
    g = Image.open(out)
    print(f'{n:28s} {side:4d}px  {base_sz//1024:5d} -> {best[0]//1024:5d} KB  colori {best[2]:3d} {best[3]:10s} PSNR {best[4]:.1f} (base {base_ps:.1f})  frames {g.n_frames} durata {sum(( (g.seek(i) or g.info["duration"]) for i in range(g.n_frames)))} ms', flush=True)
