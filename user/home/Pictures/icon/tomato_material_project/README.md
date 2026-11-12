# Tomato material turntable

Renderer proprio (numpy z-buffer + SSAA) che legge `source/tomato.obj` e produce un giro completo di 360° (72 frame, 30 ms, loop senza frame ripetuto), sfondo trasparente.

## Output (`output/`)
- tomato_material_v2.gif / _gray.gif: 576x576
- tomato_material_v2_64.gif / _gray_64.gif: 64x64 (contorno più spesso)
- tomato_material_v2_16/_32/_128.gif e gray: 16x16, 32x32, 128x128
- tomato_material.apng / .webp: 576x576 con alpha completo (versione a colori)

## GIF: 24 fps, 72 frame, giro di 3,00 s
- 576x576: massima qualità, palette 255 colori con dithering (come prima, nessuna compressione aggressiva).
- 128 / 64 / 32 / 16: palette ridotta in proporzione alla dimensione (scelta automatica del minimo numero di colori che mantiene la qualità entro 0,35 dB), nessun dithering.
- 24 fps esatti in media: ritardi alternati di 4 e 5 centesimi (le GIF non hanno 41,67 ms).
- Pesi: 576 ~2,9 MB colore / 3,5 MB grigi; 128 ~260 / 175 KB; 64 ~100 / 75 KB; 32 ~35 / 26 KB; 16 ~14 / 11 KB.

## Rigenerare
`sh source/build.sh` (fa tutto: render, GIF, font) (python3 + numpy, scipy, pillow; ffmpeg). Variabili: GRAY=1 (scala di grigi), OPX (spessore contorno, default 0.012).

## Colori (ombra / base / luce)
Colore: pomodoro #6E2230 / #B8403A / #E2705A; foglie #2B4F36 / #528B40 / #8DB968; picciolo #4E6B2E / #86A24A / #BBCB7C
Grigi: pomodoro #8E8B86 / #B2AEA7 / #D6D2CA; foglie #5F5D59 / #827F79 / #A8A49D; picciolo #6A6762 / #8D8982 / #B3AFA7
Contorno esterno #E0DACD; linee interne #BDB7AA.

## Note
- Le normali del .obj puntano verso l'interno: il renderer le inverte.
- `whisk3d_giro_llm_module.patch` è la patch originale dell'altro agent (modulo "giro" per Whisk3D), non usata dal renderer.
- Nella GIF la trasparenza è a 1 bit; per bordi lisci usare APNG/WebP.

## Font / glifo (`font/`)
- TomatoGlyph-Regular.ttf: glifo monocromatico (prende il `color` del testo), 1000 UPM, altezza 800 sopra la baseline. Le luci sono spazi vuoti: riflesso ovale e arco di luce continuo (con estremi sfumati) sul lato illuminato, sottile fessura tra foglie e frutto.
- TomatoGlyph-Color.ttf: stesso glifo a colori (COLR/CPAL v0, 3 livelli): corpo #B8403A, ombra a mezzaluna #8A2F38, foglie/picciolo #528B40. Dove COLR non e' supportato ricade sul monocromatico.
- Codepoint: U+E000 (Private Use Area) e U+1F345 (emoji pomodoro).
- Vettorializzato dal frame 18 del render (picciolo dritto); `source/mkfont.py` lo rigenera.
- Uso CSS: `@font-face{font-family:Tomato;src:url(TomatoGlyph-Regular.ttf)}` poi `<span style="font-family:Tomato">&#xE000;</span>`.
