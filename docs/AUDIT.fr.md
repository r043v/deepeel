# Audit de la matrice §2.1

Généré par `tools/audit_matrix.py` à partir de `docs/COUVERTURE.fr.md`. Les marques
sont les affirmations du document ; ce script en audite la forme et les totaux, il ne
remesure pas l'outil.

39 lignes de technique, 6 plateformes.

| Plateforme | ✅ fait | ◐ partiel | ✗ absent | — sans objet |
|---|---|---|---|---|
| GB | 32 | 6 | 0 | 1 |
| SMS/GG | 32 | 5 | 0 | 2 |
| MSX | 32 | 5 | 0 | 2 |
| NES | 14 | 23 | 0 | 2 |
| PCE | 22 | 16 | 0 | 1 |
| MD | 15 | 23 | 0 | 1 |

Cellules ouvertes (◐ ou ✗) : 78.

| # | Technique | Plateforme | État |
|---|---|---|---|
| D1 | Marquage exec natural/forced/entry/irq/start | NES | ◐ 0.228.0 ENTRY/IRQ/START par cellule (`exec_kind`) ; IRQ perdu pour une fourche reprise dans un handler |
| D1 | Marquage exec natural/forced/entry/irq/start | PCE | ◐ 0.229.0 ENTRY/IRQ par cellule (`exec_kind`) ; IRQ perdu pour une fourche reprise dans un handler |
| D1 | Marquage exec natural/forced/entry/irq/start | MD | ◐ 0.229.0 ENTRY/IRQ par cellule (`exec_kind`) ; IRQ perdu pour une fourche reprise dans un handler |
| D4 | Copies par boucle CPU | NES | ◐ boucle → PPUDATA |
| D10 | Sondes de lecture manette | NES | ◐ 0.230.0 8 boutons port 1 + 8 port 2 + 10 combinaisons ; pas de catalogue de 128 |
| D10 | Sondes de lecture manette | MD | ◐ 4 |
| D12 | Cibles indirectes observées | NES | ◐ observé, sans second passage |
| D12 | Cibles indirectes observées | PCE | ◐ observé (`jmp (abs)`, `jmp (abs,X)`), sans second passage |
| D12 | Cibles indirectes observées | MD | ◐ observé (`jmp/jsr` par An), sans second passage |
| S1 | Scan padding + tables de pointeurs | NES | ◐ padding + tables, sans pointeurs ROM→ROM hors fenêtre |
| S2 | Fermeture statique de flot | NES | ◐ saut/appel/branche, fenêtre propre |
| S2 | Fermeture statique de flot | PCE | ◐ fenêtre MPR ; 0.232.0 arêtes inter-fenêtres résolues vers la page unique observée (test `pce`), pas de vues multiples |
| S2 | Fermeture statique de flot | MD | ◐ 0.132 Bcc/DBcc/BRA/JMP/JSR depuis les plages exécutées, confiance perdue après BSR/JSR ; bus 68000 plat : une seule vue par construction ; reste le raffinement des écritures indirectes et le budget en cascade |
| S3 | Suivi de constantes / provenance | GB | ◐ HL/A |
| S3 | Suivi de constantes / provenance | NES | ◐ provenance RAM→`jmp (zp)` depuis la PRG |
| S3 | Suivi de constantes / provenance | PCE | ◐ provenance ROM→WRAM→`jmp (abs)` (comptée, sans graine) |
| S3 | Suivi de constantes / provenance | MD | ◐ témoin An |
| S4 | Tables de sauts depuis l'opérande | GB | ◐ 0.134 `ld hl,nn` → `jp hl` (≤ 64 mots, même banque) + 0.191.0 base dans DE/BC (`add hl,rr`) |
| S4 | Tables de sauts depuis l'opérande | NES | ◐ tables de mots lues en données |
| S4 | Tables de sauts depuis l'opérande | MD | ◐ 0.190.0 `jmp d8(pc,Xn)` → table de `bra.s`/`bra.w` (≤ 128 entrées, fixture `md_s4`) ; pas de tables de mots ni `jmp (An)` ; 0.233.0 aussi `jsr d8(pc,Xn)` (test `md_s4_jsr`) ; 0.234.0 `d8(An,Xn)` depuis la valeur vivante d'An (test `md_s4_an`) |
| S5 | Frères de copie | NES | ◐ copies OAM DMA / boucle CPU, tables du même flux |
| S5 | Frères de copie | PCE | ◐ tables du même flux, même page/fenêtre (VDC block move) |
| S5 | Frères de copie | MD | ◐ tables de longwords ROM, copies DMA ROM→VRAM (comptées) |
| S6 | Rejeu de décodeurs sur tables de flux | GB | ◐ 0.146 `CALL` + HL/DE/BC, tables de mots même banque ; 0.203 entrées (banque, ptr) inter-banques |
| S6 | Rejeu de décodeurs sur tables de flux | SMS/GG | ◐ 0.147 idem (SMS/GG/SG) |
| S6 | Rejeu de décodeurs sur tables de flux | MSX | ◐ 0.147 idem |
| S6 | Rejeu de décodeurs sur tables de flux | NES | ◐ 0.148 `JSR` + mot de page zéro, tables de mots même bloc ; 0.205 UxROM, 0.211 MMC1, 0.212 MMC3 (modes 0 et 1, 0.213), Camerica 71, UN1ROM 94 (0.214), 180 (0.215) (banque, ptr) ; tous les mappers à PRG commutable du pont (1, 2, 4) sont couverts, 3/7/11/66 permutent tout le PRG ou seulement la CHR (rien à rejouer inter-banques), MMC5 (0.223.0, sous-ensemble : PRG/CHR) et le reste dépendent de leur prise en charge (J5) |
| S6 | Rejeu de décodeurs sur tables de flux | PCE | ◐ 0.149 `JSR` + mot de page zéro, page ROM de 8 Kio ; 0.206 entrées (page, ptr) |
| S7 | Inférence de voisins à la carte | NES | ◐ trous ≤ 1024 |
| S7 | Inférence de voisins à la carte | MD | ◐ remplissage ≥ 8 (00/FF) ou ≥ 32, code ≤ 3 o, données ≤ 1024 o ; rendu seulement |
| G2 | Scan de dispatch par vues de banques | NES | ◐ UxROM, banque fixe (`--bank-dispatch`) |
| G2 | Scan de dispatch par vues de banques | PCE | ◐ tables de mots par fenêtre, vues de pages (`--bank-dispatch`) |
| G2 | Scan de dispatch par vues de banques | MD | ◐ tables de mots longs de pointeurs de code (`--bank-dispatch`) |
| G3 | `--seed-banks`, `--entry` | MD | ◐ `--entry ADDR` (ROM, pair) ; `--seed-banks` sans objet tant que le 68000 n'a pas de commutation ROM (SSF2 non modélisée) ; la fenêtre de banque du Z80 est suivie à part (`z80_banks_read`) |
| G4 | Semis indirect + budgets en cascade | NES | ◐ second passage sur racines statiques (`--second-pass`) |
| G4 | Semis indirect + budgets en cascade | PCE | ◐ second passage sur successeurs statiques non exécutés (`--second-pass`) |
| G4 | Semis indirect + budgets en cascade | MD | ◐ second passage sur successeurs statiques non exécutés (`--second-pass`) |
| C1 | Sections, liveness, verdicts | NES | ◐ req/forced |
| C1 | Sections, liveness, verdicts | PCE | ◐ req/forced |
| C1 | Sections, liveness, verdicts | MD | ◐ 0.135 req/forced par plage exécutée (sans liveness) |
| C2 | Éviction par contexte | NES | ◐ contexte grossier opt-in (`--ctx-cap`) |
| C2 | Éviction par contexte | PCE | ◐ contexte grossier opt-in (`--ctx-cap`) |
| C2 | Éviction par contexte | MD | ◐ contexte grossier opt-in (`--ctx-cap`) |
| C3 | Dédup exacte hors horloges | MD | ◐ `--exact-dedup` (hors horloges, en mémoire) |
| F1 | File croissante + spool + `--resume` | NES | ◐ `--spool`/`--resume` (lot disque), file bornée + parc |
| F2 | Arbre de choix persistant | NES | ◐ `--tree` (nœuds, parents, issues ; site renseigné pour les fourches de branchement ; reprise de l'arbre avec `--resume` (0.220.0)) |
| F2 | Arbre de choix persistant | PCE | ◐ `--tree` (DpTree commun ; site renseigné pour les fourches ; reprise de l'arbre avec `--resume` (0.221.0)) |
| F2 | Arbre de choix persistant | MD | ◐ `--tree` (DpTree commun ; site renseigné pour les fourches de branchement ; reprise de l'arbre avec `--resume` (0.222.0)) |
| F3 | `--jobs` par instances clonées | NES | ◐ `--jobs 2` par phase (`dp_nes2_`, `nes_jobs`), pas de file partagée |
| F3 | `--jobs` par instances clonées | PCE | ◐ `--jobs 2` par phase (`dp_pce2_`, `test_jobs`), pas de file partagée |
| F3 | `--jobs` par instances clonées | MD | ◐ `--jobs 2` par entrées explicites (`dp_md2_`, `md_jobs`), pas de file partagée |
| J1 | Sessions jouées multi-thread + pool | SMS/GG | ◐ 0.210 pool `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | MSX | ◐ 0.210 pool `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | NES | ◐ 0.207 xorshift + pool de checkpoints `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | PCE | ◐ 0.208 pool `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | MD | ◐ 0.209 pool `--play-pool` ; 0.189.0 `--play-frames/--play-sessions/--play-seed` (xorshift) ; sessions jouées dans une seule instance (le multi-thread passe par `--jobs`, F3) |
| J2 | Rejeu TAS prioritaire + shoots | MD | ◐ GMV |
| J3 | Identité ROM des films | MD | ◐ BK2 : MD5 d'en-tête comparé au ROM (avertissement seulement, le hash BizHawk MD n'est pas fiable) ; GMV sans identité |
| J4 | Go-Explore (cellules écran) | GB | ◐ `--session-cells` 0.144.0 (pool de sessions) |
| J4 | Go-Explore (cellules écran) | SMS/GG | ◐ `--novelty` 0.141.0 + `--novelty-cells 1` (cellules écran, vies apprises ; test `z80_novelty_cells`) (SMS/GG/SG) |
| J4 | Go-Explore (cellules écran) | MSX | ◐ `--novelty` 0.141.0 + `--novelty-cells 1` (idem ; test `z80_novelty_cells`) (MSX) |
| J4 | Go-Explore (cellules écran) | NES | ◐ `--novelty` (archive de cellules écran, vies sans connaissance du jeu) |
| J4 | Go-Explore (cellules écran) | MD | ◐ `--novelty` (archive de cellules plans A/B, vies sans connaissance du jeu) |
| J5 | Mode humain, export `.sta` | GB | ◐ 0.142 `--export-state` (cellule la plus profonde, fichier DPST) |
| J5 | Mode humain, export `.sta` | SMS/GG | ◐ 0.142 `--export-state` (idem) |
| J5 | Mode humain, export `.sta` | MSX | ◐ 0.188.1 `--export-state` (cellule la plus profonde, fichier DPST ; test `msx_export_state`) |
| J5 | Mode humain, export `.sta` | NES | ◐ `--export-state` (cellule la plus profonde, fichier DPST) |
| J5 | Mode humain, export `.sta` | PCE | ◐ `--export-state` (cellule la plus profonde, fichier DPST) |
| J5 | Mode humain, export `.sta` | MD | ◐ `--export-state` (cellule la plus profonde, fichier DPST) |
| O1 | `.map` texte, PNG, JSON, heat | NES | ◐ |
| O2 | `.cov`, romdata, `--evidence` | PCE | ◐ `--evidence` (pas de `.cov` ni romdata : pas de compteurs de coups) |
| O2 | `.cov`, romdata, `--evidence` | MD | ◐ `--evidence` ; pas de `.cov` ni romdata |
| O3 | `--watch` (SDL2) | GB | ◐ 0.142 `--watch [FPS]` et `--watch-dump FILE.ppm` (framebuffer VDP) |
| O3 | `--watch` (SDL2) | SMS/GG | ◐ 0.142 `--watch [FPS]` et `--watch-dump FILE.ppm` (framebuffer VDP) |
| O3 | `--watch` (SDL2) | MSX | ◐ 0.188.1 `--watch [FPS]` et `--watch-dump FILE.ppm` (framebuffer TMS9918, 160×192 ; test `msx_watch_dump`) |
| O3 | `--watch` (SDL2) | NES | ◐ `--watch`/`--watch-dump` (panneau 160×128) |
| O3 | `--watch` (SDL2) | PCE | ◐ `--watch`/`--watch-dump` (rangées VDC) |
| O3 | `--watch` (SDL2) | MD | ◐ `--watch [FPS]` (fenêtre SDL2) et `--watch-dump FILE` (PPM) |

Problèmes de forme : aucun.
