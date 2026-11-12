# Stato della revisione

La richiesta corrente del manutentore prevale sui piani legacy. Gli esiti
negativi rimangono visibili; i documenti storici non sono certificazioni.

- [Ultimo incremento: open NONBLOCK e collegamento Gnulib](OPEN-FLAGS-2026-11-12.md):
  verifica AMD64/AArch64, Coreutils 42/50, suite 17/18. Timestamp, FTS,
  status flags e gate Power12 globale rimangono aperti.
- [Checkpoint completo](CHECKPOINT-2026-11-12.md): stato pubblicato in 8035903,
  ordine di ripresa e protocollo di revisione. Il relativo inventario con hash
  è uno snapshot di quella revisione e non viene riscritto dagli incrementi.
- [Catalogo userland/system](USERLAND-SYSTEM-CATALOG-2026-11-12.md) e
  [catalogo vivo graphics](GRAPHICS-LIVE-CATALOG-2026-11-12.md): perimetro da
  verificare prima di ulteriori migrazioni di servizi, font, compositor e UART.

I PASS funzionali non dimostrano stabilità della latenza né completamento
della migrazione graphics. Finestra vuota al resize e qualificazione ISA su
hardware fisico rimangono obbligazioni esplicite del checkpoint.
