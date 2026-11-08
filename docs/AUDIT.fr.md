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
| NES | 22 | 15 | 0 | 2 |
| PCE | 26 | 12 | 0 | 1 |
| MD | 23 | 15 | 0 | 1 |

Cellules ouvertes (◐ ou ✗) : 58.

| # | Technique | Plateforme | État |
|---|---|---|---|
| D10 | Sondes de lecture manette | NES | ◐ 0.230.0 8 boutons port 1 + 8 port 2 + 10 combinaisons ; 0.243.0 `--pad-full` : les 256 masques du port 1 (test `nes_pad_full`), hors défaut ; pas de multitap |
| D10 | Sondes de lecture manette | MD | ◐ 0.240.0 14 sondes dont pad 6 boutons (runtime m0boy `pad_ext`) ; 0.245.0 `--pad-full` : les 256 masques à trois boutons (test `md_pad_full`), hors défaut ; 0.249.16 `--pad-full6` : les 256 masques × 16 combinaisons X/Y/Z/Mode du pad 6 boutons (test `md_pad_full6`) ; pas de multitap |
| S2 | Fermeture statique de flot | NES | ◐ saut/appel/branche, fenêtre propre |
| S2 | Fermeture statique de flot | PCE | ◐ fenêtre MPR ; 0.232.0 arêtes inter-fenêtres résolues vers la page unique observée (test `pce`), 0.242.0 vues multiples : l'arête est suivie dans chaque page observée de la fenêtre cible (≤ 16) ; reste le mapping qui change en cours de page (TAM) |
| S2 | Fermeture statique de flot | MD | ◐ 0.132 Bcc/DBcc/BRA/JMP/JSR depuis les plages exécutées, confiance perdue après BSR/JSR ; bus 68000 plat : une seule vue par construction ; reste le raffinement des écritures indirectes et le budget en cascade |
| S3 | Suivi de constantes / provenance | GB | ◐ HL/A + BC/DE (0.248.0) |
| S3 | Suivi de constantes / provenance | NES | ◐ provenance RAM→`jmp (zp)` depuis la PRG |
| S3 | Suivi de constantes / provenance | PCE | ◐ provenance ROM→WRAM→`jmp (abs)` ; 0.249.0 graines voisines de la table (`--second-pass`, test `pce_provenance_roots`) |
| S3 | Suivi de constantes / provenance | MD | ◐ témoin An |
| S4 | Tables de sauts depuis l'opérande | GB | ◐ 0.134 `ld hl,nn` → `jp hl` (≤ 64 mots, même banque) + 0.191.0 base dans DE/BC (`add hl,rr`) |
| S4 | Tables de sauts depuis l'opérande | NES | ◐ tables de mots lues en données |
| S4 | Tables de sauts depuis l'opérande | MD | ◐ 0.190.0 `jmp d8(pc,Xn)` → table de `bra.s`/`bra.w` (≤ 128 entrées, fixture `md_s4`) ; pas de tables de mots ni `jmp (An)` ; 0.233.0 aussi `jsr d8(pc,Xn)` (test `md_s4_jsr`) ; 0.234.0 `d8(An,Xn)` depuis la valeur vivante d'An (test `md_s4_an`) ; 0.235.0 tables de déplacements 16 bits validées par la cible observée (test `md_s4_words`) ; 0.241.0 `jmp (An)` dont An vient de `movea.l d8(Am ou pc,Xn),An` (test `md_s4_ptr`) |
| S5 | Frères de copie | NES | ◐ copies OAM DMA / boucle CPU, tables du même flux |
| S5 | Frères de copie | PCE | ◐ tables du même flux, même page/fenêtre (VDC block move) |
| S5 | Frères de copie | MD | ◐ tables de longwords ROM, copies DMA ROM→VRAM (comptées) |
| S6 | Rejeu de décodeurs sur tables de flux | GB | ◐ 0.146 `CALL` + HL/DE/BC, tables de mots même banque ; 0.203 entrées (banque, ptr) inter-banques |
| S6 | Rejeu de décodeurs sur tables de flux | SMS/GG | ◐ 0.147 idem (SMS/GG/SG) |
| S6 | Rejeu de décodeurs sur tables de flux | MSX | ◐ 0.147 idem |
| S6 | Rejeu de décodeurs sur tables de flux | NES | ◐ 0.148 `JSR` + mot de page zéro, tables de mots même bloc ; 0.205 UxROM, 0.211 MMC1, 0.212 MMC3 (modes 0 et 1, 0.213), Camerica 71, UN1ROM 94 (0.214), 180 (0.215) (banque, ptr) ; tous les mappers à PRG commutable du pont (1, 2, 4) sont couverts, 3/7/11/66 permutent tout le PRG ou seulement la CHR (rien à rejouer inter-banques), MMC5 (0.223.0, sous-ensemble : PRG/CHR) et le reste dépendent de leur prise en charge (J5) |
| S6 | Rejeu de décodeurs sur tables de flux | PCE | ◐ 0.149 `JSR` + mot de page zéro, page ROM de 8 Kio ; 0.206 entrées (page, ptr) |
| G2 | Scan de dispatch par vues de banques | NES | ◐ UxROM, banque fixe (`--bank-dispatch`) |
| G2 | Scan de dispatch par vues de banques | PCE | ◐ tables de mots par fenêtre, vues de pages (`--bank-dispatch`) |
| G2 | Scan de dispatch par vues de banques | MD | ◐ tables de mots longs de pointeurs de code (`--bank-dispatch`) |
| G4 | Semis indirect + budgets en cascade | NES | ◐ second passage sur racines statiques (`--second-pass`) |
| G4 | Semis indirect + budgets en cascade | PCE | ◐ second passage sur successeurs statiques non exécutés (`--second-pass`) |
| G4 | Semis indirect + budgets en cascade | MD | ◐ second passage sur successeurs statiques non exécutés (`--second-pass`) |
| C1 | Sections, liveness, verdicts | NES | ◐ req/forced + `speculative` (0.249.17, test `nes_speculative`) ; 0.249.24 liveness par section : pages de RAM et classes de registres lus (PPU, pads, autres), test `nes` (`test_liveness`) ; reste : banque/IRQ dans le contexte, non utilisée par l'éviction C2 |
| C1 | Sections, liveness, verdicts | PCE | ◐ req/forced + `speculative` (0.249.18, test `pce_speculative`) ; 0.249.25 liveness par section : blocs matériels lus (VDC, VCE, PSG, timer, pad, IRQ ; test `pce` `test_liveness`) ; reste : pages de RAM lues (pas de hook de lecture RAM), non utilisée par C2 |
| C1 | Sections, liveness, verdicts | MD | ◐ 0.135 req/forced par plage exécutée ; 0.249.19 compteur `speculative` (plages de la fermeture statique seule, test `md_speculative`) ; 0.249.82 sections vivantes : blocs matériels lus/écrits par section (video, sound, input, bank, sram, sys), fusion `--jobs` et ordre canonique, test `md_section_liveness` ; reste : non utilisée par C2 |
| C2 | Éviction par contexte | NES | ◐ contexte opt-in (`--ctx-cap`) ; 0.249.83 projection sur la liveness (`--ctx-live` : pages RAM/PRG-RAM, PPU+OAM, manette, APU, bord NMI) ; le défaut actif a été **mesuré et rejeté** (DuckTales : 60,6 → 52,4 % connus, la liveness par fenêtre 16 Kio est plus grossière que les sections GB) — à reprendre avec des sections par course |
| C2 | Éviction par contexte | PCE | ◐ contexte grossier opt-in (`--ctx-cap`) |
| C2 | Éviction par contexte | MD | ◐ contexte grossier opt-in (`--ctx-cap`) ; 0.249.84 projection sur la liveness de section (`--ctx-cap --ctx-live`) — **mesurée et rejetée par défaut** (Sonic, 3 M pas, cap 1 : 261 571 pas / 5 184 octets exécutés contre 410 513 pas / 10 794 octets avec la clé grossière ; RAM/VRAM/manette hors projection = états évincés non équivalents) ; reprise avec des pages de RAM lues par section |
| F3 | `--jobs` par instances clonées | NES | ◐ `--jobs 2` par phase (`dp_nes2_`, `nes_jobs`), pas de file partagée |
| F3 | `--jobs` par instances clonées | PCE | ◐ `--jobs 2` par phase (`dp_pce2_`, `test_jobs`), pas de file partagée |
| F3 | `--jobs` par instances clonées | MD | ◐ `--jobs 2` par entrées explicites (`dp_md2_`, `md_jobs`), pas de file partagée |
| J1 | Sessions jouées multi-thread + pool | SMS/GG | ◐ 0.210 pool `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | MSX | ◐ 0.210 pool `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | NES | ◐ 0.207 xorshift + pool de checkpoints `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | PCE | ◐ 0.208 pool `--play-pool` |
| J1 | Sessions jouées multi-thread + pool | MD | ◐ 0.209 pool `--play-pool` ; 0.189.0 `--play-frames/--play-sessions/--play-seed` (xorshift) ; sessions jouées dans une seule instance (le multi-thread passe par `--jobs`, F3) |
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
| O3 | `--watch` (SDL2) | GB | ◐ 0.142 `--watch [FPS]` et `--watch-dump FILE.ppm` (framebuffer VDP) |
| O3 | `--watch` (SDL2) | SMS/GG | ◐ 0.142 `--watch [FPS]` et `--watch-dump FILE.ppm` (framebuffer VDP) |
| O3 | `--watch` (SDL2) | MSX | ◐ 0.188.1 `--watch [FPS]` et `--watch-dump FILE.ppm` (framebuffer TMS9918, 160×192 ; test `msx_watch_dump`) |
| O3 | `--watch` (SDL2) | NES | ◐ `--watch`/`--watch-dump` (panneau 160×128) |
| O3 | `--watch` (SDL2) | PCE | ◐ `--watch`/`--watch-dump` (rangées VDC) |
| O3 | `--watch` (SDL2) | MD | ◐ `--watch [FPS]` (fenêtre SDL2) et `--watch-dump FILE` (PPM) |

Problèmes de forme : aucun.
