# Plan de travail — maximiser la couverture (plan final, 0.388.0)

Relecture et restructuration du 16 novembre 2026, arbre à la version 0.388.0
(HEAD 4fd3815b ; le code est inchangé depuis 20e5c66e). Ce document remplace
la structure chronologique du plan d'origine (analyse du 15 novembre 2026 à la
version 0.345.0) : l'état courant en tête, les travaux engagés, les défauts
actuels du noyau, les améliorations de couverture proposées, l'ordre de
travail, et l'historique mesuré condensé en annexe (les bancs, les essais
rejetés et les nombres sont conservés). **Analyse seule : aucun code modifié,
aucun test ni aucune ROM exécutés.** Les références `fichier:ligne` sont celles
de l'arbre lu ; chaque défaut doit d'abord être reproduit par une fixture avant
d'être corrigé (AGENTS.md). Les nombres produits sur ROMs réelles sont des
observations de l'outil (machine concrète, RAM à zéro, registres événementiels
statiques), pas des mesures de matériel.

Documents de référence : `docs/COUVERTURE.fr.md` (norme commune : techniques
exigées, typage, sources, frontière différée, feuille de route),
`docs/PLAN-MUTUALISATION.fr.md`, `docs/GB.fr.md`, `docs/AUDIT-PARITE-MOTEURS.fr.md`.
Code lu en entier pour cette relecture : `src/explore/core.c` (3 061 lignes),
`core.h`, `src/core/dpmachine.h`, `src/core/coverage.h`, backends `src/mach/*.c`,
CLI `src/deepeel.c`, `src/explore/core_map.c`, `seedset.h`.

---

## 0. État actuel (0.388.0)

- **Noyau unique** : `dp_core_explore` est le seul moteur ; les cinq moteurs
  historiques sont retirés (NES 0.368.0, PCE 0.369.0, MD 0.370.0, Sega/MSX
  0.371.0, GB 0.372.0). C2 (rejeu S6), C4 (scans), C8 (travailleurs), C10
  (copies), P6 (reprises) et P8 (sorties) sont résolus par construction ; la
  couche de rapport est unique (C7, 0.375.1, vecteur de classes
  `dp_core_map_build`) et PNG/texte/JSON en sont des projections.
- **Techniques B1-B11 toutes présentes**, chacune avec compteur au rapport et
  option de coupure (relecture 0.380.0) : B1 `--probe-ctx-cap` (sondes
  ré-armées par contexte), B2 promotion du forcé (`promoted_forced`) et
  énumération complète en `--unbounded`, B3 points de contrôle de film et de
  session dans le même pool, époques de checkpoint (`Pending.epoch`, 0.378.0),
  rafales d'entrées (0.379.0), graines vivantes GB, B4 longueur de table
  prouvée (SM83/Z80, 6502/HuC6280, 68000 0.376.1) et énumération `--unbounded`,
  B5 dédup exacte (`exact_states`/`state_repeats` ; Sega : limite backend
  justifiée), B6 report `sat_path`/`sat_loop`/`max_total` (avec `--state-dir`),
  B7 fourches de banque (`bank_forks`, `--bank-fork-cap`), B8 désassemblage
  spéculatif, B9 références immédiates (`imm_ptr`/`imm_byte` toutes ISA), B10
  graine SRAM + signatures apprises, B11 canaux de copie non CPU (`on_copy` :
  DMA VDP, DMC ; HDMA GB sans objet runtime DMG).
- **C11 implémentée** (0.381.0, décidée par le propriétaire) :
  `DpMachine.ctx_reg_hash` sur GB/Sega/NES/PCE/MD + `proj_key` dans le noyau ;
  Ace of Aces : 1 618 fourches évincées, octets identiques. C13 fait
  (`ram_index` dense). C14 partiel : `arm_read` existe (NES seul — voir A6).
- **C3 soldé** : propagation de constantes par `DpIsa.reg_effect` + amorçage
  concret `get_regs` (0.383.0), retours par pile concrète z80/sm83 (0.384.0)
  puis 6502/68000 (`sp_ret`, 0.387.0), pile de retours fantôme (0.382.0),
  simulation des écritures de banque (0.380.1), `port_effect` des OUT
  (`out_port`/`port_control`, 0.388.0), provisoire distinct du statique,
  vraisemblance à l'entrée (`entry_data_sig`), ordre des marches indépendant
  de l'arrivée et vérité terrain (`tests/truth_check.py`, 0.386.0). Propriété
  de sûreté tenue à chaque tranche (0 octet « noyau seul » hors fermeture Sega
  sur les 4 ROMs de banc) ; effet zéro-donnée mesuré (Aerial : −2 671 erreurs
  de donnée déclarée code reclassées).
- **Régimes** : comparaison borné/non borné faite (Ace SMS : naturels ×6,6,
  statique Sega entièrement contenu dans noyau ∪ provisoire, sega-only 0 ;
  Zelda : `--unbounded` tel quel **dégrade** la couverture, 85 776 → 4 835
  naturels — nouveau point ouvert, voir A8). La **campagne en deux phases**
  (bornée puis non bornée) est validée sur Zelda : inconnu −31 %, +3 024
  naturels, +14 348 octets de copies, provisoires confirmés ×6 (§1).
  **Audit E11 clos** : le contrat d'arrêt/spool `--unbounded` est conforme sur
  disque suffisant (frontière complète sauvée au SIGINT puis reprise fidèle,
  `pending_dropped 0` sur les deux allers-retours — annexe).
- **Tests** : 168 tests + `campaign_fixtures` (dernière passe verte 0.382.0,
  1 217 s). Le banc de parité contre les moteurs historiques n'existe plus
  (références figées dans `tests/golden/`).
- **Restent ouverts, décision du propriétaire** : la vérité terrain des ROMs de
  banc (fichiers hors dépôt), puis le retrait de `sega/closure.c` au banc octet
  à octet SMS/GG/SG/MSX — contribution propre restante 964 / 0 / 2 094 / 608
  octets (famille 2 du diagnostic : erreurs de Sega que le noyau ne copie plus).

### Jalons

| Version | Contenu |
|---|---|
| 0.345.0 | analyse d'origine : A1-A26, B1-B11, C1-C15, phases P0-P8 |
| 0.346-0.348 | P0, P1, P2 (C13, C12, C14, champs 32 bits) |
| 0.351-0.364 | C3/C4/B tranches 1-2, B8, B11 `on_copy`, C11 liveness, B3 checkpoints, `campaign_fixtures` verte (967 s) |
| 0.366.0 | retrait complet des moteurs décidé par le propriétaire |
| 0.368-0.372 | retrait NES, PCE, MD, Sega, GB — le noyau est le seul moteur |
| 0.373-0.375 | rendu Sega dans le noyau, typage des registres (toutes plateformes), couche de rapport unique (C7) |
| 0.376-0.379 | `ctx_components`, signatures/B4 toutes ISA (0.376.1), `closure_unresolved`, cap par époque (0.378.0), rafales (0.379.0) ; mesures fermeture Sega (85 octets Action Fighter, banc 4 ROMs) |
| 0.380-0.382 | simulation des banques (0.380.1), pile de retours fantôme (0.382.0), C11 projection (0.381.0) ; exigence « zéro donnée déclarée code » |
| 0.383-0.385 | fermeture registres (`reg_effect`/`get_regs`), retours pile concrète z80/sm83, mode bloc absorbé, cap d'amorces étalonné (résultat négatif) |
| 0.386.0 | provisoire, vraisemblance, vérité terrain, ordre des marches ; effet zéro-donnée −2 671 |
| 0.387.0 | retours pile concrète 6502/68000 (`sp_ret`) |
| 0.388.0 | `port_effect` des OUT ; notes ouvertes instruites (Gradius 2) ; audit E11 clos ; comparaison des régimes ; campagne deux phases validée |
| 0.388.0 (relecture) | ce plan final : défauts P1-P16 (§2), améliorations A1-A10 (§3), campagne corrigée (§1), mutualisation restante (§4) |
| 0.389.0 | implémentation : campagne deux phases (`--campaign`, canal cellules, compteurs de reprise, décision P12), A8, P1-P11 au fil des fixtures (§2) — voir « État du plan (0.389.0) » |

---

## 1. Travaux engagés : la campagne en deux phases (implémentée 0.389.0)

La mesure est faite (Zelda GB, obs. outil) : bornée d'abord (défauts, forme
largeur-d'abord, `--state-dir`), non bornée ensuite (`--resume --unbounded`, un
quart d'heure, SIGINT) — inconnu 35 312 → **24 358** (−31 %), naturels 85 776 →
**88 800**, copies 4 616 → **18 964**, provisoire 27 408 → 4 637 (confirmés
×6). Le mécanisme existe (`--state-dir`/`--resume` : 205 612 états sauvés par la
phase 1, 205 612 repris, `pending_dropped 0`). Reste à l'implémenter proprement.

### Ce que la phase 1 élague déjà pour la phase 2 (état des lieux, code vérifié à 0.388.0)

1. **La frontière est le travail reporté**, pas une éviction : à l'arrêt borné
   (halt max-time/max-total/SIGINT avec `--state-dir`), le chemin actif est
   checkpointé et la frontière entière (file RAM + spool + différées) est
   sauvée ; `--resume` la recharge.
2. **La couverture OU-ée élague à deux endroits** : l'évaluation des fourches
   « les deux côtés déjà couverts » (`covered()` lit les drapeaux fusionnés,
   `pruned_covered`) décide d'emblée sur les sites que la phase 1 a couverts ;
   et le drain de file saute les états en attente dont le pc est déjà couvert
   (hors sessions, B3 — `core.c:2510`). C'est le canal dominant — cf. Gradius 2 :
   6,68 M évaluations d'élagage, évitées d'emblée quand le site est connu.
   **Prérequis (P2)** : ce canal repose sur `covered()` au pc en attente, résolu
   dans le mapping **courant du worker** — le défaut A19. Le corriger d'abord
   (index ROM de la cible dans `Pending`) pour que les sauts soient sûrs :
   aujourd'hui un état peut être sauté à tort (pc couvert dans le mapping
   courant mais pas dans le sien).
3. **Les états exacts ne prêtent rien à la campagne bornée → non bornée**
   (corrige la version précédente de cette section) : l'ensemble exact n'est
   peuplé que sous `--unbounded` (`core.c:1846-1848`) et `exact.bin` n'est écrit
   que s'il est non vide (`core.c:2904`) — une phase 1 **bornée** n'y dépose donc
   rien. Deux options : (a) hacher aussi en borné (coût par pas à instruire
   contre le profil Gradius 2 : 2,02 M hachages de cellule pour 31 071 chemins) ;
   (b) consigner le canal comme vide et compter sur cellules + couverture OU-ée.
   À décider avant l'implémentation (renvoie P12).
4. **Ce qui ne passe pas** : l'élagage par cellule est désactivé sous
   `--unbounded` (`no_cells=1` forcé, `core.c:2553`), alors que `cells.bin` de
   la phase 1 est écrit puis rechargé — sans servir à élaguer. C'est le canal
   d'accélération à ouvrir (garde `cell_hi > cell_lo || !no_cells`,
   `core.c:2202`). **Note phase 2** : `--unbounded` lève `closure_seed_cap` mais
   pas les tableaux fixes de la fermeture (`stack[256][4]`, `rets[256]`,
   seen 8192, `core.c:457` — P11) : consigner ou corriger. Taint-cut et
   forced-RAM-cut restent désactivés sous `--unbounded` (choix conservé,
   justifié). Précision CLI : `--tier` est accepté-ignoré (`deepeel.c:304`) et
   `frontier drain` (`deepeel.c:1614`) est un simple `explore --core --resume`
   pour la frontière noyau — il n'y a pas de paliers dans la frontière noyau
   (P7) ; ne pas présenter `--tier` comme un palier.

### Méthode 1 — la phase bornée (borne adaptée)

Enchaîner les deux phases dans un processus, au niveau CLI (`src/deepeel.c`) :
`--campaign` lance la phase 1 avec les défauts de la plateforme (forme
largeur-d'abord préservée) et une borne explicite — `--max-time S` noyau
(défaut proposé : 120 s, halt 1) ou `--max-total N` — puis, sur le même
`--state-dir`, relance `--resume --unbounded` jusqu'à SIGINT ou épuisement. La
borne reste à la main de l'utilisateur ; le contrat « frontière entière
reportée, jamais silencieux » est déjà épinglé (audit E11). Le rapport gagne
une ligne par phase (la reprise affiche déjà « frontier: N resumed » et les
octets de couverture repris).

### Méthode 2 — la phase 2 utilise le déjà-fait pour accélérer (`src/explore/core.c`)

1. **Ouvrir le canal cellules** : sous `--resume --unbounded` avec un
   `cells.bin` non vide, charger les cellules archivées et laisser l'élagage
   par cellule actif (la garde `core.c:2202` devient aussi vraie quand la
   reprise a rempli `cellset`) — une fourche qui retombe dans une cellule déjà
   archivée est élaguée dès sa première visite au lieu d'être rejouée. Sans
   `--resume`, `--unbounded` reste sans cellules (pas de données antérieures à
   élaguer). Le coût de hachage qui a mordu sur Gradius 2 sert ici à couper des
   fourches entières, pas seulement à archiver.
2. **Compteurs de reprise** : afficher au chargement les cellules et états
   exacts repris (aujourd'hui silencieux) — l'accélération doit être visible,
   jamais implicite.
3. Ne rien changer à `pruned_covered`, `state_repeats` et au drain de file
   (canaux déjà actifs) — mais le drain ne doit devenir un canal compté qu'après
   P2 (prérequis de sûreté, ci-dessus). Ne pas ré-archiver ce qui l'est
   (existant).

### Banc et acceptation

Trois cartes Zelda GB (borné seul / non borné seul / deux phases — les nombres
mesurés ci-dessus servent de référence) ; Ace SMS en deux phases (cas
sous-alimenté : vérifier que la phase 2 ne perd rien). Acceptation :
inconnu(2 phases) ≤ min(les deux seuls) ; `resumed == saved` ;
`pending_dropped == 0` aux deux arrêts ; `cell_pruned > 0` en phase 2 sur un
cas à cellules pré-remplies ; aucun message d'échec ; aucun effet des tableaux
fixes de la fermeture (P11) sur les octets. Suite verte complète, version (le
rapport devient visible), `GB.fr.md` + `COUVERTURE.fr.md` (la recette rejoint
la norme commune), commit avec trailer.

### Tâches

`src/explore/core.c` (P2 : index ROM dans `Pending` ; garde cellules +
chargement reprise + compteurs ; décision exact P12) ; `src/deepeel.c`
(`--campaign`, bornes de phase 1) ; `tests/CMakeLists.txt` + test noyau deux
phases (fixture générée : phase 1 à budget minuscule avec `--state-dir`,
phase 2 `--resume --unbounded`, épingles `resumed == saved` et
`cell_pruned > 0`) ; docs.

---

## État du plan (0.390.0)

**Implémenté** (chaque défaut reproduit par fixture avant correctif, suite verte) :
- **0.389.0 — campagne deux phases** : `--campaign` (phase bornée à l'accordage
  plateforme, borne `--max-time`/`--max-total` explicite — défaut 120 s —, puis
  `--resume --unbounded` dans le même processus ; un rapport par phase, la phase 2
  s'arrête par SIGINT ou épuisement comme tout régime non borné). Canal cellules ouvert :
  la phase 2 réarme l'élagage sur les cellules archivées ; les reprises (cellules, états
  exacts) sont affichées. **Décision P12** : option (b) — l'ensemble exact reste une
  production du non borné (le hachage coûterait par pas en borné, cf. Gradius 2) ; la
  phase 2 élague avec cellules + couverture OU-ée + sa propre dédup exacte. Tests
  `core_campaign_z80` (mécanique), `core_campaign_epoch` (l'époque et les reportées
  traversent la frontière).
- **0.389.0 — défauts** : P1+P16 (`--seed-banks` PCE, fenêtre de semis paramétrée),
  P2 (index ROM dans `Pending` — le tas par score reste à faire si le scan O(file) mord),
  P3 (hygiène, `DpMachine.rom_top_win`), P4 (tables `jp (ix)/(iy)`), P5 (époque dans
  `SpoolHead`), P6 (fourches forcées hors cellules), P7 (frontière différée), P9
  (compteurs de sondes propres), P10 (règle cov nommée — terminée en 0.390.0),
  P11 (plafonds de marche levés), A8 (la forme plateforme conservée). Correctif Sega :
  `tl_clo` réinitialisé à la fusion (crash `--campaign`).
- **0.390.0 — mutualisation et chaleur** : **M1/P8** (plus aucune ISA nommée dans le
  noyau — hooks `DpIsa` ram_load/cmp_imm/ram_signature_wide/table_proven/
  rom_table_follows/table_entry_width sur les cinq ISA), **P10 fini** (offsets cov par
  `DpMachine.bank_window`, épine `z80_cov_geom` au nouveau contrat), **P13/A9** (hits
  d'exécution par octet du noyau → tuile de chaleur + `dpCh` bit 5 ; acteurs par octet
  restés 0, écart consigné), **A2** (ré-armement site × cellule étendu aux rejeux
  d'index, RNG, signatures, IRQ — cap `probe_ctx_cap` par site, saturations comptées),
  **A1** (graines vivantes sur `cfg->seeds`, le reset compris — validations de boot
  depuis un état de jeu), **A5** (mémoïsation de l'élagage : clé (site, projection)
  dépensée = dépensée pour toujours, mémo par worker sans verrou), **A10** (`cut_pc`
  PCE/MD, B4 ×4 pour les tables de longs 68000), **P14** (options mortes retirées ;
  rectification : `--queue-max` vivant, `--rtcode` produit y compris Sega),
  **P15/M5** (`DpProbe` retiré — sondes pad + hooks événementiels le remplacent),
  **M7** (matrice techniques 3/4/7 × ISA verte : 6502, huc6280, z80, sm83, 68000),
  **A6/A7 vérifiés déjà résolus** (sondes run+patch toutes plateformes ; CPU son MD
  dans chaque instantané, épinglé par `md_milestone`).
- Versions 0.389.0/0.390.0 (rapports et fenêtre changés) ; `GB.fr.md` à jour.

**Restent ouverts** : la vérité terrain puis le retrait de `sega/closure.c` (M2,
décision propriétaire gated), les acteurs par octet côté noyau (le reste de P13/A9),
le tas par score de la file (le reste de P2 si le scan mord).

## 2. Défauts actuels du noyau (relecture 0.388.0 — corrigés en 0.389.0 sauf mention)

Priorité : **1** = perte de couverture ou couverture fausse, **2** = règle
« jamais silencieux / rien n'est perdu » violée, **3** = robustesse. Chacun
doit d'abord être reproduit par une fixture avant d'être corrigé.

| # | P | Lieu | Défaut | Effet | Correctif | Fixture minimale |
|---|---|---|---|---|---|---|
| P1 | 1 | `deepeel.c:1285` | `--seed-banks` PCE est parsé mais `seedset_build(..., 0, ...)` passe `all_banks = 0` : l'option est ignorée en silence (NES, lui, passe 64 : `deepeel.c:1157`) | les graines par banque PCE n'existent pas ; la musique-rip fonctionne (`0x4000`, `deepeel.c:1289`) mais l'entrée par banque non | passer les banques avec la géométrie PCE (pages 8 Kio) et rendre `seedset.h` paramétrable (P16) | PCE : `--seed-banks`, CHECK graines dans chaque banque, exécution dans la bonne fenêtre |
| P2 | 1 | `core.c:2509-2519` | choix par nouveauté et drain : `covered()` résout le pc en attente dans le mapping **courant du worker** (A19), balayage O(file) sous verrou | mauvais ordre et sauts d'états possibles sur cibles bancarisées ; goulot avec `--jobs` et file pleine ; **prérequis du canal 2 de la campagne** (§1) | stocker l'index ROM de la cible dans `Pending` (résolution à la mise en file) ; tas par score | GB/SMS : cible en banque non courante, CHECK ordre et non-saut |
| P3 | 1 | `core.c:821`, `:901` | restes de constantes GB dans la fermeture z80/sm83 : cibles sous `$0150` ou au-delà du double de la fenêtre refusées (`core.c:821`), racines `base < 0x100` sautées (`core.c:901`, reste de A11) | code SMS/MSX légitime sous `$0150`/`$0100` perdu (fermeture des tables et racines) | bornes par plateforme (régions de code de `DpPlatform`/`DpIsa`) | z80 : table dont une cible est en `$0100`, CHECK racine suivie |
| P4 | 1 | `core.c:2014-2016` | seules les tables derrière `jp (hl)` (`bytes[0] == 0xE9`) sont suivies ; `DD E9`/`FD E9` (`jp (ix)/(iy)`) ignorés ; table absolue limitée à `0x6C`/`0x7C` | le dispatch par registres d'index (courant sur SMS/MSX) ne donne aucune fermeture de table ; tables absolues perdues | dispatch préfixe par `DpIsa` (préfixe + opcode + registre de paire, `DpFlowInfo.pair` existe) | SMS : table derrière `jp (ix)`, CHECK racines de table |
| P5 | 2 | `core.c:1165`, `:1273`, `:1277` | `SpoolHead` ne sérialise ni l'époque (`Pending.epoch`) ni un palier ; bits forcés/seed/vector + origine (≤ 31) seulement | un état spoolé reprend hors époque (cap par époque perdu pour lui) ; la frontière différée par paliers (§6 de la norme) n'a pas de champ | ajouter époque (octet) et palier au `SpoolHead` ; le format d'état est déjà versionné par la taille | aller-retour spool avec époque (réutiliser `fix_epoch.gb`), CHECK époque conservée |
| P6 | 1 | `core.c:993-994` | `obs_read` marque `ramread` même sur chemin **forcé** (pas de garde `!o->forced`, contrairement au bitmap vivant `:1000`) | les cellules (clé sémantique) et le DSY voient des lectures que seules les fourches fabriquées font : élagage faussé, faux « alimenté » | même garde que le bitmap vivant ; les fourches forcées ne marquent pas | fourche forcée lisant une adresse, CHECK cellule non créée par elle |
| P7 | 2 | `core.c:2120`, `:2209`, `:1180` | `sat_site` incrémente sans report ; `defer_state` exige `--state-dir` **et** non-`--unbounded` ; pas de paliers dans la frontière noyau (`--tier` accepté-ignoré `deepeel.c:304` ; `frontier drain` `deepeel.c:1614` = simple resume) ; `sat_queue` jamais incrémenté | les états refusés par cap de site sont abandonnés hors `--state-dir` : contre la frontière différée par paliers de la norme (B6 partiel) | reporter `sat_site` (borné), champ palier (avec P5), `frontier drain` honore les paliers | cap de site petit + budget minuscule puis drain = run long, CHECK états repris |
| P8 | 2 | `core.c:1372-1534`, `:1441`, `:1458`, `:1481` | la connaissance ISA vit encore dans le noyau : ~10 `strcmp(isa->name…)` (`direct_ram_load`, `ram_signature`, `table_proven_len`, `rom_table_follows`) + opcodes codés en dur (0x3A/0xFA/0xF0, 0xAD/0xAE/0xAC, 0xA5/0xA6/0xA4, 0xBD/0xB9, 0xC9/0xFE, 0x21 ; fermeture : 0x21/0x60/0x69/0x06/0x0E/0x19/0x09/0x11/0x01) | **corrige la note d'avancement P5 du plan d'origine** (« plus aucune ISA nommée dans core.c » — fausse à 0.388.0) ; toute nouvelle ISA devra imiter des opcodes z80/6502 | finir C1 : membres `DpIsa` restants (`ram_load`, `cmp_imm`, `table_access` ; `imm_ptr`/`imm_byte` existent déjà) | refactor : suite verte + fixtures ISA inchangées |
| P9 | 2 | `core.c:2874`, `:2998`, `:2088`, `:2976` | compteurs de rapport faux : boucle morte `out->steps += 0` ; `event probes N at M sites: N forced variants` (N imprimé deux fois) ; `sat_queue` affiché mais jamais incrémenté ; `sh->probectx[ri]++` sans verrou | rapport mensonger sur les sondes et la saturation de file ; course bénigne mais réelle sur `probectx` | corriger les lignes d'impression, incrémenter ou retirer `sat_queue`, verrouiller ou compteur atomique | épingles de rapport sur fixture à sondes et à cap de file |
| P10 | 2 | `core.c:3053-3058` | `dp_core_write_cov` : géométrie 16 Kio et offset `strcmp(id, "gb")` codés en dur | `--cov` faux hors GB 16 Kio (MD 16 Kio non bancarisées, PCE 8 Kio) ; toute plateforme non-GG décale | géométrie et fenêtre de base depuis `DpMachine` (taille de vue, `bank_view`) | `--cov` SMS/MD avec banque > 0, CHECK adresses |
| P11 | 2 | `core.c:457` | tableaux fixes de la fermeture (`stack[256][4]`, `rets[256]`, seen 8192, kthru 4096) non levés sous `--unbounded` (contrairement à `closure_seed_cap`, levé) | en régime non borné (phase 2 de campagne), la marche reste plafonnée en profondeur/dédup interne : incohérent avec le contrat « plafonds levés » | croître ou lever sous `--unbounded` ; dépassements comptés (`closure_stack_dropped` existe) | marche > 256 branches, CHECK profondeur atteinte sous `--unbounded` |
| P12 | 2 | `core.c:1846-1848` | dédup exacte bornée à `--unbounded` (borné n'a que les cellules) ; `exact.bin` écrit seulement si non vide (`:2904`) | en borné, aucun état exact n'est conservé → le canal 3 de la campagne est vide (§1) ; utile aussi au diagnostic de non-terminaison | décision §1 : (a) hacher aussi en borné (coût à instruire), (b) consigner le canal vide | si (a) : fixture à boucle, CHECK `exact_states > 0` en borné |
| P13 | 3 | `core_map.c` (`dp_core_map_build`) | `DpFirstLog` reconstruit avec acteur 0 ; pas de hits/chaleur/acteurs par octet dans les sorties noyau | la courbe de rendement par source (§E) n'est pas mesurable ; la chaleur jouée par octet du PNG (`dpCh` bit 5, écart consigné au retrait NES) manque | écrire hits/heat/acteurs depuis le noyau (sources DP_SRC_* déjà OR-ées) | `mapx` : première découverte par source non nulle sur fixture multi-techniques |
| P14 | 3 | `deepeel.c:1076-1096`, `:1214-1216`, `:342-343` | options mortes des commandes `nes`/`pce` (`--play-pool`, `--ctx-live`, `--defer-tiers`, `--queue-max`, `--bank-dispatch`, `--max-paths`) ; `--rtcode`/`--ram-code` acceptés non produits | l'utilisateur croit régler quelque chose d'inerte ; sorties promise-non-tenue | retirer (réponse : « option retirée ») ou implémenter ; produire `--rtcode`/`--ram-code` (ramarch existe côté GB) | CLI : option retirée répond sur stderr |
| P15 | 3 | `dpmachine.h:200-205` | `DpProbe` déclaré, jamais consommé par le noyau | le catalogue de sondes par plateforme (C12) reste lettre morte | consommer (catalogue de sondes fourni par la plateforme) ou retirer la déclaration | `test_dpmachine` : sondes annoncées == sondes consommées |
| P16 | 3 | `seedset.h:26-27` | `$8000` et fenêtres 16 Kio codés en dur dans l'aide commune (commentaire « NES ») ; Sega corrige après coup (`deepeel.c` commande z80) ; PCE casserait si `all_banks > 0` | piège latente pour toute nouvelle plateforme (et cause de P1) | fenêtre de base et largeur depuis `DpMachine.bank_view` | couverte par la fixture P1 (PCE) |

**Soldés** : les défauts A1-A8, A10-A15, A17-A18, A23-A26 du plan d'origine
(état 0.366.0) sont vérifiés corrigés dans le code à 0.388.0 (valeurs lues
transmises, FIFO de lectures, index denses, clés par index ROM, origine
héritée et sources dédiées, pile de fermeture comptée, `pending_dropped`,
mises en file par `queue_push`, bit `vector` au spool, clé de résolution,
`jp_seen` extensible, plages WRAM par machine, budget sessions). **Seul A19
reste ouvert = P2.**

---

## 3. Améliorations de couverture proposées (à instruire)

Rangées par rendement attendu / coût. Chacune arrive avec son option de
désactivation, son compteur et sa fixture ; le rendement se mesure à budget
égal avant/après (les ablations de l'annexe montrent que le rendement est
souvent différé — ne pas juger à petit budget).

| # | Objet | Mécanisme | Croise | Rendement attendu |
|---|---|---|---|---|
| A1 | Graines vivantes généralisées | lancer les graines de pointeurs, vecteurs et banques depuis le pool de checkpoints (états de jeu réels) au lieu de l'état de mise sous tension ; `core_live_seeds` (GB, vecteurs) étendu à toutes les ISA et à tous les types de graines | B3 | élevé : le code atteint s'exécute dans un état de jeu réel et va plus loin avant les coupes ; coût faible (mécanisme existant) |
| A2 | Ré-armement par contexte généralisé | étendre B1 (clé site × cellule, `--probe-ctx-cap`) aux rejeux d'index, aux octets RNG, aux signatures apprises et aux fourches IRQ — aujourd'hui ré-armés une seule fois par site/adresse | B1, A7-A8 d'origine | élevé sur les jeux réels (menus, modes, pause) : la routine de lecture manette ou le RNG n'est sondé que dans le premier contexte |
| A3 | Report `sat_site` + paliers + drain | = P7 : la frontière différée par paliers de la norme (§6) complète dans le noyau | B6, P5/P7 | élevé en campagne : les états refusés par cap de site rejoignent la phase 2 au lieu d'être perdus |
| A4 | Index ROM dans `Pending` | = P2 : sûreté du drain et ordre de choix corrects sur bancarisé | P2 | prérequis de la campagne ; améliore aussi le choix par nouveauté |
| A5 | Mémoïsation de l'évaluation d'élagage | décision « deux côtés couverts » mémorisée par (site, projection) ; Gradius 2 : 6 682 318 évaluations pour 31 071 chemins | canal cellules §1 | temps, pas d'octets : rend la campagne praticable sur les ROMs à boucle d'attente dense |
| A6 | Résolution sur lectures matérielles | porter `arm_read` (C14, NES seul — `nes_machine.c:148`) sur GB/Sega/PCE/MD (m0boy le sait faire) ; B2 résout alors ports et registres comme la RAM | B2, C14 | moyen à élevé : conditionnels sur registres VDP/PSG/pad résolus au lieu de forcés |
| A7 | Instantané Z80 son MD | intégrer l'état du Z80 sonore dans save/load de `md_machine` (backport m0boy) ; sinon les checkpoints/TAS MD désynchronisent le son et J7 est perdu en `--core` | question « à vérifier » d'origine | nécessaire à la fidélité MD ; préalable à toute campagne MD |
| A8 | `--unbounded` respecte l'accordage plateforme | lever les plafonds, pas la forme des chemins : ne pas mettre `max_path_steps` à 0 quand la plateforme est accordée largeur-d'abord (`core.c:78`) ; Zelda : 85 776 naturels bornés → 4 835 non bornés | point ouvert 0.388.0, §1 | condition pour présenter `--unbounded` comme régime couverture-max sur GB |
| A9 | Hits/chaleur/acteurs par octet | = P13 : écrire les hits par octet, la chaleur jouée et les acteurs (TAS/RNG/ATTRACT/SCRIPT de `DpFirstLog`) depuis le noyau | P13, §E | rend enfin mesurable la courbe de rendement par source ; heat PNG (`dpCh` bit 5) |
| A10 | Tables `jp (ix)/(iy)` + `cut_pc` PCE/MD + B4 longs | = P4 ; ajouter `cut_pc` aux backends PCE/MD (présent GB/`gb_machine.c:334`, NES `:243`, Sega `:323`) ; B4 : l'échelle d'énumération suppose des entrées de 2 octets (`core.c:1913`, ×2) — ×4 pour les longs 68000 | P4 | statique et dispatch sur les routines indexées par IX/IY ; éviction plus juste PCE/MD |

---

## 4. Mutualisation restante

| # | Objet | État | Suite |
|---|---|---|---|
| M1 | Connaissance ISA dans le noyau | restes confirmés (P8) — la note « plus aucune ISA nommée » du plan d'origine est fausse | finir C1 : membres `DpIsa` `ram_load`, `cmp_imm`, `table_access` |
| M2 | `sega/closure.c` (592 lignes) | reste en place, gated : vérité terrain (propriétaire) puis banc octet à octet SMS/GG/SG/MSX ; contribution propre restante 964 / 0 / 2 094 / 608 octets (famille 2 du diagnostic) ; en régime non borné son statique est contenu dans noyau ∪ provisoire (sega-only 0) | retrait après le banc ; ne rien porter de plus sans vérité terrain |
| M3 | Sorties manquantes du noyau | `--cov` géométrie (P10), `--rtcode`/`--ram-code` non produits (P14), heat/hits (P13/A9) | tranches de sortie au fil de §6 |
| M4 | CLI : options mortes | cmd_nes/cmd_pce (P14) | retirer ou implémenter |
| M5 | Catalogue de sondes / `DpInput` | `DpProbe` non consommé (P15) ; masques d'entrée posés NES (`0xFF`, `nes_machine.c:218`) et MD (`0x17F` + ext) | consommer DpProbe ou le retirer ; terminer le catalogue commun (C12) |
| M6 | Géométrie de `seedset.h` | fenêtres codées en dur (P16) | paramétrer par `DpMachine` |
| M7 | Générateur de fixtures technique × ISA | `build_fixtures.py` existe, génération par technique partielle | étendre le générateur (C15) au fil des correctifs de §2 |

**C1-C15 d'origine** : C2/C4/C8/C10 résolus par le retrait des moteurs ;
C3 → M2 ; C5 → frontière noyau sans paliers (P7) ; C6 → résolu par retrait
(`DpMovie` unique, sessions du noyau) ; C7 → résolu (0.375.1), restes = M3 ;
C9 → résolu par retrait, restes = M4 ; C11 → implémenté (0.376.0/0.381.0) ;
C12 → M5 ; C13 → fait (`ram_index`) ; C14 → partiel (`arm_read` NES seul, A6) ;
C15 → M7 ; C1 → M1 (restes, P8).

---

## 5. Mesure et vérité terrain

- **Outil** : `tests/truth_check.py` audite une carte `map v1` contre un
  fichier de vérité (`<lo>-<hi> c|d`, plages hex inclusives) : code trouvé /
  manqué, et « données déclarées code » séparées en **prouvé** (naturel ou
  statique sur un octet de données : l'erreur, critère de sortie) et
  **non prouvé** (forcé, inféré-code, probable-code : rapportés sans être
  fataux). La fixture `fix_z80_truth.sms` + `fix_z80_truth.truth` (émise par le
  générateur) et le test `core_closure_truth_z80` épinglent le refus d'entrée
  et l'audit « zéro erreur prouvée ».
- **Vérités des ROMs de banc** (Ace of Aces SMS, Addams Family GG, Aerial
  Assault GG, Bank Panic SG) : propriétaire, hors dépôt (désassemblage manuel
  d'une ou deux SMS/GG, ou ROM issue d'un source connu). Le banc les audite
  dès que le fichier existe ; c'est le préalable au retrait de `sega/closure.c`
  (M2).
- **Indicateurs** : octets « connus stricts » (naturel + session), puis
  « plausibles » (résolu, sondé, index). Le forcé et le statique seul ne
  comptent pas comme gain. La courbe de rendement par source (§E) exige A9/P13.
- **Nombres de référence** (obs. outil, `--jobs 1`) : Zelda borné 85 776
  naturels / 40 634 statique / 36 788 inconnus ; deux phases 24 358 inconnus ;
  Ace borné t4 (1,5 M pas) statique noyau 530 / + Sega 1 494 ; en non borné
  Ace : naturels 10 517, statique Sega contenu (annexe).

---

## 6. Ordre de travail

Chaque phase : fixtures d'abord (elles échouent), correctif, `ctest --test-dir
build --output-on-failure` vert en fin de phase, version incrémentée si le
rapport ou la fenêtre change, docs FR à jour, commit avec trailer.

1. **Fixtures et petits correctifs** : P1 (+P16), P3, P4, P6, P9, P10 —
   indépendants, tout de suite rentables (P9/P10 changent le rapport :
   version à bump au moment du correctif).
2. **Campagne deux phases** : P2 (prérequis de sûreté du drain) → décision
   canal exact (P12) → `--campaign` + canal cellules + compteurs de reprise →
   banc deux phases (critères §1) ; **A8** avec elle (l'accordage `--unbounded`).
3. **Techniques de contexte** : A1 (graines vivantes), A2 (ré-armement
   généralisé), A3 (report `sat_site` + paliers + drain) — tranches avec
   fixtures par ISA.
4. **Backports m0boy** : A6 (`arm_read` générique), A7 (Z80 son MD).
5. **Vérité terrain** (propriétaire, hors dépôt) puis retrait de
   `sega/closure.c` (M2) au banc octet à octet ; **A9/A10** ; nettoyage M1
   (ISA dans core.c), M4-M7.
6. **P13/A9** peuvent avancer à tout moment : sans eux la mesure du rendement
   par source reste aveugle.

---

## Annexe — historique mesuré condensé (0.345 → 0.388)

Les bancs, essais rejetés et diagnostics ci-dessous sont la mémoire de
méthode : ils fondent les décisions du plan final et ne doivent pas être
retentés sans nouvelle hypothèse.

### Mesures de la fermeture Sega (0.379-0.386)

- Correction de méthode : les écarts de plusieurs milliers d'octets relevés
  d'abord venaient de runs **sans `--jobs 1`** (exploration non déterministe).
  Mesure déterministe : **85 octets** propres à la fermeture Sega sur Action
  Fighter UE (0 octet propre au noyau), puis banc 4 ROMs borné t4 (1,5 M pas) :
  +964 / +763 / +2 803 / +614.
- À chaque tranche du portage générique, la propriété de sûreté est tenue :
  l'ensemble statique du noyau reste un **sous-ensemble strict** de la même run
  avec la fermeture Sega (0 octet « noyau seul » sur les 4 ROMs). Statiques
  seuls des tranches successives — Ace/Addams/Aerial/Bank Panic : 0.382.0
  (pile fantôme) 9 749 / 8 229 / 12 739 / 1 281 ; 0.383.0 (registres) 9 735 /
  8 229 / 13 234 / 1 277 ; 0.384.0 (pile concrète z80/sm83) 9 735 / 8 229 /
  13 263 / 1 281 ; 0.386.0 (provisoire split) statique 530 / 3 266 / 7 103 /
  14, provisoire 10 668 / 4 972 / 5 972 / 1 273, refus d'entrée 91 / 94 / 43 /
  0, Sega ajoute 964 / 0 / 2 094 / 608.
- **Diagnostic de l'écart restant** (0.385.0, cap 256, diff cartes) : pas des
  plages mais des **paires de 2 octets** (491/382/1 263/306), deux familles —
  (1) du vrai code derrière les clôtures de données du noyau (`ld (ix+n),a`
  d'Ace, écritures de port GG d'Addams), (2) des **erreurs de Sega** (runs de
  zéros, tables marquées code). L'écart ne se ferme ni par le budget ni par
  plus de mécanismes : il se tranche par la **qualité des clôtures de données**,
  donc par la vérité terrain.
- Effet zéro-donnée (0.386.0) : Aerial −2 743 octets marqués dont **−2 671
  reclassés données** (les plaques du diagnostic), +2 544 ailleurs ; Ace +1 431
  (retombées d'appel redeviennent marquées, en provisoire) ; Bank Panic −3 ;
  Addams neutre. La plage `0xb172-0xb2d4` reste marquée code (349 octets) :
  à juger sur la vérité terrain.
- Le mode bloc Sega est **absorbé** (0.385.0, relecture côte à côte : BC/DE
  suivis dès l'amorçage, ldir/djnz classifiés `DP_FLOW_COND`, le registre `c`
  ne sert que `port_effect`). Le cap d'amorces 4 → 256 est un **résultat
  négatif** (0 octet sur 3 ROMs, +52 tuiles = erreurs sur Aerial) : le défaut
  reste cap 4, le knob `--closure-seed-cap` reste pour `--unbounded`.

### Essais C3 rejetés (méthodologie — à ne pas retenter sans nouvelle hypothèse)

- Rejouer les racines sous chaque vue de mapper observée : sur-marquage
  (10 428 → 17 534 sur Ace) ; le gain Sega d'Action Fighter ne vient pas des
  vues croisées.
- `jp (hl)` par constante `ld hl,nn` : 1 saut résolu, 0 octet.
- Retours résolus par pile fantôme de marche : 75 suivis, 0 octet.
- Confiance après appel (avec ou sans appelé connu) : 7 855 → 17 472 :
  sur-marquage massif (données après appel). Le port exige la condition de
  retour de Sega (retour établi par la pile concrète) — c'est fait depuis
  (0.382.0/0.384.0).
- Règle « huit instructions sans transfert de contrôle » à l'entrée de marche :
  condamnée par le banc (−121 octets dont du vrai code déroulé d'Ace, cascade
  sur Addams) ; les signatures ISA seules (`entry_data_sig`) refusent 91/94/43/0
  entrées par ROM.
- Préfixe DD/FD + opcode sans IX/IY déclaré illégal : résultat **chaotique**
  (−401, des plages de données reviennent selon l'ordre des marches) ; retiré.

### Mesures E : rendement (Zelda GB, obs. outil)

- Courbe par budget (3·10⁵ / 10⁶ / 3·10⁶ pas) : les techniques à état
  (résolution, rejeu d'index, RNG, IRQ) ne produisent qu'à partir d'~10⁶ pas ;
  la fermeture statique domine entre 10⁶ et 3·10⁶ (6 283 → 51 235) ; le choix
  par nouveauté ne dépasse le choix simple qu'au plus grand budget.
- Ablation à trois budgets, une technique coupée à la fois : à 3·10⁶ pas —
  choix par site −35 points, taint-cut −32, résolution −12, signatures −11,
  fermeture −6, sondes −2,5 ; rejeu d'index et RNG 0 sur cette ROM (utiles en
  général). À 10⁶ les mêmes paraissent inutiles voire nuisibles : rendement
  **différé**, ne pas juger à petit budget. Un budget intermédiaire peut
  inverser un signe (`--no-site-score` : +0,5 puis −35).

### Notes ouvertes instruites (0.388.0)

- **Gradius 2 (MSX)** : 6 682 318 fourches « deux côtés couverts » évaluées et
  2 017 357 hachages de cellule pour 31 071 chemins — les pas n'avancent plus,
  tout va à l'évaluation d'élagage d'une boucle d'attente. Pas un écart de
  couverture ; la mémoïsation (A5) est une optimisation. La note « ralentit
  fortement » venait de `--attract-frames` sur le chemin MSX.
- **Aleste (MSX)** : fermeture Sega sans effet (7 094 avec et sans).
- **QuackShot (MD)** : 292 fourches, `projection-pruned forks 0` — densité de
  fourches trop faible à ce budget ; la projection sert là où elles sont
  denses (Ace : 1 618 évincées).
- État Zelda 0.379.2 : 55/69 entrées du registre touchées par des chemins
  naturels, 229 arêtes statiques hors de la vue ROM comptées, 1 369 fourches
  résolues dont 74 promues du forcé, `epoch_forks` 149 (cap 4) → 75 (cap 1).

### Audit E11 (clos, 0.388.0)

Zelda GB, spool/état sur la partition Windows (capacité) : arrêt SIGINT —
frontière complète sauvée (« frontier: 0 states resumed, 262131 saved »),
`frontier.bin` 6 603 866 295 octets = 12 + 262 131 × 25 193 (état GB 25 177),
dont 196 595 déversés puis relus du spool (`refused 0`), `pending_dropped 0`,
rapport produit, sortie 0. Reprise : 262 131 repris = 262 131 sauvés,
couverture OU-ée (36 389 octets ajoutés), reprise de l'avantage (naturels
206 → 4 819), second SIGINT « saved 566113 » (14 262 084 821 octets), rename
atomique, encore `pending_dropped 0` et `refused 0`. L'espace requis est
compt × état ; relecture ntfs ~9 Mo/s. L'échec tmpfs antérieur n'était qu'une
question de capacité.

### Comparaison des régimes (0.388.0, obs. outil)

- **Ace SMS non borné** (7,5-10 min) : racines de closure 218 (noyau) vs
  18 729 (Sega) ; statique seul 4 843 vs 5 096 — le statique Sega est
  **entièrement contenu** dans statique ∪ provisoire du noyau (sega-only 0) ;
  son surplus de confirmation tombe à +253 (contre +964 en borné). Naturels
  ×6,6, donnée lue ×12 par rapport au borné.
- **Zelda GB non borné tel quel dégrade** : 85 776 naturels bornés → 4 835 ;
  cause : `--unbounded` met `max_path_steps` à 0 alors que l'exploration GB est
  accordée « chemins courts (1 000 pas), entrées nombreuses » — 22 100 chemins
  courts deviennent 216 chemins de 65 536 pas, la largeur s'effondre. D'où
  A8 et le point ouvert du propriétaire : lever les plafonds, pas l'accordage.
- **Campagne deux phases validée** (Zelda) : inconnu 35 312 / 35 875 /
  **24 358** ; naturels 85 776 / 4 835 / **88 800** ; forcés 17 361 / 101 969 /
  33 632 ; copies ROM→? 4 616 / 48 / **18 964** ; provisoire 27 408 / 1 034 /
  4 637 (confirmés ×6). La classe carte d'un octet peut glisser
  statique→copié (précédence de la carte) : présentation, pas perte
  (drapeaux OU-és).

### Écarts consignés lors des retraits moteurs (à traiter dans le noyau, pas par option)

- NES : tables de sauts inter-banques (0 racine au noyau à l'époque — à
  re-vérifier), chaleur jouée par octet PNG (`dpCh` bit 5 → A9),
  `--export-state` disparu.
- PCE : copies jumelles, tables indirectes, vues `lda/tam`, typage
  CD-ROM2/ADPCM/SuperGrafx, racines de glissement de fenêtre, aiguillage de
  banque.
- MD : S4/copies jumelles/`bsr`-`jsr` statiques, chaleur, liveness, chaîne de
  films `--tas-dir`, rtcode/`--cov`/`--romdata` à re-tester côté noyau.
- Sega : rendu (fait 0.373), typage (fait 0.375) ; restant : evidence/romdata,
  `--rtcode` (P14).
- GB : sessions de lecture historiques, frontière par lots, evidence/romdata,
  cellules legacy.

### Précisions de méthode conservées

- `--cov` ne contient que les octets naturels et forcés (format de l'ancien
  moteur), pas le statique : comparer le statique par vidage des drapeaux
  (`DP_CORE_STATIC` sans `NATURAL`/`FORCED`), octet par octet.
- Comparer des runs : toujours `--jobs 1` ; hors `--jobs 1` les sessions
  rendent l'exploration non déterministe (les positions marquées diffèrent).
- « Moins d'octets statiques » peut être « moins de faux code » (Aerial :
  −779 = 1 080 perdus dont des tuiles, 301 gagnés) : le total ne départage pas
  la nature — d'où le provisoire et la vérité terrain.
- `campaign_fixtures` : verte 0.380.1 et 0.382.0 (1 217 s) ; suite 165 tests
  (0.382.0) → 168 (0.384.0). Parité contre les moteurs historiques : impossible
  à rejouer (références figées `tests/golden/`).
