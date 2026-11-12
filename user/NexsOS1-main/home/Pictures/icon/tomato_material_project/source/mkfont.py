"""Glifo pomodoro (mono + COLR) con luci come spazi vuoti. Uso: mkfont.py frame.png outdir"""
import sys, numpy as np, cv2
from PIL import Image
from scipy import ndimage as ndi
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.colorLib.builder import buildCOLR, buildCPAL

src, outdir = sys.argv[1], sys.argv[2]
im = np.array(Image.open(src).convert('RGBA')).astype(float)
rgb, a = im[..., :3], im[..., 3]
CREAM = np.array([224, 218, 205.]); GREY = np.array([189, 183, 170.])
near = lambda c: np.linalg.norm(rgb - c, axis=-1) < 30
solid = a > 128
r, g = rgb[..., 0], rgb[..., 1]
fill = solid & ~near(CREAM) & ~near(GREY)
body0 = fill & (r > g + 20)
leaf = ndi.binary_opening(fill & (g >= r - 5), iterations=1)
body0 = ndi.binary_fill_holes(ndi.binary_opening(body0, iterations=1)) & ~leaf

def ell(n): return cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * n + 1, 2 * n + 1))
def close(m, n): return cv2.morphologyEx(m.astype(np.uint8), cv2.MORPH_CLOSE, ell(n), borderType=cv2.BORDER_CONSTANT, borderValue=0) > 0
# corpo completo: richiude la zona coperta da calice e foglie senza uscire dalla sagoma
pad = 60
bp = np.pad(body0, pad)
body = ndi.binary_fill_holes(close(bp, 34))[pad:-pad, pad:-pad]
body_full = body.copy()
# fessura sottile tra foglie e corpo (come il contorno chiaro del render)
gap = cv2.dilate(leaf.astype(np.uint8), ell(5)) > 0
body = body & ~gap

ys, xs = np.where(body)
cx, cy = xs.mean(), ys.mean()
R = (xs.max() - xs.min()) / 2
# --- giochi di luce: luce in alto a sinistra
L = np.array([-0.60, -0.80])
yy, xx = np.mgrid[0:body.shape[0], 0:body.shape[1]]
proj = ((xx - cx) * L[0] + (yy - cy) * L[1]) / R          # +1 = lato illuminato
dist = ndi.distance_transform_edt(body)

# riflesso speculare: zona piu' chiara del corpo (dal render), pulita
gs = cv2.GaussianBlur(g.astype(np.float32), (0, 0), 3)
bg_ = gs[body0]
spec = body0 & (gs > np.percentile(bg_, 93))
spec = ndi.binary_opening(spec, iterations=3)
lab, n = ndi.label(spec)
if n:
    sizes = ndi.sum(spec, lab, range(1, n + 1)); spec = lab == (1 + int(np.argmax(sizes)))
spec = cv2.dilate(spec.astype(np.uint8), ell(4)) > 0
# arco di luce di bordo: fascia continua lungo il lato illuminato, con estremi sfumati (niente segmenti)
dist_f = ndi.distance_transform_edt(body_full)
def sstep(x): x = np.clip(x, 0, 1); return x * x * (3 - 2 * x)
taper = sstep((proj - 0.30) / 0.40) * sstep((1.25 - proj) / 0.25)           # estremi dell'arco
dleaf = ndi.distance_transform_edt(~cv2.dilate(leaf.astype(np.uint8), ell(2)).astype(bool))
taper *= sstep((dleaf - 8) / (0.16 * R))                                      # si spegne verso il calice
mid, half = 0.105 * R, 0.036 * R
arc = np.abs(dist_f - mid) < half * taper
arc = cv2.GaussianBlur(arc.astype(np.float32), (0, 0), 2.0) > 0.5
arc &= body
lab, n = ndi.label(arc)
if n > 1:
    sizes = ndi.sum(arc, lab, range(1, n + 1)); arc = lab == (1 + int(np.argmax(sizes)))
print('arc components', n)
holes = spec | arc
body_lit = body & ~holes
# ombra: mezzaluna sul lato opposto
sh = ndi.shift(body.astype(np.float32), (L[1] * 0.26 * R, L[0] * 0.26 * R), order=1) > 0.5
shade = body_lit & ~sh                      # mezzaluna curva lungo il bordo in ombra
shade = cv2.GaussianBlur(shade.astype(np.float32), (0, 0), 3) > 0.5
shade &= body_lit
mono = body_lit | leaf
print('body', body.sum(), 'holes', holes.sum(), 'shade', shade.sum(), 'leaf', leaf.sum())

def contours(mask, up=3):
    m = cv2.resize(mask.astype(np.float32), None, fx=up, fy=up, interpolation=cv2.INTER_CUBIC)
    m = cv2.GaussianBlur(m, (0, 0), 1.2 * up / 2)
    b = (m > 0.5).astype(np.uint8)
    cs, h = cv2.findContours(b, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_NONE)
    out = []
    for i, c in enumerate(cs):
        if cv2.contourArea(c) < 6 * up * up: continue
        c = cv2.approxPolyDP(c, 0.7 * up / 3, True)[:, 0, :].astype(float) / up
        out.append((c, h[0][i][3] >= 0))
    return out

ys, xs = np.where(body | leaf)
x0, x1, y0, y1 = xs.min(), xs.max(), ys.min(), ys.max()
UPM = 1000
scale = 800.0 / (y1 - y0)
adv = int((x1 - x0) * scale + 160)
def tx(p): return ((p[0] - x0) * scale + 80, (y1 - p[1]) * scale)
def area(pts): return 0.5 * np.sum(pts[:, 0] * np.roll(pts[:, 1], -1) - np.roll(pts[:, 0], -1) * pts[:, 1])

def glyph(mask):
    pen = TTGlyphPen(None)
    for pts, hole in contours(mask):
        P = np.array([tx(p) for p in pts])
        cw = area(P) < 0
        if cw == hole: P = P[::-1]
        P = np.round(P).astype(int)
        pen.moveTo(tuple(P[0]))
        for q in P[1:]: pen.lineTo(tuple(q))
        pen.closePath()
    return pen.glyph()

def build(path, color):
    names = ['.notdef', 'tomato', 'tomato.body', 'tomato.shade', 'tomato.leaf']
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap({0xE000: 'tomato', 0x1F345: 'tomato'})
    gl = {'.notdef': TTGlyphPen(None).glyph(), 'tomato': glyph(mono), 'tomato.body': glyph(body_lit),
          'tomato.shade': glyph(shade), 'tomato.leaf': glyph(leaf)}
    fb.setupGlyf(gl)
    hm = {}
    for n in names:
        gg = gl[n]; gg.recalcBounds(None) if hasattr(gg, 'recalcBounds') else None
        hm[n] = (adv, getattr(gg, 'xMin', 0) if gg.numberOfContours else 0)
    fb.setupHorizontalMetrics(hm)
    fb.setupHorizontalHeader(ascent=800, descent=-120)
    fb.setupNameTable({'familyName': 'Tomato Glyph' + (' Color' if color else ''), 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-120, usWinAscent=900, usWinDescent=150)
    fb.setupPost()
    if color:
        fb.font['COLR'] = buildCOLR({'tomato': [('tomato.body', 0), ('tomato.shade', 1), ('tomato.leaf', 2)]}, version=0)
        c = lambda h: tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5)) + (1.0,)
        fb.font['CPAL'] = buildCPAL([[c('#B8403A'), c('#8A2F38'), c('#528B40')]])
    fb.save(path)

build(f'{outdir}/TomatoGlyph-Regular.ttf', False)
build(f'{outdir}/TomatoGlyph-Color.ttf', True)
dbg = np.zeros(body.shape + (3,), np.uint8)
dbg[body_lit] = (184, 64, 58); dbg[shade] = (138, 47, 56); dbg[leaf] = (82, 139, 64)
Image.fromarray(dbg).save(f'{outdir}/mask_debug.png')
