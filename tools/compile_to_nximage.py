#!/usr/bin/env python3
"""
compile_to_nxtimg.py — Converte immagini PNG/JPG in file .nxtimg (testo ANSI)
Uso: python3 compile_to_nxtimg.py input.png output.nxtimg [--width 80] [--height 24]

Novità:
- La larghezza (--width) è ora l'asse principale: definisce il numero di tile (caratteri) orizzontali.
- L'altezza viene calcolata automaticamente per mantenere il rapporto d'aspetto originale.
- Se specifichi sia --width che --height, viene usato --width come priorità e l'altezza viene scalata.
- Miglior rilevamento dimensioni originali e logging.
"""

import sys
import os
from PIL import Image


def rgb_to_ansi_256(r, g, b):
    """Converte RGB a codice ANSI 256 colori."""
    if r == g == b:
        if r < 30:
            return 16  # nero
        if r > 225:
            return 231  # bianco
        # scala di grigi 232-255
        gray_idx = round((r / 255.0) * 23)
        return 232 + gray_idx

    # Cubo 6x6x6 (16-231)
    r_idx = round((r / 255.0) * 5)
    g_idx = round((g / 255.0) * 5)
    b_idx = round((b / 255.0) * 5)
    return 16 + r_idx * 36 + g_idx * 6 + b_idx


def image_to_nxtimg(input_path, output_path, term_width=80, term_height=24):
    """
    Converte un'immagine in formato .nxtimg (testo ANSI raw).
    Usa il carattere block inferiore ▀ (U+2580) per raddoppiare la risoluzione verticale.
    """
    try:
        img = Image.open(input_path).convert('RGBA')
    except Exception as e:
        print(f"[ERR] Impossibile aprire '{input_path}': {e}")
        return False

    orig_width, orig_height = img.size
    print(f"[INFO] Immagine originale: {orig_width}x{orig_height} px")

    # === NUOVA LOGICA: larghezza come asse principale ===
    target_char_width = term_width

    # Calcola altezza target mantenendo aspect ratio
    aspect_ratio = orig_width / orig_height
    target_pixel_height = round(target_char_width / aspect_ratio)

    # Se l'utente ha passato anche --height, usiamo comunque la larghezza come priorità
    # (puoi rimuovere questo controllo se vuoi dare precedenza assoluta a --height)
    if term_height != 24:  # valore di default modificato
        print(f"[INFO] --height ignorato (priorità a --width). Altezza calcolata: {target_pixel_height//2}")

    # Ridimensiona
    img = img.resize((target_char_width, target_pixel_height), Image.Resampling.NEAREST).convert('RGBA')

    output_lines = []
    term_height_actual = (target_pixel_height + 1) // 2

    print(f"[INFO] Output: {target_char_width}×{term_height_actual} tile (caratteri)")

    # Processa ogni riga di caratteri
    for char_y in range(term_height_actual):
        line_chars = []
        last_fg = None
        last_bg = None

        for char_x in range(target_char_width):
            # Campionamento diretto (1:1 con i pixel dopo resize)
            top_pixel = img.getpixel((char_x, char_y * 2))
            bottom_pixel = img.getpixel((char_x, char_y * 2 + 1)) if char_y * 2 + 1 < target_pixel_height else top_pixel

            top_a = top_pixel[3]
            bottom_a = bottom_pixel[3]

            top_visible = top_a > 20
            bottom_visible = bottom_a > 20
            top_opaque = top_a > 153
            bottom_opaque = bottom_a > 153

            if not top_visible and not bottom_visible:
                # Fully transparent
                if last_fg is not None or last_bg is not None:
                    line_chars.append("\033[0m")
                    last_fg = None
                    last_bg = None
                line_chars.append(" ")

            elif top_visible and not bottom_visible:
                fg_code = rgb_to_ansi_256(*top_pixel[:3])
                if fg_code != last_fg or last_bg is not None:
                    line_chars.append(f"\033[0m\033[38;5;{fg_code}m")
                    last_fg = fg_code
                    last_bg = None
                line_chars.append("▀")

            elif not top_visible and bottom_visible:
                fg_code = rgb_to_ansi_256(*bottom_pixel[:3])
                if fg_code != last_fg or last_bg is not None:
                    line_chars.append(f"\033[0m\033[38;5;{fg_code}m")
                    last_fg = fg_code
                    last_bg = None
                line_chars.append("▄")

            else:
                # Both visible
                if top_opaque and bottom_opaque:
                    fg_code = rgb_to_ansi_256(*top_pixel[:3])
                    bg_code = rgb_to_ansi_256(*bottom_pixel[:3])
                    if last_bg is None or fg_code != last_fg or bg_code != last_bg:
                        line_chars.append(f"\033[0m\033[38;5;{fg_code}m\033[48;5;{bg_code}m")
                        last_fg = fg_code
                        last_bg = bg_code
                    line_chars.append("▀")
                else:
                    # Semi-transparent → scegli il più opaco
                    if top_a >= bottom_a:
                        fg_code = rgb_to_ansi_256(*top_pixel[:3])
                        if fg_code != last_fg or last_bg is not None:
                            line_chars.append(f"\033[0m\033[38;5;{fg_code}m")
                            last_fg = fg_code
                            last_bg = None
                        line_chars.append("▀")
                    else:
                        fg_code = rgb_to_ansi_256(*bottom_pixel[:3])
                        if fg_code != last_fg or last_bg is not None:
                            line_chars.append(f"\033[0m\033[38;5;{fg_code}m")
                            last_fg = fg_code
                            last_bg = None
                        line_chars.append("▄")

        # Reset a fine riga
        if last_fg is not None or last_bg is not None:
            line_chars.append("\033[0m")

        output_lines.append("".join(line_chars))

    # Scrivi il file .nxtimg
    try:
        with open(output_path, "w", encoding="utf-8", newline='\n') as f:
            f.write("\n".join(output_lines))
        print(f"[OK] Compilato: {output_path} ({target_char_width}x{term_height_actual})")
        return True
    except Exception as e:
        print(f"[ERR] Impossibile scrivere '{output_path}': {e}")
        return False


def main():
    # Batch mode per loghi
    if "--logos" in sys.argv:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        src_dir = os.path.join(script_dir, "logo")
        dst_dir = os.path.join(script_dir, "..", "example", "minios", "logos")

        if not os.path.exists(dst_dir):
            os.makedirs(dst_dir, exist_ok=True)

        if not os.path.exists(src_dir):
            print(f"[ERR] Cartella loghi non trovata: {src_dir}")
            sys.exit(1)

        print(f"[*] Batch mode: Compilazione loghi da {src_dir}...")
        count = 0
        for f in os.listdir(src_dir):
            if f.lower().endswith(('.png', '.jpg', '.jpeg')):
                in_p = os.path.join(src_dir, f)
                out_name = os.path.splitext(f)[0] + ".nxtimg"
                out_p = os.path.join(dst_dir, out_name)
                # Usa larghezza fissa ragionevole per i loghi
                if image_to_nxtimg(in_p, out_p, term_width=48):
                    count += 1
        print(f"[*] Completato. {count} loghi compilati.")
        sys.exit(0)

    if len(sys.argv) < 3:
        print("Uso:")
        print(" python3 compile_to_nxtimg.py input.png output.nxtimg [--width 80]")
        print(" python3 compile_to_nxtimg.py --logos")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]
    term_width = 80
    term_height = 24

    # Leggi da ambiente
    try:
        cw = os.environ.get("COLUMNS")
        if cw and cw.isdigit():
            w = int(cw)
            if w >= 20:
                term_width = w
    except Exception:
        pass

    # Parse argomenti
    i = 1
    while i < len(sys.argv):
        if sys.argv[i] == "--width" and i + 1 < len(sys.argv):
            term_width = int(sys.argv[i + 1])
            i += 1
        elif sys.argv[i] == "--height" and i + 1 < len(sys.argv):
            term_height = int(sys.argv[i + 1])
            i += 1
        i += 1

    if not os.path.exists(input_path):
        print(f"[ERR] File non trovato: {input_path}")
        sys.exit(1)

    success = image_to_nxtimg(input_path, output_path, term_width, term_height)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()