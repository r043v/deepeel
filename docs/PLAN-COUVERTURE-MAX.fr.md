# Plan de travail — maximiser la couverture, corriger les méthodes, mutualiser

Analyse du 15 novembre 2026, arbre de travail à la version 0.345.0 (modifications
non commitées comprises). **Analyse seule : aucun code modifié, aucun test ni
aucune ROM exécutés.** Les défauts ci-dessous viennent de la lecture du code ; chacun
doit d'abord être reproduit par une fixture avant d'être corrigé. Les références
`fichier:ligne` sont celles de l'arbre lu.

Documents lus : `AGENTS.md`, `docs/COUVERTURE.fr.md` (norme, §2, §10, §11),
`docs/AUDIT-PARITE-MOTEURS.fr.md`, `docs/PLAN-MUTUALISATION.fr.md`. Code lu en
entier : `src/explore/core.c`, `core.h`, `src/core/dpmachine.h`,
`src/core/coverage.h`, `src/mach/md_machine.c`. Les quatre moteurs historiques
(`src/platform/{sega,nes,pce,md}/explore.c`) et les autres backends n'ont été
que survolés (liste des fonctions, points d'entrée) : l'inventaire des doublons
en §C est fiable, mais ces moteurs n'ont pas été audités ligne à ligne.

## Avancement (0.366.0)

| Phase | État |
|---|---|
| P0, P1 | faites (0.346.0, 0.347.0) |
| P2 | faite (0.348.0) : C13, C12, C14, champs 32 bits |
| P3 | B5 et E11 faits (`state_hash` NES, MD, GB, PCE ; Sega limite technique justifiée) ; B6 : `sat_path`, `sat_loop` et le mur `max_total` (avec `--state-dir`) reportent ; `sat_site` et la sortie de boucle forcée justifiés (voir GB.fr.md) |
| P4 | B1, B7, A26 faits ; B10 : graine automatique GB/NES faite, signatures non ; B3/A22 : points de contrôle du film, état final et graines de vecteurs sur états vivants faits (GB, `core_live_seeds`), pool commun non ; B4 : énumération 0..255 en `--unbounded`, longueur prouvée SM83/Z80 faite, 6502/HuC6280/68000 non |
| P5 | C1 première tranche faite (plus aucune ISA nommée dans `core.c`) ; B8 faite (fenêtres 16 Kio, NES, MD linéaire, PCE banque fixe) ; B9 faite pour SM83/Z80/68000/6502/HuC6280 (`imm_ptr`, `imm_byte`) ; C3 : marcheur noyau unique avec A13, arrêts padding/données/pointeurs (restent indices de banque, arêtes non résolues, fusion des fermetures historiques) ; C4 non commencé |
| Depuis 0.351.0 | C3/B et C9 (noyau par défaut, `--legacy`) ; C4 tranche 1-2 (`infer_static` : bourrage, trous, frères de table copiée, tables de sauts communes, `--no-infer`) ; B8 pages PCE jamais exécutées ; B11 `on_copy` (DMA VDP MD, DMC NES ; HDMA GB sans objet, runtime DMG) ; C11 liveness (pages RAM lues par section du noyau) ; B3 checkpoints attract/play, cellule la moins visitée, rafales d'entrées (0.362.0-0.364.0). `campaign_fixtures` verte sur 0.364.0 (967 s, plafond 1800 s) |
| Reste | C4 retrait des scans historiques ; C2 rejeu S6 commun ; C10 module de copies unique ; C11 projection de contexte `DpCtxComp` (liveness des registres : demande le décodage des opérandes par ISA ; seul le GB déclare des composantes) ; C6/E12 sessions sur `DpMovie` (type unique fait 0.365.0) ; C8 workers du noyau seuls ; C7 couche de rapport unique |
| P6 à P8 (état initial) | non commencées : refontes d'architecture (fermeture statique unifiée, reprise des cinq moteurs historiques, bascule du noyau par défaut) à découper en tranches par plateforme |

Ce plan complète `PLAN-MUTUALISATION.fr.md` (dont les étapes 1 et 3 sont en
grande partie faites dans le noyau) et l'audit de parité (écarts E5, E6, E10,
E11, E12 encore ouverts). Il ne les remplace pas.

---

## 1. Constat

1. **Le noyau `dp_core_explore` porte les techniques neuves, mais il n'est le
   défaut que sur GB.** NES, PCE, MD, SG/SMS/GG/MSX lancent encore le moteur
   historique sans `--core` (`deepeel.c:1735`, `:1932`, `sega/explore.c:3009`).
   Les techniques 1 à 9 (concolique, sondes, rejeu par index, RNG, cellules…)
   n'atteignent donc pas ces plateformes par défaut, et à l'inverse le noyau n'a
   pas ce que les historiques ont : dédup exacte, reports différés par palier,
   pool de checkpoints, shoots TAS, S6, frères de copie, fermeture à vues,
   provenance complète, sections avec liveness.
2. **Le noyau a des défauts qui coûtent de la couverture ou faussent sa
   nature** (§A). Les plus lourds : états synthétiques comptés comme
   « naturels », sondes et rejeux armés une seule fois par site ou par adresse,
   tables pliées sur 16 bits (faux sur MD et sur les fenêtres bancarisées),
   copies MD cassées.
3. **Chaque technique existe encore en 4 à 7 exemplaires** (§C) : c'est la cause
   directe des ◐ de la matrice §2.1 et des corrections qui divergent.
4. **Le levier de couverture le plus fort n'est pas exploité : le contexte.**
   Presque tout part de l'état de mise sous tension (graines, sessions, sondes
   armées à la première visite). Les états profonds atteints par les films et
   les sessions ne servent pas de point de départ aux fourches, sondes et
   graines (§B).

---

## A. Défauts relevés dans les méthodes actuelles

Priorité : **1** = perte de couverture ou couverture fausse, **2** = règle
« jamais silencieux / rien n'est perdu » violée, **3** = robustesse.

| # | P | Lieu | Défaut | Effet | Correctif | Fixture minimale |
|---|---|---|---|---|---|---|
| A1 | 1 | `md_machine.c:46` | `on_read` transmet la valeur **0** au lieu de l'octet lu | la détection de copie verbatim du noyau (`core.c:492`, `v == last_val`) ne marche sur MD que pour des octets nuls ; `fed`/`fedsrc` faux, donc coupe « forcé en RAM » et `exec_wram_rom` faux | passer la valeur lue (le hook MD doit la fournir) | MD : boucle `move.b (a0)+,(a1)+` ROM→RAM non nulle, CHECK `copy_bytes` |
| A2 | 1 | `core.c:486-499` | une seule lecture mémorisée (`last_rom`) ; un `move.w`/`move.l` produit 2 ou 4 lectures puis 2 ou 4 écritures : seule la dernière lecture est appariée à la première écriture | copies larges du 68000 non reconnues ; idem toute ISA à accès multi-octets | petite FIFO de lectures (≥ 8) appariée dans l'ordre | MD : copie `move.l (a0)+,(a1)+` |
| A3 | 1 | `core.c:506-512` | cellule sémantique : `ramread[addr & 0xFFFF]` puis `fetch(a)` pour `a < 0x10000` — sur MD c'est la **ROM** (RAM en `$FF0000`) | le hachage de cellule est constant ; l'élagage `cell_pruned` se réduit à « cible déjà vue » et jette des fourches forcées de contextes différents | index RAM dense fourni par la machine (§C13) | MD : deux états RAM distincts, même cible, CHECK 2 cellules |
| A4 | 1 | `core.c:1050`, `:1124`, `:1071`, `:1546-1549` | `visits`, `rng_stamp`, `fed`, `fedsrc`, `fedprod`, `ramread` indexés par `pc/addr & 0xFFFF` | MD : deux adresses distantes de 64 Kio partagent compteur de boucle (`sat_loop` prématuré) et bit « alimenté » ; plateformes bancarisées : deux banques au même pc partagent le compteur | même index dense (§C13) ; `visits` par index ROM | MD : deux boucles à +64 Kio |
| A5 | 1 | `core.c:447`, `:1255`, `:1298` | le cap de fourches par site est indexé par **pc**, pas par index ROM | `$4123` en banque 1 consomme le cap de `$4123` en banque 5 ; site refusé à tort (`sat_site`) | clé = `rom_index(pc)` | GB/SMS : même pc dans deux banques, cap 1, CHECK deux fourches |
| A6 | 1 | `core.c:426`, `:711`, `:1112`, `:1143`, `:1169`, `:1217`, `:1247`, `:1704` | les états fabriqués (concolique, rejeu par index, octets RNG, signatures, `--seed-ram`, sondes, IRQ décalée) sont mis en file avec `forced = 0` et marquent `DP_CORE_NATURAL` / `DP_SRC_FORK_NATURAL` | le « naturel » est gonflé ; un index 0..15 hors des bornes de la table exécute des octets quelconques comptés comme code prouvé ; les trois niveaux de vérité du plan (natural > solved > forced) n'existent pas ; `DP_SRC_SESSION` n'est jamais posé par les sessions ni le TAS du noyau | `Pending.origin` hérité par les fourches ; sources `DP_SRC_SOLVED`, `_PROBE`, `_INDEX`, `_RNG`, `_SIG`, `_SEEDRAM`, `_IRQ`, `_SESSION` ; « connu strict » = naturel + session, « plausible » compté à part | fixture index hors table : l'octet atteint ne doit pas être `NATURAL` |
| A7 | 1 | `core.c:1101`, `:1129` | clé de rejeu par index et d'octet RNG = **adresse RAM seule** | une variable qui indexe plusieurs tables n'est rejouée qu'au premier site | clé (site, adresse) | deux sites lisant le même octet, CHECK deux séries de rejeux |
| A8 | 1 | `core.c:1207` | sonde événementielle et manette armées **une fois par site ROM**, à la première visite | la routine de lecture manette est en général un site unique : elle n'est sondée que dans le premier contexte (boot/titre), jamais en jeu | voir B1 (clé site × contexte, bornée) | routine pad appelée depuis deux modes, branche propre au second |
| A9 | 1 | `core.c:1289-1294` | l'élagage « deux côtés couverts » accepte un côté couvert seulement en **forcé** ; la résolution concolique n'est tentée que si l'autre côté est non couvert | un côté forcé n'est jamais promu en résolu, même quand un contexte ultérieur le permettrait | voir B2 ; l'élagage contractuel reste pour la mise en file de fourches forcées | fixture : premier passage forcé, second contexte résoluble |
| A10 | 1 | `core.c:358` | `closure_abs_table` refuse les tables à adresse impaire (« word tables are even ») | tables `jmp (abs,x)` 6502/HuC6280 à adresse impaire perdues | retirer le test ; valider par les cibles | NES/PCE : table à adresse impaire |
| A11 | 1 | `core.c:385`, `:400`, `:404`, `:332`, `:322` | constantes GB (`0x150`, `0x8000`) appliquées au z80 | z80 : cibles sous `$0150` refusées (code SMS/MSX légitime), fenêtre haute ignorée | bornes par plateforme (régions du `DpPlatform`) | z80 : table dont une cible est en `$0100` |
| A12 | 1 | `core.c:379-399` | recherche arrière de `ld hl,nn` octet par octet, non alignée sur les instructions | un opérande `$21` passe pour un opcode : fausse table → faux `STATIC` | remonter par instructions exécutées (anneau des derniers pc du chemin) | opérande `$21` avant `jp hl` |
| A13 | 1 | `core.c:270`, `:1186` | la fermeture du noyau suit toujours la retombée après un CALL | données en ligne après appel marquées `STATIC` (l'historique perd la confiance ; MD 0.322.0 exige un retour observé) | règle « callee à retour observé » dans le noyau | `call` suivi de données |
| A14 | 2 | `core.c:271`, `:312` | pile de 256 cibles : dépassement ignoré sans compteur | racines statiques perdues en silence | compteur `closure_stack_dropped` ou pile croissante | fonction à > 256 branches |
| A15 | 2 | `core.c:964-967`, `:1456-1460` | à `max_total` (ou arrêt) sans `--state-dir`, la file est libérée sans décompte | états perdus en silence, contraire aux principes 2 et 3 de la norme | compteur `pending_dropped` au rapport, ou report disque par défaut | budget minuscule, CHECK compteur > 0 |
| A16 | 2 | `core.c:1041`, `:1052`, `:1040` | `sat_path`, `sat_loop` : le chemin est abandonné, pas reporté | le noyau n'a pas la frontière différée par palier des historiques (§6 de la norme) | voir B6 | budget de chemin court puis drain = run long |
| A17 | 2 | `core.c:1006`, `:1112`, `:1143`, `:1169`, `:1688`-`:1746` | mises en file directes dans `sh->q` sans `queue_push` | `queue_max` et le spool disque contournés | une seule fonction de mise en file | — |
| A18 | 3 | `core.c:655` vs `:561` | `frontier_save` n'écrit pas le bit `vector` | après `--resume`, une graine de vecteur subit la coupe taint-RET | même encodage que le spool | reprise d'une graine vecteur |
| A19 | 3 | `core.c:1485-1492` | choix par nouveauté : balayage O(file) sous verrou, et `covered()` résout le pc en attente dans le mapping **courant du worker** | mauvais ordre sur cibles bancarisées ; goulot avec `--jobs` et file de 65 536 | stocker l'index ROM de la cible dans `Pending` ; tas par score | — |
| A20 | 1 | `core.c:1525` ; audit E6 | `--unbounded` coupe les cellules et le noyau n'a pas de dédup exacte | un chemin naturel dans la boucle principale ne finit jamais : l'« épuisement » promis par AGENTS.md est inatteignable | voir B5 | fixture à boucle principale : le run `--unbounded` termine |
| A21 | 1 | `dpmachine.h:186` ; `nes_machine.c:103` ; `md_machine.c:104` ; `pce_machine.c:42` | masque manette du noyau : 7-8 bits, un joueur ; NES et PCE sans Select, MD A et C fusionnés, pas de X/Y/Z/Mode, pas de port 2 | un film rejoué par le noyau perd des boutons : désynchronisation probable ; sessions et sondes ne pressent jamais ces boutons | modèle d'entrée large (§C12) | film avec Select / bouton C seul |
| A22 | 1 | `core.c:1015` | à la fin du film, le chemin TAS s'arrête ; pas de shoots, pas de bouffées aléatoires, pas de checkpoints (les historiques les ont) | les états profonds du film ne nourrissent que les fourches rencontrées en route | voir B3 | film court, branche atteignable seulement depuis un état tardif |
| A23 | 3 | `core.c:698` | clé de résolution construite par XOR de champs qui se recouvrent (adresse 24 bits MD) | collisions : résolutions sautées | clé hachée sur (site, adresse, côté) | — |
| A24 | 3 | `core.c:409-413` | au-delà de 256 entrées `jp_seen`, chaque passage recompte les entrées | compteurs `closure_jp_entries` / `prov_data_bytes` gonflés | ensemble extensible | — |
| A25 | 3 | `core.c:856-858`, `:984`, `:994` | plages WRAM par `strcmp(m->name)`, lectures `$FFAC`/`$FFB4` propres à un jeu GB dans le message générique, MD absente du DSY noyau | sortie de diagnostic fausse hors GB/Z80 | plage RAM de travail déclarée par la machine | — |
| A26 | 2 | `deepeel.c:1736` | sessions du noyau coupées sur NES parce qu'elles partagent le budget du chemin naturel | NES noyau sans sessions par défaut | budget propre aux sessions (déjà vrai pour attract/play, pas pour les sessions à pad tenu) | — |

À vérifier (non établi par cette lecture) :
- le Z80 son de la MD est-il avancé et observé quand le noyau pilote
  `dp_mdmach_run(…, 1, …)` (`md_machine.c:36`) ? Si non, J7 est perdu en `--core` ;
- écarts d'audit encore ouverts : E5 (fermeture bancarisée hors sm83/z80), E10
  (`--core` non propagé par l'auto-détection), E11 (contrat `--unbounded` non
  épinglé), E12 (profils de synchro TAS absents du noyau GB).

---

## B. Nouvelles techniques (dans le noyau, donc pour toutes les ISA)

Chaque technique arrive avec son option de désactivation, son compteur, sa
source `DP_SRC_*` et une fixture par ISA.

### B1 — Sondes ré-armées par contexte
Clé de sonde = (site, cellule sémantique) au lieu de (site). Bornes : N
contextes par site (défaut 8, `--probe-ctx-cap`), saturation comptée. S'applique
aux sondes manette, aux registres événementiels, aux fourches d'IRQ et au
rejeu par index. C'est la correction de A8 et le gain attendu le plus grand
sur les jeux réels (menus, modes de jeu, pause).

### B2 — Promotion du forcé et résolution à portée longue
- Tenter la résolution quand l'autre côté n'est couvert qu'en forcé : s'il
  existe une valeur légale, le côté passe en `solved` (aucune fourche forcée
  ajoutée, l'élagage contractuel est respecté).
- Candidats : aujourd'hui immédiats ± 1, 0, 1, `$FF` (`core.c:687-688`). Ajouter
  les 8 puissances de deux, puis l'**énumération complète des 256 valeurs** en
  repli (≤ 16 pas chacune : coût faible).
- Portée : la fenêtre est de 8 instructions séquentielles et 4 octets de RAM.
  Garder un anneau des K derniers instantanés pris aux lectures de RAM du
  chemin (même au-delà d'un appel) ; sur un conditionnel non résolu, rejouer
  depuis chacun.
- Lectures matérielles : primitive générique « valeur de la prochaine lecture
  à l'adresse X » (§C14), pour que ports et registres soient résolus comme la RAM.
- Couples d'octets (comparaisons 16 bits, compteurs) : énumération sur deux
  octets limitée aux candidats tirés des immédiats.

### B3 — Pool de checkpoints commun et graines sur états vivants
- Les films, sessions attract/play et chemins naturels déposent un checkpoint
  toutes les N trames dans un pool commun (celui de `sessions.c` /
  `ckptpool.h`, généralisé), choisi par cellule la moins visitée (Go-Explore).
- Depuis chaque checkpoint : fourches (cap par site remis à zéro par époque de
  checkpoint, borné), sondes B1, bouffées d'entrées aléatoires, mutations de
  segments de film.
- **Graines sur états vivants** : les graines de pointeurs, de vecteurs et de
  banques partent aujourd'hui de l'état de mise sous tension (`core.c:1650`,
  `:1686`) — RAM à zéro, pile vide. Les lancer aussi depuis quelques checkpoints
  (les plus divers) : le code atteint s'exécute dans un état de jeu réel et va
  plus loin avant les coupes.

### B4 — Énumération bornée des variables de dispatch
Généralise le rejeu par index : quand un saut indirect (D12) a une table dont
la longueur est prouvée (S4, fermeture), l'octet de RAM qui fournit l'index est
énuméré sur **toute la plage prouvée**, pas sur 0..15 ; sans longueur prouvée,
les cibles sont validées (début d'instruction légal, dans la ROM) avant d'être
suivies. Source `index-enum`. Corrige aussi le faux « naturel » de A6.

### B5 — Dédup exacte et convergence dans le noyau
Hachage d'état hors horloges (chaque backend fournit sa projection, comme
`state_hash` NES et `image_hash` GB) pris aux frontières de trame et aux têtes
de boucle ; un état déjà vu termine le chemin (éviction sémantique, comptée).
Ensemble persistant (`exact.bin`) rechargé à la reprise ; ensemble plein =
l'état est gardé. C'est la condition pour que `--unbounded` s'épuise (A20) et
la réponse à E6.

### B6 — Frontière différée dans le noyau
`sat_path`, `sat_loop`, `sat_site` et le mur `max_total` reportent l'état avec
un palier au lieu de l'abandonner (format commun, §C5). En plus, à `sat_loop` :
**sortie de boucle forcée** — les sorties non couvertes de la boucle saturée
sont mises en file comme fourches forcées (généralisation de D11).

### B7 — Fourches de banque
À une écriture de mapper observée (D6), si des banques n'ont jamais été vues à
ce site, fourcher sur chaque numéro de banque valide (`bank_view` existe),
borné par site. Atteint le code de banques qu'aucun chemin n'a sélectionnées,
dans un état réel (≠ `--seed-banks`, qui part de l'état initial).

### B8 — Désassemblage spéculatif des trous
Sur les plages restées inconnues après tout le reste : descente récursive
candidate, acceptée seulement si elle décode sans opcode illégal, se termine
par un retour ou un saut, et que toutes ses cibles tombent sur des débuts
d'instruction connus ou candidats cohérents ; score renforcé par une référence
(mot pointeur, opérande immédiat). Source `static-gap`, classe spéculative,
jamais « connue ». Les entrées retenues deviennent des graines B3.

### B9 — Références par opérandes immédiats
Les immédiats d'instructions **exécutées** qui désignent la ROM (`ld hl,nn`,
`lea`, `move.l #imm,An`, `lda #<`/`#>` appariés) sont des références typées :
vers du code connu → entrée ; vers une plage inconnue → candidat donnée ou
table (alimente S5, B4, B8). Paramétré par `DpIsa`.

### B10 — Sauvegarde valide automatique
`--seed-ram` appliqué d'office à la plage SRAM quand la cartouche en déclare
une, combiné aux signatures apprises ; pour les sommes de contrôle, la
résolution B2 à portée longue sur le comparateur final.

### B11 — Canaux de copie non CPU dans le noyau
`DpObservers.on_copy(src_index, dest, len, canal)` : OAM DMA, HDMA, DMA VDP,
transferts de bloc HuC6280, DMC, fetch CHR. Le noyau ne type aujourd'hui que
les écritures CPU (`data_dest`).

---

## C. Mutualisation : ce qui peut être factorisé

| # | Objet | Exemplaires actuels | Cible |
|---|---|---|---|
| C1 | Connaissance d'opcodes dans le noyau | 14 `strcmp` sur le nom d'ISA ou de machine dans `core.c` (`direct_ram_load`, `ram_signature`, `rom_table_follows`, `closure_jp_table`, `closure_abs_table`, DSY) | membres `DpIsa` : `ram_load`, `cmp_imm`, `table_access`, `indirect_table`, `imm_refs` ; aucune ISA nommée dans `core.c` |
| C2 | Rejeu de décodeurs S6 | `streams.c` (GB), `zstm_*` (Sega), `nstm_*` (NES), `pstm_*` (PCE), `replay_streams` (MD) | un module sur `DpMachine` |
| C3 | Fermeture statique | `closure.c` (GB), `sega/closure.c` (vues), `static_closure` NES, PCE, `md_static_scan`, `closure_walk` et `closure_walk_img` du noyau | une fermeture à vues (référence : Sega) sur `DpIsa` + vues de mapping de la machine ; règle CALL commune (A13) |
| C4 | Scan statique, tables, frères, inférence de trous | `static_scan`/`jump_tables`/`siblings` Sega, `table_scan`/`copy_siblings`/`infer_gaps` NES, `tables`/`siblings`/`infer` PCE, S1/S4 MD, `copies.c`, `report/map.c` | un module paramétré par `ptr_width`, boutisme, fenêtres |
| C5 | File, parking, lots, spool, arbre | `frontier*.c` (GB), spool `DPSZ` Sega, lots `DPFB`, `lot_spill`/`*_defer`/`tree_*` ×4, spool et `DFR1` du noyau | une frontière à paliers, un format |
| C6 | Sessions, nouveauté, attract, play, TAS | `sessions.c`, `play_session`/`novelty_search` ×4, sessions du noyau ; chargeurs `tas.c`, `sega/tas.c`, `nes/tas.c`, `pce/tas.c`, `md/movie.c` | sessions sur l'interface trame de `DpMachine` ; un `DpMovie` (tous boutons, tous ports) |
| C7 | Sorties | `dp_*_map_build`, `_report`, `_summary`, `_cov`, `_romdata`, `_evidence`, `_heat` ×4, `core_map.c`, `report/` | une couche de rapport sur le modèle §4 ; classes = projection |
| C8 | Parallélisme | `nes/jobs.c`, `pce/jobs.c`, `md/jobs.c`, fusion Sega, workers du noyau | workers du noyau seuls |
| C9 | CLI | `deepeel.c` (2 788 lignes), analyse d'options Sega séparée, défauts par plateforme épars | table d'options unique, table de défauts par plateforme, `--core` propagé (E10) puis supprimé |
| C10 | Suivi de copies | `copies.c`, `record_copy` NES, `log_copy` PCE, `flush_copy` Sega, copies MD, `obs_write` du noyau | un module (FIFO de lectures, pas, hachage verbatim, canaux B11) |
| C11 | Sections, liveness, projection de contexte | `sections.c`+`ctxproj.c`, Sega, NES, PCE, MD, `dp_core_sections` (sans liveness) | sections communes sur `DpCtxComp` (déjà déclaré, non consommé par le noyau) |
| C12 | Modèle d'entrée | masque 8 bits du noyau + conversions par backend + catalogues par moteur ; `DpProbe` déclaré mais inutilisé par le noyau | `DpInput` : ports × boutons nommés, catalogue de sondes fourni par la plateforme |
| C13 | Espaces d'adresses | tableaux de 64 Kio pliés (A3, A4) | `DpMachine.ram_index(addr)` dense + taille, pendant de `rom_index` ; plage de RAM de travail déclarée |
| C14 | Sondes de lecture | chaque `event_probe` refait sauver / exécuter / patcher / restaurer | primitive « prochaine lecture à X rend V » dans `DpMachine` ; les hooks ne fournissent plus que (adresse, valeurs) |
| C15 | Tests | fixtures par technique écrites à la main, banc de parité sur 2 fixtures | générateur technique × ISA dans `build_fixtures.py` ; banc de parité étendu (sondes, copies, banques, reprise) |

Champs trop étroits à élargir dans `dpmachine.h` au passage : `DpObservers.on_step`
(pc sur 16 bits), `DpMemOp.addr` (16 bits), `DpProvRegs` (paire HL + A seulement),
`DpProvRom.read` (adresse 16 bits) — tous inadaptés au 68000.

---

## D. Ordre de travail

Chaque phase : fixtures d'abord (elles échouent), correctif, `ctest` vert en fin
de phase, version incrémentée si le rapport change, matrice §2.1 et doc de
plateforme mises à jour.

| Phase | Contenu | Dépend de | Critère d'acceptation |
|---|---|---|---|
| **P0** Correctifs locaux | A1, A2, A5, A7, A10, A11, A12, A14, A15, A17, A18, A23, A24, A25 | — | une fixture par défaut ; aucun compteur de perte muet |
| **P1** Vérité des sources | A6 (origine héritée, nouvelles sources), A9 + B2 (promotion, 256 valeurs, anneau) | P0 | rapport : naturel / résolu / sondé / forcé séparés ; sur fixtures, aucun octet hors table en naturel ; le forcé baisse à couverture totale ≥ |
| **P2** Espaces et entrées | C13 (A3, A4), C12 (A21), C14 | P0 | fixtures MD 24 bits ; film avec Select et bouton C rejoué à l'identique par le noyau et l'historique |
| **P3** Terminaison et report | B5 (A20, E6), B6 (A16), E11 | P2 | `--unbounded` termine sur fixture à boucle ; budget minuscule + drain = run long ; `pending_dropped == 0` |
| **P4** Contexte | B3 (A22), B1 (A8), B4, B7, B10 ; A26 | P1, P3 | fixtures « branche atteignable seulement depuis un état tardif » par ISA ; campagne : naturel + résolu en hausse à budget égal |
| **P5** Statique unifié | C1, C3 (A13, E5), C4, B8, B9 | P0 | banc de parité : aucun octet statique de l'historique perdu ; témoins négatifs (données après appel, padding) non classés |
| **P6** Reprises des historiques | C2 (S6), C10 + B11, C11, C6 (E12) | P2, P4 | matrice §2.1 : S5, S6, C1, C2, J1, J2 identiques sur toutes les plateformes par construction |
| **P7** Bascule | noyau par défaut sur Sega/MSX, NES, PCE, MD ; historiques gardés inactifs (`--legacy`) ; C8, C9 | P3 à P6, banc C15 | par plateforme : `missing == 0` au banc, couverture stricte ≥ historique sur la campagne |
| **P8** Sorties | C7 ; PNG/JSON avec les nouvelles sources | P7 | classes de carte identiques avant/après hors nouvelles sources |

P0 et P1 sont indépendants de l'architecture et rapportent tout de suite. P5
peut avancer en parallèle de P2–P4.

## E. Mesure

- **Banc de parité** (`tests/test_audit_parity.c`) étendu à chaque phase :
  comparaison par octet et par source, pertes interdites, différences de
  politique nommées.
- **Campagne sur ROMs réelles hors dépôt** (données sur
  `/run/media/m/Windows/deepeel-data`), mêmes budgets avant/après, par
  plateforme : octets naturel, résolu, sondé, forcé, statique, données ;
  compteurs de saturation et de report ; temps. Résultats présentés comme
  observations de l'outil.
- **Indicateur principal** : octets « connus stricts » (naturel + session),
  puis « plausibles » (résolu, sondé, index). Le forcé et le statique seuls ne
  comptent pas comme gain.
- **Courbe de rendement** par technique (octets nouveaux par million de pas,
  par source de première découverte — `first_seen` existe déjà) pour ordonner
  les défauts et décider des valeurs par défaut.

## Statut au 0.366.0 : retrait complet décidé, en cours

Livré : P0 à P5 et P7, noyau par défaut (C9), `DpMovie` unique pour les films (C6/E12, première tranche).
Non fait, volontairement : C2, C4, C8, C10 et la fin de C7 reviennent à supprimer les moteurs historiques
(`--legacy`, fenêtre SDL temps réel, références de `test_nes.c` et `test_audit_parity.c`). Ce retrait est
destructif ; il attend la décision du propriétaire (retrait complet, ou conservation de `--legacy`).
C11 (liveness des registres) demande le décodage des opérandes par ISA ; C6/E12 reste à faire côté sessions.

Mise à jour 0.366.0 : le propriétaire a décidé le **retrait complet** (anciens fichiers d'état abandonnables, noyau extensible à de
nouvelles plateformes). Ordre : (1) fenêtres sur le noyau — carte `--mapwin` faite, `--gui`/`--human`/`--watch` à faire ; (2) fonctions épinglées à
l'ancien GB (sessions play, spool/lot, reprise) ; (3) références figées `tests/golden/` — faites pour GB/NES ; (4) retraits plateforme par
plateforme (NES, PCE, MD, Sega, GB) avec campagne après chacun.

## Retrait du moteur NES historique (0.368.0)

Le noyau est le seul moteur NES : `nes/explore.c`, `nes/jobs.c`, la 2e instance (`--jobs 2` replié), le rapport/carte/évidence/heat historiques et
`DpNesResults` sont supprimés. Règle suivie : une option n'est portée que si elle sert la couverture ; ce qui ne servait qu'à un test ou se
remplace autrement est retiré.

Porté au noyau : `--entry bank:addr`, `--seed-banks`, `--pad-full` (hooks NES ; à généraliser au catalogue de sondes commun, C12).
Retiré (sans équivalent voulu) : `--tree`, `--demo N`, `--queue/--queue-max` croissants, `--ctx-cap/--ctx-live/--defer-tiers`,
`--novelty`, `--evidence`, `--heat`, `--second-pass`, `--bank-dispatch`, `--jobs 2` historique (les travailleurs noyau clonent la machine).

Tests : les 45 tests CLI NES qui épinglaient le format du rapport historique sont remplacés par `tests/nes_kernel_fixtures.sh` (une fixture par
technique, ligne du rapport noyau qui la prouve) ; `core_*` couvrent le reste.

Écarts de couverture constatés, à traiter dans le noyau (statique C4, pas par option) :
- tables de sauts inter-banques et répartition par banque (`fix_nes_xbanktable`, `fix_nes_dispatch*`) : le noyau trouve 0 racine ;
- provenance des mots de table lus en PRG puis `rts` (`fix_nes_prov`, `rts`, `rtssplit`) ;
- chaleur d'exécution jouée par octet dans la carte PNG (`dpCh` bit 5) ;
- `--export-state` (sauvegarde de la cellule de nouveauté la plus profonde) : disparu avec la nouveauté NES.

Reste du retrait : PCE, MD, Sega, GB (même démarche), puis `--legacy`/`DEEPEEL_LEGACY` et les chemins `DpExplorer`.

## Retrait du moteur PCE historique (0.369.0)

`pce/explore.c`, `pce/jobs.c` et la 2e instance PCE sont supprimés ; `--entry` PCE passe par l'assistant commun `seedset_build`
(graines music-rip + `--entry` + `--seed-banks`, factorisé pour NES/PCE/MD). Tests : `tests/kernel_fixtures.sh` (NES + PCE).
Retirés avec le moteur : `--tree`, `--demo`, `--ctx-cap`, `--defer-tiers`, `--second-pass`, `--bank-dispatch`, `--romdata`, `--novelty`, `--export-state`.

Écarts de couverture PCE à traiter dans le noyau (C4/typage, pas par option) :
- copies jumelles (`fix_pce_sibling`), sauts indirects par table (`fix_pce_ind` : tables non reconnues), vues `lda/tam` (`fix_pce_tam*`) ;
- typage des écritures CD-ROM2/ADPCM et SuperGrafx (`fix_pce_adpcm`, `fix_pce_sgx`) : pas de ligne de rapport noyau ;
- racine statique des glissements de fenêtre (`fix_pce_slide`), aiguillage de banque (`fix_pce_disp`).

## Retrait du moteur MD historique (0.370.0)

`md/explore.c`, `md/jobs.c`, `md/explore.h`, la 2e instance `md2` et `test_md_explore` sont supprimés ; `md explore` est le noyau seul.
`--entry` passe par `seedset_build`. `--pad-full` / `--pad-full6` deviennent génériques dans le noyau (`DpMachine.input_bits`,
`dp_pad_subset`) et servent NES et MD ; tests : `tests/kernel_fixtures.sh` (7 cas MD).
Retirés : `--tree`, `--ctx-cap`, `--ctx-live`, `--defer-tiers`, `--second-pass`, `--bank-dispatch`, `--novelty`, `--exact-dedup`, `--jobs 2`,
`--tas-dir` (chaîne de films), tuiles de référence de `--watch` (ClownMDEmu, BizHawk, GPGX), `--export-state`.

Écarts de couverture MD à combler dans le noyau (pas par option) :
- couche statique : tables de saut S4, copies jumelles, `bsr`/`jsr` statiques, chaleur jouée, sections de liveness ;
- tuiles de référence à porter en un mécanisme générique de watch (toutes plateformes) ;
- chaîne de films `--tas-dir` générique ; rtcode/`--cov`/`--romdata` MD à re-tester côté noyau.


## Retrait du moteur Sega historique (0.371.0)

`sega/explore.c` (3 258 lignes), `sega/visits.c`, les instances `sg2`, `gg2`, `gg3..gg16`, `msx2` et 20 tests du moteur
sont supprimés ; `z80 explore <sg|sms|gg|msx>` est le noyau seul (`sega/command.c`). La fermeture de provenance reste dans le noyau
(`before_step`/`finish`). `--mapper` (SMS/GG) est enfin appliqué au noyau (`DpMachine`, `dp_machine_sega_mapper`, ré-appliqué à
chaque clone), `--entry BANK:ADDR` et `--seed-banks` passent par `seedset_build` (extrait dans `src/explore/seedset.h`, commun aux
plateformes) ; les vecteurs RST s'y ajoutent. Les lots portables (`--lot`, `frontier drain` d'un fichier) disparaissent avec le
dernier moteur qui les écrivait ; la frontière du noyau est `--state-dir`. Tests : `kernel_fixtures.sh` (cas Z80) et les
`core_*` existants.

Retirés : `--play-steps/--play-sessions/--play-pool`, `--novelty*`, `--export-state`, `--tree`, `--spool/--resume FILE`,
`--lot/--lot-drain`, `--indirect-total`, `--input-*`, `--seed-cap`, `--tas-total/--tas-rng-*`, `--watch/--watch-dump`, `--evidence`,
`--romdata`, `--rtcode`, `--jobs 2` limité, `--legacy`.

Écarts de couverture Sega à combler dans le noyau (pas par option) :
- rendu (`DpMachine.render/watch`) pour SG/SMS/GG/MSX : `--watch`, `--watch-dump`, `--mapwin` sans écran de jeu ;
- typage des accès registres/ports (YM2413 FM typé, ports MSX2 V9938, VDP, bancs) dans le rapport du noyau ;
- sorties `--evidence`, `--romdata`, `--rtcode` du noyau (acceptées, signalées « non produites ») ;
- sessions de lecture TAS aléatoires (`--tas-rng-*`) : déjà couvertes par les checkpoints `--tas-ckpt-every` du noyau.

## Retrait du moteur GB historique (0.372.0)

Le noyau `dp_core_explore` est le seul moteur, GB compris. Retirés : `explorer`,
`forks`, `streams`, `seeds`, `ctxproj`, `sections`, `copies`, `rtcode`, `sessions`,
`tree`, `closure`, `residual`, `provenance`, `frontier_disk`, la GUI SDL historique
(`--gui`, `--human`, `--play`) et leurs tests. Conservés : le modèle de résultats
(`src/explore/results.[ch]`, utilisé par la couche rapport et la commande `map`) et
les fenêtres `--watch` / `--mapwin` (`src/gui/windows.c`). Les options retirées sont
acceptées et ignorées avec une ligne sur stderr (`retired_option`).

Écarts consignés, à combler dans le noyau : sessions de lecture historiques, frontière
par lots, `--evidence`/`--romdata`/`--rtcode` GB, cellules legacy.
