#!/bin/bash

echo "=== 1. Scansione dei file sorgente Assembly (AMD64 e AArch64) ==="
# Cerca parole chiave tipiche di multiboot o magic numbers nei file .S
grep -i -E "magic|multiboot|1badb002|e85250d6|644d5241" boot/amd64/*.S kernel/arch/amd64/boot/*.S boot/aarch64/*.S kernel/arch/aarch64/boot/*.S 2>/dev/null

echo -e "\n=== 2. Verifica Magic su binario AMD64 ==="
if [ -f "build/amd64/kernel.elf" ]; then
    echo "[*] kernel.elf trovato."
    # Cerca la signature di Multiboot 1
    hexdump -C build/amd64/kernel.elf | grep -i "02 b0 ad 1b" > /dev/null && echo "-> Rilevato MULTIBOOT 1 Magic (0x1BADB002)"
    # Cerca la signature di Multiboot 2
    hexdump -C build/amd64/kernel.elf | grep -i "d6 50 52 e8" > /dev/null && echo "-> Rilevato MULTIBOOT 2 Magic (0xE85250D6)"
else
    echo "Errore: build/amd64/kernel.elf non trovato (compilalo prima con 'make ARCH=amd64')"
fi

echo -e "\n=== 3. Verifica Magic su binario AArch64 ==="
if [ -f "build/aarch64/kernel.elf" ]; then
    echo "[*] kernel.elf trovato."
    # Cerca la signature ARM64 (Linux Image Header)
    hexdump -C build/aarch64/kernel.elf | grep -i "41 52 4d 64" > /dev/null && echo "-> Rilevato ARM64 Image Magic (ARM\x64)"
else
    echo "Errore: build/aarch64/kernel.elf non trovato (compilalo prima con 'make ARCH=aarch64')"
fi