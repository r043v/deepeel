# Campagne multi-plateforme sur fixtures générées

Observations de l'outil (machine concrète, RAM à zéro) sur les fixtures
construites par `tests/build_fixtures.py` ; **pas** des mesures de matériel,
**pas** des ROMs réelles. Généré par `tools/campaign_fixtures.py`.

| Plateforme | Fixture | Code retour | Pas explorés | Octets connus (%) | Durée (s) |
|---|---|---|---|---|---|
| Game Boy | `fix_apu.gb` | 0 | 13098 | 100.0 | 0.17 |
| Game Boy | `fix_bankdata.gb` | 0 | 1862 | 100.0 | 0.01 |
| Game Boy | `fix_branch.gb` | 0 | 3755 | 100.0 | 0.01 |
| Game Boy | `fix_closure.gb` | 0 | 1644 | 100.0 | 0.01 |
| Game Boy | `fix_copy1.gb` | 0 | 3660 | 100.0 | 0.01 |
| Game Boy | `fix_copyx.gb` | 0 | 33963 | 100.0 | 0.38 |
| Game Boy | `fix_dma.gb` | 0 | 9 | 100.0 | 0.01 |
| Game Boy | `fix_hdma.gb` | 0 | 26 | 100.0 | 0.01 |
| Game Boy | `fix_irq.gb` | 0 | 16494 | 100.0 | 0.01 |
| Game Boy | `fix_jumptable.gb` | 0 | 89 | 100.0 | 0.01 |
| Game Boy | `fix_keyirq.gb` | 0 | 32792 | 100.0 | 0.01 |
| Game Boy | `fix_loop.gb` | 0 | 696330 | 100.0 | 0.04 |
| Game Boy | `fix_mbc5.gb` | 0 | 28 | 100.0 | 0.01 |
| Game Boy | `fix_nsdata.gb` | 0 | 79999996 | 100.0 | 4.06 |
| Game Boy | `fix_nsecho.gb` | 0 | 8 | 100.0 | 0.01 |
| Game Boy | `fix_nsstack.gb` | 0 | 6 | 100.0 | 0.01 |
| Game Boy | `fix_p1poll.gb` | 0 | 3481683 | 100.0 | 0.19 |
| Game Boy | `fix_pal.gb` | 0 | 4106 | 100.0 | 0.16 |
| Game Boy | `fix_probes.gb` | 0 | 22 | 100.0 | 0.01 |
| Game Boy | `fix_unreach.gb` | 0 | 11 | 99.2 | 0.01 |
| Game Boy | `fix_vblank.gb` | 0 | 13376707 | 100.0 | 2.73 |
| Mega Drive | `fix_md_cpu_vram.md` | 0 | 207 | n/d | 0.00 |
| Mega Drive | `fix_md_cpu_vram_gap.md` | 0 | 216 | n/d | 0.00 |
| Mega Drive | `fix_md_cpu_vram_reg.md` | 0 | 212 | n/d | 0.00 |
| Mega Drive | `fix_md_dbcc.md` | 0 | 202 | n/d | 0.00 |
| Mega Drive | `fix_md_dma.md` | 0 | 71 | n/d | 0.00 |
| Mega Drive | `fix_md_dma_fault.md` | 0 | 6 | n/d | 0.00 |
| Mega Drive | `fix_md_fault.md` | 0 | 1 | n/d | 0.00 |
| Mega Drive | `fix_md_fault_reg.md` | 0 | 2 | n/d | 0.00 |
| Mega Drive | `fix_md_fork.md` | 0 | 134 | n/d | 0.00 |
| Mega Drive | `fix_md_fork_fault.md` | 0 | 69 | n/d | 0.00 |
| Mega Drive | `fix_md_loop.md` | 0 | 65 | n/d | 0.00 |
| Mega Drive | `fix_md_pad.md` | 0 | 645 | n/d | 0.00 |
| Mega Drive | `fix_md_pad_effect.md` | 0 | 853 | n/d | 0.00 |
| Mega Drive | `fix_md_reader.md` | 0 | 202 | n/d | 0.00 |
| Mega Drive | `fix_md_reader_overlap.md` | 0 | 205 | n/d | 0.00 |
| Mega Drive | `fix_md_revisit.md` | 0 | 338 | n/d | 0.00 |
| Mega Drive | `fix_md_sram_overlay.md` | 0 | 10000000 | n/d | 0.69 |
| Mega Drive | `fix_md_vint.md` | 0 | 133 | n/d | 0.00 |
| Mega Drive | `fix_md_vint_masked.md` | 0 | 66 | n/d | 0.00 |
| Mega Drive | `fix_md_watch.md` | 0 | 2 | n/d | 0.00 |
| Mega Drive | `fix_md_z80.md` | 0 | 424 | n/d | 0.00 |
| NES | `fix_nes_branch.nes` | 0 | 1000000 | 0.78 (observé) | 0.04 |
| PC Engine | `fix_pce_branch.pce` | 0 | 1200000 | 0.26 (observé) | 0.08 |
| Master System | `fix_sega_mapper.sms` | 0 | 20000002 | 99.98 | 1.16 |
| Master System | `fix_z80_branch.sms` | 0 | 20000000 | 100.00 | 5.49 |
| Game Gear | `fix_z80_branch.gg` | 0 | 20000000 | 100.00 | 5.46 |
| SG-1000 | `fix_z80_branch.sg` | 0 | 20000000 | 100.00 | 5.09 |
| MSX | `fix_z80_branch.msx` | 0 | 20000000 | 99.99 | 5.29 |

## Synthèse par plateforme

| Plateforme | Fixtures | Échecs | Pas (total) |
|---|---|---|---|
| Game Boy | 21 | 0 | 97666289 |
| Mega Drive | 21 | 0 | 10004053 |
| NES | 1 | 0 | 1000000 |
| PC Engine | 1 | 0 | 1200000 |
| Master System | 2 | 0 | 40000002 |
| Game Gear | 1 | 0 | 20000000 |
| SG-1000 | 1 | 0 | 20000000 |
| MSX | 1 | 0 | 20000000 |

`n/d` : valeur non exposée par le rapport texte de la plateforme.
Pour NES et PC Engine la colonne « octets connus » est la couverture observée
(octets touchés), pas une classification complète de la ROM.

## J13 : ROMs réelles hors GB (0.187.5, observations de l'outil)

ROMs de `m0boy/roms` (hors dépôt), réglages par défaut (bornés), un seul run chacun, RAM à zéro, registres
événementiels statiques. Observations de l'outil, pas des mesures de matériel ; aucun run n'est une fermeture.

| ROM | pas | chemins | forks | autres observations |
|---|---|---|---|---|
| MD Sonic the Hedgehog | 250 061 | 529 | 519 (irq 1) | plafond par site atteint 67 191 fois, borne de boucle 495 ; couche statique 19 882 o de bourrage, 1 484 de table, 140 de code |
| MD Castle of Illusion | 175 901 | 173 | 162 (irq 2) | plafond par site 25 404, borne de boucle 168 ; exécution hors ROM 49 (RAM) |
| NES Super Mario Bros. | 3 045 225 | 865 | 848 | 11 sections, 294 copies, 6 fautes |
| NES DuckTales | 1 941 468 | 1 505 | 1 488 | 99 sections, 1 511 copies, 101 fautes, 496 exécutions hors PRG |
| PCE Raiden | 11 360 967 | 973 | 836 | observé 8,72 %, classé 21,30 %, inconnu 78,70 % ; 41 535 exécutions hors ROM |
| MSX Aleste | 40 524 637 | 1 025 | 692 (irq 458) | carte : connu 74,35 %, inconnu 25,65 % ; 10,3 M exécutions hors ROM, 482 écritures dans du code exécuté |
| MSX Gradius 2 | 60 000 000 | 1 763 | 3 313 (irq 2 698) | carte : connu 89,58 %, inconnu 10,42 % ; 14,6 M exécutions hors ROM, 129 440 écritures dans du code exécuté |

Lecture honnête : MD s'arrête très tôt par les plafonds par défaut (plafond de forks par site saturé), donc la
couverture MD obtenue ici est une borne basse faible ; un run `--unbounded` reste à faire. NES et PCE mesurent des
octets touchés, pas une classification complète. MSX est limité par le profil (BIOS simulé partiel, pas de MSX2/SCC).
Reste pour J13 : runs `--unbounded` MD, SMS/GG sur ROMs réelles, audit complet de la matrice §2.1.

### J13 : MD Sonic avec plafonds relevés (observation de l'outil)

`md explore` n'a ni `--unbounded` ni `--time-limit` (l'option est refusée) : c'est un écart de J1/J13 à combler.
Avec plafonds relevés à la main (`--max-total 100000000 --path-steps 5000000 --queue-max 262144 --fork-cap 16
--visit-cap 1024`), Sonic atteint le mur de pas (100 M pas, 849 chemins, 2 687 forks, 1 852 forks non lancés),
le plafond par site reste saturé (6,9 M), le code statique passe de 140 à 1 728 octets. Un essai avec `--fork-cap 64
--queue-max 4194304` a été tué (code 137, probablement mémoire). Toujours une borne basse, jamais une fermeture.

### J13 : SMS / GG / SG sur ROMs réelles (0.187.6, observations de l'outil)

Réglages par défaut (bornés, mur de 60 M pas atteint), un run chacun ; « carte » = classification de l'outil
(connu = exécuté, lu ou inféré), pas une mesure de matériel.

| ROM | pas | chemins | forks (branche / irq) | carte : connu / inconnu | hors ROM |
|---|---|---|---|---|---|
| SMS Ace of Aces | 60 M | 1 235 | 1 053 / 745 | 72,21 % / 27,79 % (exécuté seul : 29,90 %) | 1 200 193 |
| GG Alien 3 | 60 M | 1 567 | 1 388 / 680 | 84,58 % / 15,42 % (exécuté seul : 25,41 %) | 12 602 501 |
| SG Bomb Jack (32 Kio) | 60 M | 1 348 | 2 203 / 583 | 100 % / 0 % | 2 885 452 |

Le mur de pas est atteint partout : ce sont des bornes basses. Seul Bomb Jack est entièrement classé, ce qui reste
une classification de l'outil (une ROM de 32 Kio), pas une preuve de couverture du comportement.
Reste pour J13 : audit complet de la matrice §2.1, `--unbounded` MD.

### J13 : MD Castle of Illusion en `--unbounded` (0.246.0, observation de l'outil)

ROM de `m0boy/roms` (hors dépôt), `md explore --unbounded`, spool sur disque, arrêt par SIGINT après 600 s,
un run. Résultat : **32 161 858 pas dans un seul chemin**, 566 660 fourches, toutes écrites dans le spool
(566 707 écrites, **0 relues**, 4,7 Gio sur disque) ; couche statique 17 630 o de bourrage, 984 de table, 3 758 de code.
Lecture honnête : sans plafond de pas par chemin, le chemin naturel (la boucle principale du jeu) ne se termine
jamais, donc le spool grossit sans être exploré : `--unbounded` MD ne gagne ici aucune couverture dynamique par
rapport au run borné. Écart à combler : relire le spool pendant l'exécution (entrelacer), ou garder un plafond de
pas par chemin même en mode illimité. Pas une fermeture.
