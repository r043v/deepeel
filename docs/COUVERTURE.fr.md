# Norme de couverture multi-plateforme

Ce document fixe **la norme** que toute plateforme de deepeel doit atteindre
pour découvrir les zones couvertes d'une ROM. La référence est le chemin
Game Boy : toutes ses techniques deviennent obligatoires partout. S'y ajoutent
les techniques nées sur les autres plateformes (z80, PCE, MD, NES) et absentes
du GB, qui deviennent à leur tour obligatoires, GB compris.

Il est aussi le cahier des charges des chantiers suivants :
- couverture typée et sources multiples ;
- frontière différée sans éviction budgétaire ;
- mutualisation du moteur ;
- intégration libretro ;
- PNG standard à blocs adaptatifs.

Ce document décrit la cible. L'état actuel est noté dans chaque fiche avec ses
références `fichier:ligne` (arbre au commit `3d76a02`, version 0.28.0).

Sommaire :
1. Principes
2. Catalogue normatif des techniques
3. Matériel intégral
4. Modèle de couverture : nature, types, sources
5. Index 0 : inférer une table depuis un accès indexé
6. Frontière différée : zéro éviction budgétaire
7. Mutualisation : architecture cible
8. libretro : deux niveaux
9. PNG standard à blocs adaptatifs
10. Propositions supplémentaires
11. Feuille de route
12. Annexes

---

## 1. Principes

1. **Une seule chaîne de couverture, plusieurs plateformes.** Une plateforme
   fournit sa machine, son ISA, son registre matériel et ses catalogues de
   sondes. Tout le reste est commun :
   - file et frontière, dédup, sections ;
   - graines, fermeture, provenance ;
   - copies, sessions, TAS ;
   - cartes et rapports.
   Aucune technique n'est réécrite par plateforme (§7).
2. **Tout est borné, rien n'est perdu.** Les gardes existants (budget de pas,
   cap d'arrière-branche, cap de fourches par site) restent : ils protègent la
   recherche live. Mais une coupe budgétaire ne jette plus un état : elle le
   **reporte** dans la frontière sur disque (§6). Seules les évictions
   *sémantiques* jettent un état : exacte, même contexte, non-sens.
3. **Rien de silencieux.** Chaque garde, report, refus et saturation a son
   compteur, qui apparaît dans le rapport texte, le JSON et `deepeel_meta`.
4. **Le matériel entier compte** (§3). Chaque surface matérielle d'une
   plateforme a au moins une des quatre contributions suivantes :
   - elle est observée (registre, bus) ;
   - elle est sondée (valeurs lues alternatives) ;
   - elle est copiée (canal DMA ou boucle CPU) ;
   - elle sert au typage (§4).
   Ou bien elle est explicitement listée comme limite.
5. **Typée et sourcée.** Chaque octet couvert porte trois choses :
   - sa nature : code, donnée, table, padding ;
   - ses types de contenu : tiles, musique, samples… ;
   - l'ensemble de ses sources de découverte, qui peuvent être plusieurs.
   Plus il y a de sources indépendantes, plus la certitude est grande.
6. **Observations de l'outil, pas mesures du matériel.** Les nombres tirés de
   ROMs réelles viennent d'une machine concrète, avec la RAM à zéro et des
   registres événementiels statiques (règle AGENTS.md).
7. **Clés ROM compactes** : `bank * 0x4000 + (addr & 0x3FFF)` (`dp_rom_index`),
   jamais `bank << 16`. Pour les plateformes à unité de banque différente
   (PCE et MSX en 8 Ko, NES PRG en 8/16 Ko, MD linéaire), l'index reste
   l'**offset physique dans le fichier ROM**. Seule la *présentation* des cartes
   découpe en tuiles de banque. L'exécution hors ROM est comptée
   (`exec_outside`) et ne marque pas la ROM.

---

## 2. Catalogue normatif des techniques

**Légende de la matrice** : ✅ fait · ◐ partiel · ✗ absent · — sans objet.

Pour chaque technique, la fiche indique :
- **Méca.** : le mécanisme ;
- **Réf.** : la référence d'origine ;
- **Bornes** : les gardes et leurs compteurs ;
- **Produit** : ce qui est marqué ;
- **Norme** : ce qui est exigé partout.

### 2.1 Matrice de synthèse

| # | Technique | GB | SMS/GG | MSX | NES | PCE | MD |
|---|---|---|---|---|---|---|---|
| D1 | Marquage exec natural/forced/entry/irq/start | ✅ | ✅ | ✅ | ◐ | ◐ | ◐ |
| D2 | Lectures de données ROM + hits | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| D3 | Accès registres par registre (`DpIoStat`) | ✅ | ✅ | ✅ | ◐ | ◐ | ◐ |
| D4 | Copies par boucle CPU | ✅ VRAM | ✅ | ✅ | ✗ | ✅ port VDC | ✅ MOVE→VDP |
| D5 | Copies DMA | ✅ OAM | — | — | ✅ OAM | ✅ TII/TDD… | ✅ DMA VDP |
| D6 | Timeline de banques | ✅ | ✅ | ✅ | ✅ UxROM | ✅ MPR | ✅ SSF2 |
| D7 | Fourches deux issues des conditionnels | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ Bcc/DBcc |
| D8 | Fourches IRQ/NMI | ✅ | ✅ | ✅ | ✅ NMI | ✅ | ✅ V-int |
| D9 | Fourches d'IRQ manette (touche → IF) | ✅ | — | — | — | — | — |
| D10 | Sondes de lecture manette | ✅ P1 8 masques | ✅ 128 comb. | ✅ | ✗ | ✅ 12 | ◐ 4 |
| D11 | Fourches de répétition (blocs) | — | ✅ ED | ✅ ED | — | ✅ BBR/BBS | ✅ DBcc |
| D12 | Cibles indirectes observées | ✅ `jp hl` | ✅ `jp (hl/ix/iy)` | ✅ | ✗ | ✗ | ✗ |
| D13 | Coupes de non-sens | ✅ | ✅ padding `pad_cuts` | ✅ | ◐ illégal | ◐ illégal | ◐ faute |
| D14 | Chien de garde HALT, glissade de padding | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| S1 | Scan padding + tables de pointeurs | ✅ | ✅ | ✅ | ✗ | ✅ inf. | ✗ |
| S2 | Fermeture statique de flot | ✅ | ✅ vues | ✅ slots | ✗ | ◐ fenêtre MPR | ✗ |
| S3 | Suivi de constantes / provenance | ◐ HL/A | ✅ complet | ✅ | ✗ | ✗ | ◐ témoin An |
| S4 | Tables de sauts depuis l'opérande | ✗ | ✅ provenance | ✅ provenance | ✗ | ✅ `JMP (abs,X)` | ✗ |
| S5 | Frères de copie | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| S6 | Rejeu de décodeurs sur tables de flux | ✗ | ✗ | ✗ | ✗ | ✗ | ✅ |
| S7 | Inférence de voisins à la carte | ✅ | ✅ | ✅ | ✗ | ✅ | ✗ |
| G1 | Graines reset/RST/IRQ | ✅ | ✅ | ✅ | ◐ reset | ◐ reset | ◐ reset |
| G2 | Scan de dispatch par vues de banques | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| G3 | `--seed-banks`, `--entry` | ✅ | ✅ | ✅ | ◐ options mortes | ✗ | ✗ |
| G4 | Semis indirect + budgets en cascade | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| C1 | Sections, liveness, verdicts | ✅ | ✅ | ✅ | ◐ req/forced | ◐ req/forced | ✗ |
| C2 | Éviction par contexte | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| C3 | Dédup exacte hors horloges | ✅ | ✅ | ✅ | ✅ | ✅ | ✗ |
| F1 | File croissante + spool + `--resume` | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| F2 | Arbre de choix persistant | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| F3 | `--jobs` par instances clonées | ✅ | ✅ | ✅ | ✗ | ✗ | ✗ |
| J1 | Sessions jouées multi-thread + pool | ✅ | ◐ | ◐ | ◐ xorshift | ◐ | ✗ |
| J2 | Rejeu TAS prioritaire + shoots | ✅ | ✅ | ✅ | ✅ | ✅ | ◐ GMV |
| J3 | Identité ROM des films | ✅ | ✅ SHA-1 | ✅ SHA-1 | ✗ | ✗ | ✗ |
| J4 | Go-Explore (cellules écran) | ✗ | ✗ | ✗ | ✗ | ✅ | ✗ |
| J5 | Mode humain, export `.sta` | ✅ | ✗ | ✗ | ✗ | ✗ | ✗ |
| O1 | `.map` texte, PNG, JSON, heat | ✅ | ✅ `.map`, maprender | ✅ | ◐ | ✅ via maprender | ✗ dump brut |
| O2 | `.cov`, romdata, `--evidence` | ✅/✅/✗ | ✅/✅/✅ | ✅/✅/✅ | ✗ | ✗ | ✗ |
| O3 | `--watch` (SDL2) | ✅ | ✗ | ✗ | ✗ | ✗ | ✗ |

**Norme** : toutes les lignes passent à ✅ sur toutes les plateformes, sauf
« sans objet » justifié. La mutualisation (§7) est le moyen d'y arriver : une
technique écrite une fois dans le noyau est acquise par toutes les plateformes
qui fournissent les hooks requis.

### 2.2 Techniques dynamiques (machine instrumentée)

**D1 — Marquage d'exécution.**
- **Méca.** : avant chaque instruction, l'opcode et ses opérandes sont marqués.
  - `NATURAL` ou `FORCED` selon le chemin.
  - `START` sur l'octet d'opcode.
  - `ENTRY` sur une arrivée non séquentielle.
  - `IRQ` sur le code exécuté dans un handler.
- **Réf.** : `src/explore/explorer.c:731-984` (`hook_step`), marquage 933-963.
- **Bornes** :
  - `--max-steps` → `step_cuts` ;
  - `--max-total` → `budget_steps` ;
  - anneau de 8 arrière-branches, `--loop-cap` → `cycle_cuts` (`explorer.c:894-910`).
- **Produit** : flags d'exec, `exec_hits`, `play_exec_hits` (sessions seules),
  `exec_outside` pour pc hors ROM.
- **Norme** : les flags `ENTRY`/`IRQ`/`START` existent partout. Sur NES/PCE/MD,
  l'`ENTRY`, l'`IRQ` et la distinction opcode/opérande sont à ajouter.

**D2 — Lectures de données.**
- **Réf.** : `hook_read`, `explorer.c:602-629`.
- **Produit** : `DATA_READ`, `data_hits`, `play_data_hits`.
- **Norme** :
  - chaque lecture doit aussi alimenter la provenance (S3) pour le typage (§4) ;
  - les lectures **non CPU** comptent aussi : DMA, fetch PPU de la CHR-ROM NES,
    lecture Z80 de la ROM MD via la fenêtre de banque, DMC NES.

**D3 — Accès registres.**
- **Méca.** : par registre du `DpPlatform`, comptes lecture/écriture, 8 valeurs,
  4 sites, copies et événements de banque (`explorer.h:60-67`).
- **Norme** :
  - la table `DpIoReg` est la source unique ;
  - chaque registre a une `category` qui sert au typage (« sound » → music/samples,
    « video » → tiles/tilemap/palette selon la destination) ;
  - les implications (`DP_IMPL_*`, `src/platform/platform.h:14-29`) doivent être
    **vraies** : un registre marqué `MODELLED` sans modèle est un bug de doc ;
    c'est le cas aujourd'hui de HDMA/VBK GB, cf. §3.1.

**D4 — Copies par boucle CPU.**
- **Méca.** :
  - le suivi est une fenêtre ±0x80 côté source et côté destination, avec un
    hachage FNV de chaque côté ;
  - une arrière-branche compte une itération ;
  - une rupture après au moins 2 itérations émet une copie ;
  - la copie est verbatim si les deux côtés ont même longueur, même compte et
    même hachage.
- **Réf.** : `src/explore/copies.c:87-124` (`dp_record_copy`) ; fusion 135-197.
- **Bornes** : `max_copies` → `copies_dropped`.
- **Norme** : toutes les destinations, pas seulement la VRAM :
  - **ROM→WRAM** (décompression, init RAM, code copié en RAM) ;
  - **ROM→SRAM** ;
  - **ROM→registre son** (wave RAM GB, YM2612 DAC, PSG) ;
  - **ROM→port VDP** (z80 et PCE le font déjà).
  Les chaînes doivent aussi être suivies par provenance : ROM→WRAM→OAM (DMA GB
  depuis la WRAM), ROM→RAM Z80→YM (MD).

**D5 — Copies DMA.**
- **État** :
  - GB : OAM `$FF46` depuis la ROM (`explorer.c:678-694`) ;
  - NES : OAM `$4014` ;
  - PCE : TII/TDD/TIN/TIA/TAI ;
  - MD : DMA VDP (`md/explore.c:83`, `:327`).
- **Norme** : tout canal DMA du matériel est un canal de copie typé par sa
  destination :
  - HDMA CGB ;
  - DMC NES ;
  - DMA VDP MD vers VRAM, CRAM ou VSRAM ;
  - DMA depuis RAM avec provenance.

**D6 — Timeline de banques.**
- **Méca.** : l'écriture dans la fenêtre mapper produit un `DpBankEvent`
  (ancienne → nouvelle banque) (`explorer.c:699-729`). Ces événements alimentent
  `banks_seen` des sections, qui sert d'indice de banque à la fermeture.
- **Norme** : la timeline couvre tous les mappers et toutes les fenêtres (MPR
  PCE, slots MSX, `$3E` Sega, SSF2 MD). Le contrôle mémoire en fait partie.

**D7 — Fourches deux issues.**
- **Méca.** : à chaque conditionnel, on force le flag de condition (Z/C, N/V/C/Z,
  CCR minimal), puis on pousse la variante `forced`.
- **Réf.** :
  - GB `fork_branch` `explorer.c:1021-1051` ;
  - NES `hook_step` `nes/explore.c:131` ;
  - PCE `fork_branch` `pce/explore.c:264` ;
  - MD `on_end` `md/explore.c:221`.
- **Bornes** :
  - cap par site (`fork_site_cap` 16 GB, 2 z80/MD, 4 NES/PCE) → `forks_capped` ;
  - élagage « deux côtés déjà couverts » (`both_sides_covered`, `explorer.c:992`)
    → `forks_covered_pruned`.
- **Norme** :
  - même cap configurable, même élagage, même compteur partout ;
  - au-delà du cap, la fourche est **reportée** dans la frontière, palier
    « fork-cap », et n'est plus abandonnée (§6) ;
  - **correctif** : `fork_counts` cesse de croître à 1<<20 entrées
    (`explorer.c:175`), après quoi `dp_map_get_or_add` rend NULL et le cap n'est
    plus appliqué (`explorer.c:1024-1025`). La saturation de la table doit
    être comptée, et l'état reporté plutôt que fourché sans cap.

**D8 — Fourches d'IRQ/NMI.**
- **Méca.** : quand IME est actif et `IE & IF` non nul, on crée une variante
  « IRQ non servie ».
- **Réf.** :
  - GB `fork_irq` `explorer.c:1053-1070` ;
  - PCE `fork_irq` placé à l'instruction qui démasque (CLI/PLP/RTI, `pce/explore.c:313`) ;
  - Pause NMI Sega, y compris pendant un HALT.
- **Norme** :
  - fourche à **l'instant du démasquage** (technique PCE) en plus de l'instant
    de la prise ;
  - cap par site partout (GB n'en a pas pour les IRQ) ;
  - toutes les sources d'IRQ du matériel : timer, série, LYC/STAT, ligne VDP,
    IRQ mapper (MMC3, MMC5), IRQ externes.

**D9 — Fourches d'IRQ manette.**
- **Méca.** : avec IE.4 actif, chaque touche est enfoncée ; la variante n'est
  gardée que si IF.4 monte (`fork_key`, `explorer.c:1078-1115`).
- **Norme** : générique pour toute IRQ déclenchée par une entrée (sans objet
  sur les plateformes qui n'en ont pas).

**D10 — Sondes de lecture d'entrées.**
- **Méca.** : à la lecture du port d'entrée, avant exécution, on propose
  d'autres valeurs lues et on fourche. Chaque masque est proposé au plus une fois
  par site. La restauration se fait par image complète.
- **Réf.** :
  - GB `fork_p1` `explorer.c:1123-1155`, 8 masques ;
  - z80 `probe_inputs`/`fork_inputs` `sega/explore.c:670`/`:721`, 8 propositions
    puis jusqu'à 128 combinaisons ;
  - PCE `pad_catalog` `pce/explore.c:278` ;
  - MD `on_read` `md/explore.c:283`, 4 motifs.
- **Norme** :
  - le **catalogue de 128 combinaisons z80** devient la référence : 8 masques
    initiaux, puis combinaisons, avec reports différés quand la file est pleine
    (`deferred retry skips`) ;
  - le catalogue est fourni par la plateforme, le mécanisme est commun ;
  - à ajouter : NES `$4016/$4017`, matrice clavier MSX, pad 6 boutons MD,
    multitap PCE, paddle et phaser SMS.

**D11 — Fourches de répétition.**
- **Méca.** : une instruction de bloc ou de boucle (LDIR/CPIR/INIR/OTIR, BBR/BBS,
  DBcc) est forcée à sortir ou à continuer.
- **Réf.** :
  - `dp_z80_force_repeat` `src/cpu/z80/z80.c:77` ;
  - PCE `branch_taken` `pce/explore.c:246` (bit en zero page) ;
  - MD reconstitution du compteur DBcc (`md/explore.c:221`).
- **Norme** : toute instruction à condition implicite est un conditionnel au sens
  de D7, avec le même cap.

**D12 — Cibles indirectes observées.**
- **Méca.** : par site de saut indirect, jusqu'à 8 cibles réellement prises, plus
  un flag `overflow`. Après les recherches, les cibles jamais exécutées sont
  semées en prioritaire.
- **Réf.** :
  - GB `explorer.c:869-886`, `DpIndirect` `explorer.h:75-81`, semis
    `dp_seed_indirect` `explorer.c:1786-1807` ;
  - z80 `record_indirect` `sega/explore.c:91`.
- **Norme** :
  - à étendre à `jmp (abs)` / `jmp (abs,x)` 6502/HuC6280 et `jmp/jsr (An)` 68000 ;
  - garder aussi l'**adresse de table** quand elle est connue, pour S4 et §5 ;
  - un débordement (plus de 8 cibles) est un indice de table de sauts : on la
    déclare via S4 au lieu de perdre les cibles en trop.

**D13 — Coupes de non-sens.**
- **Méca.** : pc en RAM écho ou en OAM, pc sur des données verbatim non
  naturelles, RET/RETI avec SP incohérent → `nonsense_*`.
- **Réf.** : `explorer.c:796-812`.
- **Norme** : chaque plateforme déclare ses zones « non exécutables »
  (régions `DP_SPACE_*`) et sa plage de pile plausible. La coupe est commune.

**D14 — Chien de garde HALT, glissade de padding, reset.**
- **Réf.** : `src/mach/m0boy.c:168-185` (HALT, 8 vblanks), glissade sur padding
  (`explorer.c:822-832`, 4096 pas → `pad_cuts`).
- **Norme** : commune à toutes les plateformes (WAI 65C02, STOP 68000, HALT z80).

### 2.3 Techniques statiques (sans machine)

**S1 — Scan de padding et de tables de pointeurs.**
- **Méca.** : les séries d'au moins 8 octets `00`/`FF` sont marquées `PAD`. Au
  moins 4 mots consécutifs pointant dans la fenêtre ROM, hors padding, sont
  marqués `POINTER`.
- **Réf.** : `dp_static_scan` `explorer.c:244-291`.
- **Norme** :
  - paramétré par l'ISA : largeur et endianness du pointeur, 16 bits petit
    boutiste ou 24/32 bits grand boutiste pour MD ;
  - paramétré par la plateforme : fenêtres ROM, octets de padding.

**S2 — Fermeture statique de flot.**
- **Méca.** : depuis les débuts d'instruction connus, on suit le fall-through,
  les sauts et appels (les deux côtés des conditionnels) et les RST. La
  confiance est perdue après un CALL (idiome des données en ligne).
- **Réf.** :
  - GB `dp_static_closure` `src/explore/closure.c:79-221` ;
  - z80 à **vues de banques et de slots** `src/platform/sega/closure.c` (583 lignes ;
    `view_hash` :45, `capture` :104, `address_effect` :223).
- **Norme** :
  - la fermeture **à vues** (z80) devient la référence, GB compris : chaque
    contexte de mapping (banque, MPR, slot) est une vue, et une arête qui sort de
    la fenêtre fixe est résolue par vue ;
  - en même temps, raffinement des écritures indirectes connues
    (`indirect_store` :388) ;
  - le budget est en cascade (§2.4 G4).

**S3 — Suivi de constantes et provenance.**
- **Méca.** :
  - z80 : paires HL/IX/IY complètes (immédiats, mots ROM absolus, INC/DEC
    exacts, LD SP,HL), octets A par transfert ;
  - la provenance dynamique suit tous les registres et la RAM, avec une
    origine par octet de RAM (`src/cpu/z80/provenance.c`, 193 lignes) ;
  - GB : HL et A seulement (`src/explore/provenance.c:91-241`).
- **Produit** :
  - `jp hl` résolus → `CODE_STATIC` ;
  - lectures à adresse connue → `DATA_STATIC` ;
  - preuves de retour.
- **Norme** :
  - la provenance z80 complète devient la référence ;
  - elle est généralisée par l'ISA (§7, `DpIsa`) et sert aussi au **typage**
    (§4) et à l'**index 0** (§5) ;
  - correctifs GB :
    - `add hl,rr` oublie HL (`provenance.c:49`) au lieu de garder « base
      prouvée + index inconnu » ;
    - les cibles de branche sont rebanquées par `bank*0x4000 + (t&0x3FFF)`
      même quand la cible change de fenêtre ;
    - `prov_steps` s'accumule entre appels.

**S4 — Tables de sauts depuis l'opérande.**
- **Méca.** : sur `JMP (abs,X)` observé, depuis la base (entrée 0), on parcourt
  les mots tant qu'ils pointent dans du code observé de la même fenêtre.
- **Réf.** : `tables()` `src/platform/pce/explore.c:788-812` → `DP_PCE_TABLE`.
- **Norme** : technique commune, pilotée par le `DpIsa` (opérande « base + index »).
  C'est la forme statique de l'index 0 (§5).

**S5 — Frères de copie.**
- **Méca.** :
  1. on part d'une copie verbatim d'au moins 16 octets ;
  2. on cherche l'adresse source comme mot dans la banque 0 ou dans la banque
     de la copie ;
  3. la table autour de ce mot donne les autres entrées (2 à 64), marquées avec
     la même longueur.
- **Réf.** : `dp_copy_siblings` `src/explore/copies.c:231-300`.
- **Norme** : commune. La longueur peut aussi venir d'un en-tête lu (décodeur, §5).

**S6 — Rejeu de décodeurs sur tables de flux.**
- **Méca.** :
  1. une routine qui lit des flux depuis des pointeurs est repérée par deux
     départs observés à pas constant (4..16) ;
  2. la table est étendue dans les deux sens, index 0 compris, tant que les mots
     sont des pointeurs ROM valides ;
  3. la routine est **rejouée** sur chaque entrée.
- **Réf.** : `src/platform/md/explore.c:120-167` (`find_decoder`, `call_entered`,
  `stream_read`), `:476-540` (`replay_one`, `replay_streams`).
- **Norme** : technique commune (§5.4). C'est la seule qui couvre exactement des
  données compressées, musiques et scripts jamais joués.

**S7 — Inférence de voisins à la carte.**
- **Méca.** : sur les plages inconnues d'une banque, appliquée au rendu seulement :
  - tout `00`/`FF` sur au moins 4 octets → padding inféré ;
  - code des deux côtés sur au plus 3 octets → code inféré ;
  - données des deux côtés sur au plus 1024 octets → données inférées.
- **Réf.** : `dp_map_build` `src/report/map.c:43-61` ; PCE `infer` `pce/explore.c:817`.
- **Norme** : commune. Source `map-inference` dans le modèle §4 : **jamais**
  confondue avec une observation.

### 2.4 Graines

**G1 — Vecteurs.**
- **Méca.** : reset en naturel ; RST, IRQ et NMI en forcé.
- **Réf.** : `dp_seed_gb` `explorer.c:1716-1781`.
- **Norme** : chaque plateforme déclare ses `DpEntry` (vecteurs 6502 `$FFFA-$FFFF`,
  table 68000, vecteurs HuC6280) ; le semis est commun.
- **Correctif** : les graines d'IRQ GB sont semées `forced=0`, donc leur code peut
  compter comme naturel. Elles doivent porter la source `seed-vector`.

**G2 — Scan de dispatch par vues de banques.**
- **Méca.** : un mot de la fenêtre commutable encadré par deux autres est une
  entrée de table de dispatch, semée dans **chaque** banque. Un mot isolé est
  semé dans la vue par défaut.
- **Réf.** : GB `explorer.c:1738-1773` ; z80 `sega/explore.c:905-946` via
  `seed_geometry`/`seed_window` (`backend.h`).
- **Norme** : commune via la géométrie de banques du backend (§7).

**G3 — `--seed-banks`, `--entry b:addr`.**
- **Norme** : options communes. Sur NES elles existent mais ne sont pas
  utilisées (`nes/explore.c:695`), ce qui est un bug à corriger.

**G4 — Semis indirect et budgets en cascade.**
- **Méca.** :
  - `closure_rounds` (`src/deepeel.c:1178-1210`) enchaîne fermeture → semis →
    recherche jusqu'à stabilité, puis provenance, frères et fermeture finale ;
  - `seed_total = 2 × seed_cap` est partagé entre les phases, avec le solde
    reporté (z80 : budgets propres par phase).
- **Norme** :
  - cascade z80 (budget propre par phase, solde reporté) partout ;
  - une graine refusée faute de solde est **reportée** dans la frontière (§6),
    et n'est plus seulement comptée ;
  - **correctif** : `--no-closure` coupe aussi la provenance et les frères
    (`deepeel.c:1180`). Il faut séparer les options.

### 2.5 Sections, contexte, dédup

**C1 — Sections et verdicts.**
- **Méca.** : section = plage de code exécutée d'un bloc. Liveness :
  - `ctx_mask` (registres, IME, IE, banque, VBK) ;
  - pages WRAM/VRAM/SRAM lues ;
  - HRAM, OAM ;
  - `io_read_mask`.
- **Verdicts** (`explorer.c:1539-1589`) :
  - `required` : boot, IRQ ou copies ;
  - `played` : uniquement naturel ;
  - `forced` sinon.
- **Réf.** : `src/explore/sections.c`, `explorer.h:84-126`.
- **Norme** :
  - commune ; les composants de contexte sont déclarés par le backend (§7) ;
  - le verdict `speculative`, prévu (`explorer.h:90`) mais jamais assigné, est
    défini ainsi : section atteinte **uniquement** par des sources statiques ou
    inférées.

**C2 — Éviction par contexte.**
- **Méca.** : à l'entrée d'une section, l'état est projeté sur ce que la section
  lit réellement (`project_ctx`, `explorer.c:1160-1186`). Un contexte déjà vu
  est évincé (`evicted_ctx`).
- **Norme** : commune.
- **Correctif** : `project_ctx` hache `api->vram + 0x2000 + pg*256` alors que
  `gbl_vram` fait `0x2000` octets (`m0boy/targets/meta/runtime/gbl.h:44`). C'est
  une surlecture dès qu'un bit de `page_mask_vram[1]` est posé.

**C3 — Dédup exacte.**
- **Méca.** : hachage de l'image hors compteurs de cycles (`image_hash`,
  `explorer.c:374`), persisté dans `exact.bin`.
- **Norme** :
  - commune ; chaque backend fournit sa projection « hors horloges » (comme
    `state_hash` NES, `nesmach.c:173`) ;
  - un set plein ne jette plus l'état : il est reporté (§6).

### 2.6 File, persistance, parallélisme

**F1 — File croissante, spool, `--resume`.**
- **Réf.** : `queue_push_pri` `explorer.c:411-498`, `queue_grow` :386-402,
  spool :1188-1359 ; z80 `spool_write_path` `sega/explore.c:377`.
- **Norme** : remplacée par la frontière différée (§6), qui la contient.

**F2 — Arbre de choix.**
- **Méca.** : chaque fourche est un nœud (`site`, `kind`, `issue`) persisté dans
  `tree.bin` (`src/explore/tree.c`). Les issues possibles :
  `open/done/merged/dead/reset/budget/capped/nonsense/spooled`.
- **Norme** :
  - commune ; l'issue `budget` disparaît au profit de `deferred` (§6) ;
  - **correctif** : `tree.bin` Sega écrase le compte de nœuds par la version
    (`sega/explore.c:1287-1288`).

**F3 — `--jobs`.**
- **Méca.** : N instances du runtime clonées par `objcopy --redefine-syms`
  (`CMakeLists.txt:70-224` GB, `:338-384` sg2/gg2), avec pool, dédup et spool
  partagés.
- **Norme** : toute plateforme fournit au moins 2 instances. PCE n'a aucun
  préfixage aujourd'hui, MSX aucun clone. Avec libretro, une copie du `.so` par
  instance (§8).

### 2.7 Couverture jouée

**J1 — Sessions jouées.**
- **Méca.** :
  - workers attract / script / TAS / aléatoire ;
  - pool de checkpoints (48) ;
  - ordonnanceur d'époques à crédit ;
  - inbox vers le fork-explorer (cap 512 → `inb_dropped`) ;
  - mode humain.
- **Réf.** : `src/explore/sessions.c` (1614 lignes).
- **Norme** :
  - commune, au-dessus de l'interface « frame » du backend (`set_input`,
    `advance`, `save`/`load`) ;
  - l'inbox pleine reporte au lieu de jeter (§6) ;
  - le xorshift NES/PCE et le `play_session` z80 sont absorbés.

**J2 — TAS.**
- **Méca.** :
  - le film est prioritaire sur tout budget ;
  - checkpoints périodiques, puis **shoots** : chaque checkpoint est semé dans le
    fork-explorer ;
  - profils de synchro VBA.
- **Réf.** : `src/explore/tas.c`, `sessions.c:1017-1060`.
- **Formats** : vbm/vmv/bk2 (GB), bk2/texte (z80), fm2/bk2 (NES), bk2/hex (PCE),
  gmv (MD).
- **Norme** : un seul type interne `DpMovie` et des chargeurs par extension
  (§7) ; les shoots partout.

**J3 — Identité ROM des films.**
- **Réf.** : `dp_tas_rom_match` (`tas.c:208`).
- **Norme** : commune (SHA-1/CRC selon le format).

**J4 — Go-Explore.**
- **Méca.** : archive de cellules (empreinte d'écran réduite, octet de vies
  appris). On revient à la cellule la moins visitée et on explore depuis elle.
- **Réf.** : `novelty_search` `src/platform/pce/explore.c:631`, `lives_frame`
  :591, `screen_key` `pcemach.c:307`.
- **Norme** : commune. Elle devient la stratégie de choix de segment du pool de
  checkpoints, en plus de `segment_end` (`sessions.c:447-493`).

**J5 — Mode humain, export `.sta`, `--watch`.**
- **Norme** : communs à toutes les plateformes qui ont un framebuffer.
  libretro niveau A en fournit un (§8).

### 2.8 Sorties

- **Norme** :
  - PNG par défaut partout (§9) ;
  - `.map` texte ;
  - JSON et résumé ;
  - `.cov` ;
  - `--evidence` (bitmap complet des preuves, aujourd'hui z80 seulement,
    `sega/explore.c:1655`) ;
  - heat ;
  - HTML.
- `--evidence` est absorbé par le chunk binaire pleine résolution du PNG (§9.3).

---

## 3. Matériel intégral

**Règle** : chaque surface matérielle d'une plateforme figure dans son
`DpPlatform`, avec catégorie et implications **vraies**. Elle reçoit au moins
une contribution :
- **Obs.** : observée ;
- **Sonde** : valeurs lues alternatives ;
- **Copie** : canal de copie ;
- **Type** : sert au typage §4 ;
- **Banque** : timeline de mapping.
Sinon elle est listée comme limite.

### 3.1 Game Boy

| Surface | État | Contribution exigée |
|---|---|---|
| SM83, IRQ ×5, RST ×8 | ✅ | — |
| PPU (LY, STAT, LYC, SCX…) | réel m0boy, registres événementiels **statiques** pour l'explorateur | **Sonde** sur lectures LY/STAT (§3.7) |
| OAM DMA `$FF46` | ✅ ROM | **Copie** depuis la WRAM avec provenance (chaîne ROM→WRAM→OAM) |
| APU NR10-NR52, wave RAM `$FF30-3F` | observé seulement | **Type** music (registres) / samples (wave RAM) |
| Timer DIV/TIMA/TMA/TAC | réel, IRQ fourchée | **Sonde** DIV/TIMA (graines RNG) |
| Série SB/SC | transfert sans partenaire | **Sonde** sur l'octet reçu (partenaire de lien), IRQ série |
| Joypad P1 | ✅ IRQ + P1 | — |
| MBC0/1/2/3/5 | ✅ | **Banque** |
| RTC MBC3 | **non simulé** (`gbl_core.c` : « no RTC on this target ») | **Sonde** des registres RTC |
| MBC6/7, MMM01, HuC1/3, TAMA5, caméra | **refusés** | Machine ou libretro (§8) |
| CGB : VBK, SVBK, HDMA1-5, palettes BCPS/OCPS, KEY1 | registres listés, **non émulés** ; la doc dit « VBK réellement commuté », ce qui est faux | **Copie** HDMA, **Banque** VBK/SVBK, **Type** palette |
| SGB (commandes par P1) | absent | **Type** des paquets SGB (bordures, palettes) |
| Boot ROM | absente (registres au reset DMG) | limite documentée |

**Corrections de doc à faire** : `docs/GB.fr.md` §Limites (l.121-135) dit encore
« Pas de PPU/APU/timer » et « VBK réellement commuté ». Les deux affirmations
sont fausses aujourd'hui, et les implications `HDMA`/`MODELLED` des registres
CGB sont mensongères.

### 3.2 SMS / Game Gear / SG-1000

| Surface | État | Exigé |
|---|---|---|
| Z80, VDP (TMS9918 / mode 4), PSG SN76489 | ✅ | **Type** : port VDP → tiles/tilemap/palette selon l'adresse VRAM/CRAM ; PSG → music |
| `$3E` contrôle mémoire, mappers sega/codemasters/korean150 | ✅ | — |
| Pause NMI | ✅ | — |
| V/H-counter `$7E/$7F` | statiques | **Sonde** (§3.7) |
| YM2413 FM (SMS japonaise) | absent | Obs. + **Type** music |
| Phaser, paddle, lunettes 3D, port de lien GG | absents | **Sonde** catalogue |

### 3.3 MSX1

| Surface | État | Exigé |
|---|---|---|
| Slots, PPI, AY, VDP TMS, 5 mappers | ✅ | — |
| BIOS | simulé par traps | BIOS réel optionnel (libretro ou ROM fournie hors dépôt) |
| Matrice clavier (PPI port B) | ? | **Sonde** catalogue (touches utilisées par les jeux : espace, F1-F5, curseurs) |
| SCC (ondes) | listé comme mapper | **Type** samples (RAM d'onde SCC) |
| V9938 (MSX2), FM-PAC, disque, cassette | absents | limites ou libretro |

### 3.4 NES

| Surface | État | Exigé |
|---|---|---|
| 6502, PPU événementiel, APU, OAM DMA, NMI/IRQ | ✅ | — |
| Mappers | **NROM, UxROM seulement** | MMC1, MMC3 (+IRQ scanline), CNROM, AxROM, MMC5… : **Banque**, **Fourche IRQ** |
| CHR-ROM | **non suivie** (seul le PRG est indexé) | Index ROM étendu au CHR, **Type** tiles par les fetchs PPU |
| DMC DMA | absent | **Copie** + **Type** samples |
| `$4016/$4017` | ? | **Sonde** manette (D10) |
| `$2002` (vblank, sprite 0) | statique | **Sonde** (§3.7) |
| Opcodes illégaux | NOP | à émuler ou comptés |

### 3.5 PC Engine

| Surface | État | Exigé |
|---|---|---|
| HuC6280, VDC, PSG, MPR, timer, IRQ VDC, pad `$1000` | ✅ | — |
| VCE (palette) | copies ROM→VCE typées palette (0.56) ; classe PNG à venir | **Type** palette |
| ADPCM, CD-ROM², System Card | absents | limites ou libretro |
| Mapper SF2 (> 384 Ko), SuperGrafx (VPC, 2ᵉ VDC) | absents (modulo des pages) | **Banque** |
| Multitap | absent | **Sonde** |
| Instance unique (pas de préfixage) | 2 instances `dp_pce2_` (0.56), partage par phase | file de fourches partagée (F3) |

### 3.6 Mega Drive

| Surface | État | Exigé |
|---|---|---|
| 68000, VDP + DMA, SSF2 | ✅ | DMA typée par destination VRAM/CRAM/VSRAM |
| YM2612 | compilé, non observé | **Type** music ; DAC → samples |
| Z80 son | observé concrètement (0.57), sans fourches | Exploration Z80 (le moteur z80 existe) : code du driver copié en RAM Z80 (**Copie** ROM→RAM Z80), lectures ROM par la fenêtre de banque `$8000` (**Type** samples/music) |
| Pad 3 boutons | sondes de 4 motifs | catalogue complet + 6 boutons |
| SRAM/EEPROM, mappers > SSF2, SVP, 32X, Sega CD | absents | limites ou libretro |

### 3.7 Sondes de registres événementiels (proposition transversale)

**Constat.** Aujourd'hui, un registre `DP_IMPL_EVENT` (LY, STAT, DIV, TIMA,
V-counter, `$2002`) est tenu statique pour l'explorateur. Une boucle d'attente
n'en sort que par les gardes. C'est la limite principale de la règle « registres
événementiels statiques ».

**Proposition.** Traiter ces lectures comme la sonde P1 :
1. à la lecture, proposer un petit catalogue de valeurs **significatives**
   fourni par la plateforme :
   - GB LY : 0, 143, 144, 153, LYC ;
   - STAT : chaque mode ;
   - DIV : 4 valeurs ;
   - V-counter : début/fin de vblank ;
   - `$2002` : bit 7 et bit 6 ;
2. au plus une fois par site et par valeur ;
3. chaque branche est marquée `forced` et porte la source `probe-event`.

**Effets attendus** :
- les attentes de ligne et de vblank se résolvent sans budget ;
- les graines RNG tirées de DIV ou de compteurs ouvrent les deux issues ;
- le compteur `probe_event_*` est rapporté.

**Garde.** Le cap par site est le même que pour D7, avec report au-delà (§6).

---

## 4. Modèle de couverture : nature, types, sources

### 4.1 Les champs par octet de ROM

| Champ | Taille | Contenu |
|---|---|---|
| `nature` | u16 flags | unification de `DP_XF_*`, `DP_DF_*`, `DP_SF_*` (`explorer.h:22-40`) : `CODE_START`, `CODE_OPERAND`, `ENTRY`, `IRQ`, `DATA_READ`, `COPIED`, `VERBATIM`, `TABLE`, `PAD`… |
| `types` | u32 flags `DP_T_*` | contenu typé (§4.2), multiple |
| `sources` | u32 flags `DP_SRC_*` | méthodes de découverte (§4.3), multiples |
| `exec_hits`, `data_hits` | u32 | tous chemins, saturés à 2³²−1 et comptés |
| `play_exec_hits`, `play_data_hits` | u32 | joué seulement (heat) |
| `section_of` | u32 | section + 1 |

- **Coût** : 2 + 4 + 4 + 16 + 4 = 30 octets par octet de ROM, soit environ
  240 Mo pour 8 Mo de ROM.
- **Variante compacte** : les hits peuvent être alloués paresseusement par banque
  touchée.
- Le chunk PNG sérialise **tous** ces champs (§9.3).

Ce modèle **remplace** les flags par plateforme :
- GB `DP_XF/DF/SF` ;
- `DP_SEGA_*` (`sega/explore.h:8`) ;
- `DP_NMAP_*` (`nes/explore.c:641-657`) ;
- `DP_PCE_*` / `DP_PMAP_*` ;
- MD `NATURAL/FORCED/DATA/QUEUED/DATA_NATURAL/DMA_SOURCE/DMA_READ/CPU_VRAM`
  (`md/explore.h:13-15`).

Correspondance complète en annexe A.

### 4.2 Types de contenu (`DP_T_*`) — premiers types

| Type | Règle d'attribution (exemples) |
|---|---|
| `code-boot` | exécuté par le flot du reset avant la première attente d'entrée |
| `code-irq` / `code-nmi` | exécuté dans un handler (D1 `IRQ`) |
| `code-main` | autre code |
| `code-ramcopy` | octet ROM copié en RAM puis **exécuté depuis la RAM** (provenance de l'`exec_outside`) |
| `tiles` | valeur arrivée en VRAM motifs (GB `$8000-$97FF`, zone motifs VDP/VDC selon registres de base), CHR-ROM fetchée par la PPU |
| `tilemap` | valeur arrivée en VRAM carte (GB `$9800-$9FFF`, table de noms VDP/VDC) |
| `sprites` | valeur arrivée en OAM / SAT (directement ou par DMA) |
| `palette` | valeur écrite dans BGP/OBP/BCPD, CRAM, VCE, palettes PPU `$3F00` |
| `music` | octet lu par une routine qui écrit des registres de catégorie « sound » dans la même section (séquenceur), sans être copié tel quel dans un registre d'onde |
| `samples` | valeur arrivée en wave RAM GB, RAM d'onde SCC, DAC YM2612 (`$2A`), DMC NES, ADPCM PCE, PSG en mode DAC (écritures de volume à haute fréquence) |
| `text` | valeur arrivée en tilemap **via une table de traduction** (lecture indexée par l'octet), ou série d'octets dans l'intervalle de la police observée |
| `level` | octets lus par une routine dont les écritures vont en WRAM sous forme de carte (lecture séquentielle 2D) ; d'abord par décodeur (S6) |
| `compressed` | entrée d'un décodeur détecté (S6) ; la sortie porte le type de sa destination |
| `pointer-table` | S1, ou tables prouvées par S4/§5 |
| `jump-table` | table dont les entrées sont **exécutées** comme cibles indirectes (D12, S4) |
| `init-ram` | copié en WRAM au boot (D4 ROM→WRAM depuis du `code-boot`) |
| `header` | en-tête cartouche déclaré par la plateforme (GB `$0100-$014F`, en-tête MD `$100-$1FF`, iNES n'est pas en ROM) |
| `vectors` | table de vecteurs (6502 `$FFFA`, 68000 `$0-$FF`) |
| `save-default` | copié en SRAM (D4 ROM→SRAM) |

**Mécanisme d'attribution.**
- La provenance (S3, généralisée par le `DpIsa`) donne à chaque valeur en registre
  ou en RAM son octet ROM d'origine.
- Quand une valeur atteint une **destination typée**, l'origine reçoit le type.
  Les destinations typées sont :
  - une adresse VRAM avec sa région déclarée par la plateforme ;
  - un registre avec sa catégorie ;
  - une destination DMA.
- La plateforme déclare ses **régions typées** à côté de `DpRegion`. Les zones
  VRAM dépendent de registres de base (VDP, VDC, LCDC) : la région est donc
  calculée au moment de l'écriture par un hook du backend.
- Un octet peut avoir plusieurs types : une donnée lue par deux routines
  différentes les garde tous.

### 4.3 Sources de découverte (`DP_SRC_*`)

| Source | Origine |
|---|---|
| `fork-natural` | chemin du fork-explorer sans fourche forcée |
| `fork-forced` | chemin issu d'une fourche (D7-D11) |
| `probe-input` / `probe-event` | branche de sonde (D10, §3.7) |
| `session-attract` / `session-random` / `session-script` / `session-human` | sessions jouées (J1) |
| `tas` | rejeu de film (J2) — avec identité du film dans la table creuse |
| `go-explore` | segment choisi par l'archive de cellules (J4) |
| `seed-vector` / `seed-pointer` / `seed-bank` / `seed-entry` / `seed-indirect` | graines (G1-G4) |
| `static-closure` | S2 |
| `provenance` | S3 |
| `jump-table` / `index-inference` | S4, §5 |
| `sibling` | S5 |
| `decoder-replay` | S6 |
| `map-inference` | S7 (rendu seulement) |
| `deferred-N` | état repris de la frontière, palier N (§6) |
| `core:<nom>` | cœur libretro ou machine qui l'a observé (§8) — un bit par cœur enregistré |

**Règles.**
- Les sources sont **OR-ées**. On ne choisit jamais une source « gagnante ».
- **Certitude** = nombre de familles indépendantes, sur trois familles :
  - *dynamique-naturel* : sessions, tas, fork-natural ;
  - *dynamique-forcé* : fork-forced, sondes, graines ;
  - *statique* : closure, provenance, tables, frères, rejeu.
  Une observation présente dans deux familles est « confirmée ». Une observation
  présente sur deux cœurs différents est « recoupée ».
- **Table creuse optionnelle** : première découverte par octet, soit
  `(source, horodatage, id de film ou de worker)`. Elle remplace et étend
  `DpDiscEntry` (`explorer.h:136-141`), qui n'est qu'agrégé par phase.

### 4.4 Classes de carte = projection

Les 12 classes actuelles (`src/report/report.h:45-47`) deviennent une **projection**
nature × sources, calculée au rendu :
- code-natural = `CODE_START` ∧ famille naturelle ;
- code-forced = `CODE_START` ∧ famille forcée seulement ;
- et ainsi de suite.

Le priorisme actuel (`dp_map_build` `map.c:22-39`, `dp_results_breakdown`
`explorer.c:1671-1686`) est conservé pour la couleur. Les types forment une
**seconde projection** : une carte « par type », avec un panneau à côté de la
carte par nature, du même type que le panneau de chaleur.

---

## 5. Index 0 : inférer une table depuis un accès indexé

**Question** : quand un jeu calcule un pointeur de donnée `base + index`, est-il
viable de déclarer le même type de contenu à partir de l'index 0 ?

**Réponse : oui, viable**, à condition de connaître la base par **preuve**
(opérande ou constante prouvée), et avec une source distincte `index-inference`.
Aujourd'hui, aucun moteur ne le fait pour les données :
- GB oublie HL sur `add hl,rr` (`provenance.c:49`) ;
- le runtime ne marque que l'octet lu ;
- seuls `JMP (abs,X)` PCE (S4) et le rejeu MD (S6) partent d'une base.

### 5.1 Par ISA : d'où vient la base

| ISA | Forme | Base | Robustesse |
|---|---|---|---|
| 6502 / HuC6280 | `lda tbl,x` / `lda tbl,y` | **l'opérande** | très bonne : base exacte, index = X/Y (8 bits ⇒ borne 255) |
| 6502 / HuC6280 | `lda (zp),y` | pointeur en zero page (provenance de `zp`) | bonne si `zp` prouvé |
| 68000 | `lea tbl(pc),a0` puis `move (a0,d0.w)` / `(d8,a0,d0)` | `a0` prouvé + déplacement | bonne |
| Z80 | `ld ix,tbl` puis `ld a,(ix+d)` ; `ld hl,tbl; add hl,de` | IX prouvé / HL prouvé avant `add` | bonne avec provenance |
| SM83 | `ld hl,tbl; add hl,de` / `ld e,a; ld d,0; add hl,de` ; `add hl,hl` pour ×2 | HL prouvé avant `add` | moyenne : idiomes variés, base biaisée fréquente |

### 5.2 Mécanisme

1. **Provenance « base + index »**.
   - Le treillis abstrait gagne une valeur `Base(b) + Index(stride)` : la base est
     connue, l'index inconnu, et le pas est déduit (`add hl,hl` multiplie par 2 ;
     `sla`/`rl` par 2 ; `add hl,de` deux fois, etc.).
   - Les deux passes produisent le même enregistrement :
     - **statique** (S3) : au niveau de l'instruction ;
     - **dynamique** : le backend consigne par site de lecture l'adresse réelle,
       et la provenance donne la base.
2. **Enregistrement par site** `DpIndexed` :
   - `site`, `base`, `stride`, `width` (octets lus par élément) ;
   - `idx_min`, `idx_max` observés ;
   - `n_obs` ;
   - types des éléments observés.
   - Il est borné : nombre de sites capé, débordement compté et reporté.
3. **Déclaration** :
   - les octets `[base + idx·stride, + width)` pour **idx de 0 à idx_max
     observé** reçoivent la nature `DATA`, les **types des éléments observés** et
     la source `index-inference` ;
   - si les éléments sont des pointeurs (lus par paire puis déréférencés), la
     table devient `pointer-table` et chaque cible inférée est semée (G4) ou
     rejouée (S6).

### 5.3 Garde-fous (obligatoires)

- **Base biaisée** : `ld hl, tbl-1` puis index à partir de 1, ou `tbl-2` pour
  des mots. L'index 0 tombe alors avant la vraie table.
  - Règle : si `idx_min ≥ 1` est observé de façon stable, on ne déclare qu'à
    partir de `idx_min`.
  - L'élément 0 n'est déclaré que si son contenu est cohérent avec les éléments
    observés : un pointeur valide dans la même fenêtre pour une table de
    pointeurs, et pas de padding.
- **Borne haute inconnue** : on n'étend **jamais** au-delà de `idx_max` observé,
  sauf avec une borne prouvée :
  - `cp N; jr nc` devant l'accès ;
  - largeur 8 bits de l'index 6502 ;
  - masque `and $0F`.
  L'extension prouvée porte une source plus faible (`index-inference` +
  `map-inference`).
- **Jamais par-dessus du code naturel** : une collision est comptée
  (`index_refused_code`), comme `sib_refused` (`copies.c`).
- **Signe** : les déplacements signés (`(ix+d)` négatif, `d8` 68000) donnent un
  `idx_min` négatif, qui est permis.
- **Banque** : la base est résolue dans la **vue** de banque active au site (S2).

### 5.4 Généralisation : rejeu par index

L'index 0 est la forme statique d'une idée plus forte, déjà prouvée sur MD
(S6) : **rejouer** la routine consommatrice sur chaque index.
1. On prend un checkpoint juste avant le calcul `base + index`.
2. On force l'index à 0, 1, …, `idx_max` (et jusqu'à la borne prouvée), comme une
   sonde.
3. On exécute.

Chaque index devient une branche `forced`, source `index-inference`, reportable
(§6). Les données atteintes sont alors **observées**, et non plus seulement
inférées. On couvre ainsi les musiques, textes et niveaux jamais joués, avec leurs
dépendances : sous-tables, décompression.

---

## 6. Frontière différée : zéro éviction budgétaire

### 6.1 Distinction

- **Éviction sémantique** : l'état est équivalent à un état déjà exploré ou il est
  absurde.
  - Ce sont `evicted_exact`, `evicted_ctx` et `nonsense_*`.
  - Elle est **conservée** : ce n'est pas une perte d'information sous le modèle
    de l'outil.
- **Coupe budgétaire** : la recherche live n'a plus de place, de pas ou de
  mémoire.
  - Elle devient un **report** : l'état est écrit, compressé, dans la frontière
    sur disque, avec tout ce qu'il faut pour continuer plus tard.
  - **Aucune branche n'est perdue faute de budget.**

### 6.2 Inventaire des points de perte actuels (à convertir)

**GB** (`src/explore/explorer.c`)

| Point | Réf. | Aujourd'hui | Devient |
|---|---|---|---|
| Set de dédup plein | 428-433 | jeté, `queue_overflows` | report palier `dedup-full` |
| File locale pleine sans spool | 469-488 | jeté, BUDGET | report (la frontière existe toujours, cf. 6.3) |
| Inbox prioritaire pleine | 469-488 | jeté, `closure_seeds_refused` | report palier `priority` |
| Spool au-delà de `--spool-cap-mb` | 1202-1207 | jeté, `spool_refused` | la frontière n'a plus de cap dur ; un plafond disque **suspend** le run (arrêt gracieux, reprise) au lieu de jeter |
| `--max-paths` atteint | 1438-1449 | état **libéré**, choix laissé ouvert | report palier `paths` |
| `--max-total` | 746-750 | chemin abandonné | snapshot au point de coupe, report `wall` |
| `--max-steps` par chemin | 751-754 | chemin coupé | snapshot, report `steps`, palier de budget ×4 |
| Cap d'arrière-branche | 894-910 | chemin coupé | snapshot, report `loop` |
| Glissade de padding, HALT | 822-832, `m0boy.c:168-185` | coupé | **reste coupé** (non-sens), compté |
| Cap de fourches par site | 1025-1028 | fourche non créée | variante créée et reportée, palier `fork-cap` |
| Élagage « deux côtés couverts » | 1029-1033 | non créée | reportée, palier `covered` (dernier palier) |
| `seed_cap` (scan de pointeurs) | 1738-1773 | arrêt silencieux | graines reportées |
| Semis indirect hors budget | 1786-1807 | `seeds_refused_budget` | report |
| Inbox sessions pleine | `sessions.c:383-392` | `inb_dropped` | report |
| Liste d'appelants (8) | 913-919 | silencieux | compté (et sans incidence sur la couverture) |

**Sega / MSX** (`src/platform/sega/explore.c`)

| Point | Réf. | Devient |
|---|---|---|
| Cap de chemins au push | 416-418 | report |
| File d'entrées pleine (non spoolée) | 434 | report |
| Graines en attente et file d'entrées en fin de phase | 1260-1275 | report |
| Boucles, fourches capées, stagnation, pas | 1165-1251 | report |
| Contexte plein `ctx_full` | 1151-1155 | report |

**NES, PCE, MD**
- Files restantes **libérées sans comptage** : NES `nes/explore.c:626`, PCE
  `pce/explore.c:984-999`. MD compte (`dropped_pending`, `md/explore.c:575-582`)
  mais libère aussi. Tout devient un report.
- **Correctifs de compteurs** :
  - NES `fork_sites` est incrémenté même si le push échoue ;
  - Sega `seeds_refused_cap` est compté avant le test de doublon (:932-943).

### 6.3 Format sur disque

```
<state-dir>/frontier/
  frontier.idx              # index versionné : lots, comptes par raison et palier
  b<bank>-s<section>-<reason>-<seq>.dpf   # un lot
```

- **Le répertoire existe toujours.**
  - Sans `--state-dir`, il est créé à côté des sorties : `<sortie>.frontier/`.
  - Le message « overflow states are dropped » (`deepeel.c:451-453`) disparaît.
- **Lot** : les états d'un même groupe (plateforme, banque, section d'entrée,
  site, raison) partagent presque toute leur RAM.
  - En-tête du lot : `'DPFB'`, version, plateforme, taille d'image, **image de
    base** compressée.
  - Chaque enregistrement est un **delta XOR** contre l'image de base, compressé
    zlib, ce qui donne un gain massif.
  - Chaque enregistrement porte le **contexte dur complet** :
    - image machine (backend) ;
    - RAM cartouche ;
    - origines de provenance (comme le spool z80 `'DPSZ'`, `sega/explore.c:376-401`) ;
    - id de choix dans l'arbre, kind, forced, sources héritées ;
    - raison, palier, pas déjà consommés ;
    - budget au moment du report ;
    - id de film TAS si applicable.
- **Lots mutualisés** : tant qu'un lot est ouvert (même groupe), les reports
  s'y ajoutent. Il est fermé au-delà d'une taille, par exemple 64 Mo.
- **Lecture** : un lot est relu séquentiellement. La position lue est persistée
  dans `frontier.idx`.
  - Cela corrige le bug actuel : à la reprise, `dp_explorer_spool_attach` pose
    `rd = wr = taille` (`explorer.c:1288-1294`), donc les enregistrements en
    attente du `spool.bin` ne sont jamais relus.
  - Autre correctif : `--resume` Sega sur un spool vide échoue au lieu de
    repartir (`sega/explore.c:1046-1055`).
- **Compatibilité** : les anciens `spool.bin` (`'DPSR'`) et `'DPSZ'` sont lus une
  fois à la reprise et convertis.

### 6.4 Ordonnancement

1. **File live d'abord.** Elle est bornée en mémoire comme aujourd'hui
   (`--queue`, `--queue-max`, `--mem-cap`).
2. **Puis la frontière, par paliers de coût croissant** :
   `priority` < `dedup-full` < `paths` < `fork-cap` < `steps` < `loop` < `wall` <
   `covered`.
3. **Dans un palier, priorité à la nouveauté** :
   - section d'entrée jamais vue ;
   - octets inconnus voisins de la banque ;
   - peu de sources.
4. **Reprise d'un report.** Le chemin reçoit le budget de son palier, augmenté :
   - `steps` → budget ×4 à chaque re-report ;
   - `loop` → cap ×4.
   Un état re-reporté monte de palier. Le palier final `∞` n'est plus repris
   automatiquement. Il reste sur disque, compté et rapporté, prêt pour un
   `frontier drain --tier ∞`.
5. **Commandes** :
   - `deepeel frontier stats <dir>` : comptes par raison, palier, banque ;
   - `deepeel frontier drain <rom> <dir> [--tier N] [--time-limit S]` : reprend
     sans rejouer le reste du pipeline ;
   - `--resume` sur `explore` reprend aussi la frontière.
6. **Partage `--jobs`** : un lot ouvert par worker. L'index est sous verrou.

### 6.5 Terminaison et bornes

- **En théorie, la frontière n'est pas finie** : compteurs de frames, timers,
  RNG.
- **En pratique**, la dédup exacte hors horloges et l'éviction par contexte
  (projection sur ce que la section lit vraiment) referment l'espace. Deux états
  qui ne diffèrent que par un compteur non lu par la section sont fusionnés.
- **Le seul plafond est le palier ∞**, compté. Un plafond disque optionnel
  (`--frontier-cap-gb`) **suspend** le run proprement au lieu de jeter.
- **Rien ne tourne sans fin.** Chaque reprise a un budget fini ; seul l'opérateur
  relance.

### 6.6 « Cette méthode peut-elle donner des résultats massifs ? »

**Oui, pour le code**, avec trois réserves à garder en tête :

1. **Ce qui progresse.** Avec la frontière, la couverture du code tend vers la
   **fermeture du code atteignable sous le modèle de l'outil**. Plus aucune
   branche n'est abandonnée, chaque report finit par être repris, et la
   fermeture statique et le semis en cascade alimentent la frontière. C'est
   strictement plus que l'état actuel, où chaque budget est un plafond définitif.
2. **Sur-approximation.** Les fourches forcées produisent des chemins **infaisables**
   en jeu réel, et la frontière en amplifie le nombre. Il est donc essentiel de
   garder séparées les familles naturel / forcé / statique (§4.3) et les verdicts
   (C1). La couverture *forcée* dit « ce code existe et est atteignable par le
   CPU », pas « le jeu l'exécute ».
3. **Données profondes.** Les graphismes, musiques et niveaux tardifs ne sont lus
   qu'avec un état de jeu profond : progression, objets, sauvegarde. Les fourches
   seules y arrivent mal. Ce sont les sessions, les TAS, Go-Explore (J4) et
   surtout le **rejeu par index** (§5.4) qui couvrent ces données.
4. **Queue exponentielle.** Les derniers pourcents coûtent le plus. Les paliers
   et la priorité à la nouveauté rendent ce coût *payable sur le long terme*
   (reprises successives), mais pas gratuit. Il faut mesurer la courbe
   « octets nouveaux par heure de reprise » (journal de découverte) pour savoir
   quand s'arrêter.

---

## 7. Mutualisation : architecture cible

### 7.1 Constat

Environ 5 500 lignes de moteur GB (`src/explore/`) contre environ 4 500 lignes de
moteur réécrites ailleurs :

| Moteur | Fichier | Lignes |
|---|---|---|
| Sega | `src/platform/sega/explore.c` | 1920 |
| PCE | `src/platform/pce/explore.c` | 1121 |
| MD | `src/platform/md/explore.c` | 757 |
| NES | `src/platform/nes/explore.c` | 732 |

Les morceaux réécrits par plateforme :
- files compressées ;
- caps de boucle (`loop_bump` PCE et NES) ;
- sets `seen` ;
- xorshift ;
- chargeurs TAS (`sega/tas.c`, `nes/tas.c`, `pce/tas.c`, `md/movie.c`) ;
- sections ;
- cartes.

L'option `--max-paths` elle-même est parsée deux fois (`deepeel.c:269`, `:975`).
La seule interface partagée, `DpSegaBackend` (`src/platform/sega/backend.h`), ne
sert qu'à SG/GG/MSX.

### 7.2 `DpMachine` — l'interface machine générique

Elle étend `DpSegaBackend`.

```c
typedef struct DpMachine {
    const char *name;                    // "gb", "sms", "nes", "libretro:<core>"…
    const DpPlatform *plat;              // registres, IRQ, régions, entrées
    const DpIsa *isa;
    // état
    size_t state_size;
    void (*save)(void *), (*load)(const void *);
    uint64_t (*state_hash)(void);        // hors horloges (cf. nesmach.c:173)
    void (*ctx_components)(DpCtxSpec *); // composants de contexte pour C1/C2
    // exécution
    int  (*step)(void);                  // une instruction ; ou NULL si frame seulement
    void (*advance)(unsigned frames);
    void (*cpu_regs)(DpRegs *);          // vue normalisée pc/sp/flags/regs
    int  (*force_branch)(int taken);     // D7/D11
    unsigned (*irq_pending)(void);
    int  (*irq_skip)(unsigned src);      // D8
    // observateurs (posés par le noyau)
    void (*observe)(const DpObservers *); // exec, read, write, io, dma, bank, fetch PPU
    // entrées
    void (*set_input)(uint32_t port, uint32_t mask);
    const DpProbeCatalog *(*probes)(void); // D10 + §3.7
    // mémoire
    int64_t (*rom_index)(uint32_t addr);   // offset physique ROM, <0 sinon
    int32_t (*ram_index)(uint32_t addr);
    int  (*bank_geometry)(DpBankGeometry *);
    int  (*seed_window)(uint32_t addr, unsigned bank);
    int  (*dest_type)(uint32_t space, uint32_t addr); // région typée §4.2
    // affichage
    const uint8_t *(*framebuffer)(int *w, int *h, int *fmt);
    // instances
    struct DpMachine *(*clone)(int i);   // F3
} DpMachine;
```

Tout membre est optionnel. Le noyau **dégrade proprement** et rapporte ce qui
n'est pas disponible. Par exemple, sans `step`, les fourches D7-D11 sont
indisponibles, ce qui est le cas de libretro niveau A.

### 7.3 `DpIsa` — le statique générique

Fonctions, toutes sur un octet de ROM dans une vue :
- `length` ;
- `flow` : séquentiel / saut / appel / retour / conditionnel / indirect / illégal,
  avec les successeurs ;
- `cond` ;
- `mem_operand` : base, index, déplacement, largeur — pour §5 ;
- `regs_effects` : pour la provenance, sous forme de micro-effets sur un treillis
  commun ;
- `ptr_width`, `endian` : pour S1/G2 ;
- `disasm`.

Implémentations existantes à envelopper :
- `src/cpu/sm83/` ;
- `src/cpu/z80/` ;
- `src/cpu/m6502/` ;
- `src/platform/pce/hucdec.c` ;
- `src/cpu/m68k/m68k_disasm.c`.

### 7.4 Modules communs (le noyau)

| Module | Remplace / absorbe |
|---|---|
| `frontier` (file live + dédup + frontière disque + arbre) | `explorer.c` 306-498/1188-1401, `tree.c`, spool z80, files NES/PCE/MD |
| `cover` (modèle §4) | flags GB/Sega/NES/PCE/MD |
| `sections` (liveness, verdicts, éviction contexte) | `sections.c`, `section_touch` z80, `sections_build` NES/PCE |
| `copies` (boucle CPU + canaux DMA + frères) | `copies.c`, `copied` z80, `log_copy` PCE, DMA MD/NES |
| `forks` (D7-D11, sondes, caps, reports) | `fork_*` GB, `fork_inputs` z80, `fork_pad/irq` PCE, `on_end` MD |
| `seeds` (G1-G4, cascade) | `dp_seed_gb`, semis z80 |
| `closure` + `provenance` (sur `DpIsa`, à vues) | `closure.c`, `sega/closure.c`, `provenance.c` ×2, `tables()` PCE, `infer` PCE |
| `replay` (S6 + §5.4) | `replay_*` MD |
| `play` (sessions, pool, Go-Explore, humain) | `sessions.c`, `play_session` z80/NES/PCE, `novelty_search` PCE |
| `movie` (`DpMovie` + chargeurs vbm/vmv/bk2/fm2/gmv/texte) | `tas.c` ×5 |
| `report` (texte, JSON, `.map`, PNG §9, `.cov`, HTML, heat) | `report/`, rapports par plateforme |
| `cli` (options communes, une seule table) | parsing dupliqué de `deepeel.c` |

Une plateforme ne fournit plus que :
1. son backend `DpMachine` ;
2. son `DpIsa` ;
3. son `DpPlatform` avec ses régions typées ;
4. ses catalogues de sondes ;
5. ses particularités, via des hooks du noyau : traps BIOS MSX, contrôle mémoire
   `$3E`, timing NES en milieu de frame.

### 7.5 Ordre de migration

1. **GB** est la référence. Le noyau est extrait de `src/explore/` sans changer
   le comportement : les 5 tests verts, `test_explorer` en particulier.
2. **Sega/MSX** : le plus proche, `DpSegaBackend` y est déjà.
3. **NES**.
4. **PCE**.
5. **MD**, avec exploration du Z80 son.

Chaque migration doit satisfaire trois conditions :
- les tests de la plateforme restent verts ;
- la couverture sur les fixtures synthétiques est **supérieure ou égale** ;
- les techniques ✗ de la matrice §2.1 passent ✅.

---

## 8. libretro : deux niveaux derrière `DpMachine`

### 8.1 Niveau A — cœur non modifié

- **Chargement** : `dlopen` du `.so`. Le backend `libretro:<core>` implémente
  `DpMachine` avec :
  - `retro_serialize_size` / `retro_serialize` / `retro_unserialize` pour l'état ;
  - `retro_run` pour `advance` (une frame) ;
  - `retro_set_input_state` / `retro_set_input_poll` pour les entrées ;
  - `retro_get_memory_data(SYSTEM_RAM / SAVE_RAM / VIDEO_RAM)` et
    `RETRO_ENVIRONMENT_GET_MEMORY_MAPS` pour la RAM et, quand le cœur la
    déclare, la ROM mappée ;
  - le callback vidéo pour `framebuffer`.
- **Disponible** :
  - sessions jouées, TAS, Go-Explore, mode humain, `--watch` ;
  - dédup par hachage RAM ;
  - pool de checkpoints ;
  - frontière (reports d'états sérialisés).
- **Couverture ROM grossière** :
  - par diff des memory maps quand le cœur expose la ROM avec un flag d'accès
    (rare) ;
  - sinon par **rejeu croisé** : les checkpoints libretro sont convertis en états
    m0boy quand un convertisseur existe (`.sta`, PNG), puis explorés à fourches
    par le backend instrumenté.
- **Pas de** `step`, `force_branch`, observateurs de bus. Les fourches D7-D11 et
  D2/D4 fins sont indisponibles, et c'est **rapporté**.
- **Source** : `core:<nom>`.

### 8.2 Niveau B — cœur avec extension de trace

Un environnement privé, par exemple `RETRO_ENVIRONMENT_DEEPEEL_TRACE`, numéroté
dans la plage privée, que le cœur implémente par un petit patch maintenu hors
dépôt ou en amont. Il expose :
- `step` (une instruction) et l'arrêt en frontière d'instruction ;
- les callbacks `exec(pc)`, `read(addr, space, val)`, `write(...)`, `dma(...)`,
  `bank(...)` ;
- la vue registres et le forçage de flags ;
- `rom_index` (mapping courant).

Avec lui, le backend libretro remplit **tout** `DpMachine` et toutes les
techniques s'appliquent. C'est la même démarche que les patches m0boy actuels
(`CMakeLists.txt:37-60` pour MD, macros `*_TRACE`).

### 8.3 Multi-instances et multi-cœurs

- **Instances** : la plupart des cœurs sont des singletons. Pour avoir N
  instances, on copie le `.so` N fois dans un répertoire temporaire et on fait
  N `dlopen` (avec `RTLD_LOCAL`). C'est le pendant runtime de l'`objcopy
  --redefine-syms` actuel.
- **Plusieurs cœurs pour une ROM** : chacun est un worker de sessions ou un
  explorateur, avec son bit `core:<nom>`.
  - **Recoupement** : un octet vu par deux cœurs est « recoupé » (§4.3).
  - **Divergences** : un octet couvert par un seul cœur est listé, ce qui détecte
    les bugs d'émulation (cf. `docs/m0boy-fidelite.fr.md`).
- **Configuration** : `--core <chemin.so>` (répétable), `--core-dir`. Le cœur est
  choisi par extension de la ROM d'après `retro_get_system_info`.
- **Aucun cœur dans le dépôt** (même règle que les ROMs).

---

## 9. PNG standard à blocs adaptatifs

### 9.1 Standard

- Le PNG est **la** sortie de carte, sur toutes les plateformes. Aujourd'hui :
  - GB : natif ;
  - z80/PCE : via `maprender` (`deepeel.c:743-860`) ;
  - MD : dump brut des flags (`dp_md_explore_map`, `md/explore.c:727`) ;
  - NES : ◐.
- Le `.map` texte reste disponible et **embarqué** (chunk `deepeel_map`).
- Un seul moteur de rendu (`src/report/map.c`), paramétré par :
  - l'unité de tuile de la plateforme (16 Ko GB/Sega, 8 Ko PCE/MSX, PRG/CHR NES,
    16 Ko MD) ;
  - le titre, les classes et types.

### 9.2 Blocs adaptatifs

**Aujourd'hui** :
- 1 pixel = 1 octet ;
- 128×128 par banque de 16 Ko ;
- 8 tuiles par rangée (largeur max 1078 px) ;
- hauteur non bornée : environ 8 600 px pour 8 Mo, le double avec la chaleur ;
- tout le buffer RGB est en mémoire.

**Règle.**
- **Budget de pixels** `P` (défaut 4 Mpx pour la carte seule ; `--png-budget`).
- **Taille de bloc** `B` = plus petite puissance de 2 telle que
  `taille_ROM / B ≤ P`.
  - 1 o/px jusqu'à 4 Mo.
  - 2 o/px jusqu'à 8 Mo.
  - Et ainsi de suite.
- **Tuile de banque** : la géométrie s'adapte, `sqrt(unité/B)` de côté ; par
  exemple 16 Ko à 4 o/px donne 64×64.
- **Couleur d'un bloc** : classe **dominante** pondérée par la priorité de
  projection (§4.4).
  - Un bloc **mixte** (deux classes ou plus) est tramé : un pixel sur deux à la
    seconde classe, ou un liseré, réglable.
  - Un bloc qui contient du code naturel est toujours vu comme code, même minoritaire.
- **Légende** : affiche « 1 px = B o » à côté du carré d'échelle (le carré 64 o
  actuel devient « carré de N o »).
- **Chaleur** : même grille et même `B`, avec la somme des hits du bloc.
- **Mémoire** : le rendu se fait **par rangée de tuiles** en flux vers zlib
  (deflate incrémental), sans buffer complet.

### 9.3 Pleine résolution embarquée

**Chunk privé ancillaire `dpCv`** (le nom respecte la casse PNG : ancillaire,
privé, copiable). Contenu binaire zlib, **pas d'hex**, versionné :
```
'DPCV' u32 version, u32 plateforme, u64 taille ROM, u32 unité de banque, u32 B
puis sections: nature[u16·N], types[u32·N], sources[u32·N],
               exec_hits[u32·N], data_hits[u32·N], play_*[u32·N] (optionnelles),
               section_of[u32·N] (optionnelle), table creuse 1re découverte
```
- Un seul flux deflate. Les sections creuses (tout à zéro) sont omises par un
  masque dans l'en-tête.
- Une table de 30 Mo brute pour 1 Mo de ROM se compresse en général à quelques
  centaines de Ko : les longues séries sont homogènes.

**Autres chunks privés** :
- `dpFr` : résumé de la frontière (comptes par raison et palier, lots) ;
- `dpIx` : enregistrements `DpIndexed` (§5) ;
- `dpTb` : tables de sauts, de pointeurs et de flux décodés.

**Compatibilité** :
- les chunks zTXt actuels (`deepeel_meta`, `deepeel_map`, `deepeel_heat`,
  `deepeel_cov`, `deepeel_rom`, `map.c:751-757`) sont conservés ;
- `zTXt` reste pour le texte, les nouveaux chunks binaires évitent le surcoût ×2
  de l'hex (`DpPngText.hex`) ;
- les anciens fichiers restent lisibles.

**Lecteurs** :
- `deepeel mapx --chunk dpCv` exporte le binaire ;
- `maprender` peut re-rendre à n'importe quel `B` depuis `dpCv` ;
- `gbrecomp --coverage` (m0boy) lit `dpCv` ou retombe sur `deepeel_cov`.

**`--evidence`** (z80) est remplacé par `mapx --chunk dpCv`.

---

## 10. Propositions supplémentaires

1. **Sondes de registres événementiels** (§3.7). C'est le plus gros gain attendu
   pour un coût faible.
2. **Fourche « sauvegarde valide »**.
   - Constat : la RAM est à zéro, donc le code « charger la partie » n'est jamais
     pris.
   - Proposition : à la première lecture de SRAM non initialisée qui compare
     contre une signature (`cp`/`cmp` immédiat après la lecture), on fourche avec
     la signature attendue écrite. La source est `probe-save`.
3. **Octets RNG appris.** Comme l'octet de vies PCE (`lives_frame`), on repère
   les octets WRAM mis à jour une fois par frame par un LFSR, puis on fourche sur
   2 à 4 valeurs aux sites qui les testent.
4. **Détection générique des décompresseurs.** Une routine qui lit séquentiellement
   la ROM et écrit en RAM/VRAM avec un ratio différent de 1 est un décodeur. On
   applique S6 et §5.4 sur toutes les tables qui l'alimentent, et ses entrées
   prennent le type `compressed`.
5. **Go-Explore partout** (J4), comme stratégie de pool.
6. **Fixtures synthétiques par technique et par plateforme**, générées par
   `tests/build_fixtures.py` : une ROM minimale par ligne de la matrice §2.1. Le
   test vérifie que la technique produit son flag et sa source. La matrice §2.1
   devient ainsi **exécutable**.
7. **Bornes « jamais silencieuses » vérifiées par test.** Chaque compteur de
   saturation a un test qui le déclenche.
8. **`tools/scan_all.py` multi-plateforme** sur le modèle commun, avec une colonne
   « sources » et une colonne « types » dans l'agrégat.
9. **`--watch` générique** sur `framebuffer` (§7.2), avec un onglet « carte en
   direct » qui affiche le PNG adaptatif.
10. **Courbe de rendement de la frontière** : octets nouveaux par minute et par
    palier, dans le journal de découverte, pour décider quand arrêter (§6.6).
11. **Corrections de doc** :
    - `docs/GB.fr.md` §Limites (PPU/APU/timer, VBK) ;
    - `docs/MD.fr.md` (dit le rejeu GMV absent et se contredit sur les sondes
      pad, alors que les deux existent) ;
    - commentaire « only platform implemented » (`src/platform/platform.h:87`).
12. **Correctifs relevés** (regroupés en J0) :
    - reprise du spool GB (§6.3) ;
    - `fork_counts` plein (D7) ;
    - `max_paths` qui libère l'état (§6.2) ;
    - surlecture VRAM (C2) ;
    - banques de la provenance GB (S3) ;
    - `--no-closure` trop large (G4) ;
    - `tree.bin` Sega (F2) ;
    - `--resume` Sega sur spool vide (§6.3) ;
    - compteurs NES/Sega (§6.2) ;
    - options NES mortes (G3) ;
    - graines IRQ GB non forcées (G1) ;
    - implications CGB mensongères (§3.1).

    **État : fait en 0.29.0** (J0), tous ces points sont corrigés, y compris
    les corrections de docs du point 11. Le détail figure dans
    `docs/GB.fr.md` §0.29.0.

---

## 11. Feuille de route

Chaque jalon remplit trois conditions :
- il garde **les 5 tests GB verts** (`ctest --test-dir build --output-on-failure`)
  et tous les tests de plateforme ;
- il ajoute ses fixtures synthétiques ;
- il incrémente la version (changement visible dans les rapports) et met cette
  norme à jour (matrice §2.1).

| Jalon | Contenu | Taille | Critère d'acceptation |
|---|---|---|---|
| **J0** ✅ 0.29.0 | Correctifs §10.12 | S | test de reprise de spool qui relit tous les enregistrements ; test de saturation `fork_counts` |
| **J1** ✅ 0.34.0 | Modèle de couverture commun §4 (nature, sources, table creuse de première découverte, projection `types` start/entry/irq/verbatim ; les types sémantiques tiles/music/… relèvent de J8) | M | les classes de carte sont identiques avant/après (projection) ; sources multiples visibles dans le JSON |
| **J2** ✅ 0.42.0 | Frontière différée §6 — **GB** : toutes les raisons (`priority`, `dedup-full`, `paths`, `fork-cap`, `steps`, `loop`, `wall`, `covered`), lots `.dpf` (deltas XOR, position de lecture), graines retenues, `frontier stats`, `frontier drain --tier N\|inf` sans réamorçage, `--frontier-cap-gb`, spool plein reporté, `inbox_dropped`. **Sega/MSX** : `steps`, `loop`, plafond de chemins et file pleine reportés ; à la fin d'un run les reports restants vont dans le spool (`defer_spooled`, reprise `--resume`, palier persisté dans le mot de drapeaux du spool). **NES/PCE/MD** : une file saturée parque l'état (zone bornée à 4096, reprise quand la file se vide, `parked`/`parked_resumed`/`parked_lost`) au lieu de le jeter. **Ajouts 0.40.0** : sur le pool parallèle, un spool plein fait reporter l'état par le worker (zone propre, fusionnée dans celle du chef à la jointure ; au-delà du plafond, perte finale comptée) au lieu de le perdre ; `--spool-to-frontier` convertit un spool `DPSR` en lots `.dpf` (raison `paths`) ; test Sega « report encore en attente au mur » (spoolé avec son palier puis repris par `--resume`) ; test CLI `frontier_cli`. **Lots disque 0.41.0 (NES/PCE/MD/Sega)** : option `--lot FICHIER` (format portable `lots.h`, extension `.dpf` : même magie `DPFB` que les lots GB, un lot = un fichier ; §6.3 : en-tête `DPFB` v2 + étiquette de plateforme + image de base zlib, puis enregistrements `DPLL` = métadonnées + delta XOR zlib contre l'image de base, curseur de lecture FIFO, compactage à la fermeture) ; NES/PCE/MD y versent le trop-plein de la zone parquée et les restes d'un mur (plus aucun reste silencieux : `pending_dropped`/`dropped_pending` ne comptent que les écritures échouées), et les relisent quand la file se vide ou au run suivant ; Sega importe le lot dans son spool `DPSZ` au départ et y réexporte le spool non lu à la fin (le fichier disparaît une fois vide) ; compteurs `lot_written`/`lot_read`/`lot_pending`. Tests : mur → lot → reprise pour NES, PCE, MD, Sega, drain MD, drain NES et PCE (tests plateforme), drain Sega via `frontier_cli` ; pool parallèle GB avec spool refusant tout (`spool_cap_bytes`) : les états refusés sont reportés par les workers et fusionnés, perte `paths` nulle. **Choix assumés** : `frontier stats FICHIER.dpf` lit aussi un lot portable (enregistrements, octets bruts/compressés) ; `frontier drain ROM LOT [--sys X]` vide aussi un lot portable (NES/PCE/MD/Sega : seuls les états sauvés sont rejoués, ni racine ni graines ; `--lot-drain` en CLI de plateforme) ; PCE verse l'état décompressé (taille variable) et le recompresse au rechargement ; les spools restent lisibles tels quels. Tests : budgets × 9 fixtures GB, `fork_deferral` Sega (+ spool et `--resume` ; cas « en attente au mur » couvert avec max_total=5), saturation file NES/PCE/MD avec reprise ; compteurs §6.2 : NES `fork_sites` ne paie que les forks gardés (en file ou parqués), Sega `seeds_refused_cap` compté après le test de doublon | L | test : un run au budget minuscule puis `frontier drain` donne la même couverture qu'un run sans budget sur la fixture ; aucun compteur `*_dropped` non nul hors non-sens |
| **J3** ✅ 0.45.0 | `DpMachine`/`DpIsa`, noyau extrait de `src/explore/`, GB migré | XL | comportement identique (tests + campagne GB à budgets égaux) — **fait** : interfaces `src/core/dpmachine.h` — `DpIsa` (longueur, flot, condition, effets de registres sur le treillis HL/A, illégal, désassemblage, `mem_operand` : accès lecture/écriture/RW, mode absolu/haut/paire avec post-incr./pile) ; `DpMachine` (état, ROM, exécution, `observe`, `ctx_components`, `probes`). `DpIsa` SM83 et `DpMachine` GB ; le moteur, la closure et la provenance n'ont plus de connaissance directe du SM83. `explorer.c` découpé : `forks.c` (fourches conditionnelles/IRQ/touches/P1), `seeds.c` (graines et cascade indirecte), `ctxproj.c` (projection de contexte), `frontier_disk.c` (spool, ensemble exact, lots disque), `frontier.c` (politique), avec `explorer_priv.h` pour les internes partagés (2092 → 1458 lignes). Vérifié : 41/41 tests, carte `.map` octet pour octet identique à 0.43.1 sur Zelda (3 M pas, closure 6 rounds), 11,6 s. Les consommateurs de `mem_operand` (typage §5) et des sondes/observateurs génériques sont J8/J9 ; la généralisation des modules à d'autres plateformes est J4-J7 (§7.5) |
| **J4** ◐ 0.52.0 | Sega/MSX sur le noyau | L | matrice §2.1 ✅ pour SMS/GG/MSX ; `--jobs` MSX — **fait** : `--jobs 2` MSX (0.46), coupe de padding (0.47), `.cov`/romdata (0.48), identité ROM des films SHA-1 (0.49), frères de copie S5 (0.50), tables de sauts par provenance S4 (0.51), chien de garde HALT attribué D14 (0.52). **Reste (donc non ✅)** : J1 pool de checkpoints commun aux sessions jouées, Go-Explore (J4 de §2.7), mode humain/`.sta` et `--watch` (O3, J5 de §2.7) — ces trois dépendent d'une interface framebuffer commune (libretro niveau A, J11) —, S6 décodeurs (J8) |
| **J5** ◐ 0.55.0 | NES sur le noyau, mappers MMC1/MMC3/CNROM/AxROM, CHR indexée | L | fixtures par mapper ; tiles typés sur la CHR — **fait** : CNROM et AxROM dans le pont, fixtures en code (`test_nes`), CHR ROM indexée avec tiles de fond/sprites typés (0.54), MMC1 et MMC3 (0.55). **Reste (donc non ✅)** : IRQ de scanline MMC3 et miroir MMC1/MMC3, NES sur le noyau commun |
| **J6** ◐ 0.56.0 | PCE sur le noyau, 2 instances, VCE | M | `--jobs 2` PCE — **fait** : 2 instances par préfixage, partage par phase, fusion (test `pce` : l'union couvre le sériel) ; VCE typée palette (canaux 3/4). **Reste (donc non ✅)** : partage de file de fourches entre les deux instances (aujourd'hui par phase), classe palette dans la carte PNG, PCE sur le noyau commun |
| **J7** ◐ 0.57.0 | MD sur le noyau + exploration du Z80 son | XL | code du driver Z80 marqué `code-ramcopy`, lectures ROM par la fenêtre Z80 typées — **fait** : observation concrète du Z80 (copie 68000→RAM Z80, exécution RAM/ROM, lectures de banque, typage sample/music, test `fix_md_z80`). **Reste (donc non ✅)** : fourches sur les conditionnels Z80, `zsrc` restauré par fourche, classe Z80 dans la carte PNG, MD sur le noyau commun |
| **J8** ◐ 0.58.0 | Typage par provenance §4.2 + index 0 §5 + rejeu par index §5.4 + décodeurs génériques | L | fixtures « table indexée biaisée », « table de musique jamais jouée » — **fait** (PCE) : inférence par index sur `abs,X|Y` (base = opérande), trous déclarés, garde-fous biais/code/page/forcé, fixture « table indexée biaisée » (`test_index`). **Reste (donc non ✅)** : rejeu par index (§5.4, donc « musique jamais jouée »), provenance base+index sur le treillis (GB/Z80/68000), bornes hautes prouvées, typage §4.2, décodeurs génériques, classe dans la carte PNG |
| **J9** ◐ 0.63.0 | Matériel manquant §3 (sondes événementielles, CGB/HDMA, RTC, série, DMC, YM…) | L | une fixture par surface — **fait** : DMC NES (copie + type samples, `test_dmc`), HDMA CGB (copie via HDMA1-5, `fix_hdma`), YM2612/DAC MD (J7). **Reste (donc non ✅)** : VBK/SVBK (bascule réelle), SGB, YM2413, V9938, ADPCM, SGX, multitap |
| **J10** ◐ 0.64.0 | PNG adaptatif + chunks `dpCv`/`dpFr`/`dpIx`/`dpTb`, rendu en flux | M | ROM synthétique de 32 Mo rendue sous le budget de pixels ; aller-retour `dpCv` → `maprender` identique — **fait** : échelle adaptative et `dpCv` (test `map_png`, 32 Mio). **Reste (donc non ✅)** : `dpFr`/`dpIx`/`dpTb`, rendu en flux (tampon RGB entier), `maprender` relisant `dpCv` |
| **J11** | libretro niveau A | M | session + TAS sur un cœur chargé hors dépôt ; dégradations rapportées |
| **J12** | libretro niveau B (extension de trace) | L | parité de fourches avec le backend m0boy sur une fixture |
| **J13** | Campagne multi-plateforme et audit de la matrice | M | tableaux par plateforme, présentés comme **observations de l'outil** |

Ordre recommandé : J0 → J1 → J2 (gains immédiats sur GB/Sega), puis J3 et la
migration J4-J7, puis J8-J13. J8 et J9 peuvent avancer en parallèle de la
migration, plateforme par plateforme, une fois J3 posé.

---

## 12. Annexes

### A. Correspondance des flags actuels → modèle §4

| Actuel | Nature | Source |
|---|---|---|
| GB `DP_XF_NATURAL` | `CODE_*` | `fork-natural` ou `session-*`/`tas` (selon le worker) |
| GB `DP_XF_FORCED` | `CODE_*` | `fork-forced` |
| GB `DP_XF_ENTRY`/`IRQ`/`START` | `ENTRY`/`IRQ`/`CODE_START` | — |
| GB `DP_XF_STATIC` | `CODE_*` | `static-closure` ou `provenance` |
| GB `DP_DF_READ` | `DATA_READ` | source du chemin |
| GB `DP_DF_COPIED`/`VERBATIM` | `COPIED`/`VERBATIM` | source du chemin |
| GB `DP_DF_STATIC` | `DATA_READ` | `provenance` |
| GB `DP_DF_SIBLING` | `DATA_READ` | `sibling` |
| GB `DP_SF_POINTER`/`PAD` | `TABLE`/`PAD` | `static-scan` |
| Sega `NATURAL/FORCED/DATA/COPIED/STATIC/STATIC_DATA/TABLE/PAD` | idem GB | idem GB |
| MD `QUEUED` | — | (état de file, pas une couverture) |
| MD `DATA_NATURAL` | `DATA_READ` | famille naturelle |
| MD `DMA_SOURCE`/`DMA_READ` | `COPIED` | + type selon la destination DMA |
| MD `CPU_VRAM` | `COPIED` | + type selon l'adresse VRAM |
| PCE `TABLE` | `TABLE` | `jump-table` |
| NES `DP_NMAP_*` / PCE `DP_PMAP_*` | classes de carte | projection §4.4 |

### B. Compteurs : nommage

Forme : `<module>_<événement>`. Exemples :
- `frontier_deferred_<raison>`, `frontier_resumed`, `frontier_tier_inf` ;
- `probe_event_forks`, `index_sites`, `index_refused_code` ;
- `type_conflicts`, `core_divergent_bytes`.

Chaque compteur apparaît dans le texte, le JSON et `deepeel_meta`. Un compteur
non nul de la forme `*_dropped` / `*_refused` hors éviction sémantique est un
**défaut** au sens de cette norme.
