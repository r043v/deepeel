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

## Avancement (0.372.0)

P0 à P5 : faites aux nuances près consignées ci-dessous. Retrait des cinq moteurs
historiques : **fait** (0.368.0 à 0.372.0) ; le noyau est le seul moteur et
`campaign_fixtures` est verte. Conséquences sur la mutualisation : C2 (rejeu S6),
C5 (frontière), C8 (parallélisme), C9 et C10 (copies) n'ont plus qu'un exemplaire, celui du
noyau, donc sont **résolues par construction** ; C4 (scans) l'est pour les scans
historiques. `--evidence` est absorbé par le chunk `dpCv` du PNG (`COUVERTURE.fr.md` §9.3),
`--romdata` et `--rtcode` sont écrits par `core_outputs`. Restent ouverts : C11 (projection
de contexte `DpCtxComp` hors GB), C7 (couche de rapport unique : `report/` sert encore
`core_map.c` et la commande `map`), P6 (reprises) et P8 (sorties), plus les écarts du retrait GB
(sessions de lecture historiques, typage des registres).
Observation : `--unbounded` sur `fix_branch.gb` ne termine pas, avec ou sans sessions.
Diagnostic (0.372.0) : ce n'est pas la dédup exacte (le hash d'état, même rendu relatif
aux ancres de cycle, ne change rien) mais la sémantique « les deux côtés de chaque
conditionnel » sans plafond : le `jr nz` de la boucle de copie, forcé « pris » quand
`c` vaut 0, fait boucler 256 itérations qui avancent `hl`/`de` ; chaque sortie naturelle
rouvre une fourche, et l'espace (hl, de, c) est fini mais énorme. L'arrêt est donc
l'épuisement ou SIGINT, comme décidé par le propriétaire ; le mode borné finit en ~8 s.

(Tableau ci-dessous : état à 0.366.0.)

| Phase | État |
|---|---|
| P0, P1 | faites (0.346.0, 0.347.0) |
| P2 | faite (0.348.0) : C13, C12, C14, champs 32 bits |
| P3 | B5 et E11 faits (`state_hash` NES, MD, GB, PCE ; Sega limite technique justifiée) ; B6 : `sat_path`, `sat_loop` et le mur `max_total` (avec `--state-dir`) reportent ; `sat_site` et la sortie de boucle forcée justifiés (voir GB.fr.md) |
| P4 | B1, B7, A26 faits ; B10 : graine automatique GB/NES faite, signatures apprises sur toutes les ISA (`ram_signature` : cp n Z80/SM83/6502/HuC6280, cmpi 68000 ; vérifié 0.376.1) ; B3/A22 : points de contrôle du film, état final et graines de vecteurs sur états vivants faits (GB, `core_live_seeds`) ; pool commun : films et sessions attract/play déposent leurs états dans la même file du noyau (source `SESSION`, cellule la moins visitée, rafales d’entrées) ; cap de fourches par site remis à zéro dans chaque époque de checkpoint (`Pending.epoch`, 0.378.0 ; borné par `tas_ckpt_cap` époques film + autant de session) ; mutations de segments de film : deux rafales d’entrées depuis chaque checkpoint de film (0.379.0, comme pour les sessions) ; B4 : énumération 0..255 en `--unbounded`, longueur prouvée faite pour SM83/Z80, 6502/HuC6280 (`lda abs,x`) et 68000 (0.376.1 : `lea` ROM + longs pointant la ROM, adresses paires) |
| P5 | C1 première tranche faite (plus aucune ISA nommée dans `core.c`) ; B8 faite (fenêtres 16 Kio, NES, MD linéaire, PCE banque fixe) ; B9 faite pour SM83/Z80/68000/6502/HuC6280 (`imm_ptr`, `imm_byte`) ; C3 : marcheur noyau unique avec A13, arrêts padding/données/pointeurs (arêtes hors de la vue ROM comptées `closure_unresolved` depuis 0.377.0 ; restent la fusion des fermetures historiques et les indices de banque des vues commutables) ; C4 : `infer_static` du noyau (bourrage, trous, frères de table copiée, tables de sauts) a remplacé les scans historiques, tous supprimés |
| Depuis 0.351.0 | C3/B et C9 (noyau par défaut, `--legacy`) ; C4 tranche 1-2 (`infer_static` : bourrage, trous, frères de table copiée, tables de sauts communes, `--no-infer`) ; B8 pages PCE jamais exécutées ; B11 `on_copy` (DMA VDP MD, DMC NES ; HDMA GB sans objet, runtime DMG) ; C11 liveness (pages RAM lues par section du noyau) ; B3 checkpoints attract/play, cellule la moins visitée, rafales d'entrées (0.362.0-0.364.0). `campaign_fixtures` verte sur 0.364.0 (967 s, plafond 1800 s) |
| Reste | voir « Statut final (0.379.0) » : C2, C4, C8, C10 et la partie moteurs de C7 sont résolus par le retrait des moteurs ; C11 (projection) et les vues commutables de C3 sont des limites justifiées |
| P6 à P8 | faits par le retrait complet : plus de moteur historique, noyau par défaut (C9), couche de rapport unique (C7) |

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
- typage des accès registres/ports dans le rapport du noyau : fait pour GB/NES/PCE (0.374.0) et Sega/MSX/MD (0.375.0), voir ci-dessous ;
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

## Rendu Sega dans le noyau (0.373.0)

`DpMachine.watch/render` existe maintenant pour SG/SMS/GG/MSX : le backend capture les
lignes (`DpSegaBackend.watch_view`, capture sans publication vers la fenêtre
framebuffer), `sega_render` recopie la dernière trame en ARGB. `--watch` couvre donc toutes
les plateformes du noyau. Test : `test_dpmachine` (trame 160×192 observée sur la fixture SMS).

## Typage des registres dans le noyau (0.374.0)

Le noyau compte, par entrée du registre de la plateforme (`DpMachine.plat->io_regs`), les lectures et écritures
CPU des chemins naturels (jamais les fourches forcées), et le nombre de valeurs d'octet distinctes écrites ;
le rapport texte les liste (`reg $lo-$hi NOM [catégorie] reads/writes/distinct-values`). Le sens (`rw`) du
registre filtre : une écriture dans un registre en lecture seule (fenêtre de banque) n'est pas comptée.
`m->plat` est désormais posé pour NES et PCE (comme GB) : leurs vecteurs RST/IRQ du registre sont semés
(« vector seeds »). Observations de l'outil sur fixtures, pas des mesures de matériel.

### Sega/MSX/MD (0.375.0)
- Z80 : nouvel observateur `DpObservers.on_io(port, v, write)` (IN/OUT enveloppés par `sega_observe`) ;
  `DpPlatform.port_io = 1` indique que le registre est une table de ports (SMS/GG/SG/MSX : YM2413, V9938,
  VDP, PSG, manette) et non des adresses mémoire.
- MD : `DpIoReg.lo/hi` passent en 32 bits et `DpPlatform.find` donne l'entrée d'une adresse 24 bits
  (miroirs repliés, régions ROM/SRAM/RAM exclues) : pas de table de 64 Kio. `dp_md_platform()` réutilise
  `dp_md_regs`, source unique.
- Test `core_reg_typing` : une fixture par plateforme (GB, NES, MD, SMS YM2413, MSX V9938).

## Couche de rapport unique (C7, 0.375.1)

`src/report/` ne garde que ce que le noyau et la commande `map` appellent : retrait de `html.c`,
de `dp_report_map_png`, `dp_report_map`, `dp_report_cov(_play)`, `dp_report_romdata(_play)`,
`dp_map_ch_encode`, `dp_map_methods_pct`, des sidecars « sticky » et de `dp_map_rgb` (aucun appelant) :
2577 → 1978 lignes. Le vecteur de classes unique est `dp_core_map_build` (`core_map.c`) ; PNG, texte
et JSON en sont des projections (`dp_map_png_from_evidence`, texte « map v1 », `--summary`).

## Composantes de contexte hors GB (C11, 0.376.0)

`DpMachine.ctx_components` est désormais déclaré pour Z80 (SMS/GG/SG/MSX), 6502 (NES), HuC6280 (PCE, registres
MPR) et 68000 (MD : D0-D7, A0-A6, SP, SR) : registres, état du mapper et pages de RAM dont un état entrant peut
dépendre. Contrôle dans `test_dpmachine`. Précision : le noyau ne consomme pas ces composantes (GB non plus : la
table décrit le contrat, la projection de contexte qui s'en servirait ne ferait qu'élaguer, sans octet de
couverture en plus) ; la liveness par page de RAM (`live`, C11 du noyau) est déjà générique sur toutes les ISA.
Une projection par décodage d'opérandes reste un raffinement d'élagage, non un écart de couverture.

## État du plan (0.382.0)

Résumé honnête au 2026-11-16. Les nombres sont des observations de l'outil (`--jobs 1`, ROMs hors dépôt), pas des mesures de matériel.

**Fait**
- P0 à P4, retrait complet des moteurs historiques (le noyau `dp_core_explore` est le seul moteur), P6 et P8 par construction (voir « Statut final »).
- B3 (cap par époque, rafales d'entrées), section E complète (courbe par budget, ablation à trois budgets), correction 0.380.1 de la simulation de banque.
- **C11 projection de contexte (0.381.0)** : `DpMachine.ctx_reg_hash` sur GB, Sega/MSX, NES, PCE, MD ; `proj_key` (registres + pages de RAM lues) évince les fourches
  déjà mises en file ; `--no-proj`. Ace of Aces, `--no-cells` : 1 618 fourches évincées, couverture identique. Sans décodage d'opérandes, tous les registres restent dans la clé.
- **C3 partiel (0.382.0)** : pile de retours dans la marche du noyau (`--no-closure-ret`). Gain sur 3 ROMs sur 4 (voir le banc ci-dessus).
- Tests : 165/165 hors `campaign_fixtures` (dernière passe complète de `campaign_fixtures` : 0.380.1, 1 217 s ; non relancée depuis 0.381.0).

**Ouvert**
1. **C3** : le noyau perd 779 octets sur Aerial Assault (GG), cause non instruite ; la fermeture Sega ajoute encore 610 à 3 007 octets (amorçage à chaque pas avec l'état
   concret, `jp (hl)`, IX/IY). `sega/closure.c` reste en place ; il n'existe ni banc octet à octet ni vérité terrain code/données, donc le sur-marquage ne se juge pas.
2. **MSX** : Aleste ne gagne rien de la fermeture Sega (7 094 octets avec et sans). Gradius 2 ne termine pas 300 000 pas en 500 s (946 fourches pour 1 000 pas) ; cause non instruite.
   `--attract-frames` ralentit fortement le chemin MSX.
3. **Projection** : utile surtout sous `--unbounded` ; avec les cellules (défaut) la clé de cellule est plus grossière. Mesure sous `--unbounded` et sur d'autres ISA non faite.
4. `campaign_fixtures` à repasser sur 0.382.0 ; la note mémoire d'avancement (0.377.0 à 0.382.0) est à mettre à jour.
5. Parité contre les moteurs historiques : impossible à rejouer (références figées dans `tests/golden/`).

## Statut final (0.379.0)

Les moteurs historiques (GB, NES, PCE, MD, Sega/MSX) sont supprimés ; le noyau `dp_core_explore` est le seul moteur. Conséquences :
- **C2** (rejeu S6 commun), **C8** (travailleurs), **C10** (copies), **C4** (scans historiques), **P6** (reprise des cinq moteurs) et **P8** (sorties) sont résolus par construction : il n'y a plus de second code à reprendre ; la couche de rapport est unique (C7, 0.375.1).
- **B3** : points de contrôle de film et de session dans le même pool (source `SESSION`), cap de fourches par site remis à zéro par époque (0.378.0), rafales d'entrées depuis chaque point de contrôle de film (0.379.0).
- **C3** : un marcheur unique sur `DpIsa` ; les arêtes hors de la vue ROM sont comptées (`closure_unresolved`, 0.377.0).

Limites déclarées, techniques et non d'implémentation :
- **Vues commutables (C3)** : une arête qui sort de la fenêtre commutée vers une autre banque dépend du registre de banque au moment de l'exécution ; la fermeture statique ne peut que supposer la banque courante. Ces destinations sont couvertes dynamiquement (`bank_forks`, `--seed-banks`, fourches de banque) et la fermeture compte l'arête non résolue au lieu de l'ignorer.
- ~~C11 (projection de contexte)~~ : **limite levée** (0.381.0, décision du propriétaire) ; voir « État du plan (0.382.0) ».
- **Sega (B5)** : hachage d'état exact limité par le backend (voir P3).
- Les nombres produits sur ROMs réelles restent des observations de l'outil, pas des mesures de matériel.

Contrôles ajoutés au statut final : test `core_movie_bursts` (film SMS à 400 trames, un point de contrôle de film et ses deux rafales
d'entrées, observés au rapport). Observation de l'outil sur ROM réelle hors dépôt (Zelda Link's Awakening, GB, `--max-total 20000000`,
40 s) : 55 des 69 entrées du registre touchées par des chemins naturels, 229 arêtes statiques hors de la vue ROM comptées, 1369 fourches
résolues dont 74 promues du forcé, saturation par site 734570 rapportée. Ce ne sont pas des mesures de matériel. Le banc de parité
contre les moteurs historiques n'existe plus : ceux-ci sont supprimés, les références figées sont dans `tests/golden/`.

Relecture des lignes C2/C4/C8/C10 du tableau §C contre le code (0.379.1) : les symboles listés (`streams.c`, `zstm_*`, `nstm_*`, `pstm_*`,
`replay_streams`, `static_closure` NES/PCE, `md_static_scan`, `record_copy`, `log_copy`, `flush_copy`, `*/jobs.c`) n'existent plus ; il reste le
noyau (`obs_copy`, `infer_static`, workers du noyau). **Écart réel restant, C3** : `src/platform/sega/closure.c` (592 lignes, fermeture de
provenance Sega avec `--target-closure`/`--block-closure`) vit à côté du marcheur du noyau ; la fusion n'est pas faite. Option `--fork-site-cap N`
ajoutée (0.379.1) pour régler le cap par site ; le cap par époque de checkpoint (0.378.0) n'a pas de fixture propre : la fixture SMS actuelle ne sature
aucun site (`site 0`), il faut une fixture à boucle de conditionnels pour l'épingler.

### C3 : fermeture Sega, nature de l'écart (0.379.1)

`sega/closure.c` n'est pas un doublon du marcheur du noyau : c'est une analyse plus riche (contextes de vue, registres HL/IX/IY et pile,
résolution des retours, des sauts indirects et des lectures mémoire : `returns_resolved`, `indirect_resolved`, `memory_resolved`) que le
marcheur générique n'a pas. Les fusionner en supprimant l'une perdrait de la couverture Sega ; la bonne fusion est de **porter ces techniques
dans le marcheur du noyau pour toutes les ISA** (suivi de registres par `DpIsa`), puis de retirer `sega/closure.c` avec un banc de parité
octet à octet sur les ROMs de `m0boy` (SMS/GG/SG/MSX). C'est un chantier d'une phase à part, non commencé.
Le cap par époque de checkpoint est exercé par `core_movie_bursts` (une époque de film créée) mais pas isolé : une fixture à boucle de
conditionnels saturant un site reste à écrire.

Mesure de l'apport de `sega/closure.c` (observation de l'outil, 0.379.1, interrupteur temporaire retiré après coup ; `--attract-frames 600
--max-total 3000000`, ROMs SMS de `m0boy`, hors dépôt) : octets statiques avec/sans la fermeture Sega — Ace of Aces 12 459 / 10 428
(+2 031), Action Fighter (UE) 12 403 / 7 996 (+4 407), em-1 sans écart (7 996 / 7 996). La fermeture Sega apporte donc de la
couverture que le marcheur du noyau seul n'atteint pas sur certaines ROMs : la fusion exige le portage des techniques, pas la suppression.

Essai C3 rejeté (0.379.1, non livré) : rejouer toutes les racines de branchement exécutées sous chaque vue de mapper observée (instantanés par
signature de mapping, rejeu en fin de run). Observation de l'outil sur Ace of Aces, fermeture Sega coupée : 10 428 → 17 534 octets statiques, soit
bien plus que la fermeture Sega (12 459) : c'est du sur-marquage, une racine rejouée sous une vue où son adresse n'est pas du code, donc un faux positif
statique (témoins négatifs non classés exigés par P5). Sur Action Fighter l'essai n'apporte rien (7 996 → 8 073). Le gain de `sega/closure.c` sur Action
Fighter (+4 407) ne vient donc pas des vues croisées ; les statistiques de la fermeture Sega montrent `indirect_resolved 0` et `memory_resolved 0` sur cette ROM
(pas de suivi de registres en cause) et `returns_resolved` non nul : la piste restante est la résolution des retours (confiance après appel dont le
retour est établi) et les graines à chaque instruction observée sous la vue en vigueur. Non portée.

Localisation de l'écart Sega (Action Fighter UE, observation de l'outil, 0.379.1) : les octets que seule la fermeture Sega marque sont de grands blocs
contigus jamais atteints par le noyau (banque 4 : 1 058 octets à l'offset $098D, 866 à $10A8, 216 à $0FBD ; banque 2 : 1 004 octets à $0D01), précédés
d'octets non marqués. Les adresses $498D (fenêtre 1 de la banque 4) sont des pointeurs stockés dans la ROM (aux index $10017 et $1A0E4) : le noyau ne les
suit pas comme racines de code, la fermeture Sega le fait par ses cibles de registres (HL/IX/IY) et ses lectures de table. Suivre ces pointeurs sans
sur-marquer exige le contrôle de plausibilité de bloc de `--block-closure` (non porté). Ce portage reste le chantier C3 ouvert ; aucun octet
supplémentaire n'est revendiqué dans cette version.

Décomposition de la fermeture Sega par couche (observation de l'outil, 0.379.1, Action Fighter UE, options `--no-block-closure`,
`--no-target-closure`, `--no-closure`) : octets statiques seuls (hors natifs) — noyau seul 6 719 ; + fermeture de base Sega 8 542 (+1 823) ;
+ cibles `--target-closure` 8 632 (+90) ; + blocs `--block-closure` 11 104 (+2 472). Le gros du gain est donc le mode « blocs » : constantes
de registres (A, C, HL/IX/IY) propagées dans le bloc et **effets sur le mapper simulés pendant la marche** (`port_effect`, `store_effect`,
`address_effect`, `map_barriers` : un `ld a,n` suivi d'une écriture de registre de banque change la vue pour la suite de la marche). C'est le
point « indices de banque des vues commutables » de C3 : le marcheur du noyau garde la banque en vigueur. Voie de portage générique : un crochet
`DpIsa` « stocke A à une adresse absolue » (Z80 `32`, SM83 `EA`, 6502 `8D`, HuC6280 `TAM`) + `imm_byte` (existe, B9) pour la constante, le registre
de banque venant de `DpPlatform.io_regs` (catégorie banque) et le changement de vue de `DpMachine.bank_view`. Non commencé : c'est le dernier
chantier C3 et il ne se prouve qu'au banc octet à octet sur les ROMs SMS/GG/SG/MSX (zéro octet statique perdu, négatifs non classés).

Compteur d'époque (0.379.2) : le rapport indique « forks past the per-site cap thanks to a checkpoint epoch » (`epoch_forks`) : fourches mises en file par un
chemin d'époque alors que le cap par site aurait refusé hors époque. Observation de l'outil : 0 sur la fixture SMS (un seul checkpoint, aucun site saturé) et
0 sur Action Fighter avec un film de 1 500 trames (aucun checkpoint de film produit par ce film synthétique) ; l'effet du cap par époque n'est donc pas encore
démontré par une valeur non nulle. Il faut un film réel de la ROM ou une fixture à boucle de conditionnels.

Cap par époque démontré (observation de l'outil, 0.379.2, ROM GB réelle hors dépôt — Zelda Link's Awakening USA/Europe, `--max-total 20000000`) :
`epoch_forks` = 149 avec le cap par défaut (4) et 75 avec `--fork-site-cap 1` ; sessions attract/play, 2 checkpoints de session et 4 rafales. Ce sont des
fourches qu'un chemin issu d'un checkpoint a pu mettre en file alors que le cap par site les aurait refusées hors époque. Le compteur n'est pas une
mesure de couverture gagnée en octets ; l'effet sur les octets n'a pas été comparé avant/après.

Fixture du cap par époque (0.379.3) : `fix_epoch.gb` (boucle dont le conditionnel sature un site) + test `core_epoch_cap` (`--fork-site-cap 1`,
checkpoints de session) exige `epoch_forks` non nul. Observation de l'outil : 1 sur cette fixture.

### C3 : simulation des écritures de banque dans le marcheur du noyau (0.380.0)

Première tranche du portage : le marcheur statique du noyau suit `ld a,n` puis le stockage de A à une adresse qui programme le mapper (écriture dans la
fenêtre ROM, ou registre déclaré par `DpMachine.mapper_reg`, Sega `$FFFC-$FFFF`) en rejouant l'écriture à travers le mapper de la machine
(`DpMachine.poke`) : la suite de la marche voit la nouvelle vue ROM, l'état vivant est restauré à la fin de chaque marche, chaque racine en attente
mémorise la chaîne d'écritures sous laquelle elle a été empilée, et la continuation est aussi marchée sous la vue d'origine (additif). Crochets `DpIsa`
`acc_imm` / `st_acc_abs` pour SM83, Z80, 6502 et HuC6280 ; `--no-closure-banks` désactive ; compteur « closure bank writes simulated during walks ».
Test : fixture `fix_bankwalk` (`core_closure_bank_sim`).

Observation de l'outil (ROMs hors dépôt, `--jobs 1`) : la couverture finale (`--cov`) est **octet pour octet identique** avec et sans la simulation
sur Zelda Link's Awakening (60 écritures rejouées) et sur Action Fighter UE (5, fermeture Sega coupée). Cette tranche ne fournit donc aucun octet de plus sur
ces ROMs, et ne perd rien. Elle réfute l'hypothèse de la section précédente : l'apport « blocs » de `sega/closure.c` sur Action Fighter ne vient pas
des changements de vue de banque mais de la propagation de constantes de registres (HL/IX/IY, C) vers des cibles de saut, d'appel et de table. Cette
propagation reste à porter ; `sega/closure.c` n'est pas retiré.

### Relecture des sections B, D et E contre le code (0.380.0)

- **B1** `--probe-ctx-cap` (sondes ré-armées par contexte) ; **B2** promotion du forcé (`promoted_forced`, `solve_tries`) ; **B3** points de contrôle de film et de
  session, époques, rafales, graines vivantes (`live_seeds`) ; **B4** longueur de table prouvée et énumération `--unbounded` (`core.c` « B4 ») ; **B5** `exact_states` /
  `state_repeats` (Sega : limite technique du backend) ; **B6** report `sat_path` / `sat_loop` / `max_total` ; **B7** `bank_forks` et `--bank-fork-cap` ; **B8**
  désassemblage spéculatif (`spec`) ; **B9** `imm_refs` par `imm_ptr` / `imm_byte` ; **B10** graine SRAM et signatures apprises ; **B11** `DpObservers.on_copy`.
  Chacune a son compteur au rapport ; les options de désactivation existent sauf celles listées dans `GB.fr.md`.
- **D** : P0 à P8 sont soldées (statut final) ; l'ordre d'acceptation par fixtures est tenu par `tests/CMakeLists.txt` (165 tests) et `campaign_fixtures`.
- **E** : le banc de parité contre les moteurs historiques n'existe plus (références figées dans `tests/golden/`) ; la mesure sur ROMs réelles reste des
  observations de l'outil, sans comparaison avant/après à budget égal pour chaque technique (la courbe de rendement par source n'est pas produite).
- **Ouvert, à décider par le propriétaire** : (1) portage de la propagation de constantes de registres de `sega/closure.c` dans le noyau (chantier C3) ;
  (2) validation de la limite C11 (projection de contexte, raffinement d'élagage sans octet de couverture) ; (3) la courbe de rendement par technique (E).

Essai C3 rejeté (0.380.0, non livré) : cible de saut indirect déduite d'un `ld rr,nn` précédent (≤ 3 instructions) dans le marcheur du noyau. Observation de l'outil
sur Action Fighter UE (fermeture Sega coupée, `--jobs 1`) : 0 cible suivie, couverture `--cov` identique. Le gain de `sega/closure.c` ne vient donc ni des
changements de banque ni de `jp (hl)` après `ld hl,nn` ; la piste restante est la propagation des constantes vers des appels et des lectures de table
(`indirect_resolved`/`memory_resolved` valent 0 sur cette ROM) et les retours résolus (`returns_resolved`). Non portée.

Deux essais C3 de plus rejetés (0.380.0, non livrés ; Action Fighter UE, fermeture Sega coupée, `--jobs 1`, observations de l'outil) :
- **Retours résolus** : pile d'adresses de retour par chemin de marche, un `RET` dans un appel marché reprend l'appelant en confiance. 75 retours suivis,
  couverture `--cov` identique au marcheur seul : aucun octet de plus.
- **Confiance après appel à un appelé exécuté** : lever la règle A13 quand l'appelé est naturel. Statique seul 7 855 → 17 472, au-delà même de la fermeture
  Sega (11 104) : sur-marquage (données après appel), donc rejeté (témoins négatifs de P5).
Hypothèses éliminées pour le gain de `sega/closure.c` : changements de banque, `jp (hl)` après `ld hl,nn`, retours résolus, confiance après appel. Il
reste la propagation de constantes de registres vers les appels, les lectures de table et les blocs entiers ; elle se localise en instrumentant la fermeture
Sega (quelles instructions marquent les octets que le noyau n'atteint pas), travail non fait.

### Correction de méthode et de la simulation de banque (0.380.1)

Erreur de mesure des sections précédentes : `--cov` ne contient que les octets naturels et forcés (format de l'ancien moteur), pas le statique ; les
comparaisons « couverture identique » faites avec `--cov` ne disaient rien du statique. La mesure juste est l'ensemble des drapeaux `DP_CORE_STATIC`
sans `NATURAL`/`FORCED` (vidage des drapeaux du résultat, comparé octet par octet). Refaite : la simulation de banque de 0.380.0 **perdait** 1 073 octets
statiques sur Zelda GB (et en gagnait 159), parce que la marche simulée consommait le budget avant la continuation d'origine. 0.380.1 : la marche continue
d'abord sous la vue d'origine (exactement le marcheur simple), la continuation sous la vue simulée passe après, avec un budget propre (`CLOSURE_INSTR/8`).
Observation de l'outil sur Zelda (60 écritures rejouées, `--jobs 1`) : 0 octet statique perdu, 0 gagné. La simulation est donc neutre sur cette ROM ; elle
n'a pas de gain démontré (les aides de bascule de banque se terminent par `ret` avec la banque en paramètre, sans constante visible). Les essais rejetés
« retours résolus » et « cibles par `ld rr,nn` » avaient eux aussi été jugés sur `--cov` : le premier reste sans effet sur le compteur de statique (7 855
inchangé), le second n'a suivi aucune cible ; leurs verdicts sur le statique sont à prendre avec cette réserve.

### Essai C3 rejeté : `jp (hl)` par constante `ld hl,nn` (0.380.1)

Observation de l'outil (Action Fighter UE, `--attract-frames 600 --max-total 3000000 --jobs 1`, fermeture Sega coupée par `DEEPEEL_NO_SEGA_CLOSURE=1`) : le
marcheur du noyau, doté d'un suivi de HL par `ld hl,nn`, ne résout qu'**un** saut `jp (hl)` ; octets statiques seuls inchangés (7 855). Retiré. Localisation
du gain de `sega/closure.c` : la fermeture est amorcée à chaque pas exécuté avec l'état concret de la CPU (pc, sp, HL/IX/IY/C, vue de banque) ; ses blocs
propres (plusieurs centaines d'octets, sans `call`/`jp` absolu qui les désigne en ROM) ne viennent donc pas des `jp (hl)` à constante visible. Candidats non
vérifiés : retours résolus par la pile concrète, tables lues en ROM. `sega/closure.c` reste en place tant qu'un banc octet à octet SMS/GG/SG/MSX ne prouve
pas l'équivalence.

### Courbe de rendement par budget (section E, observation de l'outil)

Zelda GB (ROM hors dépôt), `--attract-frames 300 --jobs 1`, trois budgets. Compteurs du rapport texte ; ce sont des observations de l'outil (machine
concrète, RAM à zéro), pas des mesures de matériel. Les octets « nouveaux » du choix par nouveauté et du choix simple sont ceux que le rapport attribue à
chaque mode.

| budget (pas) | chemins | fourches | résolues (solved) | rejeux d'index | forks RNG | forks IRQ | nouveauté / simple (octets) | statique seul |
|---|---|---|---|---|---|---|---|---|
| 300 000 | 480 | 634 | 1 | 0 | 0 | 0 | 2 535 / 4 748 | 239 |
| 1 000 000 | 1 123 | 664 | 189 | 128 | 244 | 3 370 | 2 508 / 4 510 | 6 283 |
| 3 000 000 | 3 740 | 2 596 | 670 | 698 | 344 | 9 675 | 21 949 / 8 619 | 51 235 |

Lecture : les techniques à état (résolution, rejeu d'index, RNG, IRQ) ne produisent qu'à partir d'environ 10⁶ pas ; la fermeture statique domine l'octet
gagné entre 10⁶ et 3·10⁶ pas (6 283 → 51 235), et le choix par nouveauté dépasse le choix simple seulement au plus grand budget. Ce n'est pas une courbe
par technique à budget constant : l'attribution fine (retrait d'une technique à la fois) reste à mesurer et n'est pas faite ici.

### Ablation par technique à budget constant (section E, observation de l'outil)

Zelda GB (ROM hors dépôt), `--attract-frames 300 --max-total 1000000 --jobs 1`, une technique coupée à la fois (une exécution chacune, déterministe).
Référence « tout » : connu 35,309 % (naturel 6 708, forcé 307, statique 6 283, copié 11 662).

| coupé | connu % | naturel | forcé | statique | copié |
|---|---|---|---|---|---|
| (rien) | 35,309 | 6 708 | 307 | 6 283 | 11 662 |
| `--no-solve` | 36,611 | 6 826 | 313 | 6 161 | 11 667 |
| `--no-index-replay` | 35,309 | = | = | = | = |
| `--no-rng-learn` | 35,309 | = | = | = | = |
| `--no-learn-sig` | 35,253 | 6 724 | 291 | 6 283 | 11 665 |
| `--no-site-score` | 35,799 | 5 415 | 2 921 | 5 506 | 11 696 |
| `--no-event-probes` | 35,308 | 6 728 | 287 | 6 283 | 11 664 |
| `--no-closure` | 33,517 | = | = | 0 | = |
| `--no-taint-cut` | 35,616 | 6 791 | 246 | 6 263 | 11 667 |
| `--no-vectors` | 35,309 | = | = | = | = |

Lecture, à ce budget et sur cette ROM seulement : la fermeture statique est la seule technique qui rapporte nettement (+1,8 point). Index, RNG et vecteurs
sont sans effet à 10⁶ pas (leurs compteurs sont nuls ou trop tardifs, cf. la courbe par budget). **Observation à instruire** : couper la résolution
(`--no-solve`) ou le choix par site (`--no-site-score`) *augmente* le connu à ce budget (+1,3 et +0,5 point) ; la résolution et le score de site
dépensent des pas qui ne rapportent qu'au-delà de 10⁶ (cf. la courbe : 670 fourches résolues à 3·10⁶). Ce n'est pas une preuve qu'elles nuisent à budget
infini, seulement qu'elles ne sont pas rentables à ce budget. Le total « connu » ne départage pas naturel et forcé : `--no-site-score` déplace 2 600 octets du naturel vers le forcé.

### C3 : le gain réel de la fermeture Sega est de 85 octets (observation, Action Fighter UE) — corrige les sections précédentes

Mesure déterministe (`--attract-frames 600 --max-total 3000000 --jobs 1`, vidage des drapeaux du résultat, comparaison octet à octet) : avec la fermeture
Sega 7 940 octets statiques seuls, sans elle (`DEEPEEL_NO_SEGA_CLOSURE=1`) 7 855 ; **85 octets propres à la fermeture Sega, 0 propres au noyau**, en cinq
plages (30, 24, 23, 7 et 1 octets). Les écarts de plusieurs milliers d'octets relevés plus haut venaient de runs sans `--jobs 1` (exploration non
déterministe, mélangée à l'effet de la fermeture) et ne mesuraient pas la fermeture. Le port des pistes (retours par la pile concrète, IX/IY/BC) vise donc
au plus ~1 % de statique sur cette ROM ; il reste justifié par la règle de couverture, mais n'est pas le gain massif supposé. Pas de retrait de
`sega/closure.c` sans banc octet à octet sur SMS/GG/SG/MSX ; les 85 octets sont la cible chiffrée de ce banc. Non fait.

### C3 : provenance des 85 octets (instrumentation temporaire, retirée)

Sur Action Fighter UE (`--jobs 1`), les plages propres à la fermeture Sega sont atteintes par **fall-through et branches directes** d'un nœud qui suit une
instruction déjà connue (`$1051` → `$1054`, `$6FA4` après un `call`), et non par un saut indirect ni une table : l'écart avec le noyau est la règle A13
(confiance après `call`). Le noyau, prudent par défaut, s'arrête au fall-through d'un appel ; la fermeture Sega le suit en comptant sur son suivi de pile.
L'essai « confiance après appel » du noyau (7 855 → 17 472) sur-marque, donc le port exige la **condition de retour** de la fermeture Sega (suivre le
fall-through d'un appel seulement si le retour est établi par la pile concrète), pas un simple relâchement. Port non fait ; banc octet à octet requis.

### Ablation par technique à trois budgets (section E, attribution complète, observation de l'outil)

Zelda GB (ROM hors dépôt), `--attract-frames 300 --jobs 1`, une exécution déterministe par case. Colonnes : connu %, octets naturel / forcé / statique / copié.
La ligne 10⁶ pas est la table précédente (même ordre de colonnes : naturel, forcé, statique, copié).

| coupé | 3·10⁵ pas | 10⁶ pas | 3·10⁶ pas |
|---|---|---|---|
| (rien) | 34,158 (309/6 974/239/5 162) | 35,309 | 71,321 (24 743/5 690/51 230/12 699) |
| `--no-solve` | 34,158 | 36,611 | 59,469 (18 079/3 693/43 985/12 780) |
| `--no-index-replay` | 34,158 | 35,309 | 71,321 |
| `--no-rng-learn` | 34,158 | 35,309 | 71,321 |
| `--no-learn-sig` | 34,158 | 35,253 | 60,032 (19 355/4 156/40 244/12 763) |
| `--no-site-score` | 33,955 | 35,799 | 36,098 (8 183/2 549/3 450/12 158) |
| `--no-event-probes` | 34,158 | 35,308 | 68,853 |
| `--no-closure` | 34,158 | 33,517 | 65,116 (statique 0) |
| `--no-taint-cut` | 33,784 | 35,616 | 39,549 (10 984/2 286/12 026/12 099) |
| `--no-vectors` | 33,424 | 35,309 | 71,321 |

Lecture (cette ROM, ce protocole) : **à 3·10⁶ pas, quatre techniques pèsent beaucoup** — le choix par site (−35 points), la coupe de taint (−32), la résolution
(−12) et les signatures apprises (−11) ; la fermeture statique −6 ; les sondes d'événements −2,5. Le rejeu d'index et l'apprentissage RNG restent à 0 sur
cette ROM (compteurs 128 à 698 et 244 à 344 mais sans octet nouveau : leur apport sur Zelda est nul, pas leur utilité en général). À 10⁶ pas les mêmes
techniques paraissaient inutiles voire nuisibles : leur rendement est **différé**, ce qui fonde la décision de ne pas les juger à petit budget. Les
vecteurs comptent à 3·10⁵ pas (−0,7) puis disparaissent (déjà atteints par les chemins naturels). Un seul run par case : pas d'intervalle de confiance, la
différence de 10⁶ à 3·10⁶ sur `--no-site-score` (+0,5 puis −35) montre qu'un budget intermédiaire peut inverser le signe.

### Essai C3 rejeté : fall-through d'appel de confiance si l'appelé est du code connu

Action Fighter UE (`--jobs 1`, fermeture Sega coupée) : 7 855 → 17 472 octets statiques seuls (le même chiffre que l'essai « confiance après appel » plus
haut, donc l'appelé connu n'apporte aucun filtre). +9 600 octets contre +85 pour la condition de retour de la fermeture Sega : sans vérité terrain (code vs
données), le surplus ne peut pas être accepté. Retiré. La condition de retour reste à écrire à partir de la pile concrète de la fermeture Sega ; non faite.

### C3 : banc comparatif de la fermeture Sega sur quatre ROMs (observation de l'outil)

`--attract-frames 300 --max-total 1500000 --jobs 1`, ligne « static closure » du rapport texte, avec puis sans `DEEPEEL_NO_SEGA_CLOSURE=1` (ROMs hors dépôt).
Les nombres sont les octets statiques seuls ; une exécution par case. Ce banc compare des totaux, **pas** des ensembles d'octets : il ne remplace pas le
banc octet à octet exigé avant tout retrait de `sega/closure.c`.

| ROM | sans | avec | écart |
|---|---|---|---|
| Ace of Aces (SMS) | 8 584 | 9 548 | +964 |
| Addams Family (GG) | 8 156 | 8 919 | +763 |
| Aerial Assault (GG) | 13 518 | 16 321 | +2 803 |
| Bank Panic (SG) | 1 277 | 1 891 | +614 |

Corrige l'estimation de la section précédente : les 85 octets d'Action Fighter UE étaient un cas bas ; sur ces quatre ROMs la fermeture Sega ajoute de 7 à 48 %
de statique seul. MSX (Aleste, Gradius 2) : non mesuré, la mesure a été interrompue par le système (mémoire basse) ; à refaire sans autre charge. Le port de
la condition de retour porte donc un enjeu réel de couverture, pas marginal.

MSX (mise à jour) : Aleste (`--attract-frames 300 --max-total 200000 --jobs 1`) n'a pas terminé en 120 s (arrêt par `timeout`, aucun rapport écrit), alors
que les ROMs SMS/GG/SG font 1,5·10⁶ pas dans ce temps. Le banc MSX est donc bloqué par un ralentissement propre au chemin MSX (cause non cherchée), pas
seulement par la mémoire. À instruire avant tout banc sur cette plateforme.

MSX (correction) : le ralentissement venait de `--attract-frames 300` sur le chemin MSX, pas des pas eux-mêmes ; sans cette option Aleste termine (1,5·10⁶ pas,
`--jobs 1`). Aleste : 7 094 octets statiques seuls **avec et sans** `DEEPEEL_NO_SEGA_CLOSURE=1`, donc la fermeture Sega n'agit pas sur MSX (le noyau
générique seul). Gradius 2 : mesure **invalide** (même rapport qu'Aleste, le fichier du run précédent n'a pas été réécrit, le run a dépassé `timeout 200`) ; à refaire.

## C11 : projection de contexte (0.381.0, décidée par le propriétaire)

Le propriétaire a refusé la limite déclarée : la projection sert à évincer des parties déjà faites et à accélérer la recherche.
Implémentée : `DpMachine.ctx_reg_hash` (registres CPU + mapper, sans horloges ni RAM ; GB, Sega/MSX, NES, PCE, MD) et `proj_key` dans le
noyau : clé d'une fourche = cible + hachage des registres + octets des pages de RAM que le code de la page ROM du site et de la page
de la cible a été vu lire (bitmap `live`). Une fourche déjà mise en file sous la même projection est évincée (`projection-pruned forks`
au rapport ; `--no-proj` la coupe). Limite honnête : sans décodage des opérandes, tous les registres restent dans la clé (seule la RAM
non lue est écartée). Observation de l'outil (Ace of Aces SMS, `--no-cells --jobs 1`, 3·10⁵ pas) : 1 618 fourches évincées, mêmes
octets naturels (468), forcés (3 270) et statiques seuls (9 411) qu'avec `--no-proj`. Avec les cellules (défaut), la clé de cellule est plus
grossière et la projection ne ferait rien sur Sega ; elle sert surtout sous `--unbounded` (cellules coupées). Gradius 2 (MSX) ne termine pas
300 000 pas en 500 s (946 fourches pour 1 000 pas) : mesure non obtenue, à instruire.

## C3 : pile de retours dans la marche du noyau (0.382.0)

La marche statique du noyau (`closure_run`) suit désormais une pile de retours fantôme par chemin (8 niveaux) : un appel direct dont la callee
est inconnue pousse son adresse de retour ; quand la callee atteint un `ret`, la marche reprend chez l'appelant avec la confiance du site d'appel
et traverse (sans consommer le budget, une fois par adresse, 4 096 au plus) le code que le fall-through aveugle avait déjà marqué. Le fall-through
historique reste (additif). Un `ret cc` pousse la branche de retour et continue. `--no-closure-ret` la coupe ; `returns followed` au rapport.
Banc (mêmes réglages que ci-dessus, statique seul, observations de l'outil, `DEEPEEL_NO_SEGA_CLOSURE=1` sauf dernière colonne) :

| ROM | sans pile | avec pile | avec pile + Sega |
|---|---|---|---|
| Ace of Aces (SMS) | 8 584 | 9 749 | 10 713 |
| Addams Family (GG) | 8 156 | 8 229 | 8 992 |
| Aerial Assault (GG) | 13 518 | 12 739 | 15 746 |
| Bank Panic (SG) | 1 277 | 1 281 | 1 891 |

Bilan honnête : le noyau seul gagne sur 3 ROMs (+1 165, +73, +4) et **perd 779 octets sur Aerial Assault** (cause non instruite) ; la fermeture Sega
apporte encore 764 à 3 007 octets au-dessus (amorçage à chaque pas avec l'état concret, `jp (hl)`/IX/IY). C3 n'est donc **pas terminé** : `sega/closure.c`
reste en place, aucun banc octet à octet n'a été fait, et sans vérité terrain code/données le sur-marquage ne se juge pas.

### C3 : la perte d'Aerial Assault n'est pas une question de budget (observation de l'outil)

Aerial Assault (GG), statique seul, `DEEPEEL_NO_SEGA_CLOSURE=1`, budget de marche (`CLOSURE_INSTR`) à 4 096, 16 384 et 65 536 : 13 518 sans la pile de retours et
12 739 avec, **identiques aux trois budgets**, « capped walks 0 » partout. L'hypothèse du budget est écartée. Seule différence visible : 29 racines sans la pile, 28 avec.
Piste non vérifiée : l'ordre des marches. Une racine déjà marquée est sautée ; si une marche à retour l'atteint la première et s'arrête sur une heuristique de données
(confiance 0), la marche de confiance qui en aurait tiré davantage est sautée. À instruire (journaliser la racine sautée).

### C3 : ce que perd réellement la pile de retours sur Aerial Assault (observation de l'outil, 0.382.0)

Dump des drapeaux octet à octet (statique seul, `--jobs 1`, sans fermeture Sega) avec et sans `--no-closure-ret` : 1 080 octets statiques seuls perdus, 301 gagnés
(solde −779). Les plages perdues sont des **données** : `0xb172-0xb2d4` et `0xb71c-0xb73a` (suites d'indices de tuiles `20 20 24 25 01 02 03…`),
`0xde2b-0xdfd4` et `0xde03-0xde14` (motifs de tuiles `ff 55 55 00 ff 6a 6a 00…`), `0x1e0b7-0x1e11c`, `0x1f797-0x1f7f9`. Seuls 4 octets (`0x1717-0x171a`, du code
`3a bf c0 …`) sont du vrai code perdu. Les gains (`0x149b-0x152e`, `0x1de03-0x1de91`, `0x1ffe8-0x1fff1`) sont de vraies plages à confirmer. La « perte » est donc
surtout du **sur-marquage retiré** : la marche aveugle d'avant décodait des tuiles comme du code. Cause du retrait : la racine `0x105` (`rst $18` / `jp (hl)`) était déjà
marquée par une marche à retour, l'ordre des marches change. Cela corrige l'interprétation du banc : l'écart d'octets statiques ne mesure pas la couverture de code, et
« moins d'octets » peut être « moins de faux code ». La vérité terrain reste absente ; ce jugement repose sur la lecture des octets de quelques plages.

**Fourche statique (à faire)** : amorcer une marche à chaque pas exécuté avec les valeurs concrètes de HL/IX/IY (ce que fait `sega/closure.c`) demande un accès générique
aux registres (`DpMachine` n'expose aujourd'hui qu'un hachage, `ctx_reg_hash`) : une primitive `ctx_reg(m, i)` par machine, puis un état de marche porteur de ces valeurs
pour `jp (hl)`, `jp (ix)` et `ret` de pile concrète, avec un plafond de valeurs distinctes par site. Non commencée.

### Vérification 0.382.0
`campaign_fixtures` verte sur 0.382.0 (1217 s). La fourche statique (valeurs concrètes HL/IX/IY, propagation de constantes de `sega/closure.c`) reste à porter : les cibles
de `jp (hl)` atteintes par l'exécution sont déjà marchées, les tables de pointeurs proches d'un `ld hl,nn` sont déjà lues (`closure_jp_table`) ; le reliquat est la
propagation de constantes à travers des chaînes d'instructions, qui demande une primitive d'accès aux registres par machine et un plafond de valeurs par site.

## Exigence : zéro donnée déclarée code (décision du propriétaire, 2026-11-16)

Une donnée déclarée code est une **erreur**, et un état « viable » n'en contient pas. Le critère d'évaluation du statique n'est donc plus « plus d'octets » mais « aucune
donnée marquée code » ; un octet statique seul est **provisoire** tant qu'il n'est pas prouvé.

**État mesuré (Aerial Assault GG, 0.382.0, outil)** : la marche statique marque encore des données comme code, avec ou sans pile de retours (tuiles `20 20 24 25 01 02 03…`,
motifs `ff 55 55 00…`, tables `02 fd 00 00…`, `e4 03 f3 e5 02 f3…`). La pile en retire une partie (−779 net) et en introduit d'autres (`0x1de03-0x1de91`, `0x1ffe8-0x1fff1`).

**Essai de correctif rapide, retiré** : rendre illégal, pour la marche, un préfixe DD/FD suivi d'un opcode qui ne touche ni HL/H/L ni IX/IY (`fd 00` = no-op non documenté,
jamais du code utile). Règle saine, mais le résultat est **chaotique** : −401 octets statiques seuls, dont des plages de données qui reviennent (`0xde2b`, `0x1e0b7`, `0x1f797`)
parce que l'ordre des marches change qui marque quoi. Sans vérité terrain on ne peut pas dire que le solde est meilleur ; non conservé.

**À faire (ordre)**
1. **Vérité terrain** : par ROM de banc, fichier hors dépôt `code/données` octet à octet (désassemblage manuel d'une ou deux ROMs SMS/GG, ou ROM issue d'un source connu),
   et un outil `tests/` qui compte « données marquées code » (erreurs) et « code manqué » ; le banc 4 ROMs les rapporte à côté des octets.
2. **Classe provisoire** : les octets marqués par une marche non fiable (confiance 0, arrivée par pile de retours, par chute après CALL) reçoivent un drapeau `PROVISIONAL`
   distinct de `STATIC` ; seul un octet atteint par une marche de confiance, ou confirmé par exécution, ou par une arête prouvée, compte comme code dans les rapports.
3. **Contrôle de vraisemblance à l'entrée d'une marche** : refuser une racine dont les N premières instructions sont des no-ops préfixés, des `rst $38` répétés, des
   `ld (bc),a` consécutifs, des suites d'opcodes sans transfert de contrôle, ou dont les octets sont dans une plage déjà reconnue comme tuiles/pointeurs/texte (la même
   détection que `core_ptr_stop` et `core_pad_stop`, appliquée en entrée et non seulement en sortie).
4. **Ordre des marches déterministe** : marches de confiance d'abord (racines exécutées, cibles de CALL/JP de code exécuté), non fiables ensuite, pour qu'un octet ne soit
   plus classé selon l'ordre d'arrivée.

## Portage générique du comportement de `src/platform/sega/closure.c`

Comportement actuel (Sega seulement, retiré seulement quand le générique l'égale **sans erreur de données**) :
- **Amorçage à chaque pas exécuté** avec l'état CPU concret (`DEEPEEL_NO_SEGA_CLOSURE=1` le coupe) : un nœud par (pc, banque, registres connus).
- **Nœuds à constantes connues** : HL, IX, IY (puis BC, DE, `c` en mode bloc, `a`, `sp`) avec un masque « octet haut / octet bas connu » (`target_known`) ; hachage et
  déduplication des nœuds (`mix` sur pc, banque, masque, valeurs) ; plafond de nœuds.
- **Propagation de constantes instruction par instruction** (`target_constants`, `target_constants_basic`) : `ld r,n`, `ld rr,nn`, `ld r,r'`, `inc/dec` d'octet et de paire,
  `add hl,rr`, `exx`, `ex de,hl`, `djnz` sur `b`, effets de bord sur la pile ; une instruction inconnue **efface** le registre concerné (jamais deviné).
- **Résolution des sauts indirects** (`indirect_resolved`) : `jp (hl)`, `jp (ix)`, `jp (iy)` quand la paire est entièrement connue.
- **Pile d'adresses de retour fantôme** (`returns_resolved`) : un `ret` dont l'adresse est connue par la pile concrète est suivi.
- **Mode bloc** : suit les copies `ldir/lddr` et les boucles `djnz` pour ne pas perdre les constantes sur BC/DE.
- **Fusion** : le résultat est réintégré aux drapeaux du noyau (hook `finish`).

Version générique à écrire dans `src/explore/` (pas dans la plateforme) :
- `DpIsa` : une fonction `effect(bytes, pc, DpRegState *in, DpRegState *out)` par ISA (SM83, Z80, 6502, HuC6280, 68000) qui dit quelles constantes survivent ; la table par
  instruction vit dans l'ISA, le propagateur est unique. Valeur absente = inconnue (rien deviné).
- `DpMachine` : primitive `get_regs(m, DpRegState *)` (registres nommés par la plateforme, masque de connaissance complet à l'amorçage) à la place du seul `ctx_reg_hash`.
- État de marche porteur de `DpRegState` ; pour un site indirect, **plafond de valeurs distinctes par site** (comme le plafond de fourches dynamique), compteur rapporté,
  jamais silencieux ; sous `--unbounded` le plafond est levé et l'excédent va au spool.
- La marche générique n'est acceptée que si l'**exigence zéro donnée code** ci-dessus est mesurée, par ROM, sur la vérité terrain : égalité ou mieux en code trouvé,
  **et** pas plus d'erreurs que la fermeture Sega. Seulement alors `sega/closure.c` est retiré (règle : couverture jamais perdue, erreurs jamais ajoutées).

## C3 : état de registres générique dans le marcheur du noyau (0.383.0, première tranche)

Le portage est écrit : le marcheur statique du noyau porte un état de registres (`DpRegState`,
`src/core/dpmachine.h` : 24 slots de 4 octets, masque de connaissance par octet, traité comme opaque
par le noyau — hachage et égalité) et chaque pas de marche fait avancer les constantes par la table de
l'ISA (`DpIsa.reg_effect`), la machine fournissant l'état concret à l'amorçage (`DpMachine.get_regs`).

Conformément à la spécification ci-dessus :
- **`DpIsa.reg_effect(bytes, pc, in, out, rom)`** par ISA : Z80 via le moteur de provenance partagé
  (`src/cpu/z80/provenance.c`, le même que `target_constants` de la fermeture Sega, lecture planifiée
  prouvant l'opérande explicite seule), avec re-dérivation exacte de ce que le moteur tue mais la
  fermeture Sega prouve elle-même (inc/dec de paires et d'octets, DJNZ, pas des blocs ED) ; SM83
  (`ld r,n`, `ld rr,nn`, `ld r,r'` avec lecture `(hl)` prouvée, pas de pointeur, `add hl,rr` exact,
  inc/dec, pop tue) ; 6502 (immédiats, transferts, inc/dec, chargements mémoire X/Y, liste de
  non-écriture documentée) ; HuC6280 (sous-ensemble 6502 + `tam`/`tma`/`clx`/`cly` sur MPR0-7) ;
  68000 (`moveq`, `lea`, `movea`, `move #`, `addq/subq`, `clr`, `exg`, `swap`, `move <ea>,reg`,
  taille respectée). Tout ce qui n'est pas modélisé **tue** le registre écrit (jamais deviné) ;
  non écrit = passe ; écrit par valeur prouvée = constante posée. `DpFlowInfo.pair` désigne le
  registre d'un transfert indirect par registre (`jp (hl)/(ix)/(iy)`, `jp hl`, `jmp/jsr (An)`).
- **`DpMachine.get_regs(m, DpRegState *)`** : câblé sur Sega/MSX (Z80 complet A,F,B,C,D,E,H,L +
  ombres), GB (SM83), NES (A,X,Y,S via `dp_nesmach_regs`), PCE (A,X,Y,S + MPR), MD (D0-D7/A0-A7).
  La machine libretro n'en a pas (limitation documentée, l'amorçage registre n'y est pas actif).
- **Amorçage à chaque pas exécuté** dont le flot n'est pas séquentiel : marche non fiable (règles
  de données conservées) qui traverse le code déjà connu (4096 pas au plus), portant les constantes ;
  c'est l'amorçage de la fermeture Sega, générique. Déduplication globale par (octet ROM, hachage
  d'état) 64K, plafond de 4 états distincts par site, plafond de 8 valeurs indirectes résolues par
  site ; chaque saturation **comptée au rapport** (« closure registers (C3): seed walks …, indirect
  resolved …, indirect unresolved …, indirect site cap …, seeds refused (site cap …, set full …) »),
  jamais silencieuse ; `--no-closure-regs` coupe tout ; `--unbounded` lève les plafonds.
- **Résolution indirecte** : `jp (hl)/(ix)/(iy)`, `jp hl`, `jmp/jsr (An)` suivis quand la paire est
  entièrement connue (cible dans la vue ; hors vue comptée `indirect unresolved`, jamais suivie).
- **Retours** : la continuation d'un retour garde les constantes de l'appelé (règle de la fermeture
  Sega, au-dessus de la pile de retours 0.382.0).

Fixture et test : `fix_z80_regclosure.sms` (`ld hl,($C000)` puis `jp (hl)` — la cible ne vit dans
aucun immédiat ni anneau de pc exécutés, seul le HL concret de la machine la prouve) + test
`core_closure_reg_z80` : « indirect resolved ≥ 1 » et ligne C3 absente sous `--no-closure-regs`.
Observation de l'outil sur la fixture : `seed walks 2, indirect resolved 1, indirect unresolved 0`.

Limites honnêtes de la tranche : les tables 6502/HuC6280/68000 sont des sous-ensembles sains
(alu, rotates, modes d'adressage indirect : tout tue, enrichissement après banc) ; le suivi de SP
concret (retours par la pile de la machine) n'est pas porté ; le mode bloc Sega (`ldir`/`djnz`
boucles) n'est partiellement couvert que par les pas ED du Z80 ; `reg_effect` ne prouve les
lectures mémoire que de l'opérande explicite de l'instruction (lecture planifiée, comme
`target_constants`). La réintégration des vues de contexte Sega (8 contextes) n'est pas portée.

**`sega/closure.c` n'est pas retiré** : l'acceptation reste le banc octet à octet SMS/GG/SG/MSX
contre l'exigence « zéro donnée déclarée code » (égalité ou mieux en code trouvé, pas plus
d'erreurs). Suite de tests verte sur 0.383.0 (166 tests, dont `campaign_fixtures`).

### C3 : banc octet à octet de la tranche registres (observation de l'outil, 0.383.0)

Mêmes réglages que le banc 0.382.0 (`--attract-frames 300 --max-total 1500000 --jobs 1`, ROMs hors
dépôt), vidage des cartes `map v1` (la classe `code-static` de la carte égale le « static-only » du
rapport, vérifié), trois configurations par ROM : noyau sans fermeture registres ni Sega
(`--no-closure-regs`, reproductit les totaux 0.382.0 « avec pile » : 9 749 / 8 229 / 12 739 / 1 281),
avec fermeture registres, avec fermeture registres + fermeture Sega. Statique seul :

| ROM | noyau seul | + registres | + registres + Sega |
|---|---|---|---|
| Ace of Aces (SMS) | 9 749 | 9 735 (−14, 1 plage 0x32171) | 10 713 |
| Addams Family (GG) | 8 229 | 8 229 (identique) | 8 992 |
| Aerial Assault (GG) | 12 739 | 13 234 (+523/−28) | 15 768 |
| Bank Panic (SG) | 1 281 | 1 277 (−4) | 1 891 |

**Propriété de sûreté mesurée** : sur les quatre ROMs, l'ensemble d'octets statiques seuls du noyau
à fermeture registres est un **sous-ensemble strict** de celui de la même run avec la fermeture Sega
par-dessus (0 octet « regs-only » ; Sega ajoute encore 978 / 763 / 2 534 / 614 octets). La marche
amorcée n'ajoute donc aucun marquage que la fermeture Sega ne fait pas déjà ; **`sega/closure.c`
reste en place** — le retirer perdrait ces 614 à 2 534 octets. C3 n'est pas terminé.

**Erreurs de données** (plages identifiées données du plan sur Aerial Assault, 0xb172-0xb2d4,
0xb71c-0xb73a, 0xde03-0xde14, 0xde2b-0xdfd4, 0x1e0b7-0x1e11c, 0x1f797-0x1f7f9) : la fermeture
registres y marque 348 octets, la fermeture Sega 378, et l'ensemble de la fermeture registres est
inclus dans celui de Sega (regs-only : 0). Elle ne rajoute donc **pas plus d'erreurs que la
fermeture Sega** sur ces plages ; elle en retire même une (0xaa7e-0xaa99, motifs nuls, marquée code
par le noyau 0.382.0, plus marquée avec les registres — la fermeture Sega est d'accord). Le gain
principal d'Aerial (+523) contient du vrai code (0x203c, 74 octets, écritures `ld (ix+d),r` sur la
RAM $C0xx) mais aussi des re-marquages de tuiles que Sega fait aussi — sans vérité terrain octet à
octet, ces octets restent **provisoires** (classe à venir, exigence zéro donnée code, point 2).

**Échelle des plafonds** (compteurs du rapport, jamais silencieux) : `seeds refused (site cap)`
vaut 309 079 (Ace), 235 473 (Addams), 578 489 (Aerial), 943 566 (Bank Panic) : le cap de 4 états
par site refuse presque toutes les amorces sur ROM réelle (l'amorce est tentée à chaque pas à flot
non séquentiel, l'état concret change quasi chaque pas). `indirect resolved` : 14 / 0 / 13 / 4 ;
`indirect unresolved` : 1 / 0 / 7 / 8 (cible hors vue, comptée, jamais suivie). L'enrichissement
de la tranche 2 (condition de retour par la pile concrète, plausibilité à l'entrée de marche) et
l'étalonnage de ce cap restent à instruire avant tout retrait.

### C3 : tranche 2 — retours par la pile concrète, corrections du transfert (0.384.0)

**Corrections de la tranche 1** (trouvées en relisant contre la fermeture Sega et en testant les
dérivations exactes) :
- **Z80** : les re-dérivations exactes (inc/dec de paires et d'octets, DJNZ, pas des blocs ED)
  lisaient la **sortie** déjà tuée par le moteur de provenance — inertes, la constante mourait au
  premier inc/dec (moins bien que Sega, qui dérive de `before`). Elles dérivent désormais de
  l'état d'entrée ; inc/dec sp exact ajouté (p==3).
- **Z80** : la fenêtre de la lecture planifiée `ld rr,nn` était décalée d'un octet (le moteur
  charge la paire depuis pc+1..pc+2 ; la fenêtre couvrait pc+2..pc+3) : l'octet bas de l'opérande
  n'était jamais prouvé, donc `ld hl,nn` ne donnait HL que sur l'octet haut.
- **SM83** : slot de paire faux pour inc/dec bc/de (`(op>>4)-1` : sous-débordement pour 0x03,
  mauvaise paire pour 0x13) et pour pop af/bc/de/hl (slots 11-13 hors énumération : la constante
  obsolète survivait au pop — sur-marquage silencieux). Le code rp du décodage est le slot
  (BC DE HL SP = 0-3 ; pop rp 3 = AF).

**Retours par la pile concrète** (`returns_resolved` de la fermeture Sega) : quand la marche
porte un SP entièrement connu (l'état concret à l'amorçage), un `ret` prend sa continuation à
l'adresse lue dans la pile de la machine — c'est la case des marches amorcées qui entrent dans
une routine sans avoir poussé son appel (la pile fantôme 0.382.0 ne couvre que les appels qu'elle
a elle-même poussés). Continuation non fiable (les gardes de données s'appliquent), cap par site
comme les résolutions indirectes, cible hors vue comptée jamais suivie, `ret cc` : côté pris
poussé, la marche continue au-delà. Le même SP connu fait du `pop rr` une constante prouvée
(la lecture planifiée répond pour les mots de pile). `DpIsa.sp_slot`/`sp_width` : z80 et sm83
câblés ; 6502/HuC6280/68000 restent à `sp_width` 0 (le `rts` 6502 ajoute 1 à l'adresse popée,
le 68000 poppe 4 octets — tranche suivante). Compteur « stack returns » au rapport, jamais
silencieux. Fixtures : `fix_z80_stackret.sms` + `core_closure_stackret_z80` ; tests dpmachine
(inc dérive de l'entrée, pop lit la pile).

**Banc** (mêmes réglages, statique seul, observations de l'outil) : Ace 9 735 → 9 735 (0),
Addams 8 229 → 8 229 (0), Aerial 13 234 → 13 263 (+29 : 0x7a6(3), 0x1e733(26) — la chaîne
`ld a,$0C / ld ($DE09),a …` y est du vrai code), Bank Panic 1 277 → 1 281 (+4). `stack returns`
suivis : 223 / 145 / 67 / 73 ; `indirect resolved` passe de 14/0/13/4 à 15/0/19/8 (des constantes
popées résolvent des `jp (hl)` restés hors de portée) ; `indirect unresolved` 1/0/7/8 → 0/0/4/0.
**Propriété de sûreté conservée** : l'ensemble du noyau à tranche 2 reste un sous-ensemble strict
de la même run avec la fermeture Sega (0 octet « regs-only » sur les quatre ROMs ; Sega apporte
encore 978 / 763 / 2 534 / 613). Suite verte 168 tests, dont `campaign_fixtures`.

Reste ouvert après cette tranche : le mode bloc Sega (ldir/djnz balayés, constantes BC/DE à travers
les boucles), le `port_effect` des sorties OUT (mappers MSX programmés par port), les vues de
contexte Sega (8 contextes par adresse/vue, ici le cap d'états par site joue ce rôle), les SP
6502/68000, et le cap d'amorces par site (refus comptés : 235 K à 943 K par ROM) à étalonner.
`sega/closure.c` reste en place (critère inchangé : banc octet à octet + zéro donnée déclarée code).

### C3 : le mode bloc Sega est déjà absorbé (0.385.0, relecture)

La relecture côte à côte de `src/platform/sega/closure.c` (mode bloc) et du portage générique
montre que le « mode bloc » de la fermeture Sega n'a **aucun mécanisme que le noyau n'ait déjà** :

- **Constantes BC/DE/C** : Sega les suit dans une seconde instance (la première les laisse
  volontairement inconnues, « the BC/DE refinement gets the remaining budget ») ; le noyau les suit
  dès l'amorçage (`DpMachine.get_regs` amorce A,F,B,C,D,E,H,L + ombres d'un coup) et `reg_effect`
  les maintient (`ld r,n` prouvé par la lecture planifiée, inc/dec exacts, pas des blocs ED,
  DJNZ).
- **ldir/lddr/cpir/cpdr** : Sega avance HL/DE/BC à travers les répétitions classées `DP_FLOW_COND`
  vers elles-mêmes ; le noyau z80 classifie déjà ces répétitions ainsi (`isa_flow`, `DP_Z80_REPEAT`)
  et `reg_effect` pose les mêmes pas (`zstep_pair` HL ±1, DE ±1 pour la copie, BC −1).
- **Le registre `c`** synchronisé avec l'octet bas de BC n'existe que pour servir `port_effect`
  (les OUT vers les ports de contrôle mémoire invalident la pile de retours) — c'est l'entrée
  « port_effect des OUT » de la liste ouverte, pas le mode bloc.
- **La seconde instance à budget restant** est une stratégie de budget Sega (d'abord HL/IX/IY,
  puis BC/DE sur le reliquat), pas un mécanisme : le noyau dépense son budget autrement (caps
  par site + ensemble de déduplication).

Le mode bloc quitte donc la liste ouverte ; elle devient : `port_effect` des OUT (MSX + contrôle
mémoire Sega), les vues de contexte Sega, les SP 6502/68000, et (voir ci-dessous) le diagnostic
des clôtures de données.

### C3 : tranche 3 — étalonnage du cap d'amorces (0.385.0, résultat négatif et diagnostic)

**Ce qui est écrit** :
- **Knob `--closure-seed-cap N`** (`DpCoreConfig.closure_seed_cap`, défaut inchangé : 4) :
  états d'amorce distincts acceptés par site ; 0 = sans cap ; `--unbounded` le lève. Test
  `core_closure_seedcap_z80` (la ligne C3 reste correcte sous cap 8 et cap 1).
- **L'ensemble de déduplication grandit** au lieu de refuser à 48 K entrées : réhaussé par
  rehachage ×2 jusqu'à `REG_SET_CEIL` (1 M entrées, plafond mémoire compté « set full », jamais
  silencieux). Le commentaire du code prétendait « the set grows » depuis 0.383.0 sans que le
  code grandisse — corrigé.

**Banc** (échelle des caps 4/16/64/256, noyau seul, statique seul, mêmes réglages ; obs. outil) :

| ROM | cap 4 | cap 16 | cap 64 | cap 256 | + Sega (réf. 0.384.0) |
|---|---|---|---|---|---|
| Ace of Aces (SMS) | 9 735 | 9 735 | 9 735 | 9 735 | 10 713 |
| Addams Family (GG) | 8 229 | 8 229 | 8 229 | 8 229 | 8 992 |
| Aerial Assault (GG) | 13 263 | 13 315 | 13 315 | 13 315 | 15 768 |
| Bank Panic (SG) | 1 281 | 1 281 | 1 281 | 1 281 | 1 894 |

**Résultat négatif net** : à parité Sega sur les caps (cap 256 = 128 amorces/groupe Sega, refus de
site à 0 : 88 728 marches amorcées sur Ace au lieu de 488), le statique seul ne bouge pas
(0 octet sur trois ROMs, +52 sur Aerial). L'échelle des amorces **n'est pas** l'écart restant.
Les +52 d'Aerial (cap ≥ 16) sont des **tuiles** (bitmaps `dc dc dc 00 00 6f…`, `38 3f c0 00 78 7f…`
à 0xa661/0xa86c) : des erreurs « donnée déclarée code ». Ils violent la propriété de sous-ensemble
(52 octets « regs-only ») — le défaut reste donc **cap 4** (propriété tenue : 0 « regs-only » sur
les quatre ROMs), le knob et la croissance restent pour `--unbounded` et les tranches suivantes.
Coût mesuré : 1 à 5 s par configuration, le temps n'a jamais approché le timeout.

**Diagnostic de l'écart restant** (978 / 763 / 2 534 / 613 octets Sega seuls à cap 256, diff
cartes `map v1` classe `code-static`) : le profil n'est pas des plages de code manquées mais des
**paires de 2 octets** (491 paires Ace, 382 Addams, 1 263 Aerial, 306 Bank Panic), en deux
familles :
1. **Du vrai code derrière les clôtures de données du noyau** : la marche non fiable du noyau
   s'arrête sur les marqueurs `DP_CORE_DATA` des chemins naturels (`closure_stopped_data`), Sega
   ne s'arrête que sur ses propres marqueurs. Exemple Ace 0x32171 : la chaîne
   `dd 77 01/02/05/09` (`ld (ix+n),a` — du vrai code, déjà identifiée comme la perte de la
   tranche 1) est marquée classe 3 par Sega et laissée inconnue puis **inférée donnée** (classe 9
   `data-inferred`) par le noyau ; de même les écritures de port GG d'Addams (`ed 79`, `d3 be`) et
   le bloc d'Aerial à 0x467. Où la clôture est fausse (du code lu comme donnée par un chemin
   naturel), Sega a raison.
2. **Des erreurs de Sega** : runs de zéros (Ace 0x3cbc3, 0x3cffd) et tables (Aerial 0x1efc2,
   BankPanic 0x7620) que Sega marque code. Où la clôture est juste, Sega se trompe.

L'écart ne se ferme donc ni par le budget ni par plus de mécanismes de constantes : il se tranche
par la **qualité des clôtures de données** — exactement l'exigence « zéro donnée déclarée code »
(vérité terrain, classe PROVISIONAL, vraisemblance). Tant que la vérité terrain n'existe pas,
ouvrir ces clôtures ajouterait les erreurs de la famille 2 avec celles, évitées, de la famille 1
en face. La suite ordonnée reste celle du point « zéro donnée déclarée code » ci-dessus
(vérité terrain d'abord). `sega/closure.c` reste en place.

## « Zéro donnée déclarée code » : points 2, 3 et 4 livrés (0.386.0)

**Point 2 — le provisoire distinct du statique.** Le marquage de la fermeture se splitte
selon la confiance de la marche : un octet atteint par une marche de confiance (arête
prouvée du code exécuté : cible de saut/appel, autre côté de conditionnel, table validée)
reste `DP_CORE_STATIC` ; un octet atteint seulement par une marche non fiable (chute après
CALL, retour par pile concrète) reçoit un drapeau séparé (`DpCoreResult.prov`), projeté en
classe `probable-code` de la carte (12) et compté à part au rapport (« provisional N »,
compté sur les tableaux : il disparaît dès qu'exécution, marche de confiance ou le pli Sega
le confirme). Un octet provisoire ne compte jamais comme code prouvé. Le pli de
`sega/closure.c` est inchangé : ses octets restent du statique complet (la référence du
propriétaire), et il promeut les provisoires qu'il confirme.

Propriété vérifiée octet à octet sur `fix_nes_closure` : les ensembles avant/après sont
identiques hors les 4 octets de retombée d'appel, qui passent de code-statique à
probable-code ; naturel, inféré et bourrage inchangés. Les fixtures de fermeture des cinq
ISA voient leurs octets de retombée d'appel (les seuls qu'elles marquaient) devenir
provisoires — pins mis à jour (`static-only 0, provisional 4/5`). `fix_nes_static` garde
5 octets statiques seuls (preuves) et 3 provisoires (retombées) : les deux familles
coexistent et se lisent séparément au rapport.

**Point 3 — vraisemblance à l'entrée d'une marche.** `closure_entry_is_data` vérifie les
premiers octets d'une entrée non fiable (racine de chute d'appel, continuation de chute en
cours de marche) contre la signature de données de l'ISA (`DpIsa.entry_data_sig`, câblée
z80 et sm83) : deux `rst $38` consécutifs (plaque de `$FF`), deux `ld (bc),a`/`ld (de),a`
consécutifs (écritures de tuiles par un pointeur qui n'avance jamais), préfixe DD/FD dont
l'opcode ne touche ni IX ni IY (no-op non documenté, jamais du code utile) ; 6502/HuC6280/
68000 sans signature pour l'instant. L'entrée refusée est comptée (« untrusted walk entries
refused (data plausibility, point 3) »), jamais silencieuse ; l'octet reste inconnu et
l'inférence le juge. Les gardes classiques (plaque de bourrage, mots pointeurs, drapeaux
`DP_CORE_DATA`) ne sont pas dupliquées : quand la première étape les toucherait, la marche
démarre et s'y arrête avec son propre compteur (`core_pad_stop`, `core_ptr_stop` restent
verts).

**Essai rejeté (consigné)** : la règle générique « huit instructions séquentielles sans
aucun transfert de contrôle » du point 3. Le banc l'a condamnée : du vrai code de feuille
en ligne droite (décalage déroulé `add hl,hl` ×N d'Ace of Aces à $2186, prologues
save/restore `exx`/`pop`, `ld a,($C100)`/`and a`/`jr z` d'Addams) décode exactement comme
ça et l'entrée refusée perdait du code démontrable (Ace : −121 octets dont la routine
`d9 e1 7e 87 23…` ; en cascade, le code derrière la refusée n'est plus atteint — Addams
0x126c). Retirée ; les signatures ISA seules refusent 91/94/43/0 entrées par ROM et les
plages de données caractéristiques (`ff ff`, `02 02`) restent couvertes par elles.

**Point 1 — l'outil de vérité terrain.** `tests/truth_check.py` audite une carte `map v1`
contre un fichier de vérité (`<lo>-<hi> c|d`, plages hex inclusives) : code trouvé/manqué,
et « données déclarées code » séparées en **prouvé** (naturel ou statique sur un octet de
données : l'erreur, critère de sortie) et **non prouvé** (forcé, inféré-code,
probable-code : des prétentions à juger, rapportées sans être fatales — un marquage forcé
est une exécution fabriquée, pas une preuve). La fixture `fix_z80_truth.sms` vient avec sa
vérité émise par le générateur lui-même (`fix_z80_truth.truth`) : un appel dont la retombée
plonge dans une plaque de tuiles `ld (bc),a` ; le test `core_closure_truth_z80` épingle le
refus d'entrée et l'audit « zéro erreur prouvée ». Les vérités des ROMs réelles du banc
restent à produire hors dépôt (désassemblage manuel d'une ou deux SMS/GG, ou ROM issue
d'un source connu) ; le banc les audite quand le fichier existe
(`/tmp/c3bench/bench4.sh`).

**Point 4 — ordre des marches.** Réalisé structurellement par le point 2 : un octet
provisoire est « connu » pour une marche non fiable (rien de neuf à y décoder — la marche
passe ou s'arrête comme sur du statique) et inconnu pour une marche de confiance (qui doit
l'atteindre pour le promouvoir). Un marquage provisoire ne bloque donc plus une racine de
confiance, et la classe d'un octet ne dépend plus de l'ordre d'arrivée. L'ordre
confiance-d'abord au site exécuté (cible avant chute) était déjà en place.

**Régression trouvée et corrigée le même jour.** La première version du point 2 laissait
chaque marche non fiable re-décoder les zones provisoires et ré-entrer chaque racine : sur
`fix_streams_bank.gb` la fermeture rejouait ses simulations de banque à chaque re-marche
et le run passait de 3,4 s à plus de 300 s (profil : `closure_run` → `dp_mach_op`, le poke
du mapper). Correctif : la règle « connu » ci-dessus. Carte identique octet à octet à
0.385.0 sur cette fixture, run revenu à ~3 s.

**Banc** (4 ROMs, `--attract-frames 300 --max-total 1500000 --jobs 1`, obs. outil ; noyau
seul puis avec la fermeture Sega ; le « marqué » du noyau = statique seul ∪ provisoire) :

| ROM | noyau : statique seul | provisoire | refus d'entrée | + Sega : statique / provisoire | noyau ⊆ Sega (0 = tenu) | Sega ajoute |
|---|---|---|---|---|---|---|
| Ace of Aces (SMS) | 530 | 10 668 | 91 | 1 494 / 10 668 | 0 octet noyau seul | 964 |
| Addams Family (GG) | 3 266 | 4 972 | 94 | 3 266 / 4 972 | 0 | 0 |
| Aerial Assault (GG) | 7 103 | 5 972 | 43 | 11 394 / 3 775 | 0 | 2 094 |
| Bank Panic (SG) | 14 | 1 273 | 0 | 626 / 1 269 | 0 | 608 |

**Effet zéro-donnée mesuré** (diff cartes noyau 0.385.0 → 0.386.0) : Aerial perd 2 743
octets marqués dont **2 671 reclassés données** — ce sont les plaques que le diagnostic de
la tranche 3 montrait en erreurs (`ff 00 03 ff ff…` à 0xdab6, `00 00 00 ff…` à 0xc89c,
motifs `f0 3f 3f 00` à 0x1d954, soupes `02 02` à 0x1fcba) — et en regagne 2 544 ailleurs ;
les 6 plages « connues données » du plan restent hors code (5 exactement, 0xb172-0xb2d4
encore marquée code : 349 octets, à juger sur la vérité terrain). Ace : 0 octet perdu,
+1 431 (le code retombée d'appel redevient marqué, en provisoire). Bank Panic : 3 octets
d'en-tête `7f 7f` perdus (maintenant inférés données). Addams : solde neutre (−766/+772,
dont 566 reclassés données) ; ~200 octets de vrai code derrière des refus en cascade ne
sont plus que de l'inférence-donnée — c'est la famille 1 du diagnostic (du vrai code derrière
une clôture), à rouvrir par la qualité des vérités, pas par le budget. La contribution
propre restante de `sega/closure.c` est 964 / 0 / 2 094 / 608 octets ; `sega/closure.c`
reste en place (critère inchangé).

## C3 : retours par la pile concrète sur 6502 et 68000 (0.387.0)

Le `sp_slot`/`sp_width` de 0.384.0 ne couvrait que z80 et sm83 ; le `rts` 6502 (adresse
popée + 1, push en pré-décrément) et le `rts` 68000 (4 octets, pile big-endian) restaient
hors modèle. Le hook `DpIsa.sp_ret(bytes, &width, &bias, &off)` décrit maintenant le pull
de chaque opcode de retour : largeur (le bump du pointeur), biais ajouté au mot (6502 : +1)
et décalage du mot au-dessus du pointeur (6502 : le mot de retour vit à S+1..S+2 — les
pushs pré-décrèment ; z80/68000 lisent à SP). Les masques sont séparés : `sp_addr_mask`
(les bits du registre qui adressent la pile : 6502 S est un index 8 bits dans la page
`sp_base` $0100 ; z80/sm83 16 bits à plat ; 68000 A7 32 bits) pour l'arithmétique, et le
masque de bits connus correspondant pour la garde. Les opcodes non modélisés (`rti` 6502
— il poppe aussi P ; `rte`/`rtr` 68000) ne suivent pas de continuation par pile (le
continuation fantôme générique et le fall-through historiques restent). HuC6280 : non
câblé — la page de pile dépend du mapping MPR courant, pas un `sp_base` statique.

Fixtures et tests : `fix_nes_stackret.nes` (`jsr $8010` ; le promeneur semé depuis le
`bne` du callee porte le S de la machine et le `rts` rejoint $8003 — obs. outil :
« stack returns 4 ») + `core_closure_stackret_6502` ; `fix_md_stackret.md` (`jsr $210.l`,
mot de retour lu big-endian 4 octets — « stack returns 2 ») + `core_closure_stackret_68000`.
La tranche ne change rien au z80 (banc 4 ROMs identique octet à octet : statique seul
530/3 266/7 103/14, provisoire 10 668/4 972/5 972/1 273). Il reste de la liste ouverte :
le `port_effect` des OUT (MSX + contrôle mémoire Sega) ; les vues de contexte Sega jouent
leur rôle par le cap d'états par site. `sega/closure.c` reste en place (critère inchangé :
banc octet à octet + zéro donnée déclarée code, vérité terrain d'abord).

## C3 : le `port_effect` des OUT est porté (0.388.0)

Le dernier mécanisme de la fermeture Sega que le noyau n'avait pas : un OUT vers un port de
contrôle mémoire peut masquer la pile RAM et la ROM cartouche, donc les adresses de retour
que la marche tenait pour prouvées. `DpIsa.out_port` (z80 : `out (n),a` — port immédiat
connu ; `out (c),a` et le non documenté `out (c),0` — port = C, connu seulement si C l'est)
et `DpMachine.port_control(port, port_known)` (le savoir plateforme : MSX — tout port peut
décoder un mapper, sauf un store connu qui rate la sélection de slot $A8 ; GG — $3E
MEMORY_CONTROL ; SMS/SG — les ports pairs sous $40 ; NULL = jamais de contrôle). Quand la
marche franchit un tel OUT, elle jette sa pile de retours fantômes (comptée : « ghost
returns discarded on memory-control OUT (port_effect) ») et efface son SP concret — la
continuation par pile concrète et les continuations fantômes ne partent plus d'adresses que
l'OUT peut rendre données. Exactement la sémantique de `port_effect`+`unknown_store` de
`sega/closure.c` (jeter, jamais simuler la nouvelle vue).

Fixtures et test : `fix_msx_portout.msx` — un callee avec `out ($A8),a` (les retours meurent :
3 jetés, le `ret` n'est pas suivi) et un callee avec `out ($99),a` VDP (inoffensif : le retour
par pile concrète est suivi, « stack returns 1 ») ; `core_closure_portout_msx` épingle les deux.
Banc 4 ROMs : 0 / 856 / 45 / 23 retours jetés (Ace n'en franchit aucun) ; **zéro octet de
perdu** (les continuations jetées étaient atteintes par d'autres chemins) et la propriété de
sous-ensemble reste tenue (0 octet noyau seul sur les quatre ROMs). Addams perd 325 retours
suivis (les `ed 79` à C inconnu sont suspects) ; Aerial déplace 20 octets du statique vers le
provisoire (les continuations fantômes portaient la confiance du site d'appel).

La liste ouverte de C3 est maintenant : la vérité terrain des ROMs de banc (propriétaire,
hors dépôt), puis le retrait de `sega/closure.c` au banc octet à octet ; les vues de contexte
Sega (8 contextes par adresse/vue) jouent leur rôle par le cap d'états par site. Les
mécanismes de constantes, de retours (pile fantôme + pile concrète, toutes ISA), de vues de
banque, de ports et de vraisemblance sont portés.

## Mesures des notes ouvertes (0.388.0, obs. outil)

**Gradius 2 (MSX) — cause instruite.** Le run `--max-total 300000` interrompu à 150 s montre
6 682 318 fourches « deux côtés couverts » élaguées et 2 017 357 hachages de cellule pour
31 071 chemins créés (4 290 fourches IRQ, 50 743 fourches de banque) : les pas n'avancent
plus, tout le temps va à l'évaluation et à l'élagage des fourches d'une boucle d'attente
(conditionnel aux deux côtés déjà couverts, rejoué à chaque pas — le contrat d'élagage est
respecté, mais son coût d'évaluation domine). Ce n'est pas un écart de couverture ; une
mémoïsation de la décision d'élagage par (site, projection) serait une optimisation, pas une
technique. La note « ne termine pas 300 000 pas en 500 s » s'explique.

**Projection sous d'autres ISA.** QuackShot (MD), `--no-cells --max-total 3000000` : 292
fourches au total, `projection-pruned forks 0` avec et sans `--no-proj`, runs identiques — à
ce budget la densité de fourches est trop faible pour que la projection morde ; elle reste
utile là où les fourches sont denses à projection identique (Ace of Aces : 1 618 évincées,
mesuré en 0.381.0). La mesure « sous `--unbounded` » n'a pas pu être prise : l'arrêt SIGINT
passe bien par le spool (`<texte>.state/` écrit : cells, coverage, disc, exact), mais la
sauvegarde complète de la frontière (65 549 états en attente × la taille d'état GB) a besoin
d'un espace disque égal à compt × état — sur le tmpfs d'essai l'écriture échoue (message
compté « failed to save complete kernel frontier », jamais silencieux) et le rapport texte
n'est pas produit. Le contrat d'arrêt/spool (`--unbounded`, E11) est audité depuis, sur un
disque de capacité suffisante : voir « Audit E11 » ci-dessous.

### Audit E11 (0.388.0 — disque suffisant, la partition Windows sert de tmp)

Le contrat d'arrêt/spool (`--unbounded`) jugé sur ROM réelle (Zelda — Link's Awakening FR,
GB, sessions par défaut, spool et état sur la partition Windows via `TMPDIR` + `--state-dir`),
deux allers-retours complets :

1. **Arrêt SIGINT** : frontière complète sauvée — « frontier: 0 states resumed, 262131 saved »,
   `frontier.bin` de 6 603 866 295 octets = 12 + 262 131 × 25 193 au octet près (état
   GB = 25 177 octets), dont 196 595 déversés puis relus du spool (`refused 0`),
   `pending_dropped 0`, rapport texte produit, sortie 0, aucun message d'échec. L'échec tmpfs
   de l'essai précédent (échec compté, rapport absent) ne se reproduit pas : la capacité
   était le seul facteur.
2. **Reprise `--resume`** : les 262 131 états repris sont exactement les 262 131 sauvés ; la
   couverture est OU-ée au chargement (« resumed coverage map (36389 bytes added) », sources
   conservées, 36 267 entrées du journal de découverte) ; un quart d'heure d'exploration
   reprend la couverture (naturels 206 → 4 819, forcés 36 059 → 105 660, 103,9 M pas) ;
   second SIGINT : « saved 566113 », `frontier.bin` de 14 262 084 821 octets = 12 +
   566 113 × 25 193, rename atomique par-dessus le précédent, encore `pending_dropped 0`,
   `refused 0`, aucun échec.

Observations (pas des défaillances) : l'état GB pèse 25 Ko — une salve `--unbounded` d'un
quart d'heure accumule 566 113 états en attente (14,3 Go), l'espace requis est bien
compt × état, conformément à la note d'ouverture ; sur ntfs la relecture des 6,6 Go a pris
~17 min (~9 Mo/s, bureau chargé) — un fs natif Linux serait plus rapide, la partition
Windows a été retenue pour la capacité. La frontière reprise est laissée en place sur la
partition, prête pour une reprise (`frontier drain`).

### Comparaison des régimes (0.388.0 — non borné vs borné, noyau seul vs closure Sega)

Le même arrêt SIGINT propre, appliqué à la comparaison (Ace of Aces SMS, `--attract-frames
300 --jobs 1`, spool/état sur la partition Windows ; obs. outil) :

**Ace SMS, régime non borné** (noyau seul 7,5 min ; avec `sega/closure.c` ~10 min — les
sessions rendent l'exploration non déterministe, les positions marquées diffèrent donc un
peu d'un run à l'autre) :

| | noyau seul | avec closure Sega | borné t4 (1,5 M pas) : noyau / Sega |
|---|---|---|---|
| racines de closure | 218 | 18 729 | 37 / 6 267 |
| statique seul | 4 843 | 5 096 | 530 / 1 494 |
| provisoire | 4 056 | 3 784 | 10 668 / 10 668 |
| naturels | 10 517 | 10 536 | 1 600 / 1 600 |
| inconnu | 43 852 | 39 583 | 122 500 / 122 500 |

Le statique Sega (5 096 positions) est **entièrement contenu** dans les marques du noyau
seul (statique ∪ provisoire, 8 899 positions ; sega-only 0) : la closure Sega ne prouve
aucun octet que le noyau n'ait pas déjà touché. Son surplus de **confirmation** tombe à
+253 octets (5 096 − 4 843) contre +964 en régime borné — plus d'exécution réelle, moins
d'écart Sega→noyau ; cela renforce le dossier de retrait de `sega/closure.c` (qui reste
gated par la vérité terrain). Le régime non borné lui-même couvre beaucoup plus de terrain
réel que le borné : naturels ×6,6, donnée lue ×12, inconnu 122 500 → ~40 000.

**Zelda GB — le régime non borné tel quel dégrade la couverture.** Le run borné courant
(défauts, ~2,5 min) donne 85 776 naturels, 40 634 statique, 168 800 octets de closure,
36 788 inconnus ; les runs `--unbounded` de l'audit E11 donnent 4 835 naturels, 3 588
statique, 35 875 inconnus après 40 min et deux passes. Cause : `--unbounded` met
`max_path_steps` à 0, or l'exploration GB est accordée « chemins courts (1 000 pas),
entrées nombreuses » — les 22 100 chemins bornés deviennent 216 chemins cooperatifs de
65 536 pas, la largeur s'effondre au profit de la profondeur. `--unbounded` aide là où
l'exploration était sous-alimentée (SMS) et nuit là où l'accordage plateforme est
largeur-d'abord (GB) : il doit respecter la forme de chemins de la plateforme (lever les
plafonds, pas l'accordage). À traiter avant de présenter `--unbounded` comme régime
couverture-max sur GB.

## État du plan (0.388.0)

Les tranches C3 sont soldées : propagation de constantes par `DpIsa.reg_effect` + amorçage
concret (`get_regs`), retours (pile fantôme + pile concrète z80/sm83/6502/68000 via
`sp_ret`), simulation de banques mémoire, `port_effect` des OUT (`port_control` par
plateforme), provisoire distinct du statique, vraisemblance à l'entrée, vérité terrain
(`tests/truth_check.py` + fixture à vérité générée), ordre des marches indépendant de
l'arrivée. Le banc 4 ROMs montre la propriété de sous-ensemble tenue (0 octet noyau seul)
et l'effet zéro-donnée mesuré (Aerial : −2 671 erreurs de donnée déclarée code).

**Restent ouverts, décision du propriétaire** :
1. la vérité terrain des ROMs de banc (fichiers hors dépôt `code/données` octet à octet :
   désassemblage manuel d'une ou deux SMS/GG, ou ROM issue d'un source connu) — le banc les
   audite dès que le fichier existe (`/tmp/c3bench/bench4.sh` + `tests/truth_check.py`) ;
2. le retrait de `sega/closure.c` : seulement au banc octet à octet SMS/GG/SG/MSX sans plus
   d'erreurs que la fermeture Sega — la contribution propre restante est 964 / 0 / 2 094 /
   608 octets (vraisemblablement la famille 2 du diagnostic : les erreurs de Sega — runs de
   zéros et tables marquées code — que le noyau, désormais, ne copie plus).

Audit E11 clos (voir « Audit E11 » ci-dessus) : le contrat d'arrêt/spool `--unbounded` est
conforme sur disque de capacité suffisante — frontière complète sauvée au SIGINT (262 131
états, 6,6 Go, `pending_dropped 0`), rapport produit, reprise `--resume` fidèle (262 131
repris, couverture OU-ée et repartie de l'avant), seconde sauvegarde atomique (566 113
états, 14,3 Go), aucun échec ni perte sur les deux allers-retours.

**Nouveau point ouvert côté noyau** (voir « Comparaison des régimes ») : `--unbounded` doit
respecter l'accordage plateforme de la forme des chemins — mettre `max_path_steps` à 0
effondre la stratégie GB « chemins courts, entrées nombreuses » (85 776 naturels bornés →
4 835 non bornés sur Zelda) ; lever les plafonds ne doit pas lever l'accordage.
