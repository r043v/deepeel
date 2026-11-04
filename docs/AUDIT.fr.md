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
| NES | 12 | 25 | 0 | 2 |
| PCE | 16 | 22 | 0 | 1 |
| MD | 10 | 28 | 0 | 1 |

Cellules ouvertes (◐ ou ✗) : 91.

| # | Technique | Plateforme | État |
|---|---|---|---|
| D1 | Marquage exec natural/forced/entry/irq/start | NES | ◐ |
| D1 | Marquage exec natural/forced/entry/irq/start | PCE | ◐ |
| D1 | Marquage exec natural/forced/entry/irq/start | MD | ◐ |
| D3 | Accès registres par registre (`DpIoStat`) | PCE | ◐ |
| D3 | Accès registres par registre (`DpIoStat`) | MD | ◐ |
| D4 | Copies par boucle CPU | NES | ◐ boucle → PPUDATA |
| D10 | Sondes de lecture manette | NES | ◐ 8 boutons, port 1 absent |
| D10 | Sondes de lecture manette | MD | ◐ 4 |
| D12 | Cibles indirectes observées | NES | ◐ observé, sans second passage |
| D12 | Cibles indirectes observées | PCE | ◐ observé (`jmp (abs)`, `jmp (abs,X)`), sans second passage |
| D12 | Cibles indirectes observées | MD | ◐ observé (`jmp/jsr` par An), sans second passage |
| D13 | Coupes de non-sens | NES | ◐ illégal |
| D13 | Coupes de non-sens | PCE | ◐ illégal |
| D13 | Coupes de non-sens | MD | ◐ faute |
| D14 | Chien de garde HALT, glissade de padding | PCE | ◐ glissade $00/$FF coupée (`padding cuts`) + opcode illégal ; pas de HALT/WAI attribué |
| D14 | Chien de garde HALT, glissade de padding | MD | ◐ glissade `ori.b #0,d0` coupée sur option (`--cut-padding`, 16 octets nuls) ; fautes d'opcode déjà rapportées |
| S1 | Scan padding + tables de pointeurs | NES | ◐ padding + tables, sans pointeurs ROM→ROM hors fenêtre |
| S2 | Fermeture statique de flot | NES | ◐ saut/appel/branche, fenêtre propre |
| S2 | Fermeture statique de flot | PCE | ◐ fenêtre MPR |
| S2 | Fermeture statique de flot | MD | ◐ 0.132 Bcc/DBcc/BRA/JMP/JSR depuis les plages exécutées, confiance perdue après BSR/JSR ; pas de vues |
| S3 | Suivi de constantes / provenance | GB | ◐ HL/A |
| S3 | Suivi de constantes / provenance | NES | ◐ provenance RAM→`jmp (zp)` depuis la PRG |
| S3 | Suivi de constantes / provenance | PCE | ◐ provenance ROM→WRAM→`jmp (abs)` (comptée, sans graine) |
| S3 | Suivi de constantes / provenance | MD | ◐ témoin An |
| S4 | Tables de sauts depuis l'opérande | GB | ◐ 0.134 `ld hl,nn` → `jp hl` (≤ 64 mots, même banque) + 0.191.0 base dans DE/BC (`add hl,rr`) |
| S4 | Tables de sauts depuis l'opérande | NES | ◐ tables de mots lues en données |
| S4 | Tables de sauts depuis l'opérande | MD | ◐ 0.190.0 `jmp d8(pc,Xn)` → table de `bra.s`/`bra.w` (≤ 128 entrées, fixture `md_s4`) ; pas de tables de mots ni `jmp (An)` |
| S5 | Frères de copie | NES | ◐ copies OAM DMA / boucle CPU, tables du même flux |
| S5 | Frères de copie | PCE | ◐ tables du même flux, même page/fenêtre (VDC block move) |
| S5 | Frères de copie | MD | ◐ tables de longwords ROM, copies DMA ROM→VRAM (comptées) |
| S6 | Rejeu de décodeurs sur tables de flux | GB | ◐ 0.146 `CALL` + HL/DE/BC, tables de mots même banque |
| S6 | Rejeu de décodeurs sur tables de flux | SMS/GG | ◐ 0.147 idem (SMS/GG/SG) |
| S6 | Rejeu de décodeurs sur tables de flux | MSX | ◐ 0.147 idem |
| S6 | Rejeu de décodeurs sur tables de flux | NES | ◐ 0.148 `JSR` + mot de page zéro, tables de mots même bloc |
| S6 | Rejeu de décodeurs sur tables de flux | PCE | ◐ 0.149 `JSR` + mot de page zéro, page ROM de 8 Kio |
| S7 | Inférence de voisins à la carte | NES | ◐ trous ≤ 1024 |
| S7 | Inférence de voisins à la carte | MD | ◐ remplissage ≥ 8 (00/FF) ou ≥ 32, code ≤ 3 o, données ≤ 1024 o ; rendu seulement |
| G1 | Graines reset/RST/IRQ | NES | ◐ reset + NMI/IRQ (`--seed-vectors`) |
| G1 | Graines reset/RST/IRQ | PCE | ◐ reset |
| G1 | Graines reset/RST/IRQ | MD | ◐ reset |
| G2 | Scan de dispatch par vues de banques | NES | ◐ UxROM, banque fixe (`--bank-dispatch`) |
| G2 | Scan de dispatch par vues de banques | PCE | ◐ tables de mots par fenêtre, vues de pages (`--bank-dispatch`) |
| G2 | Scan de dispatch par vues de banques | MD | ◐ tables de mots longs de pointeurs de code (`--bank-dispatch`) |
| G3 | `--seed-banks`, `--entry` | PCE | ◐ `--entry ADDR` (mapping de mise sous tension) ; pas de `--seed-banks` ni de vue de banque |
| G3 | `--seed-banks`, `--entry` | MD | ◐ `--entry ADDR` (ROM, pair) ; pas de `--seed-banks` ni de vue de banque |
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
| F1 | File croissante + spool + `--resume` | PCE | ◐ `--lot`/`--lot-drain` + parc ; pas de `--resume` ni de file croissante |
| F1 | File croissante + spool + `--resume` | MD | ◐ `--lot`/`--lot-drain` + parc (`parked`, `lot_*`) ; pas de `--resume` ni de file croissante bornée par option |
| F2 | Arbre de choix persistant | NES | ◐ `--tree` (nœuds, parents, issues ; site non renseigné) |
| F2 | Arbre de choix persistant | PCE | ◐ `--tree` (DpTree commun : nœuds, parents, issues ; site non renseigné ; pas de reprise) |
| F2 | Arbre de choix persistant | MD | ◐ `--tree` (DpTree commun ; site non renseigné ; pas de reprise) |
| F3 | `--jobs` par instances clonées | NES | ◐ `--jobs 2` par phase (`dp_nes2_`, `nes_jobs`), pas de file partagée |
| F3 | `--jobs` par instances clonées | PCE | ◐ `--jobs 2` par phase (`dp_pce2_`, `test_jobs`), pas de file partagée |
| F3 | `--jobs` par instances clonées | MD | ◐ `--jobs 2` par entrées explicites (`dp_md2_`, `md_jobs`), pas de file partagée |
| J1 | Sessions jouées multi-thread + pool | SMS/GG | ◐ |
| J1 | Sessions jouées multi-thread + pool | MSX | ◐ |
| J1 | Sessions jouées multi-thread + pool | NES | ◐ xorshift |
| J1 | Sessions jouées multi-thread + pool | PCE | ◐ |
| J1 | Sessions jouées multi-thread + pool | MD | ◐ 0.189.0 `--play-frames/--play-sessions/--play-seed` (xorshift, sessions depuis la mise sous tension ; ni pool de checkpoints ni multi-thread) |
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
