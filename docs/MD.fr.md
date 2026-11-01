# Mega Drive

Support en cours de construction (jalon 1, version 0.12.0). Les nombres
produits sont des observations de l'outil (machine concrète, RAM à zéro),
pas des mesures de matériel.

## Ce qui existe

- **Machine** : le 68000 et le bus de m0boy (`MdFull`, `M0BOY_DIR`), pont
  `src/mach/mdmach.{c,h}`. Compilé avec `-DMD_TRACE` : m0boy expose des hooks
  (pas d'instruction, lecture, écriture) dans `runtime/md_trace.h`.
- **Cartouche** (`src/core/cart_md.c`) : détection de l'en-tête `SEGA` à
  `$100`, nom, région, checksum (déclaré et calculé), vecteurs `$0–$FF`.
  Une ROM sans en-tête SEGA retombe sur le chemin Game Boy.
- **Registre** (`src/platform/md/md_registry.c`) : VDP, Z80, YM2612, pads,
  bus Z80, TMSS, SRAM, mapper SSF2, RAM.
- **Passe linéaire** (`src/explore/md_trace.c`) : exécution depuis le vecteur
  de reset, avec budget de pas. Journalise l'exécution par octet, les
  lectures de données ROM, les accès par registre, les changements de banque
  SSF2 et les copies DMA ROM→VRAM (décodées depuis les registres VDP 19–23).

## Utilisation

    deepeel explore jeu.md [--max-total N] [--text rapport.txt]

Budget par défaut : 2 000 000 pas. Une saturation du budget est toujours
rapportée (`SATURATED`).

## Clés ROM

Espace plat : clé = `addr & 0x3FFFFF` (≤ 4 Mio). Le `dp_rom_index` GB
(`bank * 0x4000`) n'est pas utilisé. Exécution hors ROM (RAM, Z80) comptée
dans `exec_outside`.

## Limites

Pas de fourches (les deux issues de Bcc/DBcc), pas de forks d'IRQ V/H, pas
d'arbre ni d'éviction, pas de GUI, Z80 non exploré, désassembleur 68000
statique non écrit. Jalon 2.

## Modifications côté m0boy (non commitées)

`md_trace.h` (nouveau), `md_m68k_ops.h`, `md_full_cpu.c`.

## Tests

`tests/test_md.c` sur `fix_md_dma.md` (générée au build) : vecteurs, registre,
budget saturé, octets exécutés, copie DMA, compteur d'écritures VDP (7 : le
`move.l` compte pour deux mots de bus).
