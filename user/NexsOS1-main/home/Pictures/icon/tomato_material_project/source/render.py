"""Turntable material-style del pomodoro: z-buffer numpy + SSAA + contorno crema/grigio.
Uso: python3 -I render.py <obj> <outdir> [frames] [size] [ss]"""
import sys, os, math
import numpy as np
from scipy import ndimage as ndi
from PIL import Image

obj, outdir = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 60
S = int(sys.argv[4]) if len(sys.argv) > 4 else 512
SS = int(sys.argv[5]) if len(sys.argv) > 5 else 3
os.makedirs(outdir, exist_ok=True)

# ---------- OBJ ----------
V, VN, F, M = [], [], [], []
mats = {}; cur = 0
for l in open(obj):
    p = l.split()
    if not p: continue
    if p[0] == 'v': V.append([float(x) for x in p[1:4]])
    elif p[0] == 'vn': VN.append([float(x) for x in p[1:4]])
    elif p[0] == 'usemtl':
        cur = mats.setdefault(p[1], len(mats))
    elif p[0] == 'f':
        idx = [tuple(int(i) if i else 0 for i in t.split('/')) for t in p[1:]]
        for k in range(1, len(idx) - 1):
            tri = [idx[0], idx[k], idx[k + 1]]
            F.append(tri); M.append(cur)
V = np.array(V); VN = -np.array(VN); M = np.array(M)   # le normali del .obj puntano verso l interno
Fv = np.array([[t[0] - 1 for t in tri] for tri in F])
Fn = np.array([[t[2] - 1 for t in tri] for tri in F])
names = {v: k for k, v in mats.items()}
print('tris', len(Fv), 'mats', mats)

# ---------- palette material (rosso spento, verdi morbidi) ----------
def hx(s): return np.array([int(s[i:i + 2], 16) for i in (1, 3, 5)], float) / 255
# ombra, base, luce  per materiale
RAMP = {
    'body':  [hx('#6E2230'), hx('#B8403A'), hx('#E2705A')],
    'calyx': [hx("#2B4F36"), hx("#528B40"), hx("#8DB968")],
    'stem':  [hx('#4E6B2E'), hx('#86A24A'), hx('#BBCB7C')],
}
CREAM = hx('#E0DACD')     # contorno esterno
GREY = hx('#BDB7AA')      # linee interne

import os
if os.environ.get('GRAY'):
    RAMP = {
        'body':  [hx('#8E8B86'), hx('#B2AEA7'), hx('#D6D2CA')],
        'calyx': [hx('#5F5D59'), hx('#827F79'), hx('#A8A49D')],
        'stem':  [hx('#6A6823'.replace('6A6823','6A6762')), hx('#8D8982'), hx('#B3AFA7')],
    }

def ramp(t, cols):
    t = np.clip(t, 0, 1)[..., None]
    lo, mid, hi = cols
    a = np.clip(t * 2, 0, 1); b = np.clip(t * 2 - 1, 0, 1)
    c = lo + (mid - lo) * a
    return c + (hi - mid) * b * (t > 0.5)

# ---------- camera ----------
elev = math.radians(14)
cy_obj = 0.22
dist = 6.0
focal = 1.0 / math.tan(math.radians(11))  # fov stretto = quasi ortografico

def view_rot(theta):
    c, s = math.cos(theta), math.sin(theta)
    Ry = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    ce, se = math.cos(elev), math.sin(elev)
    Rx = np.array([[1, 0, 0], [0, ce, -se], [0, se, ce]])   # camera sopra, guarda in giu
    return Rx @ Ry

def project(P, R):
    Q = (P - [0, cy_obj, 0]) @ R.T
    z = dist - Q[:, 2]
    x = Q[:, 0] * focal / z
    y = Q[:, 1] * focal / z
    return x, y, z, Q

# inquadratura fissa: estensione massima su tutti i giri
ext = 0
for k in range(N):
    x, y, z, _ = project(V, view_rot(2 * math.pi * k / N))
    ext = max(ext, np.abs(x).max(), np.abs(y).max())
OUTLINE_PX = float(os.environ.get("OPX", "0.012"))    # spessore contorno (frazione del lato)
margin = 1.0 + 2 * OUTLINE_PX + 0.035
scale_ndc = 1.0 / (ext * margin)

# luci in spazio camera
L_key = np.array([-0.55, 0.70, 0.45]); L_key /= np.linalg.norm(L_key)
L_fill = np.array([0.7, -0.1, 0.5]); L_fill /= np.linalg.norm(L_fill)
Vv = np.array([0, 0, 1.0])

def render(theta):
    W = S * SS
    R = view_rot(theta)
    x, y, z, Q = project(V, R)
    sx = (x * scale_ndc * 0.5 + 0.5) * W
    sy = (0.5 - y * scale_ndc * 0.5) * W
    Nrm = VN @ R.T
    depth = np.full((W, W), np.inf, np.float32)
    nbuf = np.zeros((W, W, 3), np.float32)
    mbuf = np.full((W, W), -1, np.int8)
    for t in range(len(Fv)):
        a, b, c = Fv[t]
        x0, y0, x1, y1, x2, y2 = sx[a], sy[a], sx[b], sy[b], sx[c], sy[c]
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if area <= 0: continue        # backface (winding del .obj verso l interno)
        xmin = max(int(math.floor(min(x0, x1, x2))), 0); xmax = min(int(math.ceil(max(x0, x1, x2))), W - 1)
        ymin = max(int(math.floor(min(y0, y1, y2))), 0); ymax = min(int(math.ceil(max(y0, y1, y2))), W - 1)
        if xmin > xmax or ymin > ymax: continue
        gx, gy = np.meshgrid(np.arange(xmin, xmax + 1) + 0.5, np.arange(ymin, ymax + 1) + 0.5)
        w0 = ((x1 - gx) * (y2 - gy) - (x2 - gx) * (y1 - gy)) / area
        w1 = ((x2 - gx) * (y0 - gy) - (x0 - gx) * (y2 - gy)) / area
        w2 = 1 - w0 - w1
        eps = -1.2e-2
        inside = (w0 >= eps) & (w1 >= eps) & (w2 >= eps)
        if not inside.any(): continue
        zz = (w0 * z[a] + w1 * z[b] + w2 * z[c]).astype(np.float32)
        sub = depth[ymin:ymax + 1, xmin:xmax + 1]
        ok = inside & (zz < sub)
        if not ok.any(): continue
        sub[ok] = zz[ok]
        na, nb, nc = Nrm[Fn[t]]
        nn = w0[..., None] * na + w1[..., None] * nb + w2[..., None] * nc
        nbuf[ymin:ymax + 1, xmin:xmax + 1][ok] = nn[ok]
        mbuf[ymin:ymax + 1, xmin:xmax + 1][ok] = M[t]
    return depth, nbuf, mbuf

def shade(nbuf, mbuf):
    n = nbuf / np.maximum(np.linalg.norm(nbuf, axis=-1, keepdims=True), 1e-6)
    ndl = (n @ L_key)
    wrap = np.clip((ndl + 0.35) / 1.35, 0, 1)            # half-lambert morbido
    fill = np.clip(n @ L_fill, 0, 1) * 0.18
    hemi = 0.5 + 0.5 * n[..., 1]                          # cielo/terra
    t = 0.12 + 0.70 * wrap ** 1.15 + fill + 0.10 * (hemi - 0.5)
    # blinn-phong largo e morbido
    Hh = L_key + Vv; Hh /= np.linalg.norm(Hh)
    spec = np.clip(n @ Hh, 0, 1) ** 38
    fres = (1 - np.clip(n[..., 2], 0, 1)) ** 3
    img = np.zeros(n.shape, np.float32)
    for mid, name in names.items():
        sel = mbuf == mid
        if not sel.any(): continue
        col = ramp(t[sel], RAMP[name])
        sp = {"body": 0.22, 'calyx': 0.10, 'stem': 0.14}[name]
        col = col + sp * spec[sel][:, None] * np.array([1.0, 0.96, 0.90])
        col = col + 0.06 * fres[sel][:, None] * np.array([1.0, 0.93, 0.85])  # velo caldo ai bordi
        img[sel] = col
    return np.clip(img, 0, 1)

def compose(depth, nbuf, mbuf):
    W = S * SS
    A = ndi.binary_closing(mbuf >= 0, iterations=2)
    col = shade(nbuf, mbuf)
    hole = A & (mbuf < 0)
    if hole.any():
        _, (iy, ix) = ndi.distance_transform_edt(mbuf < 0, return_indices=True)
        col = np.where(hole[..., None], col[iy, ix], col)
    # --- contorno esterno: distanza dalla sagoma (bordi tondi e lisci)
    d_out = ndi.distance_transform_edt(~A)
    r_out = OUTLINE_PX * W
    outer = np.clip(r_out + 0.5 - d_out, 0, 1)            # 1 dentro il raggio, rampa di 1px
    outer = np.where(A, 1.0, outer)
    # --- linee interne (cambio materiale / salto di profondita'), piu' sottili e morbide
    Dz = np.where(A, depth, 0).astype(np.float32)
    gz = np.maximum(np.abs(np.diff(Dz, axis=0, prepend=Dz[:1])), np.abs(np.diff(Dz, axis=1, prepend=Dz[:, :1])))
    mg = (np.abs(np.diff(mbuf, axis=0, prepend=mbuf[:1])) + np.abs(np.diff(mbuf, axis=1, prepend=mbuf[:, :1]))) > 0
    edge = A & ((gz > 0.09) | mg) & ndi.binary_erosion(A, iterations=7)
    d_in = ndi.distance_transform_edt(~edge)
    r_in = OUTLINE_PX * 0.42 * W
    inner = np.clip(r_in + 0.5 - d_in, 0, 1) * A * 0.92
    # composizione
    rgb = np.where(A[..., None], col, CREAM)
    rgb = rgb * (1 - inner[..., None]) + GREY * inner[..., None]
    alpha = outer
    # sfumatura leggera dei bordi (smussamento) prima del downsample
    alpha = ndi.gaussian_filter(alpha, 0.5 * SS)
    # premoltiplicato -> box downsample -> unpremultiply
    pm = rgb * alpha[..., None]
    pm = pm.reshape(S, SS, S, SS, 3).mean((1, 3))
    al = alpha.reshape(S, SS, S, SS).mean((1, 3))
    out = np.where(al[..., None] > 1e-4, pm / np.maximum(al[..., None], 1e-4), 0)
    out = np.dstack([np.clip(out, 0, 1), al])
    return (out * 255 + 0.5).astype(np.uint8)

for k in range(N):
    th = 2 * math.pi * k / N
    d, nb, mb = render(th)
    img = compose(d, nb, mb)
    Image.fromarray(img, 'RGBA').save(f'{outdir}/f{k:03d}.png', optimize=False, compress_level=3)
    print('frame', k, flush=True) if k % 10 == 0 else None
