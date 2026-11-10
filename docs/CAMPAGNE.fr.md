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

Diagnostic complémentaire (même ROM, observations de l'outil) : `perf` montre plus de 80 % du temps dans l'écriture
du spool (`dp_lf_put` et zlib), pas dans l'émulation ; `--exact-dedup` ne change rien (136 322 fourches en 100 s,
toujours 0 relue) ; plafonner `path_steps` ou le budget du chemin naturel n'a pas changé le compteur de pas non plus
(32 161 858 à chaque arrêt), ce qui laisse la cause du premier chemin sans fin **non élucidée** : à tracer dans
`run_path`/`run_sliced` avant de toucher aux plafonds.

Trace des tranches (`run_sliced`, observation de l'outil, run de 100 s) : la première tranche de 16 777 216 pas se
termine avec `cut=1` et `dp_stop` déjà levé ; les chemins suivants sont courts (193 à 405 185 pas) et s'arrêtent
aussitôt. Le chemin naturel n'est donc pas sans fin : le premier chemin consomme tout le temps du run
(écriture du spool), et SIGINT n'est observé qu'en fin de tranche. Le plafond reste le débit du spool
(`dp_lf_put` + zlib), pas la boucle principale ; à améliorer : écriture du spool par lots non compressés, ou
vérification de `dp_stop` plus fréquente.

Correction (0.247.1) : les « 32 161 858 pas » venaient du découpage en tranches de 2^24 pas (l'arrêt n'était vu qu'en
fin de tranche), pas d'une boucle sans fin. Avec des tranches de 2^20, un run de 20 s s'arrête à 1 048 576 pas :
le débit réel est ~45 000 pas/s, une fourche spoolée pour ~18 pas. Le spool n'est relu qu'à l'épuisement du chemin
courant ; sur cette ROM, le coût est le nombre de fourches par pas, pas un blocage.

## J13 : premiers essais NES, GG, PCE sur ROMs réelles (0.247.1)

Observations de l'outil (machine concrète, RAM à zéro, ≤ 100 s, ROMs hors dépôt, sorties sur la partition Windows) :
- NES, DuckTales (Europe), `--max-total 400000000` : PRG 128 Kio, 8 531 octets touchés (6,51 %), 1 958 188 pas, 1 527
  chemins dont 1 488 forcés, 101 fautes, exécution hors PRG 496, 1 276 écritures dans des octets exécutés (code en RAM
  auto-modifié, premier à $C1BA).
- GG, Batman Returns (USA) : carte de 262 144 octets, connu 85,01 % (inféré 156 655), 60 000 000 pas, 1 520 chemins,
  931 forks d'IRQ, `exec_outside` 89 680 ; 18 922 octets `data-copied`.
Ces chiffres sont des essais courts, pas une campagne : ni saturation levée, ni comparaison à un autre outil.
- SMS, « 20-em-1 (B) » (multicart, 262 144 octets) : connu 31,60 % par l'exécution, carte 77,96 % (inféré 56 884),
  60 000 000 pas, 1 285 chemins, 918 forks d'IRQ, `exec_outside` 264 344.
- MSX1, Aleste (Compile, 1988) : connu 2,53 % par l'exécution, carte 74,35 % (inféré 117 626), 40 524 637 pas,
  1 025 chemins, `exec_outside` 10 324 290 — l'écart entre le connu et la carte inférée est grand ; la valeur
  `exec_outside` élevée dit que l'essentiel de l'exécution se fait hors ROM (RAM/BIOS), à analyser avant de conclure.
- NES, DuckTales, second run (`--max-total 4000000000 --max-steps 2000000`, limite 900 s) : résultat identique au run
  de 100 s (1 958 188 pas, 1 527 chemins, 6,51 % du PRG touché). L'exploration s'épuise donc d'elle-même sur cette
  ROM avec ces réglages ; le plafond n'est pas le temps. 6,51 % est le résultat de l'outil à RAM nulle et entrées
  par défaut, sans `--second-pass` ni jeu : un autre mode d'entrée serait nécessaire pour aller plus loin.
- NES, DuckTales avec `--second-pass --pad-full` (100 s) : toujours 8 531 octets touchés (6,51 %) ; 2 060 169 pas,
  2 002 chemins, 512 forks de sondes de manette, 1 graine de seconde passe, 0 mot de table tracé. Ni la manette ni la
  seconde passe ne débloquent la ROM : la cause du plateau à 6,51 % reste à déterminer (le rapport compte 164 NMI et
  1 340 IRQ, donc les interruptions ont lieu ; 101 fautes et 495 exécutions hors PRG sont à examiner).
- NES, DuckTales : l'en-tête est NES 2.0, mapper 2 (UxROM, PRG 128 Kio). Avec `--seed-banks` : 4 469 octets touchés
  (3,41 %), moins que sans (6,51 %), 0 graine de dispatch de banques ; la timeline compte 1 024 changements de banque
  conservés pour 1 953 en débordement, donc le jeu bascule bien de banque pendant l'exploration. Piste non explorée :
  les 128 Kio ne se découvrent pas par les seules bascules vues ; les cellules de vue de banque (G2) ne trouvent pas
  de table de dispatch reconnaissable. À reprendre avec un `--tas` ou une séquence de jeu avant de conclure.
- NES, DuckTales avec `--play 3000 --sessions 4` (110 s, observation de l'outil) : 31 200 octets touchés (**23,80 %**
  contre 6,51 % sans jeu), 254 906 387 pas, 23 996 trames jouées, 100 021 changements de banque en débordement. Le
  plateau de l'exploration seule venait donc d'un état de jeu jamais avancé (RAM à zéro, aucune entrée), pas d'un
  défaut du moteur de fourches : les sessions jouées (Go-Explore / manette aléatoire) font évoluer la RAM qui
  choisit les banques. C'est le mode à utiliser pour les campagnes longues NES.
- PCE, Raiden (USA) avec `--play 3000 --sessions 4 --max-total 400000000` (110 s, observation de l'outil) : observé
  198 938 octets (25,30 %) contre 8,72 % sans jeu, classé 39,30 % ; 12 000 trames jouées, 113 chemins, 285 forks,
  319 sections, 1 146 copies. Même constat que le NES : le jeu fait évoluer l'état que l'exploration seule laisse nul.
- GG, SMS, MSX avec `--novelty 3000 --novelty-cells 1` (35 s chacun) : **non concluant**. Le temps part dans
  l'exploration bornée (60 010 000 pas) ; la recherche de nouveauté n'a fait qu'un essai (1 trial, 1 trame, 0 touche)
  avant l'arrêt. Cartes à la fin : GG 87,29 % (contre 85,01 %), SMS 77,83 % (77,96 %), MSX 76,21 % (74,35 %) — des
  écarts de l'ordre de la variation entre runs, sans signification. Il faut des runs d'une durée bien supérieure,
  avec la phase de jeu seule (`--max-total` réduit), pour comparer au NES et au PCE.
- Correction sur les essais Z80 ci-dessus : `--novelty N` est un budget de **pas** (une trame = 10 000 pas par défaut),
  pas un nombre de trames ; avec 3000 ou 20 000 la recherche s'arrêtait après 1 ou 2 trames. GG, Batman Returns avec
  `--max-total 2000000 --novelty 100000000 --novelty-cells 1` (100 s) : 58 essais, 9 969 trames, profondeur 168
  trames, 8 touches, 9 points archivés, 142 005 745 pas au total ; carte 84,58 % (contre 85,01 % sans jeu) : la
  phase de jeu GG n'améliore pas la carte sur cette ROM dans ce budget (observation de l'outil, un seul run).
- MD, Sonic 1 : le mode joué de MD passe uniquement par `--tas GMV` (pas de `--play`/`--novelty` comme NES/PCE/Z80) ;
  aucun GMV n'est disponible hors dépôt, donc pas de comparaison joué/non joué sur MD pour l'instant (lié à J3).
- MD, Sonic 1 (USA, Europe), exploration seule, `--max-total 50000000` (observation de l'outil) : 410 513 pas, 932 chemins,
  836 forks (1 fork d'IRQ), arrêt par épuisement de la file (rien en attente, aucun plafond saturé hors « loop bound
  885 »). ROM exécutée 2,1 % (10 794 octets), union observée 3,16 %, 47 chemins terminés par une faute machine, 6 tables
  `jmp d(pc,Xn)` (66 gestionnaires). Même schéma que NES/PCE sans jeu : l'exploration s'épuise sur un état de RAM
  initial ; la cause précise (porte d'état de jeu ou boucle d'attente d'IRQ) n'est pas isolée. À comparer avec un GMV
  quand J3 sera débloqué.
- SMS, « 20-em-1 (B) » avec `--max-total 2000000 --novelty 100000000 --novelty-cells 1` (110 s, un run, observation de
  l'outil) : 42 essais, 10 000 trames, profondeur 2 088 trames, 58 touches, 118 points archivés, 142 000 528 pas ;
  carte connue **90,71 %** contre 77,83 % à l'essai court sans vraie phase de jeu (carte précédente : 77,96 % sans jeu).
  C'est une compilation multi-jeux : le menu puis les jeux dépendent d'entrées, donc la phase de jeu aide ici, contrairement
  à GG/Batman Returns (84,58 %). Un seul run, sans répétition : écart non attribué avec certitude à la seule nouveauté.
- MSX, Aleste avec `--max-total 2000000 --novelty 100000000 --novelty-cells 1` (110 s, un run, observation de l'outil) :
  42 essais, 10 000 trames, profondeur 96 trames, 2 touches, 3 points archivés, 142 000 000 pas ; carte connue
  **81,48 %** contre 76,21 % (essai court) et 74,35 % (sans jeu). Peu de nouveauté trouvée (2 touches) : gain modeste,
  un seul run. `exec_outside` 4 651 185 pas (code hors ROM, à examiner avant d'en tirer une conclusion).

## J13 : NES Super Mario Bros. jusqu'à l'épuisement (0.249.37)

Observation de l'outil (machine concrète, RAM à zéro, ROM hors dépôt, sortie sur la partition Windows),
`--second-pass --max-total 400000000 --queue 200000` :
- PRG 32 Kio, 13 438 octets touchés (41,01 %), 6 259 308 pas, 3 673 chemins dont 3 634 forcés, 3 628 forks, 175 évincés ;
  coupes : boucle 3 610, fourche 284 261, globale 0 (le run s'arrête par épuisement de la file, pas par budget) ;
  41 fautes, 7 exécutions hors PRG, frontière 0 parqué / 0 perdu ;
  second passage : 6 racines semées, 0 refusée, 0 au-dessus du plafond ; 112 sections.
- Contraste avec la campagne courte de 0.187.6 (3 045 225 pas) : le run complet double les pas, sans saturation.
Ce n'est pas une mesure de matériel, et le 41 % reste ce que l'outil atteint sans manette réelle ni TAS.

## J13 : PCE Raiden, exploration seule (0.249.37)

Observation de l'outil (machine concrète, RAM à zéro, ROM hors dépôt), `--second-pass --max-total 400000000 --queue 200000
--time-limit 500`, sans session jouée : 66 001 646 pas, 2 024 chemins dont 1 860 forcés, 2 147 forks (88 de manette,
111 d'IRQ), 402 évincés ; coupes : boucle 1 256, fourche 13 011 426, fourche d'IRQ 3 452 179, globales 2 ; 583 fautes ;
2 054 396 pas hors ROM ; frontière 0 parqué / 0 perdu ; second passage : 3 racines semées, 0 refusée, 0 au-dessus du
plafond ; 310 sections. Observé 102 748 octets (13,07 %), classé 205 935 (26,19 %), inconnu 73,81 %. Je n'ai pas vérifié si l'arrêt vient de la file vide
ou du plafond de temps (2 coupes globales comptées). Ce n'est pas une mesure de matériel.

## J13 — MSX Gradius 2 (ROM hors dépôt)

Run `z80 explore msx … --max-total 1000000000` (observations de l'outil, RAM à zéro, BIOS partiel simulé). Arrêt sur la
limite de pas, **pas** sur l'épuisement : 1 040 000 000 pas, 31 773 chemins (10 646 forks de branche, 24 501 forks
d'IRQ), `exec_outside` 809 630 474 pas, 4 172 494 écritures dans des octets déjà exécutés (premier à `$A163`), 1 492
états restants. Ce run a révélé le débordement de pile corrigé en 0.249.38.

Sonic (MD, `--unbounded`) : le run a été interrompu avant tout rapport (limite externe) ; reprise de spool corrigée en
0.249.39, aucun chiffre retenu. J13 reste ◐.

## J14 : banc GG/SMS — chargeur bk2 continu et films SMS complets (0.251.24/0.251.25, observations de l'outil)

Deux bugs du chargeur de films `.bk2` expliquaient les « attract modes » des relectures SMS. Premier coup : le
tokenizer LogKey coupait sur l'espace (« P1 Up » devenait deux colonnes) — corrigé en 0.251.24. Second coup, le vrai
pour Castle : les cellules d'une ligne (`|Reset Pause|P1×6|P2×6|`) sont des **groupes de colonnes continues**, pas des
contrôleurs redémarrant à zéro — l'index est continu sur la ligne (0.251.25). Le TAS Castle 3292M presse **B1 à la
trame 74** pour sauter l'écran copyright (le jeu teste les fronts B1/B2 à `$0CEE` : `ld a,($C01C) ; and $30`) ; lu
comme Left, le saut ne se produisait jamais et les 42 337 trames restaient en attract, écran identique au contrôle
sans entrée, dans m0boy comme dans GPGX.

Vérité de terrain : la vidéo officielle du TAS (archive.org, décalage +105 trames — carte d'intro TASVideos) montre
le même enchaînement copyright → B1 → menu PRACTICE/NORMAL → niveau 1 ; notre relecture suit trame par trame jusqu'au
spring (~trame 4000, même scène). Verdicts :

| Film | Cœurs | Resultat |
| --- | --- | --- |
| Alex Kidd 5136S (ROM (U) V1.0, MD5 du Header.txt) | m0boy / GPGX | film complet 35 432 trames, en jeu dès ~600, ramdiff moyen 60 (déphasage de position) |
| Castle 3292M (ROM (U) V1.1, MD5 exact) | m0boy / GPGX | film complet 42 337 trames, skip + menu + niveau 1 conformes à la vidéo ; `DP_GG_PAD_SHIFT=-1` : ramdiff moyen 54 (contre 1785), 48 % d'écrans identiques trame à trame |

Divergence ouverte : à partir de ~trame 4500 nos deux cœurs (ensemble — pas un écart m0boy/GPGX) dérivent du run
original (mort au spring puis reprise dans la section à trois portes, HUD figé, alors que le run original finit le
jeu). C'est un problème de synchronisation d'entrées à la trame près (lag frames BizHawk, phase de lecture du pad
dans la trame), pas une divergence de cœur ; les décalages entiers testés (-3..+1) donnent -1 comme meilleur.

**Correction (0.251.28)** : les ramdiff 60 (Alex Kidd) et 54 (Castle) ci-dessus ont été mesurés avec les masques
faux de l'extraction ad hoc (le second bug du chargeur, colonnes continues, s'y était répercuté) — toujours extraire
les masques avec `tools/mkmask` (le même chargeur `dp_sega_tas_load` que le banc). Avec les masques corrects, la
resynchronisation SMSHawk (BizHawk 0.251.27, dumper `tools/smshawk_dump`) est établie sur Castle : pressions aux
mêmes trames des deux côtés (73/139/286/…), m0boy **sans** `DP_GG_PAD_SHIFT` (GPGX garde son -1 propre), IRQ vblank
BizHawk traitée avant les pas CPU de la scanline (placement amont ; après les pas → divergence à f3533, intra-
scanline équivalent) : ramdiff m0boy↔SMSHawk 2,9 @1000 puis < 0,5 jusqu'à **f4029**. Là, un état objet ($C31F,
écrit par la routine $86c7-$86e0 avec un compteur $C325 reincrémente `AND $0F`) prend une valeur d'avance (2 contre
1) dès la trame de transition f3921 — tous les octets visibles au repos sont identiques : la divergence est
intratrame (une itération de plus du compteur d'objet chez m0boy dans cette trame), la dérive de phase sous-trame
déjà documentée en e3cb130, pas un décalage d'entrées. Replay complet 42 337 trames : les deux cœurs jouent le film
jusqu'au bout, ramdiff moyen 1785 (un seul point de rupture). Observations de l'outil.

**Chasse intratrame (outillage `DP_GG_TRACE_FRAMES`/`SMSSHAWK_WATCH=ALL`)** : sur la trame de transition, la même
écriture `$C31F` part du même pc ($86de, `ld (ix+$1f),a`) avec A=2 chez m0boy contre 1 chez hawk ; la chaîne est
`ld a,(ix+$0d)` → `cp $80` → `ret` → dispatch → `and $01` ($86dc) → `inc a` → stockage : m0boy arrive au `and $01`
avec **A=01**, hawk avec **A=00** — un bit produit par le dispatcheur d'objets entre les deux (une itération de
dispatch de plus chez m0boy dans cette trame). Les séquences d'écritures diffèrent aussi d'un facteur ~2 (hawk 415
écritures contre 221) : le travail de trame n'est pas le même dès cette trame, alors que l'état au repos est
identique octet pour octet — la divergence naît d'une lecture intratrame, pas d'un état. Observations de l'outil.

**Cause racine (0.251.30) : le RNG du jeu lit le registre R.** La trace registres pleine trame montre le chaîne :
après le `ret` ($86d7), le dispatcheur passe par une routine de hachage à $8166 — `ld hl,($C063)`, rotations,
**`ld a,r` (ED 5F)**, `xor l`, stockage $C063/$C064 — et le bit consommé par le `and $01`/`inc a`/`ld (ix+$1f),a`
vient de ce R. Or R compte les instructions exécutées : les boucles d'attente de m0boy et de BizHawk ne sortent pas
au même nombre d'itérations (1801 contre 1798 itérations du poll $BF à f3921 — BizHawk rend le drapeau vblank
visible en cours d'instruction, m0boy aux frontières d'instruction seulement), donc R diffère (offset 0-127,
erratique, mesuré trame par trame des deux côtés avec DP_GG_RTRACE / SMSSHAWK_REGTRACE) bien que la RAM soit
identique. Le premier `ld a,r` du jeu (transition de scène f3921) transforme cette dérive invisible en divergence
d'état permanente. Le comptage de R lui-même est correct des deux côtés (+2 par préfixe DD/FD/ED, rien pendant le
halt — la dérive de m0boy pendant le halt est retirée au passage). Refermer le desync exige une équivalence de
comptage d'instructions trame par trame, c'est-à-dire la visibilité du drapeau vblank au même T-state que BizHawk :
chantier de cyclicalité exacte, pas un bug d'état. Observations de l'outil.

**Référence corrigée et phase refermée (0.251.31).** Trois causes distinctes se cachaient derrière ce desync :

- *Le dumper SMSHawk jouait le film avec une trame de retard.* Le constructeur de `DummyFrontend` avance le cœur
  d'une trame (`FrameAdvanceTo(0)` exécute son `FrameAdvance` final sans condition) avant la boucle du dumper : toute
  la référence était décalée, et R avec elle — offset **constant de 14** à chaque `ld a,r` du jeu (f3427, f3674,
  f3922, f7403…), invisible tant que le jeu ne consomme que le bit 0. Le dumper compte désormais cette trame comme la
  trame 0 du film (`SMSSHAWK_PREFRAME=1` rétablit l'ancien décalage). Les références produites avant 0.251.31 sont à
  régénérer.
- *Phase de reset.* Le Z80A de BizHawk passe 3 T-states dans sa séquence de reset avant le premier M1 : chaque
  frontière d'instruction tombe 3 T plus tard face au VDP. `DP_GG_RESET_T=3` applique cette phase au DUT — patch de
  compatibilité TAS côté banc (phase de mise sous tension d'un émulateur, pas une mesure de matériel).
- *T-state de lecture du statut.* Un `IN A,($BF)` échantillonne le bus à son dernier T-state (11ᵉ) : le drapeau
  vblank levé pendant l'instruction est vu par cette lecture. Corrigé dans le runtime m0boy (`GG_STATUS_READ_T`, 10
  par défaut). La note 0.251.30 parlait de « k = 4 » : les essais d'alors tournaient sur une archive
  `libdeepeel_sms.a` périmée (la cible `dp_sms_runtime` n'était pas dans `ALL` — corrigé), et `IN A,(n)` fait bien
  11 T chez BizHawk (3 de fetch + 8 micro-pas), pas 8.

Mesure : comparer R au bord de trame ne vaut rien (BizHawk s'arrête en cours d'instruction, m0boy à une frontière) ;
`DP_GG_LDAR=1` / `SMSSHAWK_LDAR=1` tracent R à chaque `ld a,r` exécuté, là où le jeu le lit. Avec
`DP_GG_RESET_T=3` et la référence régénérée : les **17 premiers tirages** (jusqu'à f7657) sont identiques des deux
côtés, pc et R ; premier écart au tirage de **f7826** (R=$6E contre $71, 3 comptes), la RAM divergeant durablement à
partir de là (contre f4029 avant). Reste donc une dérive de 3 incréments de R entre f7657 et f7826, à chasser avec le
même outillage. Observations de l'outil.

Preuves du pad lui-même : ROM de test Xch3l (sms_padtest, assemblée WLA-DX hors dépôt) affiche `0F0F0F0F` (U D L R
B1 B2 pressés) à l'identique sur les deux cœurs avec un film synthétique all-buttons ; lecture du pad confirmée à
$DC/$DD (29 passes/trame dans la boucle d'attract de Castle). Diagnostic : l'INT vblank n'est jamais prise par ce jeu
(il scrute $BF, bit7 vu à 1 chaque trame), les line IRQ ne partent pas (reg10 hors écran) — conforme à ce que le code
du jeu attend, vérifié par DP_GG_PC_SAMPLE/PC_RANGE/TRACE_ADDR.

**Complément 0.251.32.** La conclusion précédente sur les IRQ était fausse : les traces instruction par instruction
montrent des IRQ de ligne et de trame pendant le jeu. SMSHawk déclenche son compteur de ligne en début de ligne ; son
CPU accepte aussi une IRQ de trame arrivant exactement à la frontière d'instruction après l'instruction suivante.
Le profil de rejeu SMS reproduit ces deux phases ainsi que les 3 T de reset, sans changer le runtime matériel m0boy.
Le rejeu change maintenant les boutons à la fin d'une trame VDP plutôt qu'après un nombre fixe d'instructions.

Avec le film Castle entier (42 337 trames), le rejeu deepeel achève 253 169 986 pas sans faute ni trame refusée.
Les 38 lectures de `R` observées coïncident avec SMSHawk ; hors les 256 derniers octets de pile, la RAM du rejeu
correspond exactement à celle du banc m0boy à chaque fin de trame. En comparaison avec SMSHawk, 639 fins de trame
présentent une ou deux différences hors pile (maximum deux), compatibles avec les phases d'arrêt différentes ; ces
chaque octet concerné redevient identique à la trame suivante. Par exemple, à f42 `$C02B` vaut `$30` côté deepeel
et `$C3` côté SMSHawk : les deux exécutent les mêmes écritures, mais `$30` tombe en f43 côté SMSHawk. Ces écarts
de bord de trame ne démontrent pas une égalité cycle par cycle ni une égalité de rendu vidéo.
Le dump SMSHawk utilisait auparavant un fichier de 8 000 masques : au-delà les boutons étaient silencieusement
relâchés. Le dumper refuse désormais un fichier plus court que le nombre de trames demandé et refuse une première
trame non nulle qu'il ne peut pas fournir au constructeur de son frontend. Observations de l'outil, ROM hors dépôt.
