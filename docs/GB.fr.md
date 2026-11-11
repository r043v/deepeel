# Plateforme Game Boy

Les plateformes de couverture utilisent par défaut un passage attract de
6 000 images, puis les phases jouées et à fourches. Quand un TAS fournit des
instantanés, les essais aléatoires en lancent dix par instantané pris toutes
les 600 images; chaque essai dure au moins 2 000 images et attend ensuite
2 000 images sans nouvelle découverte. Les essais restaurent aussi la RAM
cartouche et les origines de données. Ces réglages sont partagés par les
sessions GB et Z80 et peuvent être remplacés par plateforme. Pour les
plateformes Z80, `--map carte.png` utilise le même rendu que la GB : chaleur
des sessions, apports par méthode et preuves embarquées. Le rejeu d'un PNG par
`maprender` conserve ses preuves et sa chaleur ; une carte texte seule ne peut
pas les reconstituer. Les chunks `dpFi` conservent les premières découvertes
et `dpMt` leur résumé affiché. La sortie `nes explore --map carte.png` utilise aussi ce
rendu pour la PRG-ROM, avec chaleur jouée, origines et premières découvertes ;
`maprender --sys nes` conserve ces preuves lors du nouveau rendu. Les horloges
des workers NES étant indépendantes, une découverte présente dans plusieurs
workers conserve l'origine du worker principal. La PC Engine utilise le même
format pour `pce explore --map carte.png`, y compris après nouveau rendu d'une
ROM avec en-tête de copieur. Les autres plateformes restent à raccorder à ce
format partagé. La Mega Drive produit aussi ces preuves avec `md explore --map
carte.png` ; ses dates de première découverte sont celles du début de la
tranche d'exécution et ne donnent donc pas l'ordre des octets dans une tranche.

Évolution multi-systèmes : le registre de ports, le désassemblage et les
premières explorations Z80/Sega sont décrits dans [Z80.fr.md](Z80.fr.md).
Ils utilisent des machines et rapports séparés ; la limite ROM `$8000` de
la GB ne s'applique pas aux fenêtres ROM Sega `$8000-$BFFF`.
Les rapports Z80 suivent aussi les origines à travers registres, pile et
RAM, distinguent WRAM/SRAM/VRAM/CRAM et les validations du latch de palette GG.
Les sondes de pads, Start, Pause/NMI et Reset SMS ont une seconde phase et
un budget explicite `--input-total` ; les plafonds du passage initial sont conservés.
La fermeture statique Z80 ajoute une classe `code-static`, distincte des
exécutions, avec vues de banques observées et saturations rapportées.
La classe Z80 `data-static` conserve les consultations ROM de l’analyse sans
les compter comme lectures machine ; `--evidence` exporte les preuves
superposées par octet en complément de la carte à classe principale.
Les constantes HL/IX/IY permettent aussi de résoudre des sauts indirects
statiques, sans contourner les barrières de mapping ni compter une exécution.
Leurs adresses d’écriture connues affinent aussi les barrières de banques et
les invalidations de pile, avec déplacements signés et miroirs propres au profil.
Les lectures d’octets ROM à adresse connue et les INC/DEC 16 bits permettent
aussi de reconstruire des cibles depuis une table, sans supposer de valeur RAM.
Un affinement BC/DE et des blocs utilise le budget restant des cibles, avec
contextes séparés et saturations rapportées, pour conserver les preuves antérieures.
Le pont Sega distingue les gates cartouche/RAM/entrées du contrôle mémoire ;
les changements de visibilité sont consignés dans la timeline bornée de banques.
Le choix Z80 `--mapper auto|sega|codemasters|korean150` est désormais explicite
pour SMS/GG ; le rapport indique le mapper retenu et son mode de sélection.
Un journal Z80 `memory control writes` conserve aussi les valeurs avant/après,
les bits actifs du profil et le caractère naturel/forcé, avec surplus compté,
même si l’écriture ne change aucune visibilité ROM/RAM.
Les commandes VDP indirectes et prises effectives INT/NMI ont aussi leurs
compteurs et journaux bornés, distincts des simples accès ports et fourches.
Le registre et le premier pont MSX1 isolé sont décrits dans [MSX.fr.md](MSX.fr.md) ;
leur exploration dynamique produit désormais rapports et cartes, avec slots,
BIOS, AY, entrées et IRQ. La fermeture statique MSX utilise les vues de
slots et de banques observées, avec preuves de pile limitées à la RAM visible.
Les accès internes du BIOS MSX simulé sont instrumentés au build et comparés
à la version non instrumentée.

## Registre matériel

`src/platform/gb/gb_plat.c` porte la table complète (`deepeel regs gb` pour
la lister) : 69 entrées — les fenêtres de mapper (RAMG, ROMB0, ROMB1, RAMB,
MODE), les registres système/audio/vidéo (P1, SB/SC, DIV/TIMA/TMA/TAC, IF,
NR10…NR52 + WAVE, LCDC…WX, DMA), les registres CGB (KEY1, VBK, BOOTROM,
HDMA1-5, RP, BCPS/BCPD, OCPS/OCPD, OPRI, SVBK, FF72-77) et IE. Chaque entrée
porte sa catégorie, son sens lecture/écriture et ses **implications de
mapping** (`bank-lo`, `bank-hi`, `ram-bank`, `ramg`, `mode`, `dma`, `hdma`,
`irq`, `event`, `window`, `modelled`).

Les IRQ (VBlank $40, LCDSTAT $48, Timer $50, Serial $58, Joypad $60) ont
leur vecteur et leur bit IE/IF ; les points d'entrée graine l'reset $100,
les 8 RST et les 5 IRQ (un vecteur en padding n'est pas semé). Le balayage
de pointeurs sème toute valeur ROM plausible ; une cible `$4000+` **encadrée
de deux autres mots de la fenêtre `$4000`** est traitée comme entrée de
table de dispatch : **toutes les banques commutables en sont semées** (la
banque visée par un pointeur banqué est indémontrable statiquement), un mot
isolé reste semé en banque 1. Plafond `--seed-cap`, dédup par index ROM.

## Machine embarquée (0.3.0)

La machine n'est plus approximée : deepeel embarque la **runtime GB de
m0boy** (`targets/meta/runtime/gbl_*`, dépendance `M0BOY_DIR`) —
l'interpréteur dont le run_hash/image_hash/audio_hash est tenu par la suite
m0boy. Le registre matériel ci-dessus reste la grille de lecture
(`deepeel regs gb`) ; l'exécution, le PPU (rendu par ligne, 160×144
récupéré pour le GUI et les snapshots), l'APU, le timer et le joypad
matériels sont ceux de m0boy.

- **Variantes** : `GBL_MBC`/`GBL_ERAM_SIZE` sont compile-time — le build
  compile 17 variantes MBC×RAM (0:0 ; 1:0/0x800/0x2000 ; 2:0x200 ;
  3 et 5 : 0..0x20000) et `dp_mach_pick_variant` choisit d'après l'en-tête.
- **Threads = instances** : la runtime vit dans des statics ; chaque worker
  reçoit une copie des symboles préfixée par `objcopy --redefine-syms`
  (10 instances, `DP_MACHINES`). Les hooks (`trace`, lectures/écritures
  CPU interposées par `-Dgbl_rd/-Dgbl_wr`, vblank, lignes PPU) sont
  re-câblés par instance vers le moteur.
- **États** : une image d'état (`gbl_state_io`, ~17 Kio + RAM cartouche) se
  sauve/charge à frontière d'instruction ; fourche = sauver, retourner le
  drapeau Z/C (ou vider IF) dans la copie, restituer. Les banques de boot :
  MBC1-3 démarrent en banque 1, **MBC5 démarre en banque 0** (légal).
- **Garde-fous** : HALT que rien ne réveille (8 frames sans instruction) et
  glissade dans du padding (4096 pas) coupent la machine par `longjmp`.

## Modélisation (ce que deepeel ajoute par-dessus)

- **Interposition** : chaque lecture CPU (IO y compris) et chaque écriture
  (mapper, VRAM, OAM, IO) passe par un hook deepeel avant/autour de
  l'effet machine — c'est de là viennent la carte data, les stats de
  registres, les événements de banque et les canaux de copie.
- **OAM DMA** : l'écriture à $FF46 (canal 1) produit l'enregistrement
  `rom -> oam` 160 octets comme le ferait la console.
- **Interrupteurs** : à chaque conditionnel l'autre issue est engendrée en
  retournant le drapeau testé (Z via `gbr.zr`, C via `gbr.cf`) ; aux points
  IME && IE & IF, un fork « l'IRQ ne part pas » complète la dispatch
  naturelle que la machine fait elle-même.
- **Touches** : deux sondes couvrent la manette — les presses quand l'IRQ
  joypad est armée (le press lève IF bit 4, comportement console) et les
  lectures directes de `$FF00` en polling (`ldh a,($00)` / `ld a,(C)` avec
  C=$FF00) : chaque touche pressée devient une variante forcée dont seule
  la valeur lue diffère (l'IRQ du press est effacée de la variante).
  `--no-key-fork` / `--no-p1-fork` pour désactiver ; compteur `forks_p1`.
- **Budgets en cascade** (portage z80) : les phases de re-semis (cibles JP HL
  puis chaque tour de fermeture) partagent un total de graines (2×
  `--seed-cap`) : chaque phase dépense le solde laissé par les précédentes au
  lieu d'un budget plat par phase ; un semis refusé faute de solde est compté
  (`seeds_refused_budget`) et reporté au tour suivant.
- **Provenance statique** : passe sans machine qui suit HL (et A) octet par
  octet sur les runs droits — chargements immédiats, mots ROM à adresse
  absolue, `INC/DEC HL` exacts, `ld h/l,a`, et les tables parcourues pointeur
  en main (`ld a,(hl); inc hl; ld h,(hl); ld l,a; jp hl`). Un `jp hl` à HL
  entièrement connu prouve sa cible (`code statique`, la fermeture l'étend) ;
  une lecture ROM à adresse entièrement connue prouve la donnée
  (`donnée statique`, nouvelle classe de carte). Preuve partielle (octet haut
  inconnu, fenêtre $4000 sans banque prouvée) = rien n'est prouvé, compté
  (`prov_partial`) ; caps de pas/d'états comptés (`prov_sat`).
- **Évictions** : dédup exact d'état (hash d'image hors cycle), éviction
  « même contexte » par section, budgets (pas/chemin, pas global, sites de
  fork, arrière-branches), tout compté dans le rapport.

## Limites (documentées, pas cachées)

- **Registres événementiels** : la machine est celle de m0boy (PPU, APU et
  timer réels) ; les fourches de valeur (0.249.48) couvrent LY, STAT, DIV,
  TIMA, SC et SB, mais une boucle d'attente non fourchée sort au rythme de la
  machine concrète, pas sur chaque valeur possible. Les handlers d'IRQ sont atteints
  par les graines de vecteurs (semées comme forcées depuis J0) et les forks
  d'IRQ.
- **RAM initiale à zéro**, registres au reset DMG ($01B0…) — l'exploration
  est déterministe ; deux exécutions donnent la même carte.
- **CGB non modélisé** : le runtime m0boy GB est DMG — une seule banque VRAM,
  pas de HDMA, une seule banque WRAM. VBK, SVBK et HDMA1-5 figurent au
  registre pour mémoire, sans le drapeau `modelled` (corrigé en J0 : ils
  étaient annoncés modélisés).
- RTC MBC3 : sélecteur journalisé, horloge non simulée.
- L'exploration est un superset du jeu réel : les chemins forcés explorent
  des cas qu'aucune partie ne produit (c'est le but — la nécessité par
  section les sépare dans les verdicts).

## Observations sur ROMs réelles (machine de l'outil, pas du matériel)

- Super Mario Land (MBC1, 4 banques) : 1 209 sections, banque 0 : 8 795
  octets de code (681 au flux naturel), banques 1-3 explorées via les
  changements de banque ; 6,3 s pour 30 M pas.
- Pokémon Yellow (MBC5, 64 banques) : 1 257 sections, 8 banques touchées,
  5,5 s pour 20 M pas.

Les compteurs de saturation (file, budgets, cuts) sont dans le rapport —
une exploration réelle à budget par défaut en affiche : ce sont les pertes
conscientes de l'exploration, pas des erreurs.

## Sessions jouées (`explore --play`)

Le moteur de fourches explore tout ; les sessions jouent le jeu. N workers
POSIX (défaut 10, `--sessions`, borné au nombre d'instances), chacun sa
machine instance + son contexte d'enregistrement partageant les cartes :


Le moteur de fourches explore tout ; les sessions jouent le jeu. N workers
POSIX (défaut 10, `--sessions`), chacun sa machine (la SRAM cartouche vit
dans la machine, snapshotée avec elle) :

- **inputs uniformes** (défaut) : une touche tirée uniformément parmi les 8,
  tenue 1..`--uniform-hold` frames (64) puis re-tirage — aucun biais « appuis
  attendus » ; `--inputs m0x` rend le générateur pondéré historique ;
  `--input f` rejoue un script m0boy (held-named) ; `--fixed mask` (tests).
- **TAS** : `--tas f` charge un movie tool-assisté — `.bk2` BizHawk (zip
  pouvant être imbriqué, `Input Log.txt` avec LogKey), `.vmv` VBA (binaire
  `VBM\x1a` ou en-tête texte), ou un log extrait — un worker par fichier,
  rejoué frame par frame depuis la mise sous tension (les films ancrés sur
  savestate sont signalés : le replay est un effort, pas du frame-exact) ;
  à la fin du film le worker passe en jeu aléatoire. **Le TAS est
  prioritaire** : pendant le replay aucun budget ne coupe le worker — ni
  `--frames`, ni la stagnation, ni la rotation des segments (l'index du film
  et l'état machine restent en pas l'un avec l'autre) — le film couvre un
  gameplay complet et est joué jusqu'au bout ; l'état de fin de film rejoint
  le pool au segment suivant. Validé bit à bit sur les TAS Super Mario Land
  de tasvideos (43 755 et 43 986 frames).
- **segments** : 480 + rand(1920) frames comme m0x ; 1 segment sur 8
  redémarre power-on, sinon le checkpoint **le moins choisi** du pool (48,
  jamais le slot 0) ; fin de segment sur stagnation, impasse HALT, crash
  (opcode indéfini) ou glissade dans du padding (voir plus bas).
- **scheduler d'époques** (`--epoch`, défaut 480 frames) : crédit = nouveaux
  octets exec + data + copies + registres + banques (premier basculement
  exact via atomiques) ; le thread principal draine la boîte à graines des
  sessions vers le fourcheur et avance le fourcheur pendant que les workers
  attendent (bounded par `--fork-paths`, défaut 200, 0 = coupé) ; sessions
  stagnantes re-semées depuis le meilleur crédité.
- **persistance** : `--state-dir D` écrit le pool (`pool.bin` v3, snapshots
  TAS compris) et les snapshots toutes les `--save-epochs` époques (et à
  l'arrêt, SIGINT y compris) ; `--resume` recharge et continue. Un run
  coupé/repris est identique au run continu au sein de la session rejouée
  (testé).
- **arrêt** : `--frames` (budget), `--stagnation F` frames sans découverte,
  `--until-static` (carte statique entièrement expliquée), `--mem-cap MB`,
  `--tas-solo` (fin des films), SIGINT gracieux ;
  l'arrêt des sessions enchaîne **obligatoirement** la phase de recherche
  totale (DFS sur la frontière commune, ci-dessous).

## Arbre commun et recherche totale (0.3.0)

`src/explore/tree.c` : tout chemin — graine, fourche branche/IRQ, dépôt de
session — est un **choix** relié à son parent (la frontière EST la file du
fourcheur ; chaque état en file porte son id de choix). À la fin d'un
chemin, le choix se ferme avec une issue : `done`, `merged` (HALT ou N pas
sans aucune première découverte — retombe sur du connu sans issue, le cas
gameover), `dead` (opcode indéfini exécuté), `reset` (exécution revenue à
$0100), `budget`, `capped`, `nonsense` (évidence de non-sens, voir plus
bas). `tree.bin` (`--state-dir`) persiste les choix,
`--resume` les recharge. Après les sessions, le run enchaîne la **recherche
totale** : DFS (`--dfs` activé pour cette phase) jusqu'à frontière vide et
choix tous clos, dans les limites des budgets — rapport : profondeur max,
ouverts/fermés par issue. Sur Super Mario Land avec un TAS : 6 277 choix,
profondeur 8, 341 merged, 28 dead, 1 674 budget, 148 capped au mur des 20 M
pas globaux (observations outil, pas mesures matérielles).

### Padding

Une exécution qui **glisse** dans une plombée statique (runs 8+ de 00/FF,
`DP_SF_PAD`) est un retour sauvage (RETI sur pile vide...) : les octets de
padding ne sont **jamais** marqués code, et la glissade est coupée après
quelques pas de suite (sessions) ou bornée (fourcheur, qui doit atteindre le
code derrière la plombée). Un pas isolé sur un octet plombé (le NOP du boot,
un vecteur) reste légal.

### Mode humain

`--human [slot]` (ou `H` dans la GUI) prend une lane : le pad/clavier la
joue, l'outil surveille le crédit et **sauve chaque avancée** comme snapshot
PNG (écran 160×144 + machine complète en chunk `tEXt` `deepeel_snapshot`,
style cartouche PICO-8 — un seul fichier, visible et rechargeable avec
`--from-png f` ou `dp_play_load_png`). Les découvertes humaines alimentent
le pool et le fourcheur comme celles des sessions.

### Export jouable (build pc de m0boy)

`--export-playable <projet_pc>` écrit le checkpoint choisi (le plus
productif du pool, `--export-slot N` pour un autre, le slot déposé par
`--from-png` sinon) en **slot 0** du `<SD>.STA` du projet pc, au format
de `m0_state.cpp` (en-tête `GBLSTATE`, tag de format = FNV de la version
du générateur et de la taille d'état, checksum FNV, écriture de l'en-tête
en dernier). Les constantes sont lues dans le `gbl_config.h` du projet —
pas de seconde copie des décisions du générateur — et le `.SAV` batterie
est écrit avec (l'état porte lui-même la RAM cartouche). Le firmware pc
propose ensuite le slot au menu (Etat → Charger l'etat) et `M0_STATE=0`
boote directement dessus. Testé bout en bout sur Super Mario Land :
slot accepté par le magasin d'états du firmware, écran différent avec et
sans `M0_STATE`.

### Éviction du non-sens

Un chemin du fourcheur qui prouve son non-sens est coupé sur place et ses
octets ne deviennent jamais de l'évidence de code — l'issue `nonsense`
ferme le choix, la raison est comptée au rapport (`nonsense evictions`):
exécution dans l'echo RAM ou l'OAM (`echo/OAM`), exécution dans des octets
prouvés `verbatim` en data par le suivi de copies et jamais exécutés du
flot naturel (`verbatim data`), `RET` avec un pointeur de pile hors
WRAM/HRAM (`incoherent stack`). Fixtures `fix_nsecho`, `fix_nsdata` (le
saut dans la data vit sur l'issue non prise d'un conditionnel, la preuve
verbatim n'existant qu'à la fin du chemin qui copie), `fix_nsstack`.

### Déterminisme

Perdu entre threads (l'entrelacement dépend de l'ordonnanceur) ; par contre
chaque session reste rejouable individuellement (graine ou script), et un
run à masques fixes + `--fork-paths 0` est reproductible.

## Fermeture statique et élagage (0.4.0)

La machine prouve le code en l'exécutant ; ses garde-fous (budget par chemin,
mur global, plafonds de forks, file) laissent du flux direct non visité.
`src/explore/closure.c` suit ce flux sans machine : depuis chaque instruction
exécutée, fall-through, cibles JR/JP/CALL/RST et les deux côtés de chaque
conditionnel. Ce qui est atteint ainsi est marqué `DP_XF_STATIC` — jamais
confondu avec `natural`/`forced`. Le rapport dit « static closure : … octets
atteints par flux direct seul ».

- Arrêts : opcode indéfini, padding, table de pointeurs, données prouvées
  verbatim. Comptés (`stopped by data/padding`, `by undefined opcodes`).
- Une arête de banque 0 vers `$4000+` n'est résolue que par les banques
  observées dans la section (ou la banque de boot) ; sinon elle est comptée
  `unresolved` et **toutes les vues banque de la cible sont semées** comme
  graines spéculatives (borné : sites × banques, plafond `--seed-cap`, compteur
  `bank candidates seeded`). Les `JP HL` observés sont listés avec leurs cibles
  (jusqu'à 8 par site, débordement compté) ; **après les phases de couverture,
  chaque cible observée sans exécution est semée à son tour** (phase
  `indirect-search`, compteur `JP HL targets seeded`).
- Une graine refusée par la file pleine n'est plus perdue : elle est
  re-proposée au tour suivant (les compteurs `seeds:` ne comptent que les
  graines réellement acceptées). Les graines de fermeture, les cibles
  indirectes et les snapshots TAS passent par une **boîte d'arrivée
  prioritaire** servie avant la file de travail — dans un régime saturé (chaque
  chemin forké en produit d'autres), les graines tardives n'y meurent plus de
  faim ; elles **contournent l'éviction par contexte** (sonde délibérée, peu
  nombreuses, plafonnées).
- Boucle `--closure-rounds N` (défaut 4) : fermeture → graines (les entrées non
  exécutées, plus les vues banque ambiguës) → recherche totale → fermeture…
  jusqu'à ce qu'un tour ne sème rien. Plafond de graines `--seed-cap`, rejets
  comptés (`dropped`).
- Export `.cov` : octets statiques = début d'instruction seul, drapeau 4
  (spéculatif), +2 si entrée, compte 0. Jamais le drapeau 1.
- Élagage : un site conditionnel dont les deux côtés ont déjà été exécutés ne
  reçoit plus que `--covered-keep` forks (défaut 2) ; `--no-covered-prune`
  rétablit l'ancien comportement. Compteur `forks pruned`. Les forks de touches
  sont limités à un tour par site une fois le gestionnaire joypad (`$60`) vu.
- Le mur global compte les pas du seul explorateur à forks : les pas des
  sessions jouées ne l'épuisent plus. Chaque tour de fermeture a son mur.
- TAS : pendant le replay, l'état de la machine est capturé en **snapshot
  PNG** (écran + machine complet, comme les snapshots humains) à intervalle
  `min(tas_ckpt_frames, tas_ckpt_secs×60)` — par défaut `min(frames du film /
  1024, 30 s × 60)` — plafonné à `--tas-ckpt-max` (défaut 1024) par film.
  Ces « shoots » ne nourrissent **pas** l'exploration pendant le jeu : une
  fois les films joués en entier et toute la couverture drainée (phase 3),
  ils sont semés chacun dans le fourcheur et une passe d'exploration
  supplémentaire leur est consacrée (phase 4, époques `tas-shoots` /
  `tas-shoots-search` du journal des découvertes). `--tas-ckpt-live` rétablit
  l'ancien dépôt direct dans la boîte de graines pendant le replay.
  `--tas-ckpt N` force l'intervalle en frames, `--tas-ckpt-secs 0` coupe.
- **TAS solo** : `--tas-solo` — seuls les workers TAS tournent (un par film,
  pas de foule aléatoire/attract) et le jeu s'arrête à la fin des films :
  le replay en mode single, ses snapshots conservés, puis les phases de
  couverture et l'exploration des shoots. Combiné à `--mem-cap MB` (RSS
  plafonné — la seule condition qui passe devant la priorité TAS) et
  `--resume`, un long run se mène par segments : au plafond, arrêt
  gracieux avec rapports, reprise là où il s'était arrêté.
- **reprise TAS** : `pool.bin` (v3) conserve aussi les records de snapshots
  TAS ; `--resume` relance chaque film là où s'est arrêté son dernier
  snapshot (au plus 1 intervalle de jeu perdu), pas depuis la mise sous
  tension.
- Un opcode indéfini n'est plus consigné qu'une fois par site.

Observations de l'outil (machine concrète, RAM à zéro), sans jeu, avant → après :
Super Mario Land 14 062 → 20 551 octets de code, Mystic Quest 19 341 → 28 575.
Ce sont des comptes de l'outil, statiques inclus, pas des mesures de matériel.
Aucun fichier TAS n'était disponible : le chemin TAS n'est couvert que par
les tests synthétiques.

## Données prouvées et pont .cov

Une copie ROM→VRAM dont les octets lus hachent exactement comme les octets
écrits (mêmes longueurs) est **verbatim** (`DP_DF_VERBATIM`, canaux OAM
DMA/HDMA toujours) : la source est de la donnée, pas du code. Le `.cov`
l'exporte avec le flag 8 (`1=exec, 2=entry, 4=speculative, 8=data`), jamais
sur une ligne aussi exécutée au flux naturel ; l'exécution purement
spéculative du fourcheur ne contredit pas la preuve de copie. Côté m0boy,
`Generator::load_coverage` saute le décodage des lignes flag 8 non exécutées
: le convertisseur ne compile plus les tuiles/tableaux copiés « au cas où ».
`--romdata f` écrit le profil de lectures ROM (pages de 256 octets, format
`romdata-*.prof`) pour peser la résidence flash/streaming.

## Memory map `.map` et `.map.png` (0.5.0)

Le but de l'outil est une memory map couvrant **100 % de la ROM** : chaque
octet reçoit exactement une classe. Les rapports donnent connu / inconnu,
code / data / padding.

- Classes observées : `code-natural` (exécuté en jeu), `code-forced`
  (chemin forcé), `code-static` (fermeture statique), `data-copied` (copié
  ROM→VRAM/WRAM ou verbatim), `data-read` (lu), `table` (pointeurs
  statiques), `padding`.
- Classes **déduites** (toujours étiquetées) : `inferred-code` (≤ 3 octets
  entre deux codes), `inferred-data` (≤ 1024 octets entre deux data),
  `inferred-padding` (≥ 4 octets ne contenant que du 00/FF, mélangés). Jamais
  à travers une banque.
- Le balayage statique marque un run de padding **en entier** dès le seuil de
  8 octets (la tête du run n'est plus laissée de côté).
- La ligne `residual unknown` du rapport liste les plages que rien n'a pu
  observer ni déduire, les plus grandes d'abord — la frontière honnête de la
  carte.
- « Connu » inclut donc les octets déduits ; la part déduite est rapportée à part.
- `deepeel maprender ROM in.map out.png` re-rend une couverture depuis un
  `.map` sauvegardé ; pour les cartes z80 (offsets physiques de 16 Kio) :
  `--z80` réécrit les plages en vues fenêtrées, et `--sys sms|gg|msx`
  + `--title nom` rendent la couverture d'une ROM non GB sans re-scan.
- `deepeel explore ROM --map x.map` écrit le texte (`banque début fin classe`,
  fin incluse, adresses vues du CPU) ; tout autre chemin écrit l'image PNG
  (défaut depuis 0.26.0, voir plus bas : 1 pixel par octet, panneau de
  chaleur, chunks zTXt compressés). Rouge = inconnu, bleus = code,
  verts = data, ocre = table, gris = padding. Elle **embarque le `.map`
  complet** (chunk `deepeel_map`, comme une cartouche PICO-8) et les
  paramètres de la conversion (chunk `deepeel_meta` : version, ROM,
  config, résultat) ; sous la grille, une légende dessinée : nom interne de
  la ROM (repli : nom de fichier), couverture globale, signature du
  convertisseur, puis chaque classe présente en deux colonnes justifiées —
  pastille carrée de sa couleur à côté du texte, police 8-bit 5×7 (accents
  é/è/ê/à dessinés dans les rangées libres des glyphes), deux colonnes
  seulement quand l'image est assez large. `deepeel mapx x.map.png x.map`
  extrait le `.map` à l'identique (`--chunk NOM` pour un autre chunk).
- `--html f` : page web autonome (carrés par banque, légende, tableau des
  classes, journal de découverte : temps, source, crédit, octets connus).
  Le rapport texte affiche aussi les 40 dernières entrées du journal.
- `tools/tasvideos_fetch.py ROM.gb DIR [--game-id N] [--max N] [--list]`
  télécharge **tous** les films TASVideos du jeu : la Subs-List du jeu
  (`Subs-List?GameId=N`, toutes les soumissions quel que soit le statut —
  publiées, rejetées, annulées, playground), plus les publications ; reprise
  sur 503. `--tas` accepte jusqu'à 1024 fichiers, tous rejoués en entier.
- Saturation : les débordements de file de la recherche sont rapportés
  (« queue overflows »), jamais silencieux.
- Ces nombres sont des observations de l'outil (machine concrète, RAM à
  zéro, registres événementiels statiques), pas des mesures de matériel.

### File adaptative et murs de ressource (0.9.0)

- La file de travail pleine **double** au lieu de jeter des chemins entiers :
  croissance ×2 jusqu'au plafond `--queue-max` (défaut 4× la taille `--queue`),
  arrêtée par le mur `--mem-cap` (RSS sondé via `/proc`, le même mur que les
  sessions). Chaque croissance est comptée (`queue growths` dans la ligne
  `budgets:` du rapport) ; au plafond, les pertes repassent dans
  « queue overflows ». La boîte prioritaire reste bornée par `--seed-cap`.
- `--time-limit SEC` : mur horloce de la phase de jeu, arrêt **gracieux**
  (pool sauvegardé, `--resume` reprend). C'est un mur de campagne : il
  l'emporte même sur la priorité TAS — un scan est borné sans surveillance.
- **Spool disque des branches** (dès qu'un `--state-dir` est fourni) : les
  états que la file RAM ne peut pas tenir (queue pleine au plafond
  `--queue-max` ou au mur `--mem-cap`) sont écrits dans
  `<state-dir>/spool.bin` au lieu d'être jetés — `spool: N written, M read
  back, K refused` dans la ligne budgets, un refus réel n'est plus jamais
  silencieux. `dp_explore_step` réalimente la file depuis le spool dès qu'un
  slot se libère. Un arrêt (SIGINT, mur horloge, mur mémoire) **vide la file
  restante dans le spool** avant de sortir : `--resume` relit le spool tel
  quel et l'exploration reprend exactement là où elle s'était arrêtée,
  y compris après un kill d'urgence (seuls les enregistrements entiers sont
  relus). Le dedup d'états exacts persiste à côté (`<state-dir>/exact.bin`) —
  sans lui, un run repris ré-explorerait tout le spool depuis zéro. Plafond
  disque `--spool-cap-mb` (défaut 4096) ; l'arbre marque les choix spoolés
  (`issue "spooled"`).
- **Anatomie du résidu** : après la carte, chaque plage inconnue reçoit un
  verdict mécanique d'après la classe de ses deux bords — `code-code`
  (dispatch que la closure n'a jamais résolu), `code-data` (bord mixte,
  jamais inféré par conception), `data-data` (donnée qu'aucune route n'a
  lue), `padding-fringe`, `edge-code`/`edge-data`/`bank` (bord de banque).
  Le rapport texte la résume (`residual anatomy:`) et le summary JSON porte
  le champ `mech` sur chaque plage résiduelle : la feuille de route vers
  100 % est désormais produite par l'outil lui-même.
- **Garde-fou de la frange de remplissage** (0.251.1) : une plage remplie
  de 00/FF adjacente à du remplissage n'est inférée `inferred-padding` que
  si aucun pointeur plausible ne vise à l'intérieur (mot de 2 octets posé
  sur du code ou une table, forme bare et forme banque:adresse) ; les
  refus sont comptés (`padding fringe: N filler run(s) not inferred` dans
  le texte, `pad_fringe_refused` dans le JSON). Mesure sur l'état gelé
  Zelda (reprise + fermeture seule, 19 s de banc rapide) : 4 plages
  refusées, 32 octets basculent d'`inferred-padding` vers l'inférence
  data, le résidu est inchangé — le garde-fou protège, il ne regress rien.
  Le restant « padding-fringe » de Zelda (9658 octets) est de la donnée
  clairsemée (tilemaps 00-04, plans graphiques) que le run de référence
  8 h30 a lue comme donnée : l'inférer padding serait un faux positif de
  classe — limite documentée, la règle est saturée.
- **Preuve de dispatch stride-3** (0.251.2) : la table d'opérandes d'un
  `jp hl` exécuté peut être au pas 3 (adresse + octet de banque par entrée,
  l'idiome multi-banques) — chaque entrée nomme sa propre banque, la cible
  est marquée dans la vue `banque*0x4000 + (adresse & 0x3FFF)`, y compris
  pour une adresse basse (< $4000) d'une autre banque. L'acceptation d'une
  cible non encore exécutée est renforcée : elle doit décoder, sans opcode
  indéfini, jusqu'à un transfert (ret/jr/jp/rst/call/halt) dans une fenêtre
  de 48 instructions ; les octets d'une table validée sont prouvés données.
  Fixture `dispatch3` (table stride-3, entrée prise en banque 1, la
  fermeture S4 est appelée explicitement par le harnais). Mesure Zelda
  (banc rapide 19 s) : résidu inchangé (11787 octets, même anatomie) — les
  21 plages code-code n'y sont pas des tables S4 ; 8 tables validées contre
  16 sous l'ancien critère (« premier octet légal » suffisait) : les 8
  perdues avaient des cibles qui ne décodent pas jusqu'à un transfert, des
  racines statiques douteuses que la preuve renforcée refuse.
- **Ciblage des plages résiduelles** (0.251.3) : nouveau module
  `src/explore/residual.c`, appelé à chaque round de fermeture. Les plages
  résiduelles de classe dispatch (`code-code`, `edge-code`, et
  `padding-fringe` dont le bord gauche est du code) sont traitées une à une
  (ensemble `tried` persistant entre rounds, échec marqué, jamais de
  re-boucle) : le prédécesseur — le `jp hl` exécuté le plus proche derrière
  la plage, même banque, fenêtre de 2 Ko — voit sa table d'opérandes
  re-prouvée avec l'acceptation renforcée (pas 2/3, cible doit décoder
  jusqu'à un transfert) ; une table validée marque ses entrées et prouve ses
  octets données. Une plage qui commence juste après un transfert
  inconditionnel exécuté (ret/reti/jp/jr — le `jp hl` non prouvé refuse la
  plage comme table d'opérandes présumée) reçoit une graine froide forcée :
  la machine en dispose. Les plages refusées sont comptées
  (`residual_targets` / `residual_seeded` / `residual_static` /
  `residual_refused` dans le JSON, ligne `residual targeting:` dans le
  texte). Mesure Zelda (banc rapide sur l'état gelé) : 64 plages
  dispatch essayées, 16 entrées forcées semées, 5 refusées — le résidu passe
  de 11787 à 11408 octets (932 → 909 plages) dès les rounds statiques ; le
  reste du gain attendu vient de l'exploration qui consomme les graines.
- **Recherche arrière depuis les états sauvegardés** (0.251.4) : chaque
  instantané TAS porte désormais la banque ROM où tournait le PC à la
  capture — dans le nom du fichier (`tasNN-bB-fF.png`, l'index
  banque→états sur disque, visible par nom) et dans les enregistrements du
  pool (format `pool.bin` v4 ; les fichiers v3 ne reprennent plus — la
  reprise repart proprement de zéro). La phase post-jeu sème les états en
  deux passes : d'abord ceux dont la banque de capture détient encore du
  résidu inconnu (vues statiques de la carte, priorité 1 — des états de jeu
  réels **locaux à la zone** que la fermeture n'a jamais résolue), puis le
  reste (priorité 0). Compteur `tas_bank_hits`, ligne stderr
  `TAS shoots: N snapshots seeded (M from residual banks: backsearch
  first)`. Les répertoires d'état écrits avant 0.251.4 (pool v3) ne
  reprennent plus : la reprise repart proprement de zéro.
- **Attribution par `first_seen`, arrêt du worker attract, seuil de
  rendement** (0.251.5) : `methods_pct` n'est plus le prorata de `credit`
  sur la hausse d'une époque (le crédit d'un worker qui tourne en continu
  sans rien trouver gonflait sa part) — chaque octet est crédité à la
  source qui l'a **premièrement** prouvé (journal `first_seen`, par octet) ;
  les lignes session portent la méthode dans l'acteur (tas / rng / attract ;
  acteur 0 = chemins historiques, case statique comme avant), le débordement
  du journal reste compté (`first_dropped` dans le JSON). Le worker attract
  ne tourne plus à rendement nul toute la durée du run : après
  `attract_stall` époques vides (3 par défaut), il est retiré au jeu
  aléatoire depuis le pool (le moins choisi), compté sur stderr. Le seuil de
  rendement par méthode est opt-in : `--yield-window N --yield-min B` somme
  le crédit de chaque méthode sur les N dernières époques ; une méthode sous
  le minimum se réaffecte (attract → rng, rng stagnant → meilleur
  checkpoint), et quand **toutes** y sont le run s'arrête proprement
  (`stop_reason: "yield"`). `tools/romset_batch.py` forward
  `--yield-window/--yield-min`. Mesure sur l'état gelé Zelda : l'ancienne
  attribution créditait « attract s0 » de 315 378 octets connus cumulés ;
  l'attribution first_seen rend la part réelle de l'attract ≈ 0 % —
  l'artefact est confirmé et corrigé.
- **Graines explicites au-delà du dedup saturé** (0.251.6) : la campagne
  Zelda (2 h 15, 99,68 % connu, résidu 1678 o) a exposé une perte —
  l'ensemble de déduplication saturé (16,4 M refus) faisait mourir chaque
  dépôt priority : les 937 instantanés TAS de la phase post-jeu et les
  cibles résiduelles tombaient à « 0 seeded » (la branche de report au
  frontier n'est pas câblée pour le pool partagé). Un dépôt priority est
  une graine explicite (entrée de fermeture, cible résiduelle,
  instantané TAS) : l'ensemble saturé ne peut pas prouver qu'il est déjà
  exploré — il est déposé quand même, compté (`seeds_dedup_bypass`,
  texte et JSON) ; une graine dupliquée coûte une re-exécution, une
  graine perdue coûte la plage. L'inbox priority (plafond 512 par défaut)
  croît maintenant par doublement borné (64 K, repliage de l'anneau
  compté dans `queue_growths`) au lieu de refuser un dépôt massif.
  Tableau octets/heure-CPU Zelda (attribution first_seen, 12,77 CPU-h) :
  statique 16716, tas 12835, fork 7120, random 2306, attract 427 —
  la réaffectation du cœur attract (0.251.5) est justifiée par les
  chiffres.
- **Logo Nintendo prouvé donnée** (0.251.7) : quand la cartouche porte le
  logo canonique à $0104 (les 16 premiers octets vérifiés), la plage
  $0104-$0133 est prouvée donnée au scan statique — le boot ROM la lit
  intégralement à chaque mise sous tension sur tout matériel réel, c'est
  une donnée par construction matérielle, pas une inférence. Un blob
  différent à $0104 (ROM non démarrable) reste intact : la règle prouve
  la position, jamais le contenu. Compteur `logo_data`. Mesure : la ROM
  blargg `09-op r,r` (verrou historique de la campagne 0.250.1 : la
  distribution indirecte du harnais empêchait les fourches d'atteindre
  les cibles) passe à **100,0 % connu** — son résidu de 19 octets était
  la fin du logo que seul le boot lisait.
- **Registres natifs SMS/GG/SG + MSX** (0.251.8) : les tables de ports
  `DpSegaPort` deviennent des entrées `DpIoReg` natives (le modèle
  GB/NES/PCE/MD, `dp_platform_sega` / `dp_platform_msx`) : catégories
  vidéo/son/entrée/série/système décidées par nom de registre, les
  registres événementiels (VDP_STATUS, V/H_COUNTER, GG_START) portent
  `DP_IMPL_EVENT` — la surface des sondes §3.7. Les tables de métadonnées
  restent l'unique source de vérité (adresses, masques de décodage,
  direction) ; la conversion ne perd aucun port (épinglé par le test
  `registry` : compte exact, catégories valides, drapeaux cohérents,
  registre jamais inventé).
- **Liveness registre -> éviction C2 sur PCE** (0.251.9) :
  `deepeel pce explore ... --ctx-cap N --ctx-live` projette la clé de
  contexte sur la section cible : le PC se replie dans la page de 8 KiB
  qui le porte, et la clé porte les blocs matériels que le code de la
  page lit (C1 : VDC/VCE/PSG/timer/manette/IRQ, `live_io` par cellule) —
  même sémantique que `--ctx-live` NES (projection sur ce que la fenêtre
  lit) et MD (mesuré sur Sonic : divise le travail mais aussi la
  couverture, la clé grossière reste le défaut). La ligne de rapport
  nomme la clé utilisée (`section-live pc/regs/MPR`).
- **Correction campagne B-F** : le message du commit 1cb3944 annonce SML2
  « match en 3,8x moins de mur » — erreur, le vrai ratio est **1,9x** : la
  référence SML2 a tourné 15 301 s (4 h 15), pas 8 h 30 (le 3,8x est correct
  pour Zelda seule : 30 606 s -> 8 107 s). Chiffres vérifiés à la source :
  référence Zelda 99,743 % / 1 347 o / 89 plages, finition Zelda 99,68 % /
  1 678 o / 117 (légère régression — les 8 h 30 de fourches de la référence
  ont creusé plus) ; référence SML2 99,431 % / 2 982 o / 263, finition SML2
  99,435 % / 2 961 o / 302 (légère amélioration).
- **Sweep des ROMs de test matériel — mooneye acceptance** (étape C) : 75
  ROMs, 30 s de jeu chacune, critère `residual_bytes == 0` : **72/75 à
  résidu nul**. Les 3 restantes sont des ROMs de timing pur, à 99,98 %
  chacune : `boot_hwio-dmg0` (7 o), `call_cc_timing` (6 o), `call_timing`
  (6 o) — les boucles de mesure de timing elles-mêmes, jamais atteintes en
  30 s de jeu. blargg `cpu_instrs` : 11/11 à 100,0 % connu, résidu 0 ;
  ZEXALL SMS : 100,0 %, résidu 0.
- **Sondes de lecture d'événement §3.7 côté sega** (0.251.11) : un `in`
  sur un port événementiel du registre natif (VDP_STATUS, V/H_COUNTER —
  les entrées `DP_IMPL_EVENT`, GG_START reste aux sondes d'entrée) fourche
  par valeur plausible (bits de statut, phases de compteur de ligne/trame) ;
  la valeur substituée est portée par le chemin en file et lue une fois
  (`in_bus`), la machine parente n'est jamais touchée. Garde de principe :
  jamais de sonde quand une IRQ/NMI est en attente ou sur un chemin de
  fourche IRQ (`skip_irq`) — une substitution ne doit pas masquer une ligne
  d'interruption (c'est la régression qu'a attrapée `sega_hardware`).
  Opt-out `--no-event-probes` ; fourches/coupures comptées (texte, JSON,
  fusion `--jobs`, journal 64 entrées). Mesure A/B Aleste (SMS, 70 M pas) :
  couverture identique (69,897 %, résidu 4309 o), 14 fourches d'événement
  tirées — neutre sur ce jeu, la garde IRQ limite le tir ; aucun faux
  positif observé. cgb_sound/dmg_sound : les singles blargg 12/12 à
  99,98 % (6 o résiduels chacun), les suites complètes restent à 87-88 %
  (harnais audio, CGB listé non émulé) ; `oam_bug` 99,76 %.
- `--tas-dir D` (répétable) : découvre tous les films du répertoire (triés
  par ressemblance de titre ; `.vmv`, `.vbm`, `.bk2`,
  `.gbm`, `.fm2`, `.log`), les films retenus sont loggés sur stderr.
- `--summary f` : profil JSON compact d'une passe — identité ROM, écho de la
  config, résumé de la carte (toutes classes), liste **complète** des plages
  résiduelles, tous les compteurs (y compris ceux que le texte taisait :
  `forks_capped`, `halt_cuts`, `exec_outside`, toute la famille closure),
  arbre (choix, ouverts, issues) et faits de jeu (frames, TAS, raison
  d'arrêt). `--json` gagne les mêmes compteurs et une section `map`.
- `tools/scan_all.py` : campagne multi-ROMs — `scan` applique la recette
  générique à chaque ROM d'un répertoire (30 min de jeu par défaut, films TAS
  minés localement avec assignation exclusive par ressemblance — «
  supermarioland2 » ne pollue pas « supermarioland »), `--full --only <slug>`
  enchaîne une passe à fond avec `--resume`, `digest` agrège les profils en
  `REPORT.md` (couverture, anatomie du résidu, murs et saturations).

### TAS sans limite de temps ni de budget (0.5.0)

Un film TAS est rejoué en entier : `--frames`, `--stagnation` et `--until-static`
attendent la fin de tous les films (seul un SIGINT ou un argument explicite
borne un film). Une coupure du fork explorer pendant le film (halt mort,
glissade dans le padding) n'abandonne plus le film : il reprend à la frame
suivante. Garde-fou : plus de 1000 coupures consécutives sans avancer d'une
frame = machine réellement bloquée, repli en jeu aléatoire. Les deux compteurs
(« TAS cuts absorbed », « wedged-machine fallbacks ») sont rapportés sur
stderr ; une saturation n'est jamais silencieuse.

## Fenêtre de surveillance `--watch` (0.5.1)

`deepeel explore ROM --play --tas film.vbm --watch [FPS]` ouvre une petite
fenêtre SDL2 (sans carte ni stats) qui montre l'écran du jeu rejoué par le
TAS, échantillonné à 1..30 images/s en temps humain (défaut 30). Une image
sur 1/FPS s est rendue par la machine du film, les autres ne le sont pas :
le TAS n'est pas ralenti. Depuis 0.249.74 la fenêtre est une **mosaïque** :
une vignette par worker qui rejoue un film (grille choisie pour la plus grande
image, fenêtre redimensionnable), avec le numéro du film, son nom, la frame
courante et la vitesse réelle du rejeu en frames/s (observation de l'outil) ;
un film terminé garde sa dernière image marquée « done ». `Échap` ferme la
fenêtre seule, le run continue. Sans SDL2, l'option affiche un message et est ignorée.

## Copies des longues sessions, provenance, sources sœurs, identité des films (0.22.0)

Quatre corrections génériques du moteur et de la carte, mesurées sur toute la
campagne (observations de l'outil, pas des mesures de matériel).

- **Copies des sessions** : le traceur de boucles de copie n'émettait un
  enregistrement qu'en fin de chemin ; une lecture ROM ou une écriture VRAM
  hors fenêtre (IRQ, routine suivante) **réinitialisait** la fenêtre accumulée
  sans l'émettre. Une session jouée (un TAS entier) perdait ainsi presque
  toutes ses copies réelles. La fenêtre est désormais émise avant d'être
  réinitialisée (même règle des ≥ 2 itérations). Ligne de rapport
  `copies: N records, M emitted at window breaks, K dropped by the cap` ; le
  plafond `max_copies` est compté (`copies_dropped`), plus jamais silencieux.
- **Provenance réellement exécutée** : `closure_rounds` quittait la fonction
  (`return`) au premier tour sans graine — la passe de provenance et la
  closure finale ne tournaient jamais (« 0 data bytes static » partout).
- **Closure par flot garanti** : un successeur d'instruction exécutée (ou
  d'une ligne droite déjà garantie) traverse une marque heuristique `table`
  du scan de pointeurs — le flot de contrôle prouve une instruction. La
  garantie tombe après un `CALL`/`RST` (données en ligne après un appel) ;
  padding, data verbatim et opcodes illégaux arrêtent toujours. Chaque état
  est visité au plus deux fois (garanti / non garanti) : borné.
- **Sources sœurs d'une copie** (`DP_DF_SIBLING`, classe `inferred-data`) :
  une copie ROM→VRAM *verbatim* (≥ 16 octets) dont l'adresse source est un
  mot d'une table ROM lue (mots contigus, non exécutés, lus ou pointeurs,
  même fenêtre, ≤ 64 entrées) rend les autres entrées de la table sources de
  même longueur — les tuiles d'un niveau jamais joué, chargées par le même
  chargeur. Refus comptés (une plage sœur sur du code naturel), plafonds
  comptés ; ligne stderr `copy siblings: tables, ranges, bytes (refused,
  caps)` ; compteurs `sib_*`. Une inférence ne masque jamais une observation.

Campagne `scan_all` bornée (300 s de jeu par ROM, `--jobs 7`, deux passes
parallèles ; le moteur est déterministe : deux passes de référence
identiques à l'octet) :

| ROM | 0.13 (référence) | 0.22 | résidu |
|---|---|---|---|
| Super Mario Land | 99,826 % | 99,991 % | 114 → 6 o |
| Zelda LA | 91,70 % | 91,88 % | 43 513 → 42 590 o |
| Mystic Quest | 90,27 % | 90,81 % | 25 516 → 24 105 o |
| SML2 | 85,20 % | 86,85 % | 77 606 → 68 925 o |
| Pokémon Jaune | 77,55 % | 80,30 % | 235 455 → 206 592 o |

La part déduite **baisse** sur SML2 et Pokémon : les copies des sessions sont
de l'observation nouvelle. Aucun octet de code n'est étiqueté data par la
règle des sœurs (contrôle : décodage des plages nouvellement déduites).

Règles essayées puis **écartées** (elles étiquetaient du code en data) :

- *bords mixtes code|data* (trou après un terminateur `RET`/`JP`, ou avant du
  code atteint par saut) : sur Zelda, ~43 % des octets ainsi déduits se
  décodent en vraies routines jamais exécutées (`call $3B8D … ret`) ;
- *segments entre cibles d'une table de pointeurs copiée* : une table de
  routines non exécutées (Zelda 1:$6027) passe pour des segments de données ;
  exiger une lecture observée du segment la rend inopérante là où il n'y a
  pas de jeu.

Limite connue de la règle préexistante `data|data` (≤ 1024 octets) : une
marque `table` (heuristique) compte comme bord de data ; des routines non
explorées encadrées de faux pointeurs deviennent ainsi `inferred-data`
(vu sur Zelda 6:$53B8, Pokémon 15:$6870). L'exclure entièrement fait
s'effondrer la carte (Mystic 90 % → 53 %) : dans les banques de tuiles la
règle est juste ; elle reste donc, documentée.

Super Mario Land : les 6 octets restants (1:$58D1, $5915, $592D–$592F, $5953)
sont dans le flux de niveau du monde 4, comme les tuiles de 1:$47F2–$4FB1
(désormais `inferred-data` par la règle des sœurs). Le monde 4 n'est jamais
joué : les films TAS désynchronisent au jeu bonus de 1-3 — lot tiré de
`DIV & 3` en $3E7D ; un décalage de phase de DIV donne le bon lot, puis
l'aléa des poissons de 2-1 (DIV encore) diverge : la phase de DIV n'est pas
reproduite au cycle près par rapport à VBA/Gambatte.

**Identité des films TAS** : l'attribution des films par `scan_all.py` se
fait au nom de fichier ; deepeel vérifie maintenant l'identité de la ROM
enregistrée dans le film — SHA-1 du `Header.txt` BizHawk, checksums d'en-tête
et global de l'en-tête VBM ($31-$33). Un film d'une autre révision est
**refusé** et journalisé (`TAS … refused: recorded on another ROM`),
`--tas-any-rom` le force. Sur Super Mario Land World, quatre des sept films
visaient la Rev A et désynchronisaient par construction (Gambatte lui-même
décroche en 2-1 en rejouant le BK2 Rev A sur la ROM World).

**Fidélité de la machine** : les écarts entre m0boy et deux références
indépendantes (SameBoy et Gambatte, identiques entre elles instruction par
instruction et cycle par cycle sur la zone étudiée) sont corrigés dans le
runtime m0boy ; deepeel compile ce runtime avec `GBL_ACCESS_BLOCK=1`
(blocage VRAM/OAM par mode). Rapport : `docs/m0boy-fidelite.fr.md` (rétroporté dans m0boy, commit `645df84`).

## Rejeu TAS : trame 0 à la mise sous tension, profil de synchronisation VBA (0.24.0)

Diagnostic fait contre une référence : le cœur GB de VBA-rr v24 (« V8 »,
`/home/m/dev/vba-rerecording`) compilé sans affichage rejoue le film
`muggv3` jusqu'en 4-3 sur la ROM World (le cœur « V7 » de v23 désynchronise
lui-même). Comparaison trame par trame des hachages de WRAM (hors pile $CFxx
et pilote sonore $DFxx), puis des traces d'instructions à la première trame
divergente. Trois causes :

- **Trame 0 = trame de mise sous tension** (bogue générique de deepeel, tous
  formats) : l'entrée de la trame 0 d'un film est tenue dès la première
  instruction (VBA comme BizHawk), chaque vblank fournit ensuite la trame
  suivante. deepeel fournissait la trame 0 à la première vblank : tout film
  était rejoué **une trame en retard** (symptômes : compteur de trames du jeu
  $FFAC à +1, DIV à +18 aux tirages aléatoires). Corrigé dans `sessions.c`.
- **Lecture de DIV en fin d'instruction** (particularité de VBA) : le cœur V8
  décompte les cycles d'une instruction *avant* de l'exécuter ; ses lectures
  de registres voient la fin de l'instruction, 4 cycles après l'accès matériel
  (dernier cycle machine). Sur SML cela décide la lecture de DIV en $5A76
  (fin du jeu bonus de 1-3).
- **Horloge de trame LCD éteint** (convention de VBA) : LCD éteint, l'horloge
  de lignes de VBA garde sa phase ; une trame se termine à chaque passage
  LY 144→145 une fois l'écran blanc affiché ((150 − LY) lignes + 49 cycles
  machine après l'extinction). m0boy redémarrait une trame virtuelle de
  70 224 cycles à l'extinction. Le matériel n'a pas de vblank LCD éteint :
  ce sont deux conventions de trame, celle du film fait foi.

Les deux particularités sont des **réglages de synchronisation de film** du
runtime m0boy (`gbl_sync_div_skew`, `gbl_sync_vba_off_clock`, défaut 0 =
comportement matériel, chemin C seulement), exposés dans `DpGblApi`. deepeel
les arme sur la machine d'un worker **seulement pendant qu'il rejoue un VBM
binaire** (ligne stderr `VBA sync profile`) et les remet à 0 quand le film se
termine ou que la machine est déclarée bloquée : le jeu aléatoire, le
fork-explorer et les autres films tournent avec la temporisation matérielle.
m0boy reste fidèle (SameBoy et Gambatte concordent avec lui) ; seul le rejeu
reproduit l'émulateur d'enregistrement, comme les « sync settings » de
BizHawk. Le format du film est consigné dans `DpTas.recorder`.

Résultat (observation de l'outil) : Super Mario Land World, films seuls
(`--tas-solo`) — `muggv3` atteint le monde 4 ; **100,0 % connus, 0,0 %
déduits, résidu nul** (0.22 : 99,991 %, 6 octets du flux du monde 4).

Deux corrections de campagne vont avec :

- **`--tas-solo` sans film jouable** : si tous les films sont illisibles ou
  enregistrés sur une autre révision (refusés par l'identité), le mode solo
  s'arrêtait à la trame 0 — aucune session jouée, en silence (Zelda France :
  ses trois films visent d'autres versions). deepeel le signale
  (`TAS solo: no movie loaded (N refused), random play until the time limit`)
  et garde la foule normale jusqu'à `--time-limit`.
- **`scan_all.py`** : deux révisions au même titre d'en-tête (`SUPER
  MARIOLAND` World et Rev A) partageaient le même répertoire de sortie ; le
  nom de fichier départage désormais (`unique_slugs`). Un film à égalité de
  score va à chacune ; l'identité de deepeel refuse la mauvaise révision.

Campagne `scan_all` bornée (300 s, `--jobs 7`), observations de l'outil :

| ROM | 0.22 | 0.24 | résidu |
|---|---|---|---|
| Super Mario Land (World) | 99,991 % | **100,0 %** | 6 → 0 o (0,0 % déduit) |
| Super Mario Land (Rev A) | — | 99,7 % | 170 o (films Rev A encore désynchronisés) |
| Zelda LA (France) | 91,88 % | 92,5 % | 42 590 → 39 579 o |
| Mystic Quest | 90,81 % | 90,8 % | 24 105 → 24 105 o |
| SML2 | 86,85 % | 89,3 % | 68 925 → 56 041 o |
| Pokémon Jaune | 80,30 % | 80,8 % | 206 592 → 201 161 o |

## Compatibilité TAS hors de m0boy, profil VBA 1.7.2, diagnostic Rev A (0.25.0)

**Séparation** : m0boy ne garde que les corrections matérielles (fidélité
DMG vérifiée contre SameBoy et Gambatte). Les réglages de synchronisation de
film (`gbl_sync_div_skew`, `gbl_sync_vba_off_clock`) ne sont pas du
matériel : ils vivent dans deepeel, `cmake/m0boy-tas-compat.patch`, appliqué
à une **copie** du runtime dans l'arbre de build (`cmake/dp_gbl_build.cmake`,
`DP_GBL_PATCH`). Si m0boy évolue au point que le patch ne s'applique plus, la
construction s'arrête en le disant.

**Profil VBA 1.7.2** (VBM de version mineure 0 : VBA 1.7.2 rerecording
jusqu'à VBA-rr v23.0, cœur « V7 ») :

- l'entrée de la trame k est lue à la k-ième vblank (pas à la mise sous
  tension, contrairement à v23.1+/v24 et BizHawk) ;
- aucune trame tant que le LCD est éteint (une toutes les 600 longueurs de
  trame au plus, pour que le crochet de trame de l'hôte tourne encore) ;
- au rallumage, la ligne 0 dure dix lignes en mode 0 (VBA charge la durée du
  mode 1), sans le front STAT LYC=0 immédiat.

Profil **partiel**, assumé : ces cœurs ont aussi des défauts CPU que deepeel
ne reproduit pas (`RES b,(HL)` en 12 cycles au lieu de 16, réveil de HALT sur
STAT/timer décalé, DIV à 0 au démarrage). Ils s'accumulent dans la phase de
DIV, et le jeu bonus de 1-3 tire `DIV & 3` : sur Super Mario Land Rev A,
TheAxeMan (rr 9), xenos (rr v19.2) et mugg-hard (v21) suivent désormais le
cœur V7 trame pour trame jusqu'au jeu bonus (puis 2-1) au lieu de mourir en
1-2, mais le prix tiré diffère (V7 lit $D2, m0boy $D3) et le film décroche.
Le cœur V7 lui-même ne synchronise pas xenos (enregistré sur une version
antérieure, introuvable à l'identique). Le film BizHawk « Small Only 100% »
est enregistré en mode GBC (matériel CGB non émulé).

Couverture Rev A inchangée (observation de l'outil) : 99,7 %, 170 octets du
contenu des mondes 3 et 4, que seul un film synchronisé jouerait.
Super Mario Land World : toujours 100,0 % (0,0 % déduit) avec le patch.

## Carte PNG compressée, 1 pixel par octet, chaleur d'usage pour m0boy (0.26.0)

- **PNG par défaut et compressé** : `--map P` écrit le PNG sauf si P finit
  par `.map` ou `.txt` (texte) ; sans extension, `.png` est ajouté. L'image
  passe par zlib (niveau 9, `compress2`) au lieu de blocs DEFLATE « stored »,
  et toutes les charges utiles vont dans des chunks **zTXt** (zlib), snapshots
  compris (`deepeel_snapshot`). Les anciens fichiers (stored, tEXt) restent
  lisibles. zlib devient une dépendance (`find_package(ZLIB)`).
- **1 pixel = 1 octet** : chaque banque est une tuile de 128×128 pixels (une
  ligne par 128 octets), colorée par la classe exacte de chaque octet — les
  vraies zones, plus de cellule de 64 octets à la classe dominante. À droite
  de chaque banque, un **panneau de chaleur** de même géométrie : usage joué,
  échelle log2, du violet sombre au jaune pâle (légende en bas).
- **Usage réel seulement** : `play_exec_hits` / `play_data_hits` comptent les
  instructions exécutées (octet d'opcode) et les octets ROM lus par les
  sessions jouées (TAS, aléatoire, attract, humain), jamais les chemins forcés
  du fork-explorer. Tous les compteurs par octet saturent à 2³²−1 au lieu de
  reboucler ; les plafonnements sont comptés (`hits_saturated`, rapport texte
  et JSON).
- **Statistiques par zone** (`src/report/heat.c`) : une zone = une ligne du
  `.map` ; octets, exécutions et lectures jouées, rang par usage, et
  **ensemble chaud** (les zones qui portent 90 % de l'usage). Sorties :
  top 20 dans le rapport texte, bloc `heat` du JSON et de `deepeel_meta`,
  chunks du PNG :
  - `deepeel_heat` : `bank start end class bytes exec reads rank hot`, toutes
    les zones ;
  - `deepeel_cov` : le `.cov` joué (hits 32 bits des sessions), débuts
    d'instruction seulement, plages de données prouvées en une ligne avec une
    5ᵉ colonne de longueur ;
  - `deepeel_rom` : lectures jouées par page de 256 octets (format
    `--rom-profile` de m0boy).
- `--cov` garde son format (hits désormais 32 bits, plus plafonnés à 0xFFFF).
- **m0boy** lit ce PNG : `gbrecomp --coverage x.map.png` prend `deepeel_cov`
  (classement du budget de flash par les comptes joués) et, sans
  `--rom-profile`, `deepeel_rom` pour choisir les blocs ROM résidents.
- **m0boy lit aussi le `.cov` directement** (interface de consommation, axe 9
  du plan) : `--coverage campagne.cov` — le format « gbl coverage v1 » de
  deepeel est le format natif du recompilateur (`bank addr flags(1=exec,
  2=entry,4=speculative,8=data) hits`, plages de données prouvées avec une
  5ᵉ colonne de longueur). Une campagne deepeel complète pilote donc la
  recompilation sans découverte propre côté m0boy. Mesure bout-en-bout
  (Super Mario Land World, `.cov` de la campagne TAS deepeel, 42 706
  enregistrements) : 6 963 instructions compilées en 74 régions,
  **interpreter-only 0**, 21 125 enregistrements de données prouvées laissés
  en données — la recompilation sans interpréteur tient déjà sur une
  couverture partielle ; ce qui reste à 100 % est la couverture elle-même
  (axe 10), pas le repli.

Super Mario Land World (TAS muggv3) : PNG de 146 Ko contre 155 Ko pour
l'ancien format, qui ne portait qu'une image par cellules de 64 octets et le
`.map` (image + carte : 26 Ko au lieu de 155 Ko) ; l'ensemble chaud tient en
23 zones, 7 241 octets (moteur en banque 0, banque 3). Observations de
l'outil, pas des mesures de matériel.

## Correctifs J0 (0.29.0)

Les correctifs relevés par la norme (`docs/COUVERTURE.fr.md` §10.12) :

- **Reprise du spool** : le curseur de lecture est sauvé à côté du spool
  (`spool.bin.rd`). Un `--resume` relit maintenant tous les enregistrements
  encore non lus ; avant, il repartait du bout du fichier et ne relisait
  rien. Sans annexe valide, la relecture repart du début et le signale sur
  stderr. `--resume` sans spool crée le spool au lieu d'échouer.
- **Table de fourches pleine** : quand la table des compteurs par site est
  pleine, la fourche est refusée et comptée. Nouveau compteur
  `forks_table_full` dans le texte et le JSON, inclus dans `forks_capped`.
  Avant, une table pleine levait le plafond par site.
- **`--max-paths`** : l'état qui atteint le mur reste en file au lieu d'être
  libéré, et il est compté dans `budget_paths`.
- **Contexte VRAM** : le hash de contexte ne lit plus que la banque VRAM
  réelle du runtime DMG.
- **Provenance** : les cibles de saut sont ramenées sur la banque de leur
  fenêtre. Une cible au-delà de $8000, ou dans une banque inconnue, est
  comptée `prov_unresolved` au lieu d'être indexée de travers. Le budget de
  pas est désormais par appel, et non plus cumulé sur les rondes.
- **Options** : `--no-provenance` et `--no-siblings` coupent chacune leur
  passe. `--no-closure` ne coupe plus que la fermeture.
- **Graines d'IRQ** : les vecteurs RST et IRQ sont semés comme forcés, et non
  plus comme flux naturel.
- **Registre** : VBK, SVBK et HDMA1-5 ne sont plus marqués `modelled`. Le
  runtime est DMG (voir §Limites).
- **Autres plateformes**, même jalon :
  - Sega : `tree.bin` passe en version 2 et porte enfin le nombre de nœuds,
    que la version 1 écrasait ; `--resume` sans spool démarre à neuf ;
    `seeds_refused_cap` ne compte plus les doublons.
  - NES : un site de fourche n'use son budget que si l'état a bien été mis en
    file. Nouveau compteur `pending dropped` pour les états restés en file au
    mur. Les champs d'options morts `seed_banks` et `entry_*` sont retirés.
  - PCE : nouveau compteur `pending dropped`, sur le même principe.

## Modèle de couverture commun (0.30.0, J1)

Chaque octet de ROM porte désormais, en plus des drapeaux historiques, un
masque de **sources** (`DP_SRC_*`, OR-ées, jamais de « gagnante ») :
session jouée, fork naturel, fork forcé, fermeture statique, provenance,
scan statique, copie, inférence de voisin. La classe de carte est une
projection de la nature (`dp_nature_class`, `src/core/coverage.h`) : les
cartes sont identiques à celles d'avant (tests `coverage` et `sega_coverage`).
Le rapport JSON expose `"sources"` (octets par source, `bytes_any`,
`bytes_multi`) pour GB et pour Sega/MSX. Différé : la table creuse
`(source, horodatage, id film/worker)` et la projection `types`.
Ces nombres sont des observations de l'outil, pas des mesures de matériel.

## Frontière différée, palier `steps` (0.31.0, J2 partiel)

Un chemin coupé par `--max-steps` par chemin n'est plus perdu : son état
machine est conservé (4096 reports au plus par explorateur) puis repris quand
la file est vide, avec un budget ×4 (3 paliers). Au-delà, la coupe reste
définitive mais comptée (`steps_final`) ; une frontière pleine est comptée
(`defer_dropped`). Rapporté dans `counters` : `steps_deferred`,
`steps_resumed`, `steps_final`, `defer_dropped`. La reprise repart avec des
transitoires remis à zéro (glissade de padding, tampon d'arrière-branche) :
elle peut donc aller un peu plus loin qu'un run sans budget, jamais moins
(test `explorer`). Différé : frontière disque, autres paliers, Sega.

Palier `loop` (0.32.0) : la coupe d'arrière-branche (`cycle_cuts`) est
désormais aussi reportée, avec un cap ×4 par palier. Ce qu'un mur global
(`--max-total`) laisse non repris est compté dans `defer_pending`.

Sega/MSX (0.33.0) : mêmes paliers `steps` et `loop` (option `--defer-tiers N`,
défaut CLI 3, défaut bibliothèque 0 : les coupes y restent définitives). Une
continuation reportée n'est jamais évincée par contexte (même chemin, en
milieu de section) ; elle apparaît dans le résumé (`frontier:`) et dans le
JSON (`steps_deferred`, `loop_deferred`, `defer_resumed`, `*_final`,
`defer_dropped`, `defer_pending`). Même correctif côté GB.

Sega/MSX (0.249.89, mutualisation §7.4) : l'admission des continuations
reportées passe par la **politique partagée** `src/explore/frontier.c`
(`dp_frontier_room`, raisons `DP_R_*` de COUVERTURE §6) au lieu d'une
réimplémentation locale : plafond de paliers `DP_TIER_MAX` commun, et les
reports de fourches (`DP_R_FORK_CAP`, raison bruyante) ne peuvent plus
occuper plus d'un quart de la zone d'attente (1024 des 4096) — ils ne
affament plus les reports `steps`/`loop`. La raison suit la continuation
dans le spool disque (bits 16-23 de l'en-tête) pour garder la comptabilité
par raison après une reprise `--resume`. Compteurs de rapport inchangés.

## J1 terminé (0.34.0)

Table creuse de première découverte (`DpFirstLog`, `coverage.h`) : une ligne
`(clé d'octet, source, horodatage en pas, acteur)` la première fois qu'un octet
de ROM gagne une source ; bornée à 2^20 lignes (au-delà : `dropped`). Exposée
dans le JSON (`first_seen`, lignes par source) pour GB et Sega ; acteur 1 =
résultat d'un worker fusionné (Sega). Projection `types` (GB, JSON `types`) :
start, entry, irq, verbatim. Les types sémantiques (tiles, music…) sont J8.

## Frontière sur disque (0.35.0)

À la sortie avec `--state-dir`, les continuations différées en mémoire sont
écrites dans `<state-dir>/frontier/frontier.dpf` (en-tête `'DPFB'`, un
enregistrement par continuation : palier, forced, kind, from_reset, image
zlib) avec un résumé `frontier.idx`. `--resume` les recharge avec leur palier
(elles ne sont alors plus comptées en `defer_pending`). `deepeel frontier stats
<state-dir>` affiche le résumé. **Pas encore fait** (J2 reste ◐) : lots par
banque/section, deltas XOR, lecture séquentielle à position persistée,
`frontier drain`, paliers `priority`/`dedup-full`/`paths`/`fork-cap`/`covered`/
`wall`, NES/PCE/MD. Le contexte dur complet par enregistrement n'est pas
persisté : le chemin repris repart avec transitoires à zéro (peut couvrir un
peu plus, jamais moins).

## frontier drain et critère J2 (0.36.0)

`deepeel frontier drain <rom> <state-dir> [--tier N] [--time-limit S]` reprend
les continuations sauvegardées (équivaut à `explore --resume` sur le même
`state-dir`) ; `--tier N` ne reprend que les paliers ≤ N, les autres restent
tenues. Le test `test_explorer` vérifie le critère : un run à budget minuscule
puis le rechargement de la frontière ne perd aucun code par rapport au run sans
budget. Le message de spool indisponible dit maintenant « refused and counted ».
Reste différé : lots par banque/section et deltas, paliers `priority`,
`dedup-full`, `paths`, `fork-cap`, `covered`, `wall` en frontière, NES/PCE/MD.

## Frontière complète (0.37.0)

Toutes les raisons de perte GB passent par la frontière différée : un état
qu'un budget abandonnerait est conservé (palier +1, budgets ×4, 3 paliers max).
Ordre de reprise : `priority < dedup-full < paths < fork-cap < steps < loop <
wall < covered` ; `covered` n'est repris que par `frontier drain --tier inf`.

- Disque : lots `b<banque>-s<section>-<raison>-<n>.dpf` (base zlib + deltas XOR,
  clos à 64 Mio), index `frontier.idx` avec la position de lecture, graines
  retenues dans `seeds.bin`.
- `deepeel frontier stats <dir>` : enregistrements en attente par raison,
  palier et banque. `frontier drain <rom> <dir> [--tier N|inf]` reprend.
- Rapport JSON : bloc `frontier` (`deferred`, `final`, `seeds_*`).
- Un site de bifurcation ne parque que `2*fork_site_cap` variantes ; au-delà la
  perte est finale et comptée.

Limites : Sega/MSX ne couvrent que `steps`/`loop` ; NES/PCE/MD n'ont pas
encore de frontière disque. Chiffres = observations de l'outil.

## 0.38.0

- `--frontier-cap-gb N` : plafond disque des lots ; au-delà, la perte est finale
  et comptée par raison (`frontier.final`).
- `frontier drain` ne réamorce plus la racine : seules les continuations
  sauvegardées tournent. Le dossier d'état est créé s'il manque.
- Un spool plein au flush reporte l'état dans la frontière au lieu de le jeter.
- JSON : `frontier.inbox_dropped` (inbox de sessions pleine).
- Sega : `fork_deferred`/`fork_final` (plafond de chemins, file pleine).
- Les anciens spools `DPSR` restent lus tels quels (pas de conversion nécessaire).

## 0.39.0

- NES / PCE / MD : une file pleine ne jette plus l'état, il attend dans une zone
  bornée (4096) et reprend quand la file est vide ; compteurs `parked`,
  `parked_resumed`, `parked_lost` dans le rapport texte.
- Sega/MSX : les reports encore en attente en fin de run sont écrits dans le
  spool (`defer_spooled`).

## 0.40.0

- Pool parallèle : si le spool est plein, le worker reporte l'état dans sa zone (fusionnée dans celle du chef à la jointure) au lieu de le perdre ; `queue_overflows` reste compté.
- `--spool-to-frontier` (avec `--resume --state-dir`) convertit un spool `DPSR` en lots `.dpf` de raison `paths`.
- Tests : `frontier_cli` (spool → conversion → `frontier stats` → `drain`), Sega « report en attente au mur ».
- Observations de l'outil, pas des mesures matériel.

## 0.41.0

- `--lot FICHIER` pour NES, PCE, MD et Sega/MSX : lot disque portable (`src/explore/lots.h`) des états différés. Trop-plein de la zone parquée et restes d'un mur y sont écrits, relus quand la file se vide ; une relance avec le même fichier reprend le reliquat. Sega : import dans le spool au départ, export du spool non lu à la fin.
- Compteurs `lot_written`, `lot_read`, `lot_pending` (rapports texte NES/PCE/MD). Observations de l'outil, pas des mesures matériel.
- `frontier stats FICHIER.dpf` lit un lot portable ; test du pool parallèle avec spool plein (reports des workers fusionnés dans le chef).

## 0.42.0

- Les lots portables suivent le format §6.3 : en-tête `DPFB` v2 (plateforme, taille d'image, image de base zlib), enregistrements `DPLL` (métadonnées + delta XOR zlib), curseur de lecture FIFO, compactage à la fermeture, fichier supprimé une fois vide.
- `frontier drain ROM LOT [--sys X]` vide aussi un lot NES/PCE/MD/Sega : seuls les états sauvés sont rejoués (ni racine, ni graines). Équivalent plateforme : `--lot FICHIER --lot-drain`.
- PCE verse l'état décompressé et le recompresse au rechargement (taille variable, incompatible avec un delta XOR fixe).
- Tests : drain MD (`test_md_explore`), drain Sega (`frontier_cli`). Observations de l'outil, pas des mesures matériel.
- Les lots portables s'écrivent en `.dpf` (même magie `DPFB` que les lots GB) ; tests de drain NES/PCE ajoutés.

## 0.43.0

- J3 (début) : `src/core/dpmachine.h` définit `DpIsa` et `DpMachine`. Le SM83 et le backend GB (m0boy) les implémentent ; la closure GB classe le flot des instructions par l'ISA. Pas de changement de rapport attendu ; test `dpmachine`. Le reste de J3 (extraction du noyau) est ouvert.

## 0.43.1

- J3 : la politique de la frontière différée (raisons, paliers, plafonds, pertes finales comptées) vit dans `src/explore/frontier.{h,c}` ; `explorer.c` ne fait plus que l'appliquer. Aucun changement de rapport.

## 0.44.0

- J3 : `DpIsa` complet pour le SM83 (condition, effets de registres, illégal). Le moteur GB, la closure et la provenance passent par l'ISA ; la provenance est générique (flot + `regs_effect` sur un treillis paire-pointeur/accumulateur). Carte identique à 0.43.1 sur Zelda à budget égal.

## 0.45.0

Fin de J3 : `DpIsa.mem_operand` (opérande mémoire du SM83), `DpMachine.observe`,
`ctx_components` et `probes` pour la GB, et découpage de `explorer.c`
(`forks.c`, `seeds.c`, `ctxproj.c`, `frontier_disk.c`, `explorer_priv.h`).
Aucun changement de comportement : carte de Zelda (3 M pas) identique octet
pour octet à 0.43.1. Observation de l'outil, pas une mesure matérielle.

## 0.46.0

J4 (début) : `--jobs 2` sur MSX. Le runtime MSX est recompilé une seconde fois
(`dp_msx_backend2`, symboles préfixés `dp_msx2_`) ; l'explorateur Sega/MSX lit
l'état de mapper, l'observateur et le test de trappe BIOS via le backend
(`msx_info`/`msx_observe`/`msx_bios_trap`) au lieu de singletons. Le repli
mono-cœur `jobs_capped` n'existe plus pour MSX ; `test_sega_jobs` compare le
parcours à 2 instances au parcours mono-cœur.

## 0.47.0

J4 : coupe de non-sens Sega/MSX (D13). Un chemin qui glisse dans une plage de
8 octets identiques `$00`/`$FF` de la ROM est coupé et compté (`pad_cuts`,
JSON `counters` et ligne `cuts:` du texte), comme `is_padding_at` côté GB. Le
Z80 n'a pas d'opcode illégal : seul le décodage impossible (`decode_cuts`) et
le padding restent à couper.

## 0.48.0

J4 : `deepeel z80 explore … --cov F --romdata F`. Le moteur Sega/MSX compte
maintenant les passages en exécution et les lectures de données par octet de
ROM (`exec_hits`/`data_hits`, saturants, sommés à la fusion des workers).
`.cov` : `bank offset flags hits` en banques physiques de 16 Kio (flags 1 =
exécuté, 4 = spéculatif, 8 = donnée ; l'exécution l'emporte) — ce n'est pas le
format `gbl coverage v1` de m0boy, qui suppose les adresses CPU de la GB.
`--romdata` : `page count` par page de 256 octets, comme côté GB. Observations
de l'outil.

## 0.49.0

J4 : identité ROM des films pour Sega/MSX. `--tas` lit la ligne `SHA1` de
l'en-tête du `.bk2` et la compare au SHA-1 de la ROM : une ROM différente ou
un film sans identité est signalé sur stderr (le rejeu continue, au mieux) et
le verdict est consigné dans le résumé JSON (`tas_identity` : 1 même ROM,
2 ROM différente, 3 identité absente). Observation de l'outil, pas une preuve
de synchronisation du film.

## 0.50.0

J4 : frères de copie (S5) pour SMS/GG/MSX. Une copie ROM→VRAM/RAM verbatim d'au
moins 16 octets dont l'adresse source (CPU) figure comme mot dans une table lue
(banque 0 ou banque de la copie) marque les autres entrées de la table avec la
même longueur, classe `inferred-data` (jamais prioritaire sur une observation).
Hypothèse : les entrées pointent dans la banque de la copie (fenêtre 0 = banque
fixe). Bornes : 64 entrées par table (`sib_caps`), plage coupée à la banque de
16 Kio, plage sur du code exécuté naturellement refusée (`sib_refused`) ;
compteurs `sib_tables/ranges/bytes` dans le résumé JSON et le rapport texte.
La fusion `--jobs 2` fait maintenant aussi l'OU des drapeaux statiques.
Observations de l'outil, pas des mesures de matériel.

## 0.51.0

J4 : tables de sauts (S4) pour SMS/GG/MSX. Le Z80 n'a pas de saut indirect
indexé : l'opérande est remplacé par la **provenance** — sur un `jp (hl)/(ix)/(iy)`
observé, si les deux octets de la paire viennent d'octets ROM consécutifs dont
le mot vaut la cible, on connaît le mot de table. La table s'étend des deux
côtés tant que les mots pointent sur du code exécuté de la même fenêtre
(fenêtre 0 = banque fixe), 64 entrées au plus (`jt_caps`). Les octets sont
marqués `table` (source provenance) ; compteurs `jt_tables/entries/bytes` dans
le résumé JSON et le rapport texte. Limite assumée : seules les entrées déjà
exécutées sont admises, une table dont une seule entrée a été prise n'est pas
reconnue. Observations de l'outil.

## 0.52.0

J4 : chien de garde HALT (D14) pour SMS/GG/MSX. Un HALT que rien ne réveille
tourne dans le plafond de boucle (`--loop-cap`) puis la branche est coupée ;
la coupe est désormais **attribuée** : `halt_cuts` (inclus dans `loop_cuts`),
dans le résumé JSON et la ligne `cuts:` du rapport. Pas de seuil de vblanks
comme sur GB : le plafond de visites par site borne déjà le HALT, et les
réveils par IRQ sont explorés par les fourches d'IRQ. Observation de l'outil.


## 0.53.0

J5 (partiel) : le pont NES (`src/mach/nesmach.c`) gère désormais CNROM
(mapper 3 : écriture en $8000+ = banque CHR 8 Kio) et AxROM (mapper 7 :
banque PRG de 32 Kio) en plus de NROM/UxROM. Le registre est ajouté à
l'image d'état (4 octets) et au hash d'état exact ; `rom_index` suit les
fenêtres réelles (`bank_base`/`fixed_base`), toujours en tranches de
16 Kio. Le journal des banques note un changement de banque CHR
(fenêtre 1). Les cartouches MMC1 et MMC3 restent refusées avec un
message : le runtime m0boy n'a que deux fenêtres PRG de 16 Kio et un CHR
plat de 8 Kio, il faut l'étendre (dépôt voisin). Observation de l'outil.

## 0.54.0

J5 (suite) : CHR ROM indexée côté NES. Le rapport gagne une ligne `chr:`
(taille, tiles de fond et de sprites référencées, lectures `$2007`). Les
tiles sont déduites à chaque fin de frame de l'état du PPU (nametables
quand le fond est activé, OAM quand les sprites le sont, pages de
`$2000`), sans modifier le runtime m0boy ; la clé est
`banque CHR * 8 Kio + adresse de motif`, donc stable sous CNROM. Les
cartouches CHR-RAM n'ont pas de carte CHR. Observation de l'outil : une
tile affichée à un instant donné, pas une mesure du matériel.

## 0.55.0

J5 (suite) : MMC1 (mapper 1) et MMC3 (mapper 4) au pont NES. Les fenêtres
sont composées côté deepeel : tranches de 8 Kio pour la PRG, de 1 Kio pour
la CHR ; `rom_index` et la CHR indexée suivent ces tranches (clé =
décalage linéaire dans la ROM). Registres : port série MMC1 (modes PRG
16/32 Kio, CHR 4/8 Kio), paires MMC3 (R0–R7, inversions PRG/CHR). Le
journal des banques distingue la fenêtre PRG $8000 (0), $C000 (1) et la
CHR (2). Le runtime m0boy gagne 8 Kio de PRG-RAM en $6000-$7FFF (modif
locale du dépôt voisin, non poussée). **Non modélisés** : le compteur
d'IRQ de scanline MMC3 (`$C000-$E001`) et le miroir MMC1/MMC3 ; les IRQ
MMC3 ne sont donc pas explorées. Observation de l'outil.

## 0.56.0

J6 : PC Engine.
- **`--jobs 2`** (`deepeel pce explore ... --jobs 2`) : le runtime m0boy, le
  pont et l'explorateur étant des globales de processus, la pile PCE est
  compilée une seconde fois et préfixée `dp_pce2_` (objcopy, comme les
  clones Sega/MSX). Le partage est **par phase** : l'instance secondaire
  prend TAS, sessions jouées et recherche de nouveauté, puis vide les
  points de contrôle qu'elles laissent ; la primaire vide l'arbre de
  fourches du démarrage puis lance la finalisation (clôture statique,
  tables, sections, inférence) sur l'**union** des cartes. Les compteurs
  sont sommés, les journaux fusionnés (dédoublonnés, débordements
  comptés). Sans phase à répartir, le mode retombe sur une instance, avec
  un message. Les budgets (`--max-total`) sont par instance.
- **VCE** : une copie ROM→port de couleur `$0404-$0405` (store CPU ou
  déplacement de bloc) est typée **palette** : `pal_flags` par cellule,
  canaux de copie 3 et 4, ligne `VCE colour-data writes … palette bytes`
  dans le rapport. Observation de l'outil.

## 0.57.0

J7 (partiel) : Mega Drive, observation du Z80 son.
- Le Z80 de m0boy tournait déjà mais n'était pas observé. Des crochets sont
  injectés à la construction dans une copie générée de `md_full_audio.c`
  (même technique que le DMA) : avant/après chaque pas Z80, lectures de ROM
  par la fenêtre de banque `$8000`, tout store sur le bus son (RAM Z80,
  YM2612, PSG).
- Nouveau tableau `z80_flags` (un octet par octet de ROM) : `COPIED` (copié
  par le 68000 dans la RAM Z80), `CODE_RAM` (…puis exécuté par le Z80 :
  **code-ramcopy**), `EXEC_ROM`, `DATA` (lu par la fenêtre de banque, hors
  fetch), `SAMPLE` (octet lu puis écrit sur le DAC YM2612, registre $2A),
  `MUSIC` (écrit sur un autre registre YM2612 ou le PSG). Rapport : bloc
  « z80 sound cpu », et lignes `z80-*` dans le fichier d'évidences.
- Limites assumées : le Z80 s'exécute **concrètement** (pas de fourche sur
  ses conditionnels, il avance avec l'horloge du 68000 et n'entre pas dans le
  budget de pas) ; la provenance d'un octet de RAM Z80 (`zsrc`) est globale à
  l'exploration, pas restaurée par les fourches ; la copie 68000→RAM Z80 est
  reconnue par égalité de valeur avec le dernier octet de ROM lu (preuve
  heuristique) ; la classe n'est pas encore dans la carte PNG. Observations
  de l'outil, pas des mesures de matériel.
- Test : `fix_md_z80` (68000 copie un programme Z80 de 13 octets, le Z80 lit
  `$8320` et l'écrit sur le DAC).

## 0.58.0

J8 (partiel) : inférence par index (§5), tranche 6502/HuC6280.
- Les lectures `lda/ldx/ldy/adc/sbc/and/ora/eor/cmp abs,X|Y` ont une base
  **prouvée** (l'opérande) et un index borné à 8 bits. Par site, l'outil
  consigne `base`, `idx_min`, `idx_max`, `n_obs` (cap 256 sites, débordement
  compté).
- À la finalisation, les octets **non observés** entre `idx_min` et `idx_max`
  sont déclarés `idx_flags` (« index-inference »), sans jamais :
  descendre sous `idx_min` (base biaisée `tbl-1` : l'élément avant la table
  n'est pas déclaré, le site est compté `biased`), dépasser `idx_max`
  (aucune borne haute prouvée n'est encore exploitée), couvrir du code
  (`refused over code`), ni franchir la page de 8 Kio de la base.
- Les lectures des chemins **forcés** sont ignorées pour la plage (une
  fourche forcée peut boucler sur tout l'index) et comptées.
- `--jobs 2` fusionne les sites des deux instances.
- Limites : seulement PCE ; pas de rejeu par index (§5.4), pas de
  provenance « base + index » sur le treillis, pas de classe dans la
  carte PNG. Test : `test_index` (table à trous, table biaisée).

## 0.59.0

J9 (partiel) : deux surfaces matérielles de plus.
- **NES, DMA du canal DMC** : `$4012` (adresse `$C000 + A·64`, repli sur
  `$8000`) et `$4013` (longueur `L·16+1`) sont suivis ; une écriture
  `$4015` bit 4 consigne une copie (canal 2) et marque les octets de ROM
  `DP_NES_SAMPLE` (type samples). Rapport : « DMC DMA starts … distinct
  sample bytes from ROM ». Test `test_dmc`.
- **GB, HDMA CGB** : l'exécution reste DMG (rien n'est émulé), mais les
  registres `$FF51-$FF54` donnent source et destination et `$FF55` la
  longueur : une copie ROM→VRAM (canal 2, `hdma`, verbatim) est consignée et
  la source marquée donnée. Un transfert à cheval sur deux fenêtres est
  coupé à la fenêtre. Fixture `fix_hdma`.
- Observations de l'outil sur les registres écrits, pas des mesures de
  matériel.

## 0.60.0

J9 (partiel) : typage APU des données ROM sur Game Boy. Un octet de ROM lu
puis écrit tel quel dans la wave RAM (`$FF30-$FF3F`) est marqué `sample`
(`DP_DF_SAMPLE`) ; écrit dans un registre APU (`$FF10-$FF25`), `music`
(`DP_DF_MUSIC`). L'attribution est par égalité de valeur avec la dernière
lecture de ROM (heuristique : une coïncidence de valeur peut sur-marquer).
Rapport : ligne « apu typing ». Fixture `fix_apu`. Observation de l'outil,
pas une mesure de matériel.

## 0.61.0

J9 (partiel) : sondes événementielles Game Boy. Sont comptés, par site de
code distinct (cap 16 chacun) : les démarrages de transfert série
(`$FF02` bit 7) et les sélections de registre RTC MBC3 (écriture `$4000-$5FFF`
de la valeur 8 à `$C`). Rapport : ligne « probes ». Fixture `fix_probes`.
Les registres ne sont pas émulés : c'est l'observation des écritures, pas du
comportement matériel.

## 0.62.0

J9 (partiel) : sondes de lecture temporelles Game Boy. Les sites de code
distincts (cap 16 chacun) qui lisent `DIV` (`$FF04`), `STAT` (`$FF41`) et `LY`
(`$FF44`) sont comptés : ce sont les boucles d'attente de synchronisation.
Rapport : ligne « timing probes ». Fixture `fix_probes` étendue. Observation
de l'outil sur les lectures, pas une mesure de matériel.

## 0.63.0

J9 (partiel) : typage palette CGB. Un octet de ROM lu puis écrit tel quel au
port de données de palette `BCPD` (`$FF69`) ou `OCPD` (`$FF6B`) est marqué
`DP_DF_PALETTE`. Même heuristique d'égalité de valeur que le typage APU
(sur-marquage possible sur coïncidence). Rapport : ligne « cgb typing ».
Fixture `fix_pal`. Observation de l'outil ; l'exécution reste DMG, les ports
ne sont pas émulés.

## 0.64.0

J10 (partiel) : PNG de carte adaptatif et vecteur de classes `dpCv`.
- **Échelle adaptative** : au-delà d'un budget de pixels (16 Mpx par défaut,
  `dp_map_png_budget`), un pixel couvre 2, 4… jusqu'à 128 octets. La classe
  affichée est la dominante du groupe (à égalité, une classe connue l'emporte),
  la chaleur est celle de l'octet le plus chaud. Le pied de l'image indique
  « 1 px = N o ». Sous le budget, le rendu reste exact (1 px = 1 o) : les
  cartes existantes ne changent pas.
- **`dpCv`** : mot-clé zTXt contenant le vecteur de classes complet de la ROM,
  codé en séquences (classe, longueur LEB128). Il rend le PNG sans perte même
  quand le rendu est réduit ; décodage `dp_map_cv_decode` (refuse une taille
  inexacte).
- Test `map_png` : ROM synthétique de 32 Mio (2048 banques) rendue sous le
  budget, `dpCv` relu identique octet pour octet. Ce sont des observations de
  l'outil.
- **Reste pour J10** : segments `dpFr`/`dpIx`/`dpTb`, rendu en flux (le tampon
  RGB est encore alloué en entier), `maprender` relisant `dpCv`.

## 0.65.0

J10 (partiel) : `deepeel maprender rom carte.map.png out.png` accepte aussi un
PNG de carte en entrée : son vecteur `dpCv` est décodé (taille vérifiée contre
la ROM) et re-rendu, donc une carte réduite par le budget de pixels se
re-rend sans perte. Un PNG sans `dpCv` ou de taille incohérente est refusé.
Test `maprender_cv` (aller-retour `dpCv` identique). Reste pour J10 : `dpFr`,
`dpIx`, `dpTb`, rendu en flux.

## 0.66.0

J10 (partiel) : mot-clé `dpFr` dans le PNG de carte : JSON du résumé de
frontière (différés et coupures définitives par raison, repris, en attente,
sur disque, abandonnés). Test dans `explorer`.

Écart assumé avec COUVERTURE §9.3 : `dpCv` est aujourd'hui le **vecteur de
classes** (RLE), pas encore le format binaire complet (nature, types, sources,
hits par octet), et les segments sont des mots-clés zTXt, pas des chunks PNG
privés. Reste pour J10 : `dpIx`, `dpTb`, `dpCv` complet, rendu en flux.

## 0.67.0

J11 (partiel) : backend libretro niveau A (`src/mach/libretro.[ch]`, §8.1).
- `dlopen` d'un cœur non modifié (`RTLD_LOCAL`), API publique seulement :
  `retro_run` (avance), `retro_serialize`/`unserialize` (état), entrées
  joypad, mémoire (`SYSTEM_RAM`, `SAVE_RAM`, `VIDEO_RAM`), framebuffer.
- Hachages d'état (`dp_lr_state_hash` : état sérialisé complet, horloges
  comprises ; `dp_lr_ram_hash` : SYSTEM_RAM seule) et rejeu TAS
  (`dp_lr_replay` : un masque par image, hachage RAM par image).
- **Dégradations rapportées** (`dp_lr_degradations`) : ni pas à pas, ni
  fourche forcée D7-D11, ni observateurs de bus, ni couverture ROM précise ;
  les memory maps déclarées par le cœur sont signalées, pas décodées.
- Test `libretro` sur un **cœur factice construit par les tests**
  (`tests/mock_libretro.c`, aucun binaire dans le dépôt) : chargement, entrées,
  aller-retour de sérialisation, rejeu TAS déterministe, framebuffer, rapport
  de dégradations.
- Non fait : branchement sur les sessions/TAS de l'explorateur, Go-Explore,
  mode humain/`--watch`, multi-instances par copie du `.so` (§8.3), essai sur
  un vrai cœur chargé hors dépôt. Les cœurs sont des singletons : une
  instance active par fil.

## 0.68.0

J11 (partiel) : multi-instances libretro (§8.3). `dp_lr_open_copy` copie le
`.so` dans un fichier temporaire, le `dlopen` en `RTLD_LOCAL` puis le délie :
chaque instance a ses propres globales, donc son propre état (les cœurs sont
des singletons par bibliothèque). Test `libretro` : deux instances du cœur
factice, entrées et images indépendantes. Reste pour J11 : branchement sur
les sessions de l'explorateur, vrai cœur hors dépôt, Go-Explore/mode
humain/`--watch`.

## 0.69.0

J12 (partiel) : libretro niveau B (§8.2), côté outil.
- **ABI de l'extension de trace** (`src/mach/libretro.h`) : appel
  d'environnement privé `DP_LR_ENV_TRACE` (`0x7DEE0001`) par lequel un cœur
  remplit `DpLrTraceApi` (`step`, `set_observers`, `pc`, `force_branch`,
  `rom_index`, version 1). Un cœur sans l'extension refuse l'appel : le
  backend reste au niveau A, et `dp_lr_degradations` le dit.
- Le backend expose `dp_lr_level`, `dp_lr_step`, `dp_lr_pc`,
  `dp_lr_force_branch`, `dp_lr_rom_index`, `dp_lr_observe` (exec/read/write).
- Test `libretro` : le cœur factice implémente l'extension avec un petit CPU ;
  la fourche (les deux issues d'un conditionnel par `force_branch`) donne des
  chemins d'exécution et d'écriture différents, observés par callbacks ;
  chaque instance est chargée par copie du `.so`.
- Pas encore fait : callbacks `dma`/`bank`, le patch d'un vrai cœur (hors
  dépôt), le branchement sur l'explorateur à fourches et la **parité de
  fourches avec le backend m0boy** sur une fixture (critère J12).

## 0.70.0

J13 (partiel) : campagne multi-plateforme sur fixtures générées.
`tools/campaign_fixtures.py BUILD OUT.md` lance l'outil sur chaque fixture
(Game Boy, Mega Drive, Master System) et écrit `docs/CAMPAGNE.fr.md` : un
tableau par fixture (code retour, pas explorés, octets connus, durée) et une
synthèse par plateforme. Observations de l'outil, pas des mesures de matériel,
et pas des ROMs réelles (aucune dans le dépôt). Limites : NES et PC Engine
n'ont pas de fixtures fichier (leurs ROMs sont construites en mémoire par
`test_nes`/`test_pce`) ; certaines valeurs sont `n/d` quand le rapport texte
de la plateforme ne les expose pas. L'audit de la matrice et la campagne sur
ROMs réelles hors dépôt (`tools/scan_all.py`) restent à faire.

## 0.71.0

J12 (partiel) : l'extension de trace libretro porte aussi les callbacks `dma`
(copie matérielle : source, destination, longueur) et `bank` (bascule de
banque : fenêtre, banque) dans `DpLrObservers`. Le cœur factice les émet et le
test `libretro` les vérifie. Reste pour J12 : patch d'un vrai cœur hors dépôt,
branchement sur l'explorateur, parité de fourches avec le backend m0boy.

## 0.72.0

J12 (partiel) : exploration à fourches sur un cœur libretro de niveau B.
- Nouveau callback `branch` (un conditionnel est tranché à `pc`) dans
  `DpLrObservers`.
- `dp_lr_explore` : à chaque conditionnel, l'état d'avant le pas est
  sauvegardé (serialize) puis les deux issues sont mises en file avec
  `force_branch(0/1)` ; déduplication des items de fourche par hash d'état.
  Bornes explicites avec compteurs de saturation rapportés : pas par chemin
  (`cut_path_steps`), mur global (`cut_total`), file pleine (`dropped_items`).
  Résultat : carte d'exécution par index ROM compact, sites de branchement,
  fourches, chemins.
- Test : sur le cœur factice, les deux côtés du conditionnel sont couverts
  (le chemin naturel seul manque 8 et 10) ; une file minuscule et un mur de
  pas minuscule remontent leur saturation.
- Reste : exécution sur un vrai cœur, intégration à l'explorateur existant
  (rapport, cartes PNG), parité de fourches avec le backend m0boy.

## 0.73.0

J12 (partiel) : commande `deepeel lr explore CORE ROM [--text F] [--map F]
[--path-steps N] [--max-total N] [--queue N]`. Elle charge un cœur libretro
(copie du `.so`), lance `dp_lr_explore` et écrit un rapport texte (pas,
chemins, fourches, sites de branchement, octets de début de code atteints,
saturation) et une carte par plages sur les index ROM compacts. Code retour :
0 exploration complète, 3 saturation (file pleine, mur de pas, budget de
chemin), 1 cœur sans extension de trace (le niveau A ne peut pas fourcher) ou
erreur. Test `lr_explore_cli` sur le cœur factice. La bibliothèque
`deepeel_libretro` est maintenant liée au binaire principal (`libdl`).
Observations de l'outil ; le rapport ne fait pas encore de carte PNG.

## 0.74.0

J12 (partiel) : `deepeel lr explore` accepte `--png FILE` : carte PNG des
octets de début d'opcode atteints (classe `code-natural`, le reste `unknown`),
produite par le moteur de carte existant à partir d'une carte texte au format
« banque début fin classe » (banques de 16 KiB). `--map` écrit désormais ce
même format. Seuls les débuts d'opcode sont marqués : l'extension de trace ne
donne pas la longueur des instructions, donc la carte est creuse par
construction. Test : signature PNG vérifiée dans `lr_explore_cli`.

## 0.75.0

J12 (partiel) : **parité de fourches** entre le backend m0boy de l'explorateur
et le pilote de fourches libretro de niveau B.
- `dp_lr_open_native` : backend de niveau B en processus (pas de `.so`), dont
  la table de trace et les images d'état viennent de l'appelant.
- `tests/test_lr_parity.c` : la machine GB m0boy est exposée derrière l'ABI de
  trace (un pas = une instruction, `force_branch` retourne la condition en
  modifiant le drapeau Z/C, observateurs exec/read/write/branch) et pilotée
  par `dp_lr_explore` ; la même machine est explorée par `dp_explore`
  (graine de reset seule, sans fermeture statique ni scan de pointeurs, sans
  forks IRQ/touches/P1). Résultat sur 11 fixtures générées (branch, loop,
  vblank, irq, copy1, copyx, unreach, jumptable, nsdata, pal, probes) : les
  ensembles de débuts d'opcode sont identiques (0 manquant, 0 en trop).
  Observation de l'outil sur ROM ROM-only de 32 KiB.
- Hors périmètre de la comparaison, dit tel quel : les vecteurs RST/IRQ
  (< $100, graines froides de l'explorateur, pas des fourches) et le padding
  exécuté (non enregistré par l'explorateur).
- Nouveau garde du pilote : plafond de fourches par site (`site_cap`, défaut
  2), compteur `capped_forks` rapporté par `deepeel lr explore` (code retour 3
  s'il est non nul) ; sans lui, une boucle à état changeant noyait la file.
- Reste : un vrai cœur patché hors dépôt, le rapport complet (typage, copies,
  longueurs d'instruction), cartes multi-banques (la parité ne couvre que les
  ROM 32 KiB sans bancs).

## 0.76.0

J13 (partiel) : fixtures NES (`fix_nes_branch.nes`, NROM 32 KiB : conditionnel
+ OAM DMA) et PC Engine (`fix_pce_branch.pce`, 16 Kio : MPR, conditionnel sur
WRAM, écriture VDC) générées par `tests/build_fixtures.py` ; la campagne
(`tools/campaign_fixtures.py`, aussi test `campaign_fixtures`) couvre
maintenant GB, MD, SMS, NES et PCE (45 runs). Pour NES/PCE la colonne « octets
connus » est la couverture observée, pas une classification complète ; les pas
(1 000 000 / 1 200 000) sont le mur du budget par chemin sur la boucle d'attente.
Observations de l'outil. Reste : ROMs réelles hors dépôt, audit de la matrice,
fixtures Game Gear/MSX/SG-1000.

## 0.77.0

J13 (partiel) : fixtures `fix_z80_branch.{sms,gg,sg,msx}` (même programme Z80 :
lecture RAM, conditionnel `jr z` aux deux issues, `out` VDP, boucle d'attente ;
en-tête « AB » pour MSX) générées par `tests/build_fixtures.py`. La campagne
compte 49 runs sur 8 plateformes (GB, MD, SMS, GG, SG-1000, MSX, NES, PCE),
0 échec. Pour les plateformes Z80 la colonne « octets connus » est la carte
complète (le padding et l'inféré y sont classés) ; les 20 000 000 de pas sont le
mur global consommé par la boucle d'attente. Observations de l'outil. Reste :
ROMs réelles hors dépôt, audit de la matrice §2.1.

## 0.78.0

J13 (partiel) : audit de la matrice §2.1 par `tools/audit_matrix.py` (aussi test
`audit_matrix`) : vérifie la forme (8 cellules par ligne, marques connues) et
produit `docs/AUDIT.fr.md` (totaux par plateforme et liste des 97 cellules
ouvertes ◐/✗ sur 39 techniques × 6 plateformes). Les marques sont celles du
document ; l'audit n'en remesure pas l'outil. Reste : campagne sur ROMs réelles
hors dépôt, et fermeture des cellules ouvertes listées.

## 0.79.0

NES : `--entry bank:addr` (répétable, 32 max) n'est plus une option morte.
Chaque entrée est une graine racine (reset, banque commutable et PC posés par
`dp_nesmach_enter`) ; NROM n'accepte que la banque 0, UxROM toute banque
valide, les cartes ponteuses (CNROM, AxROM) refusent. Le rapport indique
`entries: seeded N refused M` : un refus n'est jamais silencieux. Test
`nes_entry`. Cellule G3 NES de la matrice §2.1 : reste ◐ (`--seed-banks`
absent sur NES, seul `--entry` est acquis).

## 0.80.0

NES : `--seed-banks` sème chaque banque commutable à `$8000` (UxROM ; une
graine par banque, y compris la banque fixe ; sans effet sur NROM et refusé
par les cartes ponteuses, compté dans `entries`). Fixture `fix_nes_uxrom.nes`
(4 banques, CHR-RAM) : sans l'option 1 chemin / 1 section, avec 5 chemins /
4 sections. Test `nes_seed_banks`. Cellule G3 NES de la matrice §2.1 passée
à ✅. Observations de l'outil (`--max-steps 1000`, boucle d'attente).

## 0.81.0

NES : sondes de lecture manette (D10). Au premier `STA/STX/STY $4016` d'un site
(une visite par site, borné par la taille de la PRG), l'explorateur forke un
état par bouton isolé (A, B, Select, Start, haut, bas, gauche, droite) ; le
masque courant est sauté ; la déduplication par état exact élimine les
doublons. Rapport : `pad probes: N forks at M sites, refused K` ; `--no-p1-fork`
les désactive. Limite : le runtime NES ne lit que la manette 0 (port 1 sans
chemin de lecture), et les combinaisons de boutons ne sont pas sondées, donc
D10 NES reste ◐. Fixture `fix_nes_pad.nes`, test `nes_pad_probe` (16 forks sur
2 sites : strobe à 1 puis à 0).

## 0.82.0

NES : cibles indirectes observées (D12, partiel). Chaque `JMP ($xxxx)`
exécuté lit sa cible sur le bus (alias de page 6502 compris) ; sites et
couples (site, cible) distincts sont comptés, ensembles bornés à 65 536 couples
avec un compteur de débordement. Rapport : `indirect jumps: N sites, M
targets, overflow K`. Les cibles sont déjà exécutées par le chemin naturel ;
il n'y a pas encore de second passage qui les sème depuis d'autres états, d'où
◐. Fixture `fix_nes_ind.nes`, test `nes_indirect`.

## 0.83.0

NES : copies par boucle CPU (D4, partiel). Une écriture `$2007` (PPUDATA)
précédée, dans les 4 pas, d'une lecture de donnée PRG est une copie ROM→VRAM :
les octets contigus du même `pc` sont fusionnés en un seul enregistrement
(canal 3, champ `len`), les cellules source portent `DP_NES_COPIED`. Rapport :
`cpu loop copy bytes N` et `ch3 src … len L`. Limites : la destination est
l'adresse PPU au moment de l'écriture ; les boucles vers la RAM (WRAM/OAM par
le CPU) et les sources RAM ne sont pas suivies, d'où ◐. Fixture
`fix_nes_cpucopy.nes` (16 octets), test `nes_cpu_copy`.

## 0.84.0

NES : scan de padding et inférence de trous (S1/S7, partiels). Après
l'exploration, les plages d'un même octet non touchées (`$00`/`$FF` dès 8
octets, autres dès 32) sont du padding (`DP_NES_PAD`, classe `padding`) ; les
trous ≤ 3 octets entre cellules de code sont du code inféré, les trous ≤ 1024
adjacents à du padding du padding inféré, les autres trous ≤ 1024 adjacents à
du classé des données inférées. Ce sont des inférences, jamais des
observations (classes `inferred-*`). Rapport :
`padding N bytes; inferred code/data/padding`. Pas encore de tables de
pointeurs (S1 reste ◐). Test `nes_padding`.

## 0.85.0

NES : fermeture statique de flot (S2, partielle). Pour chaque instruction
exécutée dont la cible (`jmp`, `jsr`, branche) n'a jamais été exécutée,
l'explorateur décode linéairement depuis la cible jusqu'à un `jmp`/`rts`/`rti`/
indirect/jam et marque les cellules `DP_NES_STATIC` (classe `code-static`),
sans jamais les compter comme observées (la couverture « touched » les exclut,
ainsi que le padding). Une cible dans la fenêtre 16 Kio de la source se résout
dans la banque de la source ; les autres fenêtres utilisent le mapping final.
Bornes : une visite par cellule, 4096 instructions par balayage ; les
saturations sont comptées (`cap hits`). Rapport : `static closure: N bytes
from M roots, cap hits K`. Limites : pas de suivi de constantes (S3), pas de
tables de sauts (S4), les cibles inter-banques UxROM dépendent du mapping
final. Fixture `fix_nes_static.nes`, test `nes_static`.

## 0.86.0

NES : tables de pointeurs / de sauts (S1/S4, partielles). Autour d'une cellule
lue comme donnée, une suite d'au moins 2 mots 16 bits alignés dont chacun
pointe sur une instruction exécutée par un chemin est une table (classe
`table`, inférence, 256 entrées max par table). Résolution d'un mot : NROM
linéaire ; UxROM `$C000+` dans la banque fixe, `$8000–$BFFF` dans la banque de
la table ; autres cartes ignorées. Les cibles non exécutées ne sont pas
retenues (ancrage sur l'observé), donc une table dont les cibles n'ont jamais
tourné n'est pas vue. Rapport : `pointer tables: N tables, M bytes`. Fixture
`fix_nes_table.nes`, test `nes_table`.

## 0.87.0

NES : glissade de padding (D14, partielle). Un chemin qui exécute `$00` (brk)
ou `$FF` au début d'une plage de 8 octets identiques de la PRG a quitté le
code : il est coupé et compté (`padding cuts: N`), jamais silencieusement. Les
plages de `$EA` (nop) ne sont pas coupées (traînes de nop légitimes). Pas de
HALT sur 6502, donc pas de chien de garde équivalent : D14 reste ◐. Fixture
`fix_nes_slide.nes`, test `nes_slide`.

## 0.88.0

NES : accès aux registres par registre (D3). Les lectures et les écritures de
`$2000–$3FFF` (PPU, miroirs repliés sur `$2000–$2007`) et de `$4000–$401F`
(APU, DMA, manettes) sont comptées par registre (`io_reg_reads`,
`io_reg_writes`, 40 emplacements) ; le rapport imprime `io reads N` et une
ligne `reg $XXXX reads R writes W` par registre touché. Corrige l'ancien
compteur d'écritures, dont l'indice `(addr − $2000) & $3F` mélangeait le
registre `$2000` et `$4000`. Test `nes_ioreg`. Observations de l'outil (le
nombre de lectures suit les chemins explorés, pas une fréquence matérielle).

## 0.89.0

NES : identité ROM des films (J3). `dp_nes_tas_identity` compare le SHA-1 du
film (bk2) à celui de la ROM, fichier entier ou sans l'en-tête iNES de 16
octets (majuscules acceptées). Verdict : même ROM, ROM différente (le film est
rejoué quand même, avec un avertissement), ou SHA-1 non enregistré (fm2). Le
rapport imprime `tas identity: same ROM N, different ROM N, not recorded N`.
Test unitaire `test_tas_identity` dans `nes`.

## 0.90.0

NES : graines de vecteurs (G1, partielle). `--seed-vectors` ajoute comme
racines les cibles des vecteurs NMI (`$FFFA`) et IRQ/BRK (`$FFFE`), lus dans
la dernière banque (fixe sur UxROM). Une cible sous `$8000` est refusée et
comptée. Rapport : `vector seeds: N`. Le vecteur de reset reste la racine par
défaut ; les vecteurs dépendant d'une bascule de banque ne sont pas résolus.
Test `nes_seed_vectors`.

## 0.91.0

NES : `--evidence FILE` (O2, partielle). Écrit les plages de cellules PRG de
même drapeau, `banque début fin masque` (banques de 16 Kio, masque = drapeaux
de cellule : 01 naturel, 02 forcé, 04 lu en données, 08 copié, 10 IRQ,
20 padding, 40 échantillon, 80 statique), au même format que Sega. Les cellules
sans drapeau ne sont pas listées. `.cov` et romdata restent absents : le
moteur NES ne tient pas de compteurs de coups par cellule. Test `nes_evidence`.

## 0.92.0

NES : `--cov FILE` et `--romdata FILE` (O2). Le moteur tient désormais des
compteurs saturants par cellule PRG : exécutions de l'octet d'opcode
(chemins explorés et joués) et lectures de données. `.cov` : lignes
`banque offset drapeaux coups` (1 exécuté, 4 spéculatif, 8 donnée), même forme
que Sega ; romdata : `page coups` par page de 256 octets lue en données. Les
octets d'opérande d'une instruction portent 0 coup (seul l'opcode compte).
Observations de l'outil, pas des mesures matérielles. Test `nes_cov`.

## 0.93.0

NES : frères de copie (S5, partielle). Une copie ROM→VRAM/OAM d'au moins 16
octets (OAM DMA : 256 ; boucle CPU : longueur coalescée) prouve que sa source
est de la donnée. Si l'adresse CPU de la source est un mot d'une table lue en
données, les autres entrées sont les sources du même chargeur : elles
reçoivent la longueur observée (bornée à la banque de 16 Kio sur UxROM) et la
classe `inf-data`. Au plus 64 entrées par table (saturation comptée), plages
sur code naturel refusées et comptées, l'observation gagne toujours. Rapport :
`copy siblings: N tables; N ranges (N bytes); N refused…; N tables cut at the
cap`. Fixture `fix_nes_sibling.nes`, test `nes_sibling`. Reste ◐ : pas les
copies DMC, pas de résolution pour les mappeurs à pont (1/3/4/7).

## 0.94.0

NES : second passage (G4, partielle). `--second-pass` : après le premier
drainage, les cibles des `jmp`/`jsr`/branches exécutés que nul chemin n'a
exécutées (les racines de la fermeture statique) sont entrées comme racines
forcées (banque de la cible dans la fenêtre `$8000–$BFFF`, banque fixe
au-delà), puis la file est drainée une seconde fois sous le même budget total.
Au plus 256 graines ; refus d'entrée, racines au-delà du plafond et graines
sont comptés : `second pass: N seeds, N refused, N roots over the cap`.
Sur `fix_nes_static.nes` (2 pas) la racine statique devient exécutée (forcée)
et la fermeture statique n'a plus de racine. Reste ◐ : un seul second passage,
pas de budgets en cascade, indirects non semés. Test `nes_second_pass`.

## 0.95.0

NES : `--spool FILE` et `--resume FILE` (F1, partielle). Alias de forme Sega
sur le lot disque existant : `--spool` = `--lot` (débordement du parc et
restes d'un mur écrits sur disque), `--resume` = `--lot` + `--lot-drain`
(seuls les états sauvegardés tournent, sans racine). Un run à mur de 3 pas
laisse 1 état ; `--resume` le relit et vide le lot. Reste ◐ : la file reste
bornée (avec parc), l'ensemble de déduplication n'est pas persisté entre
runs. Test `nes_spool_resume`.

## 0.96.0

NES : arbre de choix (`--tree FILE`, F2, partielle). Un nœud par état mis en
file (racine de reset, entrée `--entry`, vecteur, fourche de branche, fourche
NMI, sonde manette, graine du second passage), rattaché au chemin qui l'a
mis en file, avec son issue : terminé, évincé par déduplication, déversé sur
le lot disque, ou abandonné au budget (jamais silencieux). Format binaire
`DPTZ` version 2, identique à Sega : en-tête (magie, version, nombre), puis
par nœud `parent u32, site u32, pc u16, kind u8, issue u8`. Le champ `site`
vaut `0xFFFFFFFF` (cellule d'entrée non renseignée côté NES) ; le rapport
imprime `choice tree: N nodes`. Test `nes_tree` : 5 nœuds, 72 octets sur
`fix_nes_branch.nes`. Reste ◐ : pas de reprise de l'arbre entre runs.

## 0.97.0

NES : provenance des octets de RAM (S3, partielle). Chaque écriture en RAM
(`$0000–$07FF`) faite dans les 4 pas qui suivent une lecture de donnée PRG
retient la cellule PRG d'origine (dernier écrivain, tous chemins confondus :
approximation, pas une provenance par chemin). Un `jmp ($zp)` dont les deux
octets de pointeur viennent de deux cellules PRG consécutives désigne un mot
de table de sauts ; avec `--second-pass`, les mots voisins de la table
(64 par côté) qui pointent sur de la PRG non exécutée deviennent des racines
forcées du second passage (bornées par le plafond de 256 graines, compté).
Rapport : `provenance: N jump-table words traced from PRG reads, N roots
seeded`. Fixture `fix_nes_prov.nes` (table `$8020,$8030,$8040`, seule la
première prise : 2 racines semées), test `nes_provenance`. Reste ◐ : pas de
suivi de constantes arithmétiques, pas de provenance par chemin ni de
propagation registre→registre au-delà de la lecture immédiate.

## 0.98.0

NES : dispatch par vues de banques (G2, partielle, UxROM). `--bank-dispatch`
(implique `--second-pass`) : dans la banque fixe, une suite d'au moins 3 mots
alignés dont l'octet haut tombe en `$80–$BF` est une table de dispatch ; chaque
cible distincte est entrée dans chaque vue de banque dont les octets s'y
décodent en code (6 instructions sans jam, pas une plage de 8 octets
identiques) ; les vues refusées sont comptées. Bornée par le plafond de 256
graines du second passage. Rapport : `bank dispatch: N tables, N seeds, N bank
views refused (not code)`. Fixture `fix_nes_dispatch.nes` (3 cibles × 4
banques : 7 graines, 5 vues refusées), test `nes_bank_dispatch`. Reste ◐ :
UxROM seulement, pas de tables hors banque fixe, pas de vue à bascule dynamique.

## 0.99.0

NES : éviction par contexte grossier (C2, partielle). `--ctx-cap N` : un état
qui a passé la dédup exacte mais dont le contexte (pc, A/X/Y/S/P, registres de
mapper ; RAM et horloges exclues) a déjà été admis N fois est clos sans être
exécuté, compté (`context eviction: N states closed`) et marqué évincé dans
l'arbre. Désactivée par défaut. Ce n'est pas l'éviction GB : il n'y a pas de
liveness par section, donc un chemin dépendant de la RAM peut être coupé à
tort — d'où l'option explicite. Fixture `fix_nes_ctx.nes` (boucle qui ne
varie que par la RAM), test `nes_ctx_cap`.

## 0.99.1

NES : D14 pinglé. Le 6502 n'a pas d'instruction HALT ; son équivalent est le
jam (famille KIL), qui verrouille le CPU. L'explorateur y clôt le chemin et le
compte (`faults`). Fixture `fix_nes_jam.nes` (`LDA #1 ; KIL`) : 2 pas, 1 faute,
test `nes_jam`. Avec la glissade de padding (0.89.0), la cellule D14 NES passe ✅.

## 0.99.2

PCE : identité ROM des films (J3). `dp_pce_tas_identity` compare le SHA-1
enregistré par le film (BK2 `HeaderTable`/`SHA1`) à celui de l'image, ou de
l'image privée de son en-tête de 512 octets. Le rapport affiche
`tas identity: N same ROM, N different, N unrecorded` et un film d'une autre
ROM est signalé sur stderr. Test unitaire dans `test_pce`. Un film d'une autre
ROM est quand même rejoué (comme sur NES) : l'identité informe, elle ne bloque pas.

## 0.99.3

Correction de la matrice (aucun changement de code) : PCE F3 et F1 étaient
notées ✗ alors que `--jobs 2` par instances `dp_pce2_` (0.56, `test_jobs`) et
le lot disque avec parc (`--lot`, `--lot-drain`) existent. Elles passent ◐ avec
leur reste explicite : pas de file de fourches partagée entre instances, pas
de `--resume` ni de file croissante côté PCE.

## 0.100.0

PCE : `--evidence FILE` (O2, partielle). Même format que NES : plages de
cellules de même masque (`01` code naturel, `02` forcé, `04` donnée, `08`
copiée, `10` IRQ, `20` padding, `40` statique, `80` table), banques physiques
de 8 KiB. Fixture `fix_pce_branch.pce`, test `pce_evidence` (la plage forcée
`0018–001A` est épinglée). Pas de `.cov` ni de romdata côté PCE : il n'y a pas
de compteurs de coups par cellule.

## 0.101.0

PCE : coupe de glissade de padding (D14, partielle). Un chemin naturel ou
forcé qui arrive sur 8 octets identiques `$00`/`$FF` de la ROM est coupé et
compté (`padding cuts: N`) ; avant, il les exécutait comme du code jusqu'au
plafond de pas. Les opcodes illégaux étaient déjà coupés (`illegal_cuts`).
Fixture `fix_pce_slide.pce` (`LDA #1 ; JMP $E020` vers une plage de zéros :
2 pas, 1 coupe), test `pce_slide`. Reste ◐ : pas de chien de garde HALT/WAI
attribué, et les sessions de jeu (`play`) ne coupent pas.

## 0.102.0

PCE : `--entry ADDR` (G3, partielle), jusqu'à 16 fois. Chaque adresse démarre
un chemin forcé depuis l'état de mise sous tension (mapping MPR du reset :
toutes les pages sur la banque 0). Une adresse hors ROM est refusée et
comptée. Rapport : `entries: N seeded, N refused`. Nouveau pont
`dp_pce_set_pc`. Test `pce_entry`. Reste ◐ : pas de `--seed-banks`, pas
d'entrée dans une autre vue de banque (il faudrait fixer les MPR).

## 0.103.0

PCE : `--ctx-cap N` (C2, partielle), même principe que NES (0.99.0). Un état
qui passe la dédup exacte mais dont le contexte (A/X/Y/S/P, pc, 8 MPR, masque
d'IRQ ; RAM et horloges exclues) a déjà été admis N fois est clos, compté
(`context eviction: N states closed`). Désactivée par défaut ; pas de liveness
par section, donc un chemin dépendant de la RAM peut être coupé à tort.
Fixture `fix_pce_ctx.pce` (WRAM en MPR1, boucle ne variant que par la RAM :
1 état clos avec `--ctx-cap 3`, aucun sans), test `pce_ctx_cap`.

## 0.104.0

MD : `--entry ADDR` (G3, partielle), jusqu'à 16 fois. Chaque adresse (ROM,
paire) est empilée comme un état forcé depuis la machine de mise sous
tension ; une adresse impaire ou hors ROM est refusée et comptée. Rapport :
`entries: N seeded, N refused`. Test `md_entry` sur `fix_md_loop.md` : sans
entrée 0 octet exécuté par fourche, avec `--entry 0x2f0` des milliers (le
code est la plage de zéros exécutée comme `ori`). Reste ◐ : pas de
`--seed-banks` (mapper SRAM/bankswitch SSF2), pas d'entrée dans une vue
de banque.

## 0.104.1

Correction de la matrice (aucun changement de code) : MD F1 (lot disque,
`--lot-drain`, parc `parked`) et MD O2 (`--evidence`) existent déjà ; elles
passent ◐ avec leur reste explicite (pas de `--resume`, pas de `.cov` ni
romdata).

## 0.105.0

PCE : frères de copie (S5, partielle). Un mot de table de la ROM (cellule lue
comme donnée) qui nomme la source d'une copie VDC par bloc d'au moins 16
octets désigne une table de flux ; les mots voisins de la même fenêtre qui
pointent sur des cellules non classées de la même page sont typés `inferred
data` sur la longueur de la copie (jusqu'à 64 entrées par table, saturation
comptée en `table caps`). Cela corrige aussi le typage `inferred padding` qu'une
plage isolée entre deux zones de padding recevait. Rapport : `copy siblings: N
tables, N entries, N bytes typed, N table caps`. Fixture `fix_pce_sibling.pce`
(table de 3 mots, 1 copie, 2 frères de 32 octets : 64 octets), test
`pce_sibling`. Reste ◐ : tables dans la même page que la cible seulement, pas
de copies CPU→VCE ni de tables inter-banques.

## 0.106.0

PCE : cibles indirectes observées (D12, partielle). `jmp (abs)` et
`jmp (abs,X)` lisent leur cible sur le bus vivant ; les sites et les paires
(site, cible) distinctes sont comptées, bornées à 65 536 paires avec
`overflow` rapporté. Rapport : `indirect jumps: N sites, N targets, overflow N`.
Fixture `fix_pce_ind.pce` (deux sauts, deux cibles), test `pce_indirect`.
Comme sur NES, la cellule reste ◐ : les cibles sont observées, pas semées en
second passage.

## 0.107.0

PCE : provenance de constantes (S3, partielle). Un store (`sta/stx/sty`,
zéro-page ou absolu) qui suit de moins de 4 pas une lecture de donnée ROM
retient la cellule source pour l'octet de WRAM visé ; un `jmp (abs)` /
`jmp (abs,X)` dont le pointeur est en WRAM et dont les deux octets viennent de
cellules ROM consécutives compte un mot de table (`provenance: N jump-table
words traced from ROM reads`). Fixture `fix_pce_prov.pce`, test
`pce_provenance`. Reste ◐ : comptage seulement, pas de graine de second
passage pour les cibles, pas de suivi à travers registres ni piles.

## 0.108.0

MD : cibles indirectes observées (D12, partielle). Un `jmp`/`jsr` sans cible
statique (An, (d,An), (An,Xn)) enregistre le pc suivant réellement exécuté ;
sites et paires (site, cible) distinctes comptés, bornés à 65 536 paires
(`overflow` rapporté). Rapport : `indirect jumps: N sites, N targets, overflow
N`. Fixture `fix_md_ind.md`, test `md_indirect`. Reste ◐ : observation, pas de
second passage.

## 0.109.0

MD : coupe de padding (D14, partielle). Avec `--cut-padding`, un chemin qui
exécute un mot nul (`ori.b #0,d0`) dont les 16 octets suivants sont tous nuls
est arrêté et compté (`padding cuts: N`). Désactivée par défaut : les tests
`md_explore` épinglent des comportements qui traversent du padding. Fixture
`fix_md_zeros.md`, test `md_padding`. Reste ◐ : option, pas de glissade `$FFFF`
(déjà une faute d'opcode) ni de HALT/STOP attribué.

## 0.110.0

MD : frères de copie (S5, partielle). Un longword de table de la ROM (lu
comme donnée) égal à la source d'une copie DMA ROM→VRAM d'au moins 16 octets
désigne une table de flux ; les entrées voisines (paires, ≥ `$200`, dans la
ROM, vers des octets non observés ; 64 max par table, saturation en `table
caps`) sont comptées comme flux frères de même longueur. Rapport : `copy
siblings: N tables, N entries, N bytes, N table caps`. Fixture `fix_md_sib.md`,
test `md_sibling`. Reste ◐ : comptage seulement (pas de typage par octet dans
la carte), copies 68000 vers VRAM par CPU non traitées.

## 0.111.0

PCE : arbre de choix (F2, partielle). `--tree FICHIER` écrit l'arbre commun
`DpTree` : chaque état mis en file est un choix (parent = chemin qui l'a
produit) clos avec son issue : `done` (chemin joué), `merged` (déjà vu),
`capped` (éviction `--ctx-cap`), `spooled` (disque), `budget` (restant à l'arrêt
global). Rapport : `choice tree: N nodes`. Test `pce_tree`. Reste ◐ : site de
branchement non renseigné, pas de reprise depuis `tree.bin`, états repris du
lot sans nœud.

## 0.112.0

MD : arbre de choix (F2, partielle). `--tree FICHIER` écrit l'arbre commun
`DpTree` : chaque état poussé (fork, entrée, IRQ) est un choix de parent le
chemin qui l'a produit, clos en `done`, `spooled` (lot disque) ou `budget`
(restant à l'arrêt). Rapport : `choice tree: N nodes`. Test `md_tree` (dbcc :
2 forks, 3 nœuds). Reste ◐ : site de branchement non renseigné, pas de reprise
depuis le fichier, états repris du lot sans nœud.

## 0.113.0

MD : éviction par contexte grossier (C2, partielle). `--ctx-cap N` : le
contexte d'un état poussé est (pc, D0–D7, A0–A7, SR, genre d'IRQ), RAM exclue ;
la k-ième admission d'un contexte est la clé `hash(ctx, k)` d'un `DpHashSet`, et
au-delà de N l'état est clos et compté (`context eviction: N states closed`).
Désactivée par défaut. Le compteur peut compter plusieurs fois la même
tentative de pad-fork rejouée sur un site de boucle (bornée par le budget de
pas). Test `md_ctx_cap` (`fix_md_pad` : 5 chemins sans cap, 2 avec cap 1).
Reste ◐ : pas de liveness par section.

## 0.114.0

PCE : second passage (G4, partielle). `--second-pass` : après le drainage, les
successeurs statiques non exécutés d'instructions exécutées (fall-through,
branchement, saut, appel, dans la même fenêtre) sont semés comme chemins forcés
depuis la machine de mise sous tension, la page ROM étant mappée sous la
fenêtre (`dp_pce_enter`) ; le drainage reprend jusqu'à épuisement des graines.
Borné à 256 graines au total, refus (page non mappable) et saturation
rapportés : `second pass: N seeds, N refused, N capped`. Test `pce_second_pass`
(`fix_pce_slide` : 1 graine). Reste ◐ : pas de graines depuis les tables de
saut (G2 PCE), pas de semis indirect, le mapping des autres fenêtres est celui
de la mise sous tension.

## 0.115.0

MD : second passage (G4, partielle). `--second-pass` : les plages d'octets
exécutés sont parcourues instruction par instruction ; les cibles statiques
(BRA/BSR/Bcc/DBcc/JMP/JSR absolus) et la suite en fin de plage qu'aucun chemin
n'a atteintes (paires, ≥ `$200`, dans la ROM) sont semées comme chemins forcés
depuis la machine de mise sous tension ; le drainage reprend jusqu'à épuisement
des graines, 256 au total (`second pass: N seeds, N refused, N capped`). Test
`md_second_pass` (`fix_md_fork_fault` : 1 graine, 14 → 3584 octets exécutés,
la graine glisse dans le padding nul : c'est là que `--cut-padding` sert).
Reste ◐ : pas de semis depuis les tables (G2 MD) ni les cibles indirectes.

## 0.116.0

PCE : dispatch par vues de pages (G2, partielle). `--bank-dispatch` : une suite
d'au moins 3 mots alignés (≥ 3 distincts) dont les trois bits hauts désignent
une même fenêtre CPU est une table candidate ; chaque cible est entrée sous
chaque page ROM où 6 instructions se décodent sans opcode indéfini ni suite de
8 octets identiques, puis semée comme chemin forcé (plafond de 256 graines
partagé avec `--second-pass`). Rapport : `bank dispatch: N tables, N seeds, N
refused views`. Test `pce_bank_dispatch` (`fix_pce_disp` : 1 table, 3 graines,
3 vues refusées). Reste ◐ : le mapping des autres fenêtres est celui de la mise
sous tension, tables en pile/zéro-page non lues.

## 0.117.0

MD : tables de dispatch (G2, partielle). `--bank-dispatch` : une suite d'au
moins 3 mots longs alignés, tous pairs, ≥ `$200` et dans la ROM (≥ 3 distincts),
est une table candidate de pointeurs de code ; chaque cible non exécutée dont
les 6 premières instructions se décodent (sans suite de 8 octets identiques)
est semée comme chemin forcé (plafond de 256 graines partagé avec
`--second-pass`). Rapport : `dispatch tables: N tables, N seeds, N refused
targets`. Test `md_dispatch` (`fix_md_disp` : 1 table, 3 graines, 2 → 20 octets
exécutés). Reste ◐ : tables d'offsets 16 bits relatifs, tables en RAM.

## 0.118.0

MD : `--jobs 2` (F3, partielle). Le moteur MD (machine m0boy, explorateur) est
global au processus : la pile est recompilée une seconde fois, chaque symbole
défini préfixé `dp_md2_` (objcopy, comme PCE/Sega). Partage par entrées
explicites : l'instance secondaire saute le chemin de mise sous tension et prend
les `--entry` impairs, la primaire garde le chemin natif (film, fourches,
rejeux) et les `--entry` pairs ; la primaire joint ensuite les observations
brutes de la secondaire (drapeaux, lectures, compteurs, copies, lecteurs,
sites de faute, banques Z80) avant les passes de frères et les comptes
d'octets : le rapport décrit l'union. Il faut au moins 2 `--entry` (sinon
message et une seule instance). Le second passage et le dispatch ne tournent
que sur la primaire (ils demandent l'union). Rapport : `jobs: 1 secondary
instance folded`. Test `md_jobs` (rapport identique à `--jobs 1` hors cette
ligne). Reste ◐ : pas de file partagée ; les fourches d'une instance ne sont
pas redistribuées à l'autre.

## 0.119.0

NES : `--jobs 2` (F3, partielle). La pile NES (machine, explorateur) est globale :
elle est recompilée une seconde fois, chaque symbole défini préfixé `dp_nes2_`
(objcopy, comme PCE). Partage par phase : l'instance secondaire prend les
sessions TAS / `--play` et draine les points de contrôle qu'elles laissent ; la
primaire draine l'arbre de fourches de la mise sous tension, puis joint les
observations brutes de la secondaire (drapeaux PRG et CHR, compteurs d'exécution
et de lecture, registres, journaux) avant les sections et la clôture statique :
le rapport décrit l'union (tuiles CHR recomptées sur l'union). Il faut `--tas` ou
`--play` (sinon message et une seule instance). Rapport : `jobs: 1 secondary
instance folded`. Test `nes_jobs` (même résumé que `--jobs 1` hors cette ligne).
Reste ◐ : pas de file partagée ; les journaux de copies sont fusionnés par site
(les occurrences par événement diffèrent de l'exécution à une instance).

## 0.120.0

NES : recherche de nouveauté, Go-Explore (J4, partielle). `--novelty STEPS`
(`--novelty-frames N`, `--novelty-archive N`, `--novelty-cells`) : une archive
d'instantanés compressés ; chaque essai repart d'une cellule choisie parmi huit
tirées au hasard (gain par essai, profondeur, racine carrée des tirages) et joue
des touches aléatoires (pas de directions opposées). Avec `--novelty-cells`, une
nouvelle empreinte d'écran grossière (tuiles de nametable ÷ 8 échantillonnées,
défilement grossier, nametable choisie : `dp_nesmach_screen_key`) ou de
nouveaux octets PRG exécutés archivent la cellule ; les vies sont détectées sans
connaissance du jeu (octet de RAM ≤ 9 qui ne baisse que de 1 à 200 images
d'écart) et les cellules archivées peu avant une mort sont condamnées. Les 64
cellules les plus récentes amorcent la file de fourches (dédup d'état exact).
Rapport : `novelty: depth N frames, N trials, …`. `--jobs 2` donne cette phase
à la secondaire. Test `nes_novelty`. Reste ◐ : le mode humain et la reprise
d'archive entre exécutions, la clé d'écran ne voit pas les sprites.

## 0.121.0

Mega Drive : recherche de nouveauté, Go-Explore (J4, partielle). Mêmes options
que NES/PCE : `--novelty STEPS`, `--novelty-frames`, `--novelty-archive`,
`--novelty-cells`. Archive d'instantanés compressés, essais à touches
aléatoires (pas de directions opposées) pilotés par le crochet d'image ;
nouvelle empreinte d'écran (`dp_mdmach_screen_key` : noms de plans A/B, tuile
÷ 8 une sur quatre, défilement ÷ 32) ou nouveaux octets ROM exécutés archivent
la cellule ; vies détectées dans la RAM de travail sans connaissance du jeu ;
les 64 cellules les plus récentes amorcent la file de fourches. Garde : 8
essais consécutifs sans aucune image (ROM sans boucle d'image) arrêtent la
phase et incrémentent `stalled`, rapporté. Rapport : `novelty: depth N frames,
… N stalled`. Test `md_novelty`. Reste ◐ : sprites absents de la clé, pas de
mode humain, pas de reprise d'archive, pas de `--jobs` pour cette phase
(l'instance secondaire ne la joue pas).

## 0.122.0

Mega Drive : `--watch [FPS]` et `--watch-dump FILE` (O3, partielle). Le crochet
d'image de l'explorateur rend le VDP (`dp_mdmach_render`, 320×224 ARGB) toutes
les 60/FPS images et publie la copie dans un tampon commun
(`src/gui/fbwatch.[ch]`, mutex : aucun état de machine n'est partagé avec le
thread de la fenêtre). La fenêtre SDL2 affiche la dernière image
(`dp_fbwatch_*`, Échap ou fermeture) ; sans SDL2 un message l'indique.
`--watch-dump` écrit la dernière image en PPM (utilisable sans écran ; testé :
`md_watch_dump`). Rapport : `watch: N frames published`. Le tampon est
indépendant de la plateforme : NES/PCE n'ont qu'à publier. Reste ◐ : pas de
bandeau d'état (vitesse, film), l'instance secondaire de `--jobs` ne publie
pas, NES/PCE/J5 (`.sta`, mode humain) non faits.

## 0.123.0

NES et PCE : `--watch [FPS]` et `--watch-dump FILE` (O3, partielle), sur le
tampon commun de 0.122.0. NES : le panneau 160×128 du PPU
(`dp_nesmach_watch_enable`/`dp_nesmach_render`, RGB565 → ARGB) ; le rendu n'est
activé que sous `--watch`. PCE : le pas-à-pas remplaçait la boucle de lignes de
`pce_frame` sans rangées d'affichage ; `emit_row` les rétablit (première ligne
active, VCE appliquée) et un crochet d'image publie la copie (`dp_pce_render`,
256 ou 352 de large). L'image publiée est celle du dernier chemin exécuté
(un chemin forcé peut dessiner autrement que le chemin naturel). Fixture
`fix_nes_picture.nes` (palette, tuiles, rendu actif) ; tests `nes_watch_dump`,
`pce_watch_dump` (le fixture PCE n'active pas l'affichage : on ne vérifie que le
flux d'images et le format). Reste ◐ : pas de bandeau d'état, `--jobs`
secondaire sans publication, image PCE jamais vérifiée sur un vrai affichage.

## 0.124.0

NES, PCE, MD : `--export-state FILE` (J5, partielle). Avec `--novelty`, la
cellule d'archive la plus profonde (images depuis la mise sous tension) est
écrite telle quelle dans un fichier `DPST` (`src/core/statefile.[ch]`) :
en-tête de 36 octets (magie, version 1, plateforme, CRC-32 et taille de la ROM,
profondeur, taille du blob, taille compressée) puis le blob zlib de la machine.
C'est le format interne de deepeel (la machine m0boy recompilée de la
plateforme) : aucun émulateur tiers ne le lit, ce n'est donc pas encore un
`.sta` de convertisseur. Rapport : `state exported: deepest cell, N frames`.
Test `export_state` (en-tête, taille décompressée, profondeur > 0 sur les trois
plateformes). Reste ◐ : mode humain, rechargement du fichier (reprise), format
d'émulateur. Sans `--novelty`, rien n'est écrit.

## 0.125.0

Mega Drive : `--exact-dedup` (C3, partielle). `dp_mdmach_state_hash` hache la
machine sans ses horloges, son audio ni ses compteurs (registres, RAM, VRAM,
VSRAM, CRAM, registres VDP, RAM Z80, SRAM, pad, lignes) ; une fourche dont le
hachage (avec pc d'entrée et nature d'interruption) a déjà été empilé est
abandonnée et comptée : `exact dedup: N forks dropped`. Sur `fix_md_pad_effect`
95 fourches de moins, couverture exécutée identique (test `md_exact_dedup`).
Le coût est un hachage d'environ 200 Kio par fourche : option désactivée par
défaut. Reste ◐ : l'ensemble n'est pas persisté (`exact.bin`), un ensemble plein
n'est pas reporté (il grossit), le Z80 interne n'est pas dans le hachage.

## 0.126.0

Mega Drive : inférence de voisins S7 (partielle). Après l'exploration, les
plages non observées d'un même octet (00/FF dès 8 octets, autres dès 32) sont
du remplissage inféré ; un trou de 3 octets au plus entre deux octets exécutés
est du code inféré ; un trou de 1024 octets au plus voisin d'octets observés est
de la donnée inférée. Le résultat vit dans `inferred`, jamais dans `flags` : une
inférence ne masque pas une observation. Ligne de rapport `map inference (S7,
not observations)`. Reste ◐ : pas de carte `.map` classée ni de rendu PNG pour
le MD (O1), donc l'inférence n'est visible que par ses compteurs.

## 0.127.0

Mega Drive : `--map` écrit désormais une carte classée, `md map v1`, un chiffre
par octet de ROM, mêmes classes que le NES (`code-natural`, `code-forced`,
`data-copied` pour DMA/VRAM, `data-read`, puis les trois inférences S7). Avant,
le fichier était un vidage brut des drapeaux. Une inférence ne remplace jamais
une observation. Reste ◐ : pas encore de rendu PNG, JSON ni heat pour le MD.

## 0.128.0

`deepeel maprender ROM in.map out.png --sys md` accepte une carte `md map v1`
(et `nes map v1`) : chaque chiffre de classe est converti en plages sur la ROM
entière, fenêtrées par 16 Kio comme pour les plateformes Z80, puis rendues par
le moteur de carte commun. Le MD a donc sa carte PNG. Reste ◐ pour O1 MD :
pas de JSON de synthèse ni de carte de chaleur.

## 0.129.0

Mega Drive : `--summary FILE` écrit un profil JSON compact de la run : octets
par classe de la carte (somme égale à la taille de la ROM), part connue,
compteurs de recherche (pas, chemins, fourches) et de saturation
(`sat_total/path/visit/queue/site`). Observations de l'outil, pas des mesures
de matériel. Reste ◐ pour O1 MD : pas de carte de chaleur.

## 0.130.0

Mega Drive : `--heat FILE` (O1). Chaque début d'instruction exécuté par un
chemin **naturel** (jamais par une fourche) incrémente un compteur par octet de
ROM ; le fichier `md heat v1` liste, par zone de 256 octets, le nombre
d'instructions jouées. Même sémantique que la chaleur GB : l'usage joué, pas la
couverture d'exploration. La cellule O1 du MD passe ✅ : carte classée, PNG,
JSON et chaleur sont tous là (fixtures seulement, aucune ROM réelle essayée).
Limite : avec `--jobs 2` l'instance secondaire ne reverse pas ses compteurs de
chaleur ; la chaleur ne reflète que l'instance principale.

## 0.131.0

Deux changements décidés par le propriétaire : la couverture prime sur le temps
et les ressources.

- **Code exécuté en WRAM (GB).** Les écritures en WRAM alimentent le traqueur
  de copies ; une boucle ROM→WRAM verbatim est enregistrée (`-> wram`). Quand le
  pc est dans cette plage WRAM, l'instruction compte comme exécutée aux octets
  source de la ROM (`exec_wram_rom`, ligne `outside-rom exec (N traced to ROM
  copy sources)`). Avant, ce code était seulement compté. Test `wram_exec_trace`
  (fixture `fix_wramexec`). Limite : seulement les copies verbatim observées ;
  un code WRAM construit autrement (décompression, patch) reste non rattaché.
- **`--unbounded`.** Aucun plafond de forks par site, de chemins, de pas, de
  file, de spool, ni arrêt sur plafond mémoire. La file qui déborde, la zone
  d'attente pleine et l'échelle de paliers épuisée écrivent dans `spool.bin`
  (`<sortie>.state`, créé si absent), relu quand la file se vide. Le pruning
  « deux côtés déjà couverts » est conservé. Test `unbounded_spool` : file de
  taille 1, 21 états écrits puis relus, 0 débordement, même résidu que sans
  budget.

Mise en garde mesurée : sans le pruning « couvert », un essai sur
`fix_branch.gb` a écrit plusieurs Gio de spool en 20 s (quota tmpfs atteint) :
l'espace d'états à compteurs de données est infini. `--unbounded` s'arrête donc
à l'épuisement ou à SIGINT (reprise par `--resume`), pas forcément vite, et il
faut un disque en face. Je n'ai pas relancé Zelda en `--unbounded` : la
campagne reste à faire (J13).

## 0.132.0

MD : couche statique S1/S2 sans machine (`md_static_scan`), étiquetée à part des
observations. S1 : padding (≥ 8 octets `00`/`FF`, classe 8) et tables de
pointeurs (≥ 4 longs grand boutiste pairs, dans la ROM, ≥ 3 valeurs distinctes,
classe 7). S2 : depuis chaque plage exécutée, suivi de BRA/Bcc/DBcc/JMP/JSR/BSR
et du fall-through des instructions non terminales sur octets non exécutés
(classe 3 `code-static`) ; après BSR/JSR la confiance est perdue (idiome des
données en ligne) : le fall-through n'est pas suivi et le rapport le compte
(`calls with lost confidence`). L'inférence S7 ne recouvre jamais cette couche.
Ligne de rapport `static layer (S1/S2, no machine)`. Test `md_static_layer`
(fixture `fix_md_static`, `--fork-cap 0`). Non validé sur ROM MD réelle : voir J13.

## 0.133.0

Le spool disque (`spool.bin`) écrit désormais des enregistrements zlib de taille
variable (`DPSZ`) ; les anciens enregistrements bruts (`DPSR`) restent lisibles.
Raison : une run `--unbounded` sur Mario Land 2 a écrit 7,7 Gio de spool en moins
de 4 minutes (limite `ulimit -f` atteinte, SIGXFSZ) ; les images machine sont
presque toutes des zéros. Sur la fixture `fix_branch.gb`, 21 états tiennent en
5,5 Kio. La reprise recompte les enregistrements entiers en parcourant les
en-têtes. Le débit réel de croissance sur ROM réelle est à mesurer (J13).

## 0.134.0

GB S4 : à chaque `jp hl` exécuté, si un `ld hl,nn` exécuté le précède de moins
de 24 octets, la table de mots à `nn` (≤ 64 entrées, adresses ROM, instruction
légale, hors padding, même banque que le saut) devient des racines de la
fermeture statique. Ligne de rapport `S4 operand tables of executed JP HL`.
Test `gb_jp_table` (fixture `fix_jptable`). Limite : seul le motif `ld hl,nn`
proche est reconnu ; une base calculée (`add hl,de` depuis une autre origine)
n'est pas suivie.

## 0.135.0

MD C1 : le rapport donne `sections: N executed runs, R required (natural path), F
forced only` (une section = plage exécutée maximale ; requise si un octet a
tourné sur le chemin naturel) et le `--summary` JSON ajoute `sections` et
`sections_required`. Test `md_sections`. Pas de liveness par section.

## 0.135.1

Correctif : interblocage (ordre de verrous inversé) entre `queue_push` (verrou du
pool puis du spool) et `spool_feed` (spool puis pool) avec `--jobs 2` et un
spool non vide. Constaté sur Zelda et Mario Land 2 en `--unbounded` : les deux
processus dormaient à 1 % CPU, sans écriture, après 8 min. `spool_feed` prend
maintenant le pool avant le spool.

## 0.135.2

Correctif : la croissance du pool partagé (`--jobs 2`) faisait un `realloc` sans
réordonner l'anneau ; une file enroulée (tête ≠ 0) était brouillée, et les
nouveaux emplacements non remis à zéro. Observé sur Zelda en `--unbounded` :
SIGSEGV dans `dp_mach_load`, le pointeur d'image contenait des octets d'image.
L'anneau est recopié dans l'ordre logique, tête à 0, reste mis à zéro. Bug
antérieur à `--unbounded`, déclenché bien plus souvent par lui.

## 0.136.0

But final du projet : la recompilation statique complète, sans interpréteur
annexe. Un octet exécuté en RAM sans trace dans l'archive obligerait le binaire
recompilé à embarquer un interpréteur ; tout code exécuté doit donc être connu.

- **Archive du code exécuté en RAM** (`src/explore/rtcode.c`) : chaque
  instruction distincte exécutée à pc ≥ $8000 (WRAM, écho, VRAM, HRAM) est
  gardée avec ses octets, une entrée par variante `(pc, octets)` — un emplacement
  réécrit donne plusieurs variantes (code polymorphe). Compteur d'exécutions et
  index ROM de la copie verbatim quand elle existe (`-1` = code généré, sans
  source ROM). Ligne de rapport « runtime code archive », option
  `--rtcode FILE` (une ligne `pc octets hits rom_index`).
- **Correctif drain parallèle** (`--jobs N`) : un pool momentanément vide
  terminait le worker même quand le spool contenait encore des enregistrements
  (lot entièrement rejeté par la dédup à la reprise) ; Zelda s'arrêtait ainsi
  après 3 min avec 1,24 M d'états en attente. Le drain ne finit plus que
  lorsque tous les threads sont inactifs, le pool vide et le spool épuisé.
- Le piège « BUG NULL write in ring window » ne s'applique plus aux graines
  prioritaires (faux positifs).

## 0.136.1

L'archive du code exécuté en RAM compte les variantes qui réécrivent une
instruction déjà vue à la même adresse (code auto-modifiant ou polymorphe) :
ligne « … N rewrote an earlier instruction at the same address ». Sur la
fixture `fix_wramexec`, 1 réécriture (`ld a,$00` devenu `ld a,$42`).

## 0.137.0

Code généré en RAM sans copie verbatim (décodé, XOR, décompressé) : chaque
octet WRAM écrit retient l'index du dernier octet de ROM lu avant l'écriture
(`wram_taint`). À l'exécution, une instruction sans source verbatim reçoit ce
**producteur approximatif** (ligne de rapport « N with an approximate ROM
producer », 5ᵉ colonne de `--rtcode`). C'est une approximation (plus proche
lecture ROM avant l'écriture, remise à zéro à chaque chemin), pas une preuve
de dépendance : elle désigne la région ROM à examiner (flux compressé et
décodeur), pas l'octet exact. Fixture `fix_wramgen` (routine stockée XOR $5A),
test `rtcode_generated`.

## 0.138.0

Sondes événementielles §3.7 : avant `ldh a,(n)` / `ld a,(C)` / `ld a,(nn)` lisant
`LY` ($FF44), `STAT` ($FF41) ou `DIV` ($FF04), des variantes forcées portent
les valeurs significatives (LY 0/143/144/153, mode STAT 0–3, DIV 00/40/80/FF),
bornées par site comme les fourches P1. Ligne de rapport « event probes … forced
variants », option `--no-event-fork`. Fixture `fix_lywait`, test `event_probe_ly`.
Les valeurs sont forcées, pas rythmées : observation de l'outil, pas mesure matériel.

## 0.139.0 (NES)

Deux cartes de plus sur le pont mapper NES : Color Dreams (11) et GxROM (66),
un seul registre `$8000+` choisissant une banque PRG de 32 Kio et une banque CHR de
8 Kio (CHR-ROM exigée, PRG par 32 Kio). Les mappers restant refusés (MMC2/5/…)
gardent leur message explicite. Test `test_gxrom` dans `test_nes`.

## 0.140.0 (MD)

Films BizHawk `.bk2` pour Mega Drive (`--tas`) : le journal d'entrées est converti
en octets de manette 3 boutons (X/Y/Z/Mode ignorés), l'en-tête (clé `SHA1`/`MD5`
de 32 hex) est comparé au MD5 de la ROM (`src/core/md5.c`). Une différence donne
un **avertissement**, pas un refus : le film réel essayé (Sonic 1, 60 053 images)
porte une valeur d'en-tête différente du MD5 de la ROM jouée et se rejoue pourtant
jusqu'à la dernière image (« GMV replay: 60053 / 60053 frames », observation de
l'outil : 51 716 octets de ROM exécutés, 9,9 %, en 2 min avec `--max-total 2e9`).
Test `md_bk2_movie`. Option `--tas-any-rom` acceptée côté MD (supprime l'avertissement).

## 0.141.0 (SMS / GG / SG-1000 / MSX)

`--novelty N` : recherche de nouveauté de type Go-Explore sur le backend Sega
(même principe que NES/MD). Une archive de points de contrôle compressés
(machine + RAM cartouche) ; chaque essai restaure le point le plus prometteur,
joue des manettes aléatoires maintenues quelques « images » (une image =
`--novelty-frame-steps` instructions, 10 000 par défaut) et archive les écrans
nouveaux (empreinte de la table de noms VDP) ou la couverture ROM nouvelle. Les
vies sont apprises sans connaissance du jeu (petit octet de RAM qui ne fait que
décroître par pas isolés) ; les derniers points d'archive amorcent la file de
fourches. Options : `--novelty-frames` (240), `--novelty-archive` (4096),
`--novelty-frame-steps`, `--novelty-cells 1`. Rapport : ligne `novelty: …`.
Ce sont des observations de l'outil, pas des mesures de matériel. Reste :
mesures sur ROMs réelles, GB (J4 GB).

## 0.142.0 (SMS / GG / SG-1000 / MSX)

`--export-state FICHIER` (avec `--novelty`) : écrit la cellule d'archive la plus
profonde au format DPST (`platform` = `sms`/`gg`/`sg`/`msx`, blob = état machine
+ RAM cartouche, zlib). Blob propre à la machine m0boy de la plateforme, lisible
par aucun émulateur tiers. Rapport : `state exported: deepest cell, N frames`.
Reste : `--watch` / `--watch-dump` (O3).

## 0.143.0 (SMS / GG / SG-1000 / MSX)

`--watch [FPS]` et `--watch-dump FICHIER.ppm` (O3) sur le backend Sega : le
rendu de lignes du VDP de la machine (SG/MSX : 160x192 ; GG : 160x144 ; SMS :
160x192, fenêtre de la runtime) alimente le tampon d'images commun
(`fbwatch`), affiché par la fenêtre SDL ou écrit en PPM. Le rendu n'est activé
que sous `--watch` (il met à jour l'état des sprites) ; `--watch` force un seul
travailleur. Rapport : `watch: N frames published`. Reste : J5/O3 hors
Sega (GB), mode humain/`.sta`.

## 0.144.0 (GB)

`--session-cells` (J4 GB) : les sessions jouées archivent dans le pool de
checkpoints chaque fond d'écran jamais vu (empreinte FNV de la carte de tuiles
choisie par LCDC + défilement grossier, échantillonnée toutes les 8 images en
jeu aléatoire). Le pool choisit déjà le point le moins tiré, donc une nouvelle
cellule est reprise en priorité. Rapport : `N screen cells archived into the
checkpoint pool`. Observation de l'outil, pas une mesure de matériel.

## 0.145.0 (GB — rejeu des films BizHawk Gambatte)

Le rejeu des films `.bk2` GB reste calé sur BizHawk Gambatte bien plus loin.
Mesuré sur `dadinfinitum-supermarioland2.bk2` (SML2, 74 317 images jouées), en
comparant à chaque image un hash de la WRAM $C000–$DFFF avec libgambatte :
la concordance passait de ~18 800 images à **41 774 images** (56 %). Observation
de l'outil, pas une mesure de matériel.

Ce qui a été corrigé :

- Le film démarre à la mise sous tension chez BizHawk : `$0100` arrive à
  l'image 334. Deepeel décale donc les masques d'entrée de 334 images.
- Horloge d'images « mode 3 » (`gbl_sync_vba_off_clock = 3`) : une image dure
  35 112 échantillons ou s'arrête au vblank, comme `gambatte_runfor`. Les
  périodes LCD éteint/rallumé comptent comme dans Gambatte.
- État initial : WRAM/HRAM issues de l'état post-boot Gambatte
  (`gambatte_init.c`), SRAM à $FF.
- Phase de la DIV : le jeu en tire sa graine aléatoire (SML2 lit DIV deux fois
  avant l'écran d'attente). Un décalage de lecture de **40 cycles** (valeur
  par défaut pour les films BizHawk ; toute valeur entre 25 et 64 donne les
  mêmes lectures sur ce film) rend la première lecture 0x1d comme Gambatte au
  lieu de 0x1c.

Limite connue : vers l'image 32 150 puis l'image 41 340, des compteurs en HRAM
(FF99, FFC0, FFC6, FFC8) s'écartent de 1 à 5 unités. Ils se résorbent la
première fois ; la seconde, la référence éteint le LCD à l'image 41 870 alors
que deepeel reste à l'écran d'attente jusqu'à l'image 51 251. Le film complet
n'est donc pas encore rejoué à l'identique.

Pistes écartées (observations de l'outil) : FFC0/FFC1 est une position de
scroll du jeu et non un compteur son ; à l'image 32 150 les traces
d'exécution (PC) des deux côtés sont identiques sur 1 113 instructions puis
divergent sur un `jr c` après `cp $48` qui dépend d'un octet de SRAM
(`$A8EF`, tampon de carte) lu à une adresse calculée depuis FFB7/FFB8 ; la
WRAM et la SRAM concordent au début de l'image, hors deux octets de pile
résiduelle (`$A8FB/$A8FC`, qui diffèrent dès l'image 18 sans conséquence
visible). La cause exacte du premier écart n'est pas élucidée.

Diagnostics (variables d'environnement, hors usage normal) :
`DEEPEEL_TAS_DUMP=<fichier>` (hash par image), `DEEPEEL_TAS_DUMPFRAME=<n>`,
`DEEPEEL_TAS_TRACEFRAME=<n>`, `DEEPEEL_TAS_DIVSKEW=<k>`, `DEEPEEL_TAS_NOINIT`,
`DEEPEEL_TAS_NOMODE3`, `DEEPEEL_DEBUG_WATCHR/WATCHW=<adresse hex>`.

Le décalage de 334 images, l'état initial Gambatte, l'horloge mode 3 et la phase
DIV ne s'appliquent qu'aux films dont l'en-tête annonce `Core Gambatte`. Le film
`mugg_masterjun` (BizHawk 2.0, cœur **GBHawk**) garde l'alignement simple et n'a
pas de référence native ici : sa fidélité n'est pas validée.

## 0.146.0 (GB — S6 : rejeu de décodeurs sur tables de flux)

Un `CALL` inconditionnel dont la routine lit, en première lecture de données ROM,
l'adresse contenue dans HL, DE ou BC à l'appel est un **décodeur** (`src/explore/streams.c`).
Deux départs observés dont les positions dans la ROM sont à pas constant (2 à 16 octets)
désignent une table de pointeurs (même banque 16 Kio) ; elle est étendue dans les deux sens.
Chaque autre entrée est rejouée : l'image machine d'avant l'appel est restaurée, le registre
reçoit le pointeur, et l'état est poussé comme fourche forcée. Les décodeurs dont les départs
grandissent sont rejoués périodiquement et à la fin de l'exploration.

Le rapport affiche `stream replay (S6)` : décodeurs, tables, candidats, rejeux en file,
saturations (plafond de 2048 candidats par décodeur, levé par `--unbounded`).
Fixture `streams` : le troisième flux, jamais appelé par la ROM, est lu grâce au rejeu.
Observation de l'outil sur Zelda (100 M de pas) : 56 décodeurs, 913 tables, 245 rejeux ;
l'inconnu résiduel passe de 53 783 à 51 837 octets. Reste : SMS/MSX/NES/PCE, bases calculées.

## 0.147.0 (SMS / GG / SG-1000 / MSX — S6)

Même détecteur de décodeurs que GB 0.146.0, porté sur l'explorateur Z80 (`src/platform/sega/explore.c`) :
`CALL nn` vers la ROM, première lecture de données égale à HL/DE/BC, deux départs à pas constant
(2 à 16 octets, même bloc physique de 16 Kio, pointeurs dans la même région CPU de 16 Kio) → table
étendue dans les deux sens, chaque autre entrée rejouée depuis l'instantané d'avant l'appel.
Le rejeu se déclenche quand la file est vide (phase de base). Rapport : ligne `stream replay (S6)`.
Test `sega_streams` : le flux jamais appelé est lu. Observation : sur deux ROMs SMS réelles
(Sonic 2 Rebirth, Ace of Aces), 8 et 16 décodeurs détectés mais 0 table trouvée au budget par
défaut — le gain réel n'est pas démontré sur ces ROMs. Reste : NES/PCE, plafond non levé par
`--unbounded` côté Z80 (cap fixe de 2048 candidats, compté).

## 0.148.0 (NES — S6)

Même détecteur sur l'explorateur 6502 : un `JSR` vers la PRG dont la première lecture de données
ROM égale un mot de page zéro (le pointeur posé par l'appelant) désigne un décodeur ; deux départs
à pas constant (2 à 16 octets, même bloc de 16 Kio, même fenêtre CPU) donnent une table de mots,
étendue dans les deux sens ; chaque autre entrée est rejouée depuis l'instantané d'avant l'appel avec
le mot de page zéro remplacé. Tables séparées octet bas / octet haut, indexées par `X` : non couvertes.
Test `nes` (`test_streams`). Observation : DuckTales — 4 décodeurs, 0 table au budget par défaut.
Rapport : ligne `stream replay (S6)`.

## 0.149.0 (PCE — S6)

Détecteur S6 sur l'explorateur HuC6280 : un `JSR` vers la ROM dont la première lecture de données
égale un mot de page zéro (WRAM `$2000-$20FF`) désigne un décodeur ; deux départs à pas constant
(2 à 16 octets, même page ROM de 8 Kio, même fenêtre CPU) donnent une table de mots, étendue dans les
deux sens ; chaque autre entrée est rejouée depuis l'instantané d'avant l'appel avec le mot remplacé,
poussée comme fourche forcée. Limites : pas de `BSR`, pas de pointeur `(zp),Y` hors page zéro, pas de
tables séparées octet bas / octet haut. Test `pce` (`test_streams`). Rapport : ligne `stream replay (S6)`.

## 0.150.0 (archive du code RAM sur MD, NES, SMS/MSX, PCE)

Module neutre `src/explore/ramarch.c` : une entrée par variante distincte `(pc, octets)` exécutée hors
ROM, avec compteur d'exécutions, source ROM éventuelle et compteur de polymorphisme (même `pc`, autres
octets). Option `--rtcode FICHIER` sur `md explore`, `nes explore`, `pce explore` et `z80 explore`
(même format que la GB : `pc octets hits rom_index`). MD : seul le code en RAM (`pc >= $E00000`) est
archivé, `pc` tronqué à 16 bits, octets relus après exécution. NES : toute exécution hors PRG, y compris
sur des chemins forcés dans des données (l'archive NES de DuckTales compte ~1400 variantes : bruit de
chemins forcés, pas du code de jeu). PCE : WRAM uniquement, source ROM par `ram_src`. Tests : `nes`,
`pce`, `sega_streams`. Observations de l'outil.

## 0.151.0 (J10 — chunk `dpTb`)

Le PNG de carte GB porte un chunk `dpTb` (JSON versionné) : les tables de pointeurs que le rejeu S6 a
localisées (`at` = index ROM compact, `stride`, `n` entrées), 128 tables au plus. Les tables de sauts S4
et les frères de copie n'y figurent pas encore ; `dpIx` reste à faire (la GB n'a pas d'enregistrements
indexés). Test `explorer` (fixture `streams`).

## 0.152.0 (J10 — chunks privés)

Les chunks binaires `dpCv`, `dpFr`, `dpTb` ne sont plus des zTXt : ce sont des chunks PNG privés
ancillaires de même nom (casse PNG respectée), contenant un flux zlib unique. `dp_png_text` lit les deux
formes, donc les anciennes cartes restent lisibles. Les chunks texte (`deepeel_meta`, `deepeel_map`,
`deepeel_heat`, `deepeel_cov`, `deepeel_rom`) restent en zTXt.

## 0.153.0 (J10 — chunk `dpCh`)

Le PNG de carte GB porte un chunk privé `dpCh` : en-tête `DPCH` (version, plateforme, nombre d'octets,
masque de sections) puis, par section non nulle, les `exec_flags`, `data_flags`, `static_flags` (1 octet
par octet de ROM) et les compteurs `exec_hits`, `data_hits`, `play_exec_hits`, `play_data_hits` (u32 LE).
Omis au-delà de ~4 Mo de ROM. Extraction : `deepeel mapx carte.png out --chunk dpCh`. Test `explorer`.

## 0.154.0 (J10 — écriture PNG incrémentale)

`dp_png_save_chunks` ne construit plus la copie brute (filtre + lignes) de l'image : chaque ligne passe
à un deflate incrémental (niveau 9) et les IDAT sortent par blocs de 64 Kio. Flux identique octet pour
octet à l'ancien `compress2`. Le tampon RVB de la grille reste complet (au plus le budget de pixels, 16 M
par défaut) : le rendu par rangée de tuiles du §9.2 n'est donc que partiel.

## 0.155.0 (J9 — multitap PCE)

Le pont PCE modélise l'adaptateur 5 manettes, désactivé par défaut (`dp_pce_multitap(1)`). Les écritures
`$1000` pilotent un balayage (CLR haut → pad 1 ; SEL montant avec CLR bas → pad suivant) ; la lecture rend
le quartet du pad courant selon SEL. C'est un modèle de l'outil, pas une mesure de matériel : le balayage
n'est pas dans le snapshot et l'exploration n'ouvre pas encore de fourches par pad. Test `pce` (`test_multitap`).

## 0.156.0 (J9 — audio typé SMS/GG/SG)

Le rapport Sega ajoute « audio (music data…) » avec le nombre d'écritures PSG (`$7E/$7F`) et YM2413
(`$F0/$F1`) observées. La puce FM n'est pas émulée : c'est une observation d'accès, pas de la musique
décodée. Test `sega_streams`.

## 0.157.0 (J9 — limites déclarées)

Le rapport PCE se termine par une ligne « Limits » (pas de CD-ROM²/ADPCM/System Card, pas de SF2, pas de
SuperGrafx, multitap en modèle optionnel) ; le rapport MSX ajoute FM-PAC, son SCC et V9938/MSX2. Ces
surfaces restent non émulées : la ligne « exigé » de la matrice (« limites ou libretro ») est remplie par
les limites, pas par une émulation.

## 0.158.0 (J9 — paquets SGB)

Un décodeur observe les écritures `$FF00` : impulsion `00` (début), 128 bits LSB d'abord (`10` = 1,
`20` = 0, chacun suivi de `30`), bit d'arrêt. Le rapport liste « SGB packets on P1 » avec le compte par
commande (octet 0 >> 3). Observation seule : aucune bordure ni palette SGB n'est rendue ni typée ; l'état
du décodeur est par explorateur (restauré par les fourches depuis 0.187.9, cf. fin du document). Test `explorer` (fixture `sgb`).

## 0.159.0 (archive RAM — événements SMC, GB)

Chaque octet de WRAM exécuté est marqué ; une écriture ultérieure dans un octet marqué compte comme un
événement SMC (W^X) : le rapport affiche « self-modifying WRAM code: N writes… (first at pc $…) »,
en plus du compteur de variantes polymorphes de l'archive. Un octet réécrit puis ré-exécuté compte à
nouveau. HRAM/VRAM ne sont pas suivis ; les autres plateformes n'ont pas encore ce compteur. Test
`explorer` (fixture `smc`).

## 0.160.0 (archive RAM — événements SMC, NES)

Même compteur que la GB (0.159.0) sur la RAM interne NES (`$0000-$1FFF`, miroirs repliés) : une écriture dans
un octet déjà exécuté est un événement SMC, rapporté « self-modifying RAM code ». Reste à porter sur
MD, PCE, SMS/MSX. Test `nes` (`test_smc`).

## 0.161.0 (archive RAM — événements SMC, PCE)

Compteur SMC sur les pages WRAM `$F8-$FB` de la PCE, pour les stores `STA/STX/STY` (zéro page et absolu)
exécutés depuis la ROM ; les stores des blocs de transfert ne sont pas vus. Reste à porter : MD, SMS/MSX.
Test `pce` (`test_ramcode`).

## 0.162.0 (archive RAM — événements SMC, SMS/GG/SG/MSX)

Compteur SMC sur la RAM Z80 des systèmes Sega et du MSX : une écriture dans un octet déjà exécuté
depuis la RAM est rapportée « self-modifying RAM code » (compte + premier pc). Reste à porter : MD.
Test `sega_streams`.

## 0.163.0 (archive RAM — événements SMC, MD)

Compteur SMC sur la RAM de travail MD (`$E00000+`, miroir 64 Kio) : un store (octet, mot ou long) dans des
octets déjà exécutés est rapporté « self-modifying RAM code ». Le compteur existe désormais sur GB, NES,
PCE, SMS/GG/SG/MSX et MD. La RAM du Z80 son MD n'est pas suivie. Test `md_explore` (fixture `fix_md_smc`).

## 0.164.0 (J11 — essai sur un vrai cœur)

`tools/lr_trial.c` charge un vrai cœur libretro (Genesis Plus GX, hors dépôt) sur une ROM fixture MD :
état sérialisé de 1 036 288 octets, rejeu de 20 trames identique après rechargement de l'état, RAM système
de 64 Kio exposée. `deepeel lr explore` refuse proprement sur ce cœur (niveau A : pas de trace) et rapporte
les dégradations. Le branchement sur les sessions de l'explorateur reste à faire.

## 0.165.0 (J11 — session libretro en ligne de commande)

`deepeel lr play CORE ROM [--frames N] [--tas FILE] [--ppm FILE]` : session sur n'importe quel cœur libretro
(niveau A suffit) : N trames avec entrées d'un fichier texte (un masque hexadécimal par ligne), sauvegarde,
rejeu, rechargement, rejeu et comparaison des hashes d'état (code retour 3 si divergence), dump PPM du
framebuffer. Essayé sur Genesis Plus GX (observation de l'outil). Test `lr_play_cli` sur le cœur factice.
Restent : branchement sur l'explorateur, Go-Explore, mode humain, `--watch`.

## 0.166.0 (J11 — Go-Explore sur cellules d'écran, libretro)

`deepeel lr novelty CORE ROM [--rounds N] [--seed N] [--hold N]` : Go-Explore sur n'importe quel cœur
(niveau A) : archive d'états sérialisés indexée par cellule d'écran (framebuffer réduit à 16×14 en luma
3 bits, haché), reprise depuis un état archivé tiré au hasard, entrées tenues `--hold` trames. Plafond de
100 000 états (dépassement compté, code retour 3). Essayé sur Genesis Plus GX avec une fixture statique
(1 cellule : attendu). Observation de l'outil ; test dans `lr_play_cli`. Restent : mode humain, `--watch`,
branchement sur l'explorateur.

Essai 0.166.0 sur jeu réel (hors dépôt) : Castle of Illusion (MD) sur Genesis Plus GX, `lr novelty
--rounds 300 --hold 30` : 7 cellules d'écran atteintes en 9,6 s, aucun état écarté. Observation de l'outil
(entrées aléatoires depuis le démarrage, sans TAS ; pas une mesure de couverture).

## 0.167.0 (J11 — mode humain libretro)

`deepeel lr watch CORE ROM [--scale N] [--max-frames N]` (builds avec SDL2) : fenêtre temps réel (~60 trames/s)
sur n'importe quel cœur ; flèches = croix, Z = B, X = A, A = Y, S = X, Entrée = start, Maj droite = select,
Q/Échap quitte. Essayé en pilote vidéo `dummy` sur le cœur factice et Genesis Plus GX (60 trames) ; la
jouabilité réelle au clavier n'a pas été éprouvée ici. Test dans `lr_play_cli`. Restent : branchement sur
l'explorateur, mode `--watch` d'une exploration en cours.

## 0.168.0 (correctif : SIGINT sur `explore`)

Sans `--play`, `deepeel explore` n'installait aucun gestionnaire de SIGINT : un SIGINT tuait le processus
sans rapport ni état (deux runs `--unbounded` de ~2 h 30 ont ainsi été perdus). Le gestionnaire pose
désormais un drapeau persistant (`dp_explore_interrupt`) que la boucle d'exploration teste ; les phases
qui remettent `ex->stop` à zéro ne l'effacent plus. Vérifié : run `--unbounded` interrompu au bout de
20 s, rapport écrit, processus terminé. Le `--time-limit` n'est pas appliqué hors `--play` : à traiter.

## 0.169.0 (`--time-limit` global, `--jobs auto`)

- `--time-limit N` s'applique maintenant à toute exploration (alarme + arrêt propre identique à SIGINT),
  plus seulement à `--play`.
- `--jobs auto` : un explorateur par cœur libre (tous sauf deux), borné par la RAM disponible
  (~1,5 Gio par explorateur) ; choix fait une fois au lancement, pas réajusté en cours de route.
  Essai de 15 s sur Mario Land 2 en `--unbounded` : 1 explorateur 870 890 pas, `auto` (6 explorateurs, limité
  par la RAM) 1 863 201 pas (observations de l'outil, un seul essai chacun). Correction d'une affirmation
  antérieure : le disque des runs est un NVMe, pas un support externe lent ; la cause du CPU à 38 % n'est pas établie.
Reste : réglage dynamique du nombre d'explorateurs, tampon RAM de tête de spool.

## 0.170.0 (J12 — premier cœur réel de niveau B : Genesis Plus GX patché)

`tools/gpgx_trace.patch` (181 lignes, s'applique sur le dépôt Genesis Plus GX, cœur hors dépôt) ajoute
l'extension `DP_LR_ENV_TRACE` : pas à pas du 68000 (`dp_m68k_step`), observateurs exec/read/write via le
`cpu_hook` existant (build `make -f Makefile.libretro HOOK_CPU=1`), observateur de branchement et
**forçage des conditionnels** en enveloppant les macros `COND_*` de Musashi (Bcc, DBcc, Scc : « pris » = le
corps du `if` s'exécute), index ROM = adresse. Correction d'un défaut de l'amont : `cpu_hook` défini dans
l'en-tête (échec d'édition de liens avec gcc récent), maintenant `extern`.
Résultat (observation de l'outil) : sur `fix_md_fork`, `deepeel lr explore` négocie le niveau B, trouve 1 site
de branchement, 2 fourches et atteint les 6 débuts d'opcode attendus (les deux côtés du `bne.s`).
Test `lr_gpgx_trace` (variable `DP_GPGX_TRACE_CORE`, ignoré sans cœur). Limites : pas d'observateurs
`dma`/`bank` ; pendant un pas, le VDP, les IRQ et le Z80 n'avancent pas (seul `retro_run` les fait avancer) ;
parité avec le backend m0boy vérifiée sur une fixture seulement, pas sur les 11 de GB.

## Campagne J13, Zelda (Link's Awakening FR), binaire 0.169.0 — observations de l'outil

Run `--unbounded --jobs auto --time-limit 3600` (6 explorateurs, arrêt propre à la limite, rapport écrit) :
311 580 269 pas, 31 323 chemins, 753 558 fourches (303 208 « key »), 7 302 sections ; résidu inconnu
58 050 octets en 2 202 plages. Spool : 64 887 467 états écrits, **26 705 relus**, 21,8 Gio en attente — le
moteur crée des fourches bien plus vite qu'il n'en consomme, ce qui explique la taille du fichier.
Exécution hors ROM : 257 633 pas, dont 3 468 rattachés à une source de copie ROM (≈ 1,3 %) ; archive de code
RAM : 16 273 instructions distinctes exécutées depuis la RAM, 1 rattachée à une copie ROM, 16 272 « générées »
(9 avec producteur ROM approximatif), 8 auto-modifiées ; 25 096 écritures SMC.
**À ne pas comparer tel quel** au run antérieur de `output/batch/zelda` (19,15 milliards de pas, résidu
43 171 octets, 484 M pas hors ROM) : budgets et configuration différents, 60× moins de pas ici. Le résidu plus
grand ici vient donc d'abord du budget ; la part de « générées » inclut probablement du code exécuté dans la
WRAM après des fourches « key » (états de jeu non naturels), ce que ce rapport ne distingue pas.

Campagne J13, Super Mario Land 2, même binaire et mêmes options (1 h, observations de l'outil) : 228 364 648 pas,
18 004 chemins, 732 145 fourches (198 512 « key »), 7 903 sections ; résidu inconnu 68 578 octets en 2 902
plages (run antérieur `output/batch/marioland2` non relu ici). Spool : 71,5 M états écrits, 18 098 relus,
22,9 Gio en attente. Hors ROM : 1 023 294 pas, 1 550 rattachés à une copie ROM (≈ 0,15 %) ; archive RAM :
24 899 instructions distinctes, 12 rattachées à une copie ROM, 24 887 « générées », 282 auto-modifiées ;
240 235 écritures SMC (premier au pc $0048). Même réserve que pour Zelda sur les fourches « key ».

## 0.171.0 (moteur : le spool ne sérialise plus les explorateurs)

Profil `perf` (`--unbounded --jobs 6`, Zelda) : `queue_push_pri` 27 % et `adler32_z` (zlib) 14 % du CPU, les six
threads à ≈ 25 % chacun. Cause : quand le pool partagé débordait, `spool_write_rec` — compression zlib puis
écriture — s'exécutait **sous le mutex du pool**, donc un explorateur à la fois. Correctif : le mutex du pool
est relâché avant l'écriture, et la compression est faite avant de prendre le mutex du spool (seul `fseek`/
`fwrite` reste sérialisé). Observation de l'outil sur 60 s de Zelda : 18,0 M pas contre 11,8 M avec 0.169.0
(+53 %), 2,24 M états écrits contre 1,07 M. Le passage à l'échelle reste partiel (écriture et ensemble exact
partagé toujours sous verrou) ; piste suivante : tampon RAM de tête de spool et ensemble exact découpé.

## 0.172.0 (J12 : borne de boucle du mode `lr explore`)

`deepeel lr explore` coupe désormais un chemin quand un même pc est exécuté plus de `--loop-bound N` fois (3 par
défaut), compteur `loop bound cuts` rapporté. Avec le cœur Genesis Plus GX patché (observation de l'outil) :
`fix_md_loop` passe de 100 000 pas à 4 (1 coupe), `fix_md_dbcc` atteint la sortie de boucle (4 débuts d'opcode),
`fix_md_fork` garde ses 2 fourches et ses 6 débuts d'opcode. `DBF` n'est pas un conditionnel de Musashi
(`dbra`) : il n'est pas forcé, aucune fourche n'est attendue sur `fix_md_dbcc` ; parité de contrat avec le
backend `m0boy` sur ces trois fixtures, pas de parité d'état complète.

## 0.173.0 (moteur : écritures de spool consécutives sans `fseek`)

Chaque `spool_write_rec` faisait un `fseek` vers la fin, ce qui vide le tampon stdio à chaque enregistrement
même quand la position est déjà bonne. Le spool retient maintenant la position après la dernière écriture
(`wpos`) et ne repositionne qu'après une lecture. Observation de l'outil, 60 s de Zelda, 6 explorateurs :
36,9 M pas (18,0 M en 0.171.0, 11,8 M en 0.169.0), 5,64 M états écrits. Le spool grossit à ≈ 2 Gio/min dans ce
régime : le garde-fou disque reste nécessaire pour les runs longs. Pas encore fait : tampon RAM de tête de
spool (les lectures ne sont pas le goulot : 7 798 relus contre 5,64 M écrits) et ensemble exact découpé.

## 0.174.0 (J13 : le rapport sépare le code RAM « naturel » du code atteint après fourche)

La ligne `runtime code archive` est suivie de `generated code by reachability` : parmi les instructions RAM sans
source ROM, combien ont été exécutées au moins une fois sur un chemin **non forcé** (`!cur_forced`), combien
seulement après une décision de fourche (branche, IRQ, key, P1). Observation de l'outil, 60 s de Zelda
(0.174.0) : 16 085 « générées », **0 sur chemin non forcé**, 16 085 seulement après fourche. Lecture honnête :
dans ce budget, aucune exécution RAM sans source n'est atteinte sans décision forcée ; cela ne dit pas que le
jeu n'exécute pas de code RAM en jeu réel (le chemin naturel de 60 s n'atteint peut-être pas la routine
OAM-DMA en HRAM), ni que ce code est faux : il est atteint depuis un état contrefactuel. À relire sur un run long.

## Divergence TAS Gambatte : état au 0.174.0 (observations de l'outil)

Film `dadinfinitum-supermarioland2.bk2`, comparé à `libgambatte` de BizHawk (callback de trace de registres :
cycle, PC, SP, A, B, C, D, E, F, H, L) :

- **Image 32 150 (note de 0.145.0) : ne se reproduit plus.** Les 2 957 instructions de l'image concordent sur PC,
  A, BC, DE, HL et SP ; la WRAM concorde aux images 32 140, 32 151 et 32 160. La divergence `jr c` sur `$A8EF`
  décrite plus haut vient d'états antérieurs du moteur (corrigés depuis) ou d'un mauvais alignement des flux.
- **Première divergence de WRAM : image deepeel 41 775 (BizHawk 42 109).** À 42 108 la WRAM est identique ; à
  42 109, 734 octets diffèrent : la référence a rempli `$C000…` de `$36` (tampon vidé) là où deepeel garde encore
  d'anciennes données. C'est une routine de remplissage qui s'exécute une image plus tôt (ou plus tard) côté
  référence, cohérent avec l'extinction du LCD de la référence à l'image 41 870 vs 51 251 chez deepeel :
  un écart de phase image/vblank pendant une période LCD éteint, pas une erreur de CPU. Non élucidé.
- Diagnostic : la trace `DEEPEEL_TAS_TRACEFRAME` est préfixée `<variante>.<machine>` pour isoler le flux du film
  des autres machines de l'exploration (le fichier mélange sinon plusieurs machines).

## 0.175.0 (J11 : `lr novelty --watch`)

`deepeel lr novelty CORE ROM --watch SCALE` ouvre une fenêtre SDL2 rafraîchie depuis le framebuffer du cœur à
chaque saut d'exploration : on regarde la recherche Go-Explore sur écran en cours. Q/Échap ferme l'affichage,
l'exploration continue. Sans GUI, l'option est acceptée et ignorée. Limite : l'exploration à fourches
(`lr explore`, niveau B) avance le CPU instruction par instruction sans produire de vidéo, donc elle n'a pas de
vue en direct ; son branchement dans l'explorateur complet (typage, copies, longueurs d'instruction) reste à faire.

## 0.176.0 (J10 : chunk `dpIx`)

Le PNG de carte GB porte un chunk privé `dpIx` (JSON) : pour chaque site de code qui lit la ROM comme donnée
(table lock-free de 2048 sites, débordement compté dans `dropped`), l'index ROM du site, la plage d'index ROM lue
(`kmin`…`kmax`) et le nombre de lectures. C'est une observation par site : aucune base ni aucun pas n'est inféré
(les `DpIndexed` complets de COUVERTURE §5 — base+index, `idx_max`, déclaration `index-inference` — restent à faire).
`deepeel mapx --chunk dpIx` l'exporte.

## 0.177.0 (`dpIx` : pas observé)

Chaque site de `dpIx` porte `stride` : le PGCD des écarts successifs entre index ROM lus par ce site (0 = un seul
index distinct vu). Pour une table lue entrée par entrée c'est le pas de la table ; sous concurrence la valeur est
approchée (observation de l'outil). La base reste `kmin` ; la déclaration `index-inference` n'est pas faite.

## 0.178.0 (`dpCh` : sources)

`dpCh` gagne la section 7 (bit 7 du masque) : `src_flags` u32 par octet de ROM, toutes les sources qui prouvent
l'octet (`DP_SRC_*` : fourche naturelle/forcée, session, …). Avec les drapeaux et les compteurs de coups déjà
présents, `dpCh` porte la nature d'exécution/données/statique, les sources et les hits de §9.3 ; les « types »
par octet (`dpCv` §9.3) restent absents du chunk.

### Divergence TAS à l'image 41 775 : ce que la trace de registres montre (observations de l'outil)

Trace BizHawk (libgambatte) de l'image 42109 : 10 217 instructions, dont 1 413 tours d'une boucle de remplissage
`$039E…$03A2` (l'image démarre au milieu de la boucle, SP=`$A8FB`) ; aucune touche pressée (42104…42111 vides).
La trace deepeel de l'image correspondante (`DEEPEEL_TAS_DUMP` + `DEEPEEL_TAS_TRACEFRAME=41774`) ne visite jamais
`$03A1` : ses flux de ~3 000 instructions sont d'autres chemins. Donc, à WRAM identique à 42108, la référence est au
milieu d'un remplissage que deepeel n'a pas (ou pas à cette image) : la différence est d'état matériel/temps (position dans
l'image, LCD éteint, IME/IF), pas de touches. Cause non identifiée ; prochaine étape : tracer aussi l'image 42108 des
deux côtés et comparer le PC de fin d'image.

## 0.179.0 (`--mapwin` : la carte de la ROM en direct)

`deepeel explore ROM --mapwin [ECHELLE]` ouvre une fenêtre SDL2 qui montre la ROM comme le PNG de carte : une tuile
128×128 par banque (1 pixel par octet, 8 banques par ligne), la même palette de classes, une ligne de légende avec
les pourcentages (connu / inconnu / par classe), les pas et les chemins. Un thread dédié reconstruit la carte depuis les
résultats vivants environ deux fois par seconde : on voit le rouge (inconnu) se résorber pendant l'exploration, avec ou
sans `--play`, `--jobs` ou `--unbounded`. Lecture seule : fermer la fenêtre (Q/Échap) n'arrête pas l'exploration. Sans
échelle, l'échelle entière maximale qui tient dans l'écran est choisie. Crochet de test : `DEEPEEL_MAPWIN_DUMP=<fichier>`
écrit la dernière carte dessinée en PPM (test `mapwin_cli`). Sans SDL2 l'option est signalée et ignorée.

## 0.180.0 (`--mapwin` : l'écran de jeu à côté de la carte)

Avec `--play` et un film `--tas`, `--mapwin` affiche aussi l'écran de la Game Boy (160×144 en ×2, quatre nuances) à droite
de la carte, avec le nom du film et sa position (film i/n, image/total). L'écran se rafraîchit environ quatre fois par
seconde, la carte environ une fois par seconde. Sans film, la carte seule. Une exploration peut être arrêtée (SIGINT) et
reprise avec `--state-dir D --resume` : la fenêtre se rouvre avec la même option.

### Observation TAS SML2 : WRAM identique, HRAM différente (image 41774 deepeel / 42108 référence)

Dump complet à l'image 41774 côté deepeel et 42108 côté BizHawk/gambatte (décalage de 334 images) : les 8 Kio de WRAM
sont **identiques octet pour octet**, mais 22 octets de HRAM diffèrent (compteurs et état temporaire : `$FF97`, `$FF99`,
`$FFB7-$FFB9`, `$FFC0-$FFCE`, `$FFF0`, `$FFFF`), ainsi que la RAM de cartouche (782 à 7 387 octets par banque). Le
hash de WRAM[:$F00] reste constant des deux côtés sur des dizaines de milliers d'images (attente) : le décalage de 334
images est donc ambigu, et la divergence observée tient à la sortie de cette attente (référence à l'image 42109, en plein
remplissage `$039E-$03A2` ; deepeel reste en `pc=$0300` jusqu'à l'image 51252). Observation de l'outil, cause non
établie : il faut tracer ce qui fait sortir la référence de l'attente (état de `IE/IF/$FFFF`, DIV/timer, HRAM).

## 0.181.0 (vitesse du mode `--unbounded --play`)

Profil du thread principal (`perf`, Zelda `--unbounded --play --jobs auto`) : un seul thread travaillait (94 % de CPU),
les autres explorateurs attendaient la barrière d'époque pendant que le meneur exécutait les pas de la fourche, hachait
chaque image d'état (FNV-1a octet par octet, 27 %) et compressait chaque état débordé vers le spool (zlib, `adler32` 20 %).
Changements : hachage d'image par blocs de 8 octets (l'ensemble exact n'est pas persisté, la fonction peut changer) ;
spool et frontière disque en deflate brut sans en-tête ni `adler32` (**les `spool.bin` / frontières d'une version
antérieure ne sont plus relisibles**) ; en `--unbounded`, les pas de la fourche tournent après la libération des
sessions, donc en même temps que l'époque suivante (la reproductibilité à graine fixe n'est pas promise dans ce mode).
Le rejeu du film Zelda (`bobmario511-zeldagb-playaround.vbm`) seul donne les mêmes images avec 0.25.0 et 0.181.0 :
le suivi des films n'a pas régressé. Mesures : observations de l'outil.

## 0.181.1 (profil VBA 1.7.2 : RES/SET b,(HL))

La table de cycles du cœur V7 de VBA (`gbCyclesCB`) facture `RES`/`SET b,(HL)` 3 cycles machine au lieu de 4 ; le
cœur V8 l'a corrigé. Le profil VBA 1.7.2 (`gbl_sync_vba_off_clock == 2`, `cmake/m0boy-tas-compat.patch`) reproduit
maintenant ce défaut, comme le reste de la table. Aucun effet visible sur le film Zelda
`bobmario511-zeldagb-playaround.vbm` : les images de la trame 1800 à 4500 sont identiques avant et après.

**Film Zelda, état de l'observation** : après « Shield back! », Link arrive sur la plage, reste presque immobile
contre un mur (trames ~2300–3400) puis change d'écran sans jamais prendre l'épée (emplacement A vide). Les
versions 0.25.0 et 0.181.1 rejouent la même chose, donc ce n'est pas une régression ; l'origine (désynchronisation
du cœur V7, ou film qui joue vraiment ainsi) n'est **pas établie** faute d'un VBA 1.7.2 de référence compilé
sans affichage (l'arbre `vba-rerecording` n'a pas le script de construction Unix, la maquette précédente était
temporaire). Prochaine étape : reconstruire cette référence et comparer les hachages de WRAM par trame.

## 0.181.2 (`--mapwin` : repeinture incrémentale)

La fenêtre `--mapwin` garde les classes déjà peintes et ne repeint, à chaque rafraîchissement, que les octets dont la
classe a changé (le tampon RVB persiste). `dp_map_build` recalcule encore la carte entière toutes les ~1 s : le calcul
incrémental de la carte elle-même (bitmap de pages sales) reste à faire.

### Film Zelda contre les cores libretro (observation, sans changement de code)

Rejeu des entrées de `bobmario511-zeldagb-playaround.vbm` (ROM USA/Europe rév. 0, somme `47B7` recalculée = celle du
film) dans `gambatte_libretro` et `sameboy_libretro` (une trame de film par `retro_run`, sans boot ROM) : à la trame 1800
Gambatte est encore à la saisie du nom, SameBoy à l'écran-titre puis à la saisie, alors que deepeel (profil VBA 1.7.2)
est dans la maison après « Shield back! ». Aucun des deux cores précis ne suit ce film : il dépend de la temporisation
de VBA 1.7.2 (démarrage, introduction), donc ce test ne départage pas « désynchronisation de deepeel » et « film qui
joue vraiment ainsi ». Seul un VBA 1.7.2 de référence le ferait.

## 0.181.3 (`--unbounded --play` : les films ne attendent plus la fourche)

Observation (SML2, deux films, `--unbounded --play --mapwin`) : le thread principal tournait à 99 % (deflate du
spool ≈ 50 %, `queue_push_pri` 12 %) et les sessions de film restaient à l'arrêt à la barrière d'époque : 3 600 trames
de film en 5 minutes. Correctifs : les pas de la fourche, recouverts avec l'époque suivante, s'arrêtent dès que les
sessions ont fini (la barrière n'attend plus ce thread) ; le spool compresse en `Z_RLE` (images machine surtout
nulles). Résultat observé sur la même campagne : plus de 9 000 trames de film en 90 s, sessions à ~99 % de CPU.
La fourche avance donc surtout après la fin des films (vidage parallèle). Le compteur « unknown » peut remonter : la
passe d'inférence (`map.c`) classe un trou selon ses voisins (code–code ≤ 3 octets, données–données ≤ 1024, remplissage) ;
un nouvel octet de code découvert au milieu d'un trou supprime l'inférence des deux moitiés, qui redeviennent inconnues.

## 0.181.4 (`--mapwin` suit un film encore en cours)

Quand le film affiché se termine (le film SML2 `mugg_masterjun_alyosha` n'a que 2 752 trames : c'est un enchaînement
court qui finit aux crédits, pas une troncature), la fenêtre passe au film suivant encore en cours au lieu de
figer sa dernière image (2704/2752).

## 0.182.0 (`deepeel bh` : référence BizHawk libgambatte ; correctif du LogKey BK2)

**Correctif (le film `mugg_masterjun` n'était pas joué correctement depuis la 0.3.0).** Les BK2 de BizHawk 2.x
écrivent le `LogKey` « `#P1 Up|P1 Down|…` ». Le chargeur coupait aussi sur l'espace : « P1 » devenait une touche à part
et les caractères de chaque trame étaient décalés (812 trames « appuyées » au lieu de 1 220 sur ce film). Le
découpage se fait maintenant sur `|` seul, le préfixe `P<n> ` est retiré. Test : `tas_logkey` (échoue sans le
correctif). Les films au `LogKey` sans préfixe (par exemple `dadinfinitum`) n'étaient pas touchés. Tout résultat
obtenu avec un BK2 à préfixe `P1` avant cette version est à refaire.

**`deepeel bh LIBGAMBATTE ROM --tas FILE`.** Rejoue un film sur le `libgambatte.so` de BizHawk
(`BizHawk/Assets/dll/`), chargé par `dlopen` (jamais copié dans le dépôt). L'API C de cette bibliothèque porte les
rappels d'exécution, de lecture/écriture et de CD-logger : c'est un moteur indépendant de m0boy. Sorties :
`--cov` (drapeaux par octet de ROM : 1 opcode, 2 opérande, 4 donnée), `--ranges` (plages exécutées `banque:adresse`),
`--ppm`, `--shot-every N PREFIXE`, `--bios`. Observation (SML2, `mugg_masterjun`, 2 752 trames, sans boot ROM,
d'après l'outil) : 15 148 octets de ROM exécutés dont 7 894 opcodes. Limite : ce film a été enregistré avec le cœur
GBHawk (`Core GBHawk` dans l'en-tête), pas avec Gambatte ; Gambatte le suit sans garantie d'exactitude trame à trame.

## 0.182.1 — film GBHawk : profil de boot DMG, trace de registres

- Un BK2 déclarant `Core GBHawk` avec boot ROM DMG reçoit le même alignement de trame que Gambatte
  (334 trames de boot retirées). Avant, le film `mugg_masterjun` (SML2) partait avec 334 trames de décalage.
- `DEEPEEL_TAS_REGS=<fichier>` : une ligne `trame pc sp a bc de hl` par vblank du film (worker TAS).
- Comparaison avec la trace GBHawk (`tools/gbhawk_ref`, 2 752 trames) — observation de l'outil, non concluante :
  à décalage 333 le SP coïncide sur de longues plages (p. ex. trames 531–1186) avec des écarts isolés ; les
  captures d'écran coïncident parfois (trame 450 : 0 pixel de différence) et divergent ailleurs. L'échantillonnage
  du registre au vblank dépend de la phase exacte du cycle : la première divergence réelle n'est pas établie.

## 0.183.0 — NES : source ROM dans l'archive `rtcode`

Le code exécuté en RAM ($0000–$07FF) porte désormais, dans l'archive `--rtcode` NES, l'index ROM de la dernière
lecture PRG ayant alimenté le store de son premier octet (`-1` sinon). Provenance approximative (même heuristique
que le rattachement des copies NES), pas une dépendance exacte.

## 0.184.0 — SMS/GG/SG/MSX : source ROM dans l'archive `rtcode`

Le code exécuté hors ROM renseigne l'index ROM du premier octet quand la provenance Z80 (copie contrôlée par
valeur, `valid_origin`) le rattache à la ROM ; `-1` sinon. Les tests existants passent ; aucune fixture SMS/MSX
n’affirmait la valeur de la source en 0.184.0 ; le test `rtcode_sms_source` (0.184.1, fixture `fix_sms_ramsrc.sms`) épingle `c000 3e42 → ROM 512`.

## 0.185.0 — MD : source ROM dans l'archive `rtcode`, octets RAM corrigés

- Une écriture en RAM 68000 du même `MOVE` qui vient de lire la ROM (valeur contrôlée octet par octet) enregistre
  l'index ROM source ; l'archive `--rtcode` MD le porte pour le premier octet du code exécuté en RAM (`-1` sinon).
- Correctif : les octets archivés du code RAM venaient d'un `peek` de la ROM (donc faux, souvent 0) ; ils sont lus
  dans la RAM de travail. Fixture `fix_md_ramsrc.md`, test `rtcode_md_source`.
- Seules les copies d'un seul `MOVE` (ROM → RAM, même taille) sont rattachées ; `MOVEM`, blocs DMA et copies
  transformées ne le sont pas.

## 0.186.0 — archive neutre `rtcode` : colonne producteur (MD, NES)

Le fichier `--rtcode` des plateformes à archive neutre gagne une 5e colonne `producteur` : dernier octet ROM lu
avant le store de l'octet (approximation, comme `rt_derived` sur GB), `-1` sinon ; il vaut la source quand la copie
est verbatim. Câblé sur MD et NES ; SMS/MSX/PCE écrivent `-1`. Le compteur `derived` de l'archive existe mais
n'est pas encore rapporté dans le texte. Les tests `rtcode_md_source` (producteur = source sur copie verbatim) et
`rtcode_sms_source` suivent le nouveau format ; aucune fixture de code généré (non verbatim) sur MD/NES encore.

## 0.186.1 — MD : code généré, producteur rapporté

Le rapport MD ajoute `runtime code archive: N variants, A attached…, D generated with an approximate ROM producer…`.
Fixture `fix_md_ramgen.md` (mot ROM lu, inversé, stocké en RAM + `rts` immédiat), test `rtcode_md_generated`.
Limite constatée, épinglée par le test : le producteur est le dernier octet ROM lu avant le store, donc l'`rts`
stocké en immédiat reçoit aussi `1024` alors qu'il n'en dérive pas. Seule une provenance de dépendances exacte
(J14, reste ouvert) lèverait cette imprécision.

## 0.186.2 — NES : résumé de l'archive rtcode dans le rapport

Même ligne que MD (`runtime code archive: …`). Pas de fixture NES de code généré encore.

## 0.187.0 — MD : producteur par propagation à travers les registres

Le producteur de l'archive `rtcode` MD n'est plus « le dernier octet ROM lu » : une teinte (index ROM) suit D0–D7/A0–A7
et la RAM de travail, propagée instruction par instruction d'après le texte du désassemblage (`move`/`movea`,
`moveq`/`clr`, `lea`, ALU à deux opérandes, formes à un opérande ; `movem` et les pushs ne propagent rien). Le `rts`
stocké en immédiat n'a plus de producteur (`-1`), le mot ROM lu puis inversé puis stocké garde `1024`.
Limites : un seul index représentatif (pas l'ensemble des dépendances) ; modes d'adressage lus seulement comme
texte ; teintes des registres non remises à zéro entre chemins ; `movem`, `DIV/MUL` à paires, `exg`, `swap`
inter-registres non modélisés. J14 « provenance exacte » reste donc ouvert sur les autres plateformes et sur ces cas.

## J11/J12 : adaptateur gambatte niveau B (exécuté)

`tools/gambatte_trace.patch` (observation/forçage des 16 conditionnels de `cpu.cpp`, gambatte-core de
pokemon-speedrunning) et `tools/gambatte_lr.cpp` (adaptateur `DpLrTraceApi` en processus, démarrage `NO_BIOS`)
ont été exécutés sur les fixtures de `test_lr_parity` avec le pilote de fourches `dp_lr_explore` (observations de
l'outil). Un pas = un `runFor` d'un échantillon ; sur HALT l'horloge avance (borne ~4 trames) jusqu'au réveil ; le
prefetch du HALT n'est pas compté comme exécution. Débuts d'opcode ≥ $0100, gambatte contre m0boy :
`branch` 20/19, `loop` 3/2, `copy1` 13/12, `copyx` 14/13, `unreach` 5/4, `pal` 5/4, `jumptable` 17/16,
`nsdata` 28/27, `vblank` 28/27, `probes` 12/11 : l'écart constant de 1 est le `nop` en $0100, exécuté par gambatte
(PC initial $0100) et non compté par le parité m0boy. `irq` : 32 431 débuts, et ce n'est pas un défaut de l'adaptateur : gambatte émet de vrais VBlank, donc le HALT en
$0158 se réveille, le gestionnaire revient en $0159 et la traîne de `nop` du remplissage s'exécute jusqu'à la fin de
la ROM (le parité m0boy ne la voit pas : pas de second VBlank, et il exclut le remplissage par `DP_SF_PAD`). Fourches de gambatte :
`branch` 6, `copy1` 4, `jumptable` 2.
`rom_index` suit les banques (`GB::dpRomIndex` ajouté au patch) : sur `mbc5` et `closure` les débuts de banque 2/3
sortent à $8000/$C000, l'index compact de l'explorateur. La valeur lue est transmise (`GB::dpPeek`, sans effet de bord, ROM/WRAM/HRAM seulement ; 0 ailleurs, et 0 pour les écritures). Test : `lr_gambatte_parity` (ctest) compare les décomptes de débuts d'opcode ci-dessus ; il se saute (code 77) sans
`DEEPEEL_GAMBATTE_LR` (binaire produit par `tools/gambatte_lr_build.sh DIR`, qui clone le cœur et applique le patch).
Les callbacks `dma` (OAM DMA par $FF46, HDMA/GDMA à `Memory::dma`) et `bank` (`setRombank0`/`setRombank`) sont branchés
(`dp_trace.h` dans le patch) : `fix_dma` voit 1 copie, `mbc5` 3 bascules, `closure` 16 ; `fix_hdma` en voit 1 avec `GLR_CGB=1`
(cause du cas précédent : l'en-tête de la fixture sans drapeau CGB mettait le cœur en compatibilité DMG, où le HDMA est
désactivé ; l'adaptateur pose alors le drapeau dans sa copie de la ROM). Le callback `bank` signale aussi VBK (fenêtre 2) et SVBK (fenêtre 3), observés
seulement en mode CGB (fixture `fix_cgbbank`, 2 changements ; m0boy, DMG, n'en voit aucun). Limites : pas de parité de copies/banques contre
m0boy, seulement des décomptes. J11/J12 restent donc ◐.

## 0.187.3 — rapport du niveau B : copies matérielles et bascules de banque

`dp_lr_explore` branche enfin les observateurs `dma` et `bank` : le rapport
`--libretro` affiche « hardware copies seen » et les bascules par fenêtre
(rom0/romx/vram/wram). Sur gambatte en mode CGB : `fix_dma` 1 copie, `fix_hdma`
1 copie, `fix_cgbbank` vram 1 / wram 1, `mbc5` romx 3. Observations de l'outil,
cumulées sur tous les chemins ; ce ne sont pas encore des enregistrements
typés dans l'archive de copies. m0boy reste DMG-only par conception (Gamebuino
Meta, 32 Ko de RAM) : un éventuel CGB dans m0boy devra être une option de
compilation qui laisse la cible DMG inchangée.

## 0.187.4 — banques VRAM/WRAM sélectionnées

Le rapport du niveau B ajoute deux masques : banques VRAM (VBK) et WRAM (SVBK) sélectionnées sur l'ensemble
des chemins (`fix_cgbbank` : VRAM 02, WRAM 08). Test `lr_gambatte_parity` étendu. Observation de l'outil ; les copies
ne sont pas encore étiquetées par banque.

## J11/J12 : parité des copies tuple à tuple

L'adaptateur gambatte imprime chaque copie matérielle (`GLR_COPIES=1`). Les tuples source→destination→longueur
sont comparés à ceux de l'explorateur m0boy et épinglés dans `lr_gambatte_parity` : OAM DMA `$3000→$FE00` (160 octets)
et HDMA `$4000→$8000` (256 octets), identiques des deux côtés. La destination HDMA de gambatte, relative à la VRAM,
est rebasée en `$8000` dans le patch. Reste : banques ROM comparées une à une (seuls les décomptes le sont) et
fixtures à plusieurs copies.

## J11/J12 : parité de la séquence de banques ROM

Sur `fix_mbc5`, la séquence de banques de la fenêtre ROMX vue par gambatte (après la banque initiale 1 posée au chargement)
est `2 3`, celle que `explore` consigne pour m0boy (`romN 0 -> 2`, `romN 2 -> 3`). Épinglé côté gambatte dans
`lr_gambatte_parity`, côté m0boy par `test_explorer`. Fixture `fix_mbc1m1` ajoutée (voir « Écart MBC1 mode 1 »).

## 0.187.5 — courses de données de `--jobs` (trouvées par la campagne J13)

Un run `--jobs 4` sur Super Mario Land 2 abortait par intermittence (`double free or corruption`, 2 lancements sur 8
à 15 s). Compilé avec AddressSanitizer (`-Wl,--allow-multiple-definition`, `ASAN_OPTIONS=detect_odr_violation=0`),
deux défauts distincts apparaissent :

- **Tables de hachage partagées** (`fork_counts`, `callers`, `sites`) : les explorateurs de vidage les agrandissaient
  sans verrou et l'un libérait le tableau que l'autre lisait. Un verrou par table ; les tableaux remplacés par une
  croissance sont conservés jusqu'à `dp_map_free` (un compteur incrémenté sur un tableau périmé juste après une
  croissance est perdu : borné et rare). La liste d'appelants se modifie sous le verrou de sa table.
- **Masques de vivacité des sections** (`dp_section_live_*`) : mise à jour du masque et reconstruction de `ctx_seen`
  sans verrou, d'où un `double-free`. Chemin rapide sans verrou quand le bit est déjà posé ; sinon verrou, revérification,
  une seule reconstruction.

Après correction : 0 échec sur 12 lancements, 8 lancements sous AddressSanitizer sans erreur, 125 tests verts.
Les campagnes J13 antérieures en `--jobs` pouvaient donc être faussées par ces courses.

## J13 : campagne sur ROMs réelles, GB (observations de l'outil)

Binaire 0.187.5, `--jobs 4 --time-limit 240`, ROMs de `m0boy/roms` (hors dépôt), machine concrète DMG, RAM à zéro,
registres événementiels statiques. Ce sont des observations de l'outil, pas des mesures de matériel ; les trois runs
s'arrêtent au mur de temps, donc ce sont des bornes basses de couverture, pas des fermetures.

| ROM | pas | chemins | forks (branche / irq / touche / p1) | sections | exécution hors ROM (rattachée à une copie ROM) | résidu inconnu |
|---|---|---|---|---|---|---|
| Super Mario Land 2 | 80,0 M | 12 080 | 214 628 (10 866 / 3 / 201 263 / 2 496) | 8 773 | 1 162 167 (2 109) | 65 333 o en 2 790 plages |
| Tetris DX (mode compat. DMG) | 160,0 M | 83 330 | 287 050 (9 959 / 443 / 273 400 / 3 248) | 7 621 | 18 787 660 (278 972) | 58 313 o en 1 983 plages |
| Pokémon Jaune (mode DMG) | 500,8 M | 40 848 | 119 527 (20 431 / 73 / 95 823 / 3 200) | 17 026 | 27 025 048 (194 231) | 149 263 o en 2 159 plages |

Saturations déclarées, jamais silencieuses : 4 / 8 / 26 murs de pas, 201 348 / 193 540 / 61 870 débordements de file,
18,5 M / 14,9 M / 67,5 M forks plafonnés par site. Les trois ROMs sont donc loin d'être épuisées.

Limites : m0boy est DMG-only, donc Tetris DX et Pokémon Jaune sont explorés dans leur mode de compatibilité DMG
(pas de VBK/SVBK/HDMA réels) ; le niveau B gambatte, borné par défaut, sature très tôt sur ces deux ROMs
(884 / 385 pas). Cette campagne a révélé les courses de données de `--jobs` corrigées en 0.187.5.
Reste pour J13 : MD, NES, PCE, MSX sur ROMs réelles, et l'audit complet de la matrice §2.1.

## Écart MBC1 mode 1 : fenêtre ROM0 (J11/J12, trouvé par fixture)

`fix_mbc1m1` (1 Mio, MBC1) écrit `$6000=1` (mode 1) puis `$4000=1` : la fenêtre ROM0 (`$0000-$3FFF`) affiche alors la
banque `$20` et ROMX la banque `$21`. Observation de gambatte (`lr_gambatte_parity`, assertion fenêtre ROM0 = 32) :
conforme. Observation de m0boy/deepeel : les écritures `MODE` et `RAMB` sont vues (2 chacune) mais l'exécution reste
dans la banque 0 (13 octets de code en banque 0, 2 sections) : **le remappage de la fenêtre ROM0 n'est pas modélisé**,
donc le code placé en banque `$20/$40/$60` d'une grosse ROM MBC1 est mal attribué. Écart réel, non corrigé : la
correction touche le runtime généré de m0boy (hors de ce dépôt). Le test ne fige pas le comportement fautif de m0boy.

**Corrigé en 0.187.7.** m0boy : `gbl_core.c` remappe `gbl_rmap[0..3]` sous garde `GBL_MBC == 1 && !GBL_ROM_STREAM &&
GBL_ROM_BANKS > 32` (aucun coût pour les cibles de petite ROM) et expose `gbl_rom0_bank` (variable séparée : `gbl_cpu_t`
et les fichiers d'état sont inchangés). deepeel : `dp_gbl_bank_of()` remplace `pc < 0x4000 ? 0 : rom_bank` aux sites
vivants (attribution d'exécution, cibles de fourche, voies de session, flux d'appel, lectures de copie). Test
contractuel `mbc1m1` dans `test_explorer` (code avant le basculement en banque 0, après en banque `$20`).
Reste : les sites qui lisent une image d'état différée (`explorer.c` graines, `frontier_disk.c`) supposent encore la
banque 0 pour `pc < $4000` ; ils n'affectent que la priorité/dédup des graines différées d'une ROM MBC1 ≥ 1 Mio.

## 0.187.9 — état du décodeur SGB porté par les images de fourche

Le décodeur de paquets SGB (`sgb_bit`, `sgb_buf`, écritures sur `$FF00`) vit dans l'explorateur, pas dans la machine :
une fourche prise en plein paquet repartait avec un décodeur au repos. Chaque image d'état porte désormais un
trailer de 24 octets (magic `SGB1`, bit courant, 16 octets) écrit par `ex_save` et relu par `ex_load` (`explorer.c`) ;
`ex->sb = ex->mb + 24`, le hash de dédup ne couvre que la partie machine (`ex->mb`). Une image sans trailer valide
(autres chemins d'écriture) se lit « décodeur au repos ». Les fichiers spool/lot existants sont refusés (taille
d'image différente) : ils se relancent à neuf. Test `explorer` : aller-retour mi-paquet et image sans trailer.
Non fait : rendu bordure/palettes SGB et typage associé.

## 0.188.0 — options communes mutualisées (nes, pce, md) et arrêt coopératif partagé

Les trois analyseurs d'options `nes`/`pce`/`md explore` répétaient la même chaîne `else if` (`--max-total`, `--queue`,
`--lot`, `--lot-drain`, `--tree`, `--jobs`, `--novelty*`, `--ctx-cap`, `--second-pass`, `--export-state`, `--watch`,
`--watch-dump`, `--text`, `--map`, `--evidence`, `--rtcode`). Elle vit maintenant une fois dans `src/cli_common.h`
(`DP_CLI_COMMON`, plus `DP_CLI_PLAYED` pour NES/PCE : `--max-steps`, `--loop-cap`, `--fork-cap`, `--play`, `--sessions`,
`--seed`, `--no-irq-fork`). Ce que cela apporte de visible :
- **`--time-limit SEC` pour NES et PCE** (jusque-là MD seul) ; SIGINT est aussi un arrêt propre pour ces trois commandes.
  Vérifié : NES DuckTales `--time-limit 8` rend la main à 10 s, PCE Raiden à 8 s, rapport écrit, code 0.
- Un drapeau d'arrêt unique `dp_stop` (`src/explore/stop.c`, dans `deepeel_core`, donc partagé par les clones
  `--jobs`, qui avaient chacun leur copie préfixée de l'ancien `dp_md_stop`).
Non mutualisé (réel, mais pas fait) : les boucles `run_path`/`novelty_search`/`second_pass_seed`/`lot_spill` des
`explore.c` md/nes/pce/sega (≈ 8 000 lignes, états de machine différents) et le squelette de `jobs.c`
(≈ 470 lignes pour 3 plateformes) ; GB et Sega gardent leur propre analyseur et leur propre arrêt.

## Recompilation : répartition des rôles avec m0boy (note 2026-11-04)

m0boy est le recompilateur (`gbrecomp ROM --coverage X.cov`, tables `gbl_tables`, repli interpréteur pour ce que la
couverture ne prouve pas). deepeel n'en écrit pas un second : il fournit la couverture (`--cov`, format m0boy
`bank addr flags hits`, 1 exec / 2 entrée / 4 spéculatif / 8 données) et l'archive `--rtcode`.

Première mesure de bout en bout (observation de l'outil, Super Mario Land (World), `explore --time-limit 120`, 6,8 s
réelles) : `gbrecomp --target pc --coverage sml.cov` compile **46 584 instructions en 234 régions, « interpreter only: 0 »**,
10 602 enregistrements laissés en données (flag 8), 5 996 entrées de dispatch. (La cible Meta plafonne à 6 963 par budget
flash : ce n'est pas un défaut de couverture.) Écart restant pour « sans interpréteur » :
- **code exécuté en RAM/VRAM/HRAM** : 8 341 instructions distinctes, dont 8 263 sans source ROM (code généré). Le `.cov`
  ne porte que des offsets ROM : `gbrecomp` n'a aujourd'hui aucun moyen de les recevoir, donc elles passeraient par
  l'interpréteur (ou seraient fausses). C'est la prochaine interface à définir avec m0boy (format de l'archive `rtcode`
  en entrée de `gbrecomp`) ;
- **1 079 octets de ROM inconnus** en 122 plages (dont du code mixte aux frontières) ;
- les trous matériel listés dans `docs/COUVERTURE.fr.md` §3 (un comportement non émulé ne se recompile pas).
Non vérifié : que le binaire recompilé reproduise l'exécution (comparaison trace à trace) ; que ces 46 584 instructions
couvrent tout ce qu'une partie réelle exécute.

## 0.188.1 : export du code RAM pour m0boy, éviction corrigée (2026-11-04)

- `deepeel explore ROM --ram-code f` écrit le code WRAM/HRAM exécuté en variantes
  d'octets contigus (entrée de `gbrecomp --ram-code f`, m0boy 18639cd). Les
  instructions que seule une fourche forcée a atteintes sont écartées
  (`--ram-code-forced` pour les garder) ; les octets inconnus entre deux
  instructions exécutées ne sont jamais devinés.
- Correctif : la règle d'éviction « exécution dans des données verbatim »
  coupait un chemin naturel après la première instruction d'un code copié
  ROM→WRAM. L'archive `rtcode` était tronquée (fixture `wramexec` : 2 entrées au
  lieu de 9). Elle ne s'applique plus qu'aux chemins forcés pour la WRAM.
- Correctif : `run_sliced` (MD) ne s'arrêtait pas sur un budget non épuisé après
  une tranche (`--unbounded` s'arrêtait après une tranche).
- Observations de l'outil : sur *Super Mario Land*, 0 variante naturelle (8 124
  instructions d'archive, toutes forcées). Sur la fixture `wramexec`, une
  variante de 6 octets ; le binaire généré donne les mêmes empreintes avec et
  sans variante, instructions interprétées 5 → 2.
- Matrice COUVERTURE : MSX J5 (`--export-state`) et O3 (`--watch-dump`) passent de ✗ à ◐ — les
  options existaient (backend commun Sega/MSX) mais la cellule était périmée et `--export-state`
  MSX n'avait aucun test ; `msx_export_state` l'épingle. Le nombre de cellules ouvertes reste 91
  (◐ compte comme ouvert).
- Run Sonic `--unbounded` (10 min, disque Windows) : 1 chemin, 16 777 216 pas, 642 301 états
  écrits sur le lot, 0 relu ; 4,3 % des octets ROM exécutés. Arrêt sur la limite de temps, pas une
  exploration terminée : l'ordre « lot relu après épuisement de la file » reste à valider sur un run plus long.

## 0.189.0 : MD sessions jouées (2026-11-04)

- `md explore ROM --play-frames N [--play-sessions K] [--play-seed S]` : K sessions (8 par défaut)
  de N frames depuis la mise sous tension, manette aléatoire tenue 4..43 frames (xorshift,
  directions opposées exclues), chacune un chemin naturel borné en frames. Déterministe à graine égale.
  Rapport : `played sessions: K, F frames`. Avec `--jobs 2` elles restent sur l'instance primaire.
- Limites (cellule MD J1 : ◐) : pas de pool de checkpoints partagé, pas de multi-thread des
  sessions, pas de scénarios attract/script ; une ROM sans boucle de frame (fixture `pad_effect`)
  donne 0 frame jouée.

## 0.190.0 : MD S4, tables de sauts `jmp d8(pc,Xn)` (2026-11-04)

- Sur un `jmp d8(pc,Xn)` observé, la base de table est lue statiquement (`pc+2+d8`) ; les entrées
  `bra.s`/`bra.w` consécutives (2 à 128) donnent des gestionnaires non encore atteints, semés comme
  l'autre côté d'un conditionnel (rapport : `jump tables (S4…)`). Fixture `fix_md_s4`, test `md_s4_table`.
- Limites (cellule ◐) : pas de tables de mots/offsets (`move.w (pc,d0),d0 ; jmp (pc,d0)`), pas de
  `jmp (An)` à base calculée, entrées `jmp abs.l` non suivies.

- Sega/MSX : les cellules d'écran de Go-Explore (`--novelty-cells 1`, empreinte d'écran + octet de
  vies appris) existaient sans test ; `z80_novelty_cells` les épingle sur SMS et MSX (fixture
  `fix_z80_branch`). Matrice J4 SMS/MSX mise à jour (toujours ◐ : pas de stratégie de choix de segment
  du pool de sessions).

## 0.191.0 : GB S4, base de table dans DE/BC (2026-11-04)

- Le `jp hl` exécuté reconnaît aussi `ld de,nn` / `ld bc,nn` suivi d'un `add hl,de` / `add hl,bc`
  exécuté avant le saut ; un `ld hl,nn` proche qui ne donne aucune entrée n'arrête plus la recherche
  (le motif `ld hl,0 ; add hl,de` ne cachait la vraie base que dans l'ancienne version).
- Fixture `fix_jptable_de`, test `gb_jp_table_de` (échoue sans le correctif, vérifié).
- Limite : la fenêtre reste de 24 octets de code droit, l'index n'est pas évalué (toutes les
  entrées, ≤ 64, sont prises) ; une base calculée par arithmétique autre que `add hl,rr` n'est pas suivie.

## 0.192.0 : MD, sessions jouées réparties sur `--jobs 2` (2026-11-04)

- `--jobs 2` accepte désormais une seule entrée si `--play-frames` et `--play-sessions ≥ 2` : l'instance
  primaire joue les sessions paires, la secondaire les impaires (`play_from/play_step`), puis fusion.
- Le rapport additionne `played sessions` des deux instances ; test `md_play_jobs` (union observée
  identique à l'exécution mono-instance sur la fixture).
- Limite : pas de pool de checkpoints partagé entre instances ; deux instances seulement.

## 0.193.0 : SMS/GG, écritures YM2413 typées (2026-11-04)

- Le rapport `deepeel z80 explore sms|gg` ajoute `YM2413 typed writes` : les écritures de données `$F1`
  sont classées selon le registre verrouillé par `$F0` (instrument utilisateur 00-07, rythme 0E,
  F-number 10-18, key/block 20-28, instrument/volume 30-38, autre).
- Observation de l'outil sur les chemins explorés (naturels et forcés comptés) ; la puce FM n'est
  toujours pas émulée. Fixture `fix_sms_ym2413`, test `sms_ym2413_typed`.

## 0.194.0 : MSX, ports V9938 signalés (2026-11-04)

- Le rapport `deepeel z80 explore msx` ajoute `MSX2 V9938 ports` quand le programme écrit sur `$9A`
  (palette) ou `$9B` (registre indirect) : un jeu MSX2 n'est plus silencieusement pris pour MSX1.
- Observation des écritures seulement ; le V9938 (modes graphiques, blitter, palette) n'est pas émulé.
  Fixture `fix_msx_v9938`, test `msx_v9938_ports`.

## 0.195.0 : PCE, option `--multitap` (2026-11-04)

- `deepeel pce explore ROM --multitap` active le modèle d'adaptateur cinq manettes déjà présent dans
  `pcemach.c` (CLR remet le balayage à la manette 1, chaque SEL montant avance) ; le rapport l'indique.
- Les manettes 2 à 5 restent au repos : modèle d'observation, pas une mesure de matériel. L'état du
  balayage est global au processus, donc `--multitap` force une seule instance (`--jobs` ignoré, signalé).
- Test `pce_multitap_option`. Limite : aucune entrée jouée sur les manettes 2-5.

## 0.196.0 : PCE, manettes 2-5 jouées avec `--multitap` (2026-11-04)

- En jeu (`--play`, sessions), les manettes 2 à 5 rejouent l'entrée de la manette 1 tournée de n bits :
  elles diffèrent entre elles et restent déterministes. Sans `--multitap`, rien ne change.
- Modèle d'observation arbitraire (pas des entrées de joueurs réels) ; le test `pce_multitap_option`
  couvre maintenant une session jouée avec l'option.

## 0.197.0 : PCE, écritures CD-ROM2/ADPCM typées (2026-11-04)

- Le rapport `deepeel pce explore` compte, parmi les écritures de registres journalisées, celles de
  l'interface CD-ROM2 (`$1800-$1BFF`) : ADPCM (`$180A-$180E`, `$1860-$1869`) et autres. Un code qui
  les atteint est signalé au lieu d'être silencieusement pris pour du HuCard pur.
- Observation du journal (plafonné, débordement rapporté) ; aucun CD-ROM2 ni ADPCM n'est émulé.
  Fixture `fix_pce_adpcm`, test `pce_adpcm_typed`.

## 0.198.0 : PCE, écritures SuperGrafx typées (2026-11-04)

- Le rapport `deepeel pce explore` compte les écritures journalisées vers le VPC (`$0008-$000F`) et le
  second VDC (`$0010-$0017`) de la page matérielle : un jeu SuperGrafx est signalé, pas confondu avec
  un jeu PC Engine standard. Ni VPC ni second VDC ne sont émulés ; observation du journal plafonné.
- Fixture `fix_pce_sgx`, test `pce_sgx_typed`.

## 0.199.0 : MD, les sessions jouées ne sont plus coupées par le plafond de visites (2026-11-04)

- Bogue : le plafond de visites par instruction (64 par trame) coupait les sessions jouées avant la
  première trame dès qu'une boucle d'initialisation dépassait 64 tours (Sonic 1 : « played sessions: 6,
  0 frames »). Le plafond est levé pendant une session jouée (comme pour un film TAS) ; la session reste
  bornée par `--play-frames` et `--max-total`.
- Fixture `fix_md_longloop`, test `md_play_longloop` (échoue sans le correctif, vérifié).
- Observation de l'outil sur Sonic 1 (USA, Europe), `--play-frames 1800 --play-sessions 6`, 35 s :
  10 800 trames jouées, **octets exécutés 31 740 (6,1 %)** contre 4 506 (0,9 %) avant, dont 9 996
  seulement par fourches. L'union « observée » affiche 99,96 % parce que le jeu lit toute la ROM
  (somme de contrôle) : ce n'est pas une couverture, la mesure à retenir est le code exécuté.
- Limite : une seule graine, pas de progression volontaire dans les niveaux ; boucles réellement
  infinies sans trame bornées seulement par le budget total.

## 0.200.0 : MD, démo (aucune touche) prioritaire, plafond de 2^24 pas corrigé (2026-11-04)

- `deepeel md explore ROM --demo-frames N` : avant toute fourche, la console tourne N trames depuis
  la mise sous tension **sans bouton pressé** (mode attract / démo), chemin naturel. Le rapport ajoute
  `demo (no button pressed, from power-on): N frames, K ROM bytes executed`. Ordre voulu :
  démo, puis TAS, puis le reste. Test `md_demo_frames`.
- Bogue (touchait tout chemin long) : `dp_mdmach_run` renvoie `cut = 1` quand une tranche de 2^24 pas
  est épuisée ; `run_sliced` s'arrêtait donc à la première tranche, plafonnant en silence tout chemin
  naturel à 16,7 M de pas (≈ 2 434 trames de Sonic 1). Seul le budget de l'appelant termine désormais le chemin.
- Bogue de test révélé : `demo_frames` non initialisé dans les options par défaut (échec aléatoire de `md_explore`).
- Observations de l'outil, Sonic 1 (USA, Europe), démo seule (une exécution chacune) :
  7 200 trames, 29 s, 31 648 octets exécutés par la démo (6,0 %) ; 30 000 trames, 119 s, 37 266 octets
  (7,1 %). La démo plafonne vite : l'attract de Sonic 1 ne montre qu'un niveau ; le reste du code
  demande des touches (TAS) ou des fourches. Total avec fourches : 48 004 octets (9,2 %).

## 0.201.0 : `gbconform`, tableau de passage de m0boy sur les ROMs de test (2026-11-04)

- Nouvel outil d'observation `tests/gbconform.c` (cible `gbconform`, hors `ctest`) : exécute des ROMs de test GB
  sur le cœur m0boy tel que deepeel le pilote et décide par protocole (registres Mooneye, texte série ou RAM de
  cartouche Blargg, `$FF82` gbmicrotest).
- Résultats consignés dans `m0boy/docs/GB_CONFORMANCE.fr.md` (observations de l'outil sur un cœur DMG sans boot ROM) :
  948 ROMs à protocole machine, 430 passent, 291 échouent, 227 sans verdict ; Mooneye 31/115, gbmicrotest 354/513,
  Blargg 20/58 (cpu_instrs individuels tous passés). Gambatte, mealybug et acid2 attendent une comparaison de captures.

## 0.250.2 — axe 10 : recompilation totale des ROMs à 100 % de couverture

Les campagnes 0.249.x ont atteint **100,000 % connus** sur quatre ROMs
(`results/gb/`). Le tableau « ROM → recompilé → causes restantes »
(`gbrecomp --target meta --coverage carte.png`, chiffres de l'outil) :

| ROM | enregistrements | compilé | interpreter-only | données prouvées |
|---|---|---|---|---|
| Puzzle Boy (Japan) | 17 867 | 8 601 inst. en 90 régions | **0** | 379 |
| BattleCity (Japan) | 15 566 | 8 601 inst. en 93 régions | **0** | 171 |
| Master Karateka (Japan) | 17 536 | 8 601 inst. en 90 régions | **0** | 326 |
| Game Genie (USA) (v2.1) (Unl) | — | refusé : l'en-tête annonce plus que la taille du fichier (multicart) | — | — |

Les 8 601 sont le **plafond de budget flash** de la carte cible, pas un
repli : aucun octet couvert ne retombe dans l'interpréteur. Causes
restantes : aucune sur ces trois ROMs pour l'exécution ; le Game Genie
attend un lecteur tolérant aux en-têtes multicart. La recompilation sans
interpréteur est atteinte sur couverture 100 % — l'axe 10 tient son
objectif.

## 0.249.87 — axe 11 : deepeel sur les ROMs de test matériel

Inventaire hors dépôt (aucune ROM dans le dépôt) : `~/dev/gb-test-roms/`
(Blargg cpu_instrs/mem_timing), `~/dev/game-boy-test-roms/` (mooneye,
mealybug, dmg-acid2, cgb-acid2, SameSuite, rtc3test…), `~/dev/m0boy/roms/gb/test/`
(doublons pratiques) ; SMS : `~/dev/m0boy/roms/sms/ZEXALL*`.

Explorations deepeel (bornées, observations de l'outil) :

- **ZEXALL SDSC (SMS, 32 Kio)**, 30 M pas, 588 courses : map connue
  **95,4 %**, 0 fault — mais 65 314 écritures dans des octets exécutés
  (l'auto-modification est le mode de fonctionnement du testeur Z80, il
  s'exécute depuis la RAM) : la preuve passe par les frères de copie (S5)
  et l'archive de code en RAM, pas par l'exécution directe.
- **Blargg cpu_instrs individuels (GB)**, 3 M pas chacun : 01-special
  **99,88 %** connus, 04-op r,imm **99,94 %**, 09-op r,r **99,94 %** — les
  suites de test matériel sont bien des cas quasi 100 % (axe 11c).
- **09-op r,r, la réserve** : 8210 enregistrements de couverture mais 53
  octets de code exécuté et 8157 marqués données — 250 607 fourches
  plafonnées (cap 16/site, non exposé au CLI GB) sur le site de
  distribution du harnais blargg. Le code du test n'est ni exécuté ni
  atteint par la fermeture statique (« 3 stopped by data/padding ») : il
  faut résoudre la distribution indirecte du harnais pour convertir ces
  « données » en code. Amélioration de couverture à venir, pas un échec.
- `--fork-site-cap N` (0.249.88) : le cap de fourches par site du GB (16,
  compteur par octet) est exposé au CLI (255 max, `--unbounded` le met à 255).
  Mesure sur la réserve 09-op r,r : cap 255, 13 M pas, 1109 courses —
  **toujours 53 octets de code** et 3,2 M de fourches plafonnées : le cap
  n'est pas le verrou, la distribution indirecte du harnais l'est (les
  variantes de fourche retombent dans la même boucle sans atteindre de
  nouveau code). À suivre côté résolution de cibles indirectes.
- Recompilation (axe 10, première ROM de test) : `09-op r,r.gb` via
  `gbrecomp --target meta --coverage` deepeel — 8210 enregistrements, 53
  instructions compilées, **interpreter-only 0** ; la qualité de la
  recompilation suit la couverture du code, le repli ne réapparaît pas.

## 0.202.0 : NES et PCE, `--demo N` (aucune touche) avant le TAS et les parties (2026-11-04)

- `deepeel nes|pce explore ROM --demo N` : N trames depuis la mise sous tension, manettes au repos (mode attract),
  jouées **avant** le TAS, les parties aléatoires, la novelty et les fourches. Rapport : `demo (no button pressed,
  from power-on): N frames`. Même ordre que MD (0.200.0) : démo, TAS, le reste. Tests `nes_demo_frames`, `pce_demo_frames`.
- Les instances secondaires de `--jobs 2` ne rejouent pas la démo. GB, SMS/MSX : non faits (parties jouées sans équivalent).

## 0.203.0 : GB S6, tables de flux inter-banques (2026-11-04)

- Le rejeu de décodeurs ne se limitait qu'aux tables de pointeurs de 2 octets dont les flux sont dans la **même banque**.
  Il reconnaît maintenant les tables à entrées de 3 octets (banque, ptr) ou (ptr, banque) : les départs observés
  gardent leur banque, deux entrées à pas 3 situent la table, qui est étendue dans les deux sens (banque valide,
  pointeur en $4000-$7FFF), puis chaque entrée est rejouée en sélectionnant la banque par une écriture mapper
  ($2000) avant l'appel. Fixture `fix_streams_bank` : le flux de la banque 3, jamais appelé par la ROM, est lu.
  Rapport `stream replay (S6)` : 1 table, 1 candidat (0 sans le correctif).
- Limite restante : tables à stride ≠ 3, banque codée ailleurs que dans l'entrée (octet séparé), registre de banque autre que $2000.

## 0.204.0 : S6 Sega/Z80, tables de flux inter-banques (2026-11-04)

Le rejeu de décodeurs de la famille Z80 (SMS/GG/SG-1000) lit aussi les tables
`(banque, ptr lo, ptr hi)` au pas de 3 : deux départs observés avec leur banque physique
localisent la table, chaque entrée est rejouée depuis l'instantané avec la banque écrite au
registre de slot ($FFFE/$FFFF) puis HL/DE/BC = pointeur. Le rejeu est aussi déclenché à la
sortie de la phase 0 quand la file est vidée (il ne l'était que sur une clôture à file vide).
Fixture : `test_sega_streams` (flux de la banque 3 jamais appelé, lu par la table).
Observation de l'outil, pas une mesure matérielle.

## 0.205.0 : S6 NES (UxROM), tables de flux inter-banques (2026-11-04)

Le rejeu de décodeurs NES lit aussi, sur UxROM (mapper 2), les tables `(banque, ptr lo, ptr hi)`
au pas de 3 : deux départs observés avec leur banque physique localisent la table ; chaque entrée
est rejouée depuis l'instantané avec la banque sélectionnée (`dp_nesmach_enter`) et le mot de page
zéro réglé sur le pointeur. Fixture : `test_nes` (`test_streams_banked`, flux de la banque 0 jamais
appelé). MMC1 et PCE restent à faire. Observation de l'outil, pas une mesure matérielle.

## 0.206.0 : S6 PCE, tables de flux inter-pages (2026-11-04)

Le rejeu de décodeurs PCE lit aussi les tables `(page ROM de 8 Kio, ptr lo, ptr hi)` au pas de 3 :
deux départs observés avec leur page physique localisent la table ; chaque entrée est rejouée
depuis l'instantané avec la page montée sous la fenêtre du pointeur (`dp_pce_enter`, le MPR qu'un
`TAM` poserait), le PC conservé et le mot de page zéro réglé sur le pointeur. Fixture : `test_pce`
(`test_streams_banked`, flux de la page 3 jamais appelé). MMC1 NES reste à faire. Observation de
l'outil, pas une mesure matérielle.

## 0.207.0 : J1 NES, pool de checkpoints partagé (2026-11-04)

`--play-pool N` (NES, N ≤ 64, 0 par défaut) : les sessions jouées aléatoires gardent un checkpoint
toutes les 256 frames (remplacement aléatoire quand le pool est plein) et chaque session suivante
repart d'un checkpoint tiré au hasard au lieu de la mise sous tension, donc plus profond. Les
sessions TAS et la démo restent depuis la mise sous tension. Le rapport affiche « checkpoint
pool: K kept, S sessions started from one ». Fixture : `test_nes` (`test_play_pool`). Reste : PCE,
MD, GB/Z80, multi-thread. Observation de l'outil, pas une mesure matérielle.

## 0.208.0 : J1 PCE, pool de checkpoints partagé (2026-11-04)

`--play-pool N` (PCE, N ≤ 64, 0 par défaut), même schéma que NES (0.207.0) : checkpoint toutes les
256 frames des sessions aléatoires, chaque session suivante repart d'un checkpoint tiré au hasard ;
TAS et démo depuis la mise sous tension. Fixture : `test_pce` (`test_play_pool`). Reste : MD,
GB/Z80, multi-thread. Observation de l'outil, pas une mesure matérielle.

## 0.209.0 : J1 MD, pool de checkpoints partagé (2026-11-04)

`--play-pool N` (MD, N ≤ 64, 0 par défaut), même schéma que NES et PCE : checkpoint toutes les 256
frames jouées (hors démo), chaque session suivante repart d'un checkpoint tiré au hasard au lieu de
la mise sous tension. Test : `md_play_pool` (« checkpoint pool: K kept, S sessions started from
one »). Reste : GB/Z80, multi-thread. Observation de l'outil, pas une mesure matérielle.

## 0.210.0 : J1 famille Z80, pool de checkpoints partagé (2026-11-04)

`--play-sessions N` découpe le budget `--play-steps` en N sessions ; `--play-pool P` (P ≤ 64) garde
un checkpoint à chaque frontière d'époque d'entrée (remplacement aléatoire quand le pool est
plein) et chaque session suivante repart d'un checkpoint tiré au hasard ; le catalogue d'entrées est
décalé d'une position par session. Rapport : « checkpoint pool: K kept, S sessions started from
one ». Test : `sms_play_pool`. Reste : GB, multi-thread. Observation de l'outil, pas une mesure
matérielle.

## 0.211.0 : S6 NES, MMC1 (2026-11-04)

Le rejeu inter-banques NES (0.205.0, UxROM) couvre aussi MMC1 (mapper 1, fenêtre de 16 Kio à $8000,
mode de contrôle 3) : `dp_nesmach_enter` pose le registre PRG comme le port série le ferait
(bank ≤ 15) puis réapplique le mapping ; la taille de banque vient de la taille PRG. Fixture :
`test_nes` (`test_streams_mmc1`, banque posée par cinq écritures série, flux de la banque 0 jamais
appelé). MMC3 et autres mappers restent à faire. Observation de l'outil, pas une mesure matérielle.

## 0.212.0 : S6 NES, MMC3 (2026-11-04)

Le rejeu inter-banques NES couvre MMC3 (mapper 4, fenêtre R6/R7 de 8 Kio, mode PRG 0) :
`dp_nesmach_map_8k` pose R6 ou R7 selon la fenêtre de l'adresse lue, puis réapplique le mapping ;
l'unité de banque des tables est 8 Kio pour MMC3 (16 Kio pour UxROM/MMC1). Fixture : `test_nes`
(`test_streams_mmc3`, banque posée par écritures `$8000/$8001`). Reste : MMC5 et autres mappers,
mode PRG 1 de MMC3. Observation de l'outil, pas une mesure matérielle.

## 0.213.0 : S6 NES, MMC3 en mode PRG 1 (2026-11-04)

Le rejeu inter-banques MMC3 suit le mode PRG courant : en mode 1 la fenêtre commutable est
`$C000-$DFFF` (R6, `$8000` fixe) ; `dp_nesmach_mmc3_mode1` l'indique, `dp_nesmach_map_8k` conserve
le bit de mode et la plage des pointeurs de table suit la fenêtre. Fixture : `test_streams_mmc3_mode1`
(code dans les banques hautes fixes). Reste : MMC5 et autres mappers. Observation de l'outil.

## 0.214.0 : NES, mappers Camerica (71) et UN1ROM (94) (2026-11-04)

Deux cartes de type UxROM entrent dans le pont : mapper 71 (banque de 16 Kio écrite en `$C000+`,
dernière banque fixe en `$C000`) et mapper 94 (banque en bits 2-4 d'une écriture `$8000+`). Le rejeu
S6 inter-banques les couvre via `dp_nesmach_cross16` (UxROM, MMC1, 71, 94 partagent l'entrée
`dp_nesmach_enter`). Fixtures : `test_streams_uxlike(71/94)` dans `test_nes`. Mapper 180 (banque
commutable en `$C000`) reste à faire. Observation de l'outil, pas une mesure matérielle.

## 0.215.0 : NES, mapper 180, fenêtre de pointeurs générique (2026-11-04)

Mapper 180 (Crazy Climber : banque 0 fixe en `$8000`, 16 Kio commutables en `$C000`) rejoint le pont
et le rejeu S6 inter-banques. La plage des pointeurs de table n'est plus codée en dur :
`dp_nesmach_window` la donne par carte (`$8000-$BFFF`, `$C000-$DFFF` pour MMC3 en mode 1,
`$C000-$FFFF` pour le 180). Fixture : `test_streams_180`. Reste : MMC5 et autres mappers.
Observation de l'outil, pas une mesure matérielle.

## 0.216.0 : PCE, `--seed-vectors` (G1) (2026-11-04)

`--seed-vectors` existait pour NES ; la PC Engine l'a aussi : les cibles des vecteurs IRQ2/BRK
(`$FFF6`), IRQ1 (`$FFF8`), TIMER (`$FFFA`) et NMI (`$FFFC`) partent comme racines forcées (vecteur
hors de la page haute ou hors du mapping ROM : refusé et compté). Le rapport indique
« vector roots ». Fixture : `test_seed_vectors` dans `test_pce` (cibles atteintes avec l'option,
absentes sans). Observation de l'outil, pas une mesure matérielle.

## MD : `--seed-vectors` (G1) (2026-11-04, sans changement de version : CMakeLists.txt porte un chantier TAS non committé)

`--seed-vectors` existe aussi pour la Mega Drive : les cibles des sept autovecteurs (`$64`-`$7C`,
dont HINT et V-int) et des TRAP #0-15 (`$80`-`$BC`) de la table de vecteurs partent comme racines
(doublons, adresses impaires ou hors ROM écartés et comptés ; ligne « vector roots » du rapport).
Fixture : bloc G1 de `test_md_explore` (ROM en mémoire, handler V-int atteint avec l'option
seulement). Observation de l'outil, pas une mesure matérielle.

## MD : site de bifurcation dans l'arbre de choix (F2) (2026-11-04, sans changement de version)

Les nœuds d'arbre créés par une fourche de branchement MD (`--tree`) portent désormais l'adresse ROM
du site (champ `site` de `DpChoice`) ; les autres graines (IRQ, pad, entrées) restent sans site.
Observation de l'outil : sur `fix_md_dbcc`, le nœud fils porte le site `0x202`.

## NES et PCE : site de bifurcation dans l'arbre de choix (F2) (2026-11-04, sans changement de version)

Comme pour MD : les nœuds créés par une fourche de branchement NES (cellule PRG du branchement ;
`0xFFFFFFFF` pour les autres graines) et PCE (index ROM + 1 du `pc` de la fourche ; 0 hors ROM)
portent leur site. Observation de l'outil ; pas de reprise depuis l'arbre, F2 reste ◐.

## 0.218.0 : GB, phase PPU du profil GBHawk (lecture LY exacte)

Complément au profil GBHawk de 0.217.0 : le dernier écart trace contre trace était la
phase de ligne PPU à $0100. GBHawk gèle son compteur de ligne pendant le boot (LCD off)
et la ligne 153 de sa trame remise à zéro du compteur `LY` au cycle 6 en sautant la
frontière suivante (`GB_PPU.cs`), donc son front LY 0→1 tombe 912 cycles après la
frontière de la ligne 153. Mesure sur SML2 (`tools/gbhawk_ref` étendu d'une colonne LY
et d'un journal des écritures $FF40/$FF50) : frontières de ligne 153 à $0100−405 et
0→1 à $0100+507 ; m0boy démarre sa ligne à $0100 (front à $0100+456), d'où les îles
±52 cycles des boucles d'attente `LY==0x94` et la dérive −64 cycles.

- Nouveau knob `gbl_sync_ppu_phase` (patch tas-compat, gbl.h) : cycles de la ligne 153
  écoulés à $0100 ; le profil GBHawk pose 405 et la machine appelle
  `gbl_ppu_phase_apply()` juste après le chargement de l'état initial (`DpGblApi`
  gagne le pointeur ; deepeel l'appelle dans le chemin TAS GBHawk). Le wrap 153→0 de
  m0boy tombe alors sur la frontière sautée de GBHawk et son front 0→1 sur le sien :
  les lectures LY coïncident cycle par cycle (200 000 instructions comparées, écart 0).
- Environnement : `DEEPEEL_TAS_PPUPHASE` (défaut 405). Le délai manette reste 14 481
  (fenêtre mesurée 14 470..14 550) et la phase DIV $FE4D.
- WRAM par trame : 2 406 des 2 418 trames du film identiques (m0 trame f = GBHawk
  trame f+333). Écarts : trames 1-4 (boot) et 520-526 — la transition intro→titre écrit
  la WRAM en continu et les deux côtés l'échantillonnent à ~51 235 cycles d'écart dans
  la trame (m0 au bord vblank, GBHawk au budget FrameAdvance) : états intermédiaires
  différents, point final identique (trames 527-2418 toutes conformes). Artefact
  d'échantillonnage, pas une divergence de jeu.

Observation de l'outil sur ROM réelle, pas une mesure matérielle. Reste ouvert : une
lecture LY de m0boy en dehors du profil (matériel) n'est pas remise en cause — le knob
ne touche que la machine TAS.

## PCE : `--seed-banks` (G3) (0.219.0)

`--seed-banks` existe pour NES (UxROM) ; la PC Engine l'a aussi : chaque page ROM de 8 Kio est
mappée dans la fenêtre `$4000` (la valeur MPR qui la désigne) et entrée à sa base, comme racine
forcée. Les pages qu'aucune valeur MPR ne désigne sont refusées et comptées (`entries_refused`).
Le rapport affiche `bank roots: N pages seeded`. Observation de l'outil sur fixture, pas une
mesure de matériel. Test : `test_seed_banks` (`tests/test_pce.c`).

## NES : reprise de l'arbre de choix avec `--resume` (F2) (0.220.0)

Avec `--tree FICHIER` et `--spool`/`--resume`, la reprise recharge l'arbre sauvegardé par le run
précédent quand un lot disque est en attente. Les états relus portent des identifiants de nœud :
sans rechargement, leurs parents pointaient dans le vide et la numérotation repartait de zéro.
Le test `nes` (`tests/test_nes.c`) vérifie que le nombre de nœuds ne diminue pas et que la racine
`reset` survit ; il échoue si le chargement est désactivé. NES seulement : MD et PCE restent à faire.

## PCE : reprise de l'arbre de choix avec `--resume` (F2) (0.221.0)

Comme pour NES (0.220.0), la reprise PCE recharge l'arbre sauvegardé par le run précédent quand un
lot disque est en attente. En plus, les états écrits dans le lot portent maintenant leur identifiant
de nœud (méta de 5 octets : manette forcée + id ; les anciens lots à 1 octet se relisent avec
l'id 0). Le rapport affiche `choice tree resumed: N nodes reloaded`. Test `pce`
(`tree_loaded`) : il échoue si le chargement est désactivé. MD reste à faire.

## MD : reprise de l'arbre de choix avec `--resume` (F2) (0.222.0)

Comme NES (0.220.0) et PCE (0.221.0), la reprise MD recharge l'arbre sauvegardé quand un lot disque
est en attente ; les éléments du lot portaient déjà leur identifiant de nœud, qui était remis à zéro
à la relecture : il est maintenant conservé (ramené à 0 s'il dépasse l'arbre rechargé). Rapport :
`choice tree resumed: N nodes reloaded`. Test `md_explore` : il échoue si le chargement est
désactivé.

## NES : mapper MMC5 (sous-ensemble) (0.223.0)

Le pont NES prend en charge le mapper 5 : modes PRG 0 à 3 (`$5100`, registres `$5114`–`$5117`,
dernier banc sous `$E000` à la mise sous tension) et modes CHR 0 à 3 (`$5101`, `$5120`–`$5127`).
Les écritures `$5100`–`$5127` produisent des événements de banque comme celles de `$8000+` sur les
autres cartes. **Non pris en charge** : RAM mappée dans les fenêtres PRG (bit 7), ExRAM, multiplicateur
`$5205`/`$5206`, IRQ de scanline `$5203`/`$5204`, découpage d'écran, jeux CHR distincts fond/sprites.
Un jeu qui en dépend sera exploré avec ces fonctions inertes (observation de l'outil, pas du matériel).
Test `nes` : `test_mmc5`.

## `--spool` / `--resume` pour PCE et MD (0.224.0)

Les options communes acceptent `--spool FICHIER` (alias de `--lot`) et `--resume FICHIER` (lot repris
et vidé, alias de `--lot --lot-drain`), comme NES. Test `resume_cli` (MD : un run borné laisse un lot,
`--resume` le relit).

## PCE : file croissante `--queue-max` (0.225.0)

La file d'états en attente de PCE grandit (doublement) jusqu'à `queue_max` / `--queue-max N` avant de
déborder vers le parc puis le lot disque ; le rapport affiche `queue grown N times`. Défaut 0
(comportement inchangé). Test `pce` : la file grandit au lieu de parquer.

## PCE : accès par registre matériel (D3) (0.226.0)

Le rapport PCE compte les lectures et écritures de la page matérielle registre par registre
(`reg $0002 reads N writes M`, 27 registres du registre matériel + « other ») ; `io reads` s'ajoute
à `io writes`. Les compteurs sont fusionnés par `--jobs 2`. Test `pce` : les sommes par registre
égalent les totaux. Observation de l'outil, pas une mesure de matériel.

## NES : racines `--seed-vectors` en forcé (G1) (0.227.0)

Les racines NMI et IRQ/BRK semées par `--seed-vectors` étaient marquées *naturelles* alors que rien ne
les atteint depuis le reset ; elles sont maintenant forcées, comme sur PCE et MD (la norme G1 :
reset naturel, le reste forcé). Test `nes` : `test_seed_vectors` ; PCE et MD vérifient aussi
désormais le caractère forcé.

## NES : marquage ENTRY / IRQ par cellule (D1) (0.228.0)

`exec_kind` (un octet par cellule PRG, octet d'opcode) : `DP_NES_X_ENTRY` pour une arrivée non
séquentielle, `DP_NES_X_IRQ` pour le code exécuté entre l'entrée d'un vecteur NMI/IRQ et son `RTI`.
START reste `exec_hits > 0` (compteur sur l'octet d'opcode). Limite : une fourche reprise au milieu
d'un handler perd le marquage IRQ (l'état n'est pas dans l'instantané). Test `nes` :
`test_seed_vectors`.

## PCE et MD : marquage ENTRY / IRQ par cellule (D1) (0.229.0)

Même `exec_kind` que NES (0.228.0) : `DP_PCE_X_ENTRY`/`DP_PCE_X_IRQ` (vecteurs `$FFF6-$FFFD`, fin au `RTI`)
et `DP_MDX_ENTRY`/`DP_MDX_IRQ` (vecteurs d'exception `$60-$BF`, fin au `RTE`), sur l'octet d'opcode.
Même limite : une fourche reprise au milieu d'un handler perd le marquage IRQ. Tests `pce` et
`md_explore` (`test_seed_vectors`).

## NES : sondes de manette, port 2 et combinaisons (D10) (0.230.0)

À chaque site d'écriture `$4016`, en plus des 8 boutons du port 1, on propose les 8 boutons du port 2
et un catalogue de 10 combinaisons à deux boutons sur le port 1 (A+B, Start+Select, chaque direction
avec A ou B). Un état identique à un état déjà vu (port non lu) est compté dans `refused`, comme
tout doublon évincé. Le catalogue de 128 combinaisons du z80 et le multitap restent à faire.

## MD : coupe de glissade de padding par défaut (D13/D14) (0.231.0)

Seize octets nuls consécutifs exécutés comme code (`ori.b #0,d0` en glissade) coupent désormais le
chemin par défaut, comme sur NES et PCE ; `--no-cut-padding` rétablit l'ancien comportement. Le
compteur `padding cuts` reste rapporté. Risque assumé : une ROM dont du vrai code serait fait de 16
mots nuls consécutifs serait coupée (cas jamais observé en pratique, signalé par le compteur). Les
fixtures `md_entry` et `md_second_pass`, qui exécutent volontairement des zéros, passent
`--no-cut-padding`.

## PCE : fermeture statique à vues de fenêtre (S2) (0.232.0)

Une arête statique qui sort de sa fenêtre MPR est résolue vers la page ROM observée dans la fenêtre
cible, quand une seule page y a été vue (mappage fixe, p. ex. MPR7). Avant, elle était abandonnée.
Une fenêtre qui a vu plusieurs pages reste ambiguë et l'arête est toujours abandonnée (vues multiples
à faire). Test `pce` : `test_static_views` (contrôle négatif fait).

## MD : tables de sauts `jsr d8(pc,Xn)` (S4) (0.233.0)

Le motif `jmp d8(pc,Xn)` vers une table de `bra.s`/`bra.w` (0.190.0) est reconnu aussi pour
`jsr d8(pc,Xn)` (appel indexé, très courant). Fixture `fix_md_s4j`, test `md_s4_jsr` (contrôle
négatif fait). Toujours pas de tables de mots ni de `jmp (An)`.

## MD : tables de sauts `jmp/jsr d8(An,Xn)` (S4) (0.234.0)

Quand l'instruction indexée part d'un registre d'adresse (`lea table,A0 ; jmp 0(A0,D0.w)`), la base
de la table est la valeur vivante de `An` plus le déplacement : lue sur la machine à l'exécution,
sans provenance symbolique. Même format de table (`bra.s`/`bra.w`) que `d8(pc,Xn)`. Fixture
`fix_md_s4a`, test `md_s4_an` (contrôle négatif fait). Reste : tables de mots et `jmp (An)` nu.

## MD : tables de mots à déplacements (S4) (0.235.0)

L'idiome `move.w tbl(pc,d0.w),d1 ; jmp tbl(pc,d1.w)` : quand la table n'est pas faite de `bra`, ses
entrées sont lues comme des déplacements 16 bits depuis la base. La table n'est admise que si la cible
observée du `jmp` est l'une de ses entrées, et elle s'arrête là où commence la première cible (le
code). Fixture `fix_md_s4w`, test `md_s4_words` (contrôle négatif fait). Reste : `jmp (An)` nu.

## MD : douze motifs de manette par site de lecture (D10) (0.236.0)

À chaque site lisant `$A10003`, on propose maintenant 12 motifs (Start, A, Droite, A+Droite, puis
les autres boutons seuls et deux accords) au lieu de 4. Les tests épinglés sur 4 forks (`md_explore`,
`md_ctx_cap`) passent à 12 forks / 13 chemins. Reste : le pad 6 boutons (séquence TH) et le catalogue
de 128 combinaisons.

## NES : l'état « dans un handler » suit les fourches (D1) (0.237.0)

Chaque état en file porte `in_handler` (copié à la création de la fourche) ; le chemin repris
l'adopte. Une fourche prise dans un handler NMI/IRQ garde donc le marquage IRQ. L'enregistrement de
file grossit d'un octet : un lot disque écrit par une version antérieure n'est pas relisible. Test
`nes` (`test_seed_vectors`, contrôle négatif fait). PCE et MD gardent la limite.

## PCE et MD : l'état de handler suit les fourches (D1) (0.238.0)

Même mécanisme que NES (0.237.0) : chaque état en file porte `in_handler`, adopté par le chemin
repris. Les lots disque (`--unbounded`) ne persistent pas ce champ (format inchangé, anciens lots
relisibles) : une fourche relue depuis le disque perd le marquage IRQ. Tests `pce` et `md_explore`
(fourche dans un handler, contrôle négatif fait).

## NES : une file pleine ne refuse plus de graine (G4) (0.239.0)

Les racines (entrées, vecteurs, second passage, dispatch de banque) passaient par `queue_push` et
étaient abandonnées (`seeds_refused_queue`) quand la file était pleine. Elles passent maintenant par
`seed_push` : file, sinon mise de côté (tampon, puis lot disque), comme les fourches. Le compteur ne
compte plus que les vrais refus. Test `nes` (`queue_cap = 1`, contrôle négatif fait). PCE et MD
gardent l'ancien comportement ; la cellule G4 reste ◐ (le second passage sur successeurs statiques).

Note G4 (0.239.0) : PCE et MD n'avaient pas ce défaut. Leurs racines passent par `push_live` /
`push`, qui agrandissent la file puis mettent de côté (tampon, lot disque) ; seuls la dédup et le
plafond de contexte écartent une graine. Il n'y a donc rien à porter.

## MD : pad 6 boutons (D10) (0.240.0)

Le runtime m0boy (`md_full_bus.c`) modélise maintenant la manette 6 boutons : `pad_ext` (bit 7 =
pad 6 boutons branché, bits 0-3 = Z, Y, X, Mode) et `pad_cnt` (fronts montants de TH). À la
quatrième phase, TH bas lit 0 sur les bits 0-3 (identification), TH haut lit Z, Y, X, Mode. Avec
`pad_ext == 0` le comportement trois boutons est inchangé. Pas de timeout de séquence (le compteur
tourne sur 4 fronts). Chaque site de lecture reçoit 14 sondes (12 trois boutons + pad 6 boutons au
repos, + tout enfoncé). Tests : fixture `fix_md_pad6` (octet de données lu selon l'identification,
aucun branchement ne dépend de la manette), `md_explore`, contrôle négatif fait ; `md_ctx_cap`
passe à 15 chemins. L'état de la machine grossit de 2 octets : états disque antérieurs non relisibles.

## MD : tables de pointeurs derrière `jmp (An)` (S4) (0.241.0)

Un `jmp (An)` / `jsr (An)` observé remonte à l'instruction qui a écrit An en dernier
(`an_writer`). Si c'est `movea.l d8(Am,Xn),An` ou `movea.l d8(pc,Xn),An`, la base de la table de
longs est connue (Am vivant, ou pc + 2) ; la table (≤ 128 entrées, adresses paires dans la ROM) est
acceptée quand la cible observée en est une entrée, et ses autres cibles sont semées. Test
`md_s4_ptr` (fixture `fix_md_s4p`, contrôle négatif fait). Restent hors portée : An chargé autrement
(arithmétique, pile, RAM).

## PCE : vues multiples dans la fermeture statique (S2) (0.242.0)

Une arête qui quitte sa fenêtre était résolue seulement si une seule page avait été observée dans la
fenêtre cible. Elle est maintenant suivie dans chacune des pages observées (jusqu'à 16 ; au-delà,
inconnue). C'est une sur-approximation, marquée `STATIC` comme le reste de la fermeture. Test `pce`
(`test_static_views` : deux pages en fenêtre 2, l'arête atteint l'autre page, contrôle négatif fait).

## NES : `--pad-full`, les 256 masques du port 1 (D10) (0.243.0)

Option explicite : à chaque site d'écriture de `$4016`, tous les masques de boutons du port 1 qui
ne l'étaient pas déjà (singles, accords du catalogue) sont proposés, soit 256 fourches par site au
lieu de 19. Un site par visite, refus comptés ; coûteux, donc hors défaut. Test `nes_pad_full`
(512 fourches sur les 2 sites de la fixture). MD garde ses 14 sondes.

## Go-Explore : l'archive de cellules survit à la reprise (J4) (0.244.0)

Les clés de cellules écran (`--session-cells`) sont écrites en fin de `pool.bin` et relues par
`--resume` : une reprise ne ré-archive plus les écrans déjà vus (ce qui évinçait des checkpoints
utiles du pool). Anciens fichiers sans cette fin : tolérés. Test `gb_session_cells_resume`
(1 cellule au premier run, 0 à la reprise). Reste J4 : pool commun aux autres plateformes.

## MD : `--pad-full`, les 256 masques du pad (D10) (0.245.0)

Même option que NES : à chaque site de lecture du pad (`$A10002/3`), les 244 masques à trois
boutons absents du catalogue de 14 sondes sont proposés (clé site × masque, une fois). Hors
défaut. Test `md_pad_full` (258 sondes sur le site de la fixture : 14 + 244).

## Novelty : détecteur de vies et clés de cellules mutualisés (J4) (0.246.0)

Le détecteur de vies sans connaissance du jeu et l'ensemble de clés de cellules écran étaient
copiés quatre fois (NES, PCE, MD, Sega) plus une cinquième version dans les sessions. Ils vivent
maintenant dans `src/explore/novelty.h` (`DpLives`, `DpKeys`) : un correctif porte sur tous les
cœurs. Le détecteur s'adapte à la taille de la RAM de chaque cœur ; l'ensemble de clés refuse
d'ajouter au-delà de la moitié de sa table (avant : boucle infinie possible sur table pleine).
Sans changement de comportement observé : les 150 tests passent. Le détecteur de vies
du PCE perd son affichage `DP_PCE_LIVES_DEBUG`.

## 0.247.0 — spool : deflate brut sans adler32

Les enregistrements du spool (`src/explore/lots.h`, version de fichier 3) sont compressés en deflate brut
(`Z_RLE`, sans en-tête ni somme adler32) : en `--unbounded` sur Castle of Illusion, `perf` plaçait plus de 80 % du
temps dans `dp_lf_put` et zlib, adler32 en tête. Les anciens spools ne sont plus relus (version 2 refusée).
Observation de l'outil : 154 021 fourches spoolées en 60 s contre 126 928 avant ; la cause du premier chemin qui
accapare le run reste ouverte (voir `docs/CAMPAGNE.fr.md`).

## 0.247.1 — MD : arrêt vérifié toutes les 2^20 instructions

`run_sliced` (MD) découpait le chemin en tranches de 2^24 pas et ne regardait SIGINT/limite de temps qu'entre deux
tranches : un `--unbounded` arrêté ne répondait qu'après la tranche en cours. Tranches ramenées à 2^20 pas.
Observation de l'outil (Castle of Illusion, `--unbounded`, SIGINT à 20 s) : 1 048 576 pas, 57 389 fourches spoolées,
soit une fourche pour 18 pas environ ; le « chemin sans fin » du diagnostic J13 n'était donc pas un blocage mais un
débit de ~45 000 pas/s dominé par l'écriture du spool.

## 0.248.0 — S3 GB : provenance de BC et DE

`dp_provenance` (GB) suit désormais BC et DE comme paires entières à côté de HL (`ld rr,nn`, `inc/dec rr`,
`ld a,(de|bc)` sur un octet de ROM prouvable, `ld a,b|c|d|e`, `ld h,d|b`, `ld l,e|c`). Seules les instructions connues
pour ne pas toucher B/C/D/E conservent les paires ; toute autre les oublie (jamais de valeur périmée). La clé des états
visités mélange BC/DE (une collision ne fait qu'élaguer). Test `gb_prov_de` (fixture `fix_provde`) : avant, 0 cible
`jp hl` prouvée ; après, 1 cible et 2 octets de données statiques (témoin négatif vérifié).

## 0.250.7 — S3 GB : BC et DE octet par octet

`prov_pairs` porte la connaissance **par octet** (bit3 B, bit2 C, bit1 D,
bit0 E) au lieu de paires entières : `ld b/c/d/e,n`, `ld b|c|d|e,a`,
`inc/dec r` et `inc/dec rr`, `ld r,r'` entre registres suivis (la valeur
transite quand la source est prouvée, l'octet s'oublie sinon — jamais de
valeur périmée), `add hl,bc|de` calculé quand tout est prouvé. La règle
des autres opcodes est **inversée** : seuls les opcodes qui écrivent
réellement B/C/D/E (ld r,r' vers ces destinations, ld r,n, inc/dec r,
pop bc/de, préfixe CB) ouvrent un octet ; tout le reste conserve — un `cp`
ne tuait plus toute la paire (l'ancien défaut tuait B/C/D/E pour toute
instruction non listée, ce qui rendait le suivi inutilisable en pratique).
La clé des états visités mélange les valeurs avec leurs masques (une
collision n'épure que l'état). Fixture `fix_provbyte` (test
`gb_prov_byte`) : la fourche `ld a,($44)`/`cp 0` — le côté `ld b,a`
(H inconnu) laisse son `jp hl` non résolu, le côté `ld b,$01`/`ld c,$90`
prouve sa cible `$0190`. Reste pour S3 GB : la RAM (le z80 la suit déjà).

## 0.248.1 — J4 : score et remplacement des archives de cellules partagés

`dp_cell_yield`, `dp_cell_score` et `dp_cell_victim` (`src/explore/novelty.h`) remplacent les trois copies du calcul de
rendement, du score de tirage avec vies et du choix de la cellule à remplacer dans NES, PCE et MD. Sans changement de
comportement observé : 151 tests passent. 0.248.2 : `dp_zdup` factorise aussi la compression et la copie de l'état.

## 0.248.2 — J4 : compression d'état des archives partagée

`dp_zdup` (`src/explore/novelty.h`) remplace la compression zlib niveau 1 suivie d'une copie exacte, écrite trois fois
dans `nes_archive_add`, `archive_add` (PCE) et `md_archive_add`. Sans changement de comportement : 151 tests passent.
Le cœur Sega garde sa propre archive (format différent).

## 0.249.0 — S3 PCE : graines depuis les tables tracées par provenance

Avec `--second-pass`, les mots alignés voisins (même page de 8 Kio, ≤ 64 de chaque côté) d'un mot de table tracé
jusqu'à un `jmp (zp)` et qui nomment du code non exécuté dans la fenêtre de leur page deviennent des racines forcées
(comme le fait déjà le NES). Le rapport PCE affiche `, N roots seeded` après le nombre de mots tracés. Test
`pce_provenance_roots` (fixture `fix_pce_prov` : un second mot voisin) : 1 racine. Les tests existants sont inchangés.

## 0.249.1 — J4 : Sega sur les helpers d'archive partagés

`sega_cell_add` et le tirage de `novelty_session` utilisent `dp_zdup`, `dp_cell_victim`, `dp_cell_yield` et
`dp_cell_score` (`src/explore/novelty.h`) comme NES, PCE et MD : plus aucune copie du score ni du remplacement. Sans
changement de comportement : 152 tests passent. Le format de la cellule Sega (état || image cartouche) reste propre
au cœur.

## 0.249.2 (MD, S4/S3 : témoin An avec n'importe quelle base Am)

`s4_ptr_table` (`jmp (An)` / `jsr (An)` dont An vient de `movea.l d8(Am,Xn),An`) comparait l'opcode avec le masque
`0xF1FF` : le champ du registre de base Am était inclus, donc seul A0 (et `d8(pc,Xn)`) était reconnu. Le masque devient
`0xF1F8` pour le mode indexé, `0xF1FF` pour `pc`. Fixture `fix_md_s4p2` (`lea $210,a2 ; movea.l 0(a2,d0.w),a1 ;
jmp (a1)`), test `md_s4_ptr_a2` : il échoue avant le correctif et passe après. 153 tests dont 152 passent (un ignoré). Reste S3
MD : suivi de valeur 68k sur plusieurs registres (D0–D7), non commencé.

## 0.249.3 (MD, S3 : témoin An à travers un registre de données)

Le moteur MD retient désormais, comme pour An (`an_writer`), le pc de la dernière instruction qui a modifié chaque Dn
(`dn_writer`, copié avec les points de reprise et les lots disque). `s4_ptr_table` suit `movea.l Dm,An` jusqu'à
`move.l d8(Am|pc,Xn),Dm` et traite ce chargement comme le `movea.l` direct : la table de pointeurs longs est lue depuis la
base, acceptée si la cible observée en est une entrée, ses autres cibles ROM valides sont ensemencées. Fixture
`fix_md_s4p3` (`move.l 0(a2,d0.w),d1 ; movea.l d1,a1 ; jmp (a1)`), test `md_s4_ptr_dn`. Reste : chaînes plus longues
(plusieurs copies/ALU entre le chargement et l'usage), `jmp (An)` dont An est calculé par `adda`.

## 0.249.4 (MD, S3 : copies `move.l Dx,Dm` dans la chaîne)

`s4_ptr_table` saute jusqu'à 4 copies `move.l Dx,Dm` entre le chargement de la table et le `movea.l Dm,An` (le pc de
l'écrivain est relu pour chaque Dx). Le résultat reste validé par la cible observée : l'inférence ne crée pas de table
sans qu'une des entrées lues soit la cible réellement atteinte. Fixture `fix_md_s4p4`, test `md_s4_ptr_dn_copy`.
Limite : `dn_writer` ne garde que le dernier écrivain, donc un Dx réécrit entre la copie et l'usage est mal attribué
(la validation par la cible évite alors la fausse table). Reste : An calculé par ALU (`adda`).

## 0.249.5 (J4 : choix du slot d'archive commun)

`dp_cell_slot` (`src/explore/novelty.h`) choisit le slot d'une nouvelle cellule (ajout tant que l'archive n'est pas
pleine, sinon éviction de la moins productive avec libération de son `z`) et signale le remplacement. NES, PCE, MD et
Sega l'utilisent à la place de leurs quatre copies de cette logique ; les champs propres à chaque cœur (RAM de vies,
profondeur, image cartouche) restent dans leur `*_archive_add`. Sans changement de comportement : 155 tests passent.

## 0.249.6 (MD, S3 : `movea.l d16(Am),An`)

`s4_ptr_table` accepte aussi `movea.l d16(Am),An ; jmp (An)` : Am (valeur vivante) + d16 désigne l'entrée lue, et les
longs qui la suivent sont les entrées voisines. Même validation (la cible observée doit être une des entrées lues, au
moins deux entrées valides en ROM). Les entrées *avant* l'entrée lue ne sont pas explorées (la base de la table n'est
pas connue dans ce cas). Fixture `fix_md_s4p5`, test `md_s4_ptr_disp`. Reste : An calculé par ALU (`adda`).

## 0.249.7 (NES, F1 : file croissante `--queue-max`)

La file d'états en attente du NES grandit comme celles de PCE et MD : `--queue N` fixe la capacité de départ,
`--queue-max N` le plafond de croissance (doublement jusqu'au plafond avant de mettre l'état en parc ou sur le
disque). Rapport : `queue grown N times`. Les deux options sont lues par `deepeel nes explore` (`--queue` était
annoncée dans l'aide sans être lue). Le pointeur de l'état en cours n'est plus utilisé après le premier fork, donc le
`realloc` de la file est sûr. Test `nes_queue_grow` (fixture `fix_nes_pad`, `--queue 1 --queue-max 64` : file
agrandie, rien en parc ; sans `--queue-max`, 42 états en parc). 157 tests passent (un ignoré).

## 0.249.8 (NES, O1 : `--summary` et `--heat`)

`deepeel nes explore` écrit comme MD un résumé JSON (`--summary FILE` : taille PRG, comptes par classe de la carte,
`known_pct`, compteurs de pas/chemins/forks/coupures/sections) et une carte de chaleur (`--heat FILE` : instructions
démarrées par zone de 256 octets de PRG, d'après `exec_hits`, y compris les forks). Le PNG passe déjà par `maprender`.
Test `nes_summary_heat`.

## 0.249.9 (MD, O2 : `--cov` et `--romdata`)

MD compte les lectures de données du CPU par octet de ROM (`data_hits`, chemins naturels, saturant) à côté des départs
d'instruction (`exec_hits`). `--cov FILE` écrit « décalage drapeaux coups » (1 exécution, 4 spéculatif, 8 donnée ; les
coups ne comptent que les chemins naturels, un octet atteint seulement par un fork garde 0) ; `--romdata FILE` écrit
« page coups » par page de 256 octets lue. Le format `.cov` MD n'a pas de colonne de banque (une seule fenêtre linéaire).
Test `md_cov_romdata`. Les lectures DMA ne sont pas comptées.

## 0.249.10 (PCE, O2 : `--cov` et `--romdata`)

PCE compte les départs d'opcode (`exec_hits`) et les lectures de données ROM (`data_hits`) des chemins naturels, saturant,
fusionnés par `--jobs 2`. `--cov FILE` : « banque(8 Kio) décalage drapeaux coups » (1 exécution, 4 spéculatif, 8 donnée) ;
`--romdata FILE` : « page coups » par page de 256 octets. Test `pce_cov_romdata`. Observations de l'outil, pas des mesures
matérielles ; les lectures DMA/CD ne sont pas comptées.

## 0.249.11 (NES, D4 : boucle CPU ROM→WRAM)

NES enregistre aussi les copies par boucle CPU vers la WRAM (`$0000-$07FF`) : un store juste après une lecture PRG
(même provenance que `ram_src`) fusionne les octets contigus d'un même pc en un enregistrement canal 4 (`chN src dst pc len`),
compteur « to WRAM » dans le rapport. Non enregistré en mode joué. Fixture `fix_nes_wramcopy`, test `nes_wram_copy`.
D4 NES reste ◐ : SRAM `$6000` et registres son non suivis.

## 0.249.12 (NES, D4 : boucle CPU ROM→SRAM et ROM→APU)

Même mécanisme que le canal 4 : un store en `$6000-$7FFF` (canal 5) ou `$4000-$4013` (canal 6) juste après une lecture PRG
fusionne en un enregistrement `len N` (canal 6 : la destination peut rester fixe, les octets sources sont contigus). Le
compteur « to SRAM/APU » apparaît dans le rapport. Fixtures `fix_nes_sramcopy`, `fix_nes_apucopy`, test `nes_sram_apu_copy`.
D4 NES reste ◐ : la chaîne ROM→WRAM→OAM n'est pas suivie.

## 0.249.13 (NES, D4 : chaîne ROM→WRAM→OAM)

Un OAM DMA depuis une page de RAM (`$00-$1F`) marque `copied` les cellules PRG dont les octets ont été copiés vers ces
octets de RAM par une boucle (provenance `ram_src`) ; compteur « ROM->WRAM->OAM » dans le rapport. Fixture
`fix_nes_oamchain`, test `nes_oam_chain`. Avec 0.249.11–12, D4 NES couvre toutes les destinations de la norme : cellule ✅.

## 0.249.14 (MD, C3 : dédup exacte persistée)

Avec `--exact-dedup` et `--spool`/`--resume`, MD écrit l'ensemble des hachages « hors horloges » dans `<spool>.exact`
(en-tête `DPXS`, étiquette = taille de la ROM, helpers `dp_set_save`/`dp_set_load` dans `src/core/hashset.h`) et le recharge
à la reprise ; le rapport ajoute « N hashes loaded from the previous run ». Plafond de l'ensemble porté de 2^10 à 2^22 clés ;
un ensemble plein renvoie -1 et l'état est conservé (jamais jeté). Test `md_exact_persist`. L'étiquette ne contrôle que la
taille de la ROM, pas son contenu : reprendre avec une autre ROM de même taille chargerait des hachages sans rapport.

## 0.249.15 (NES, S1 : tables de pointeurs hors NROM/UxROM)

Le scan de tables de pointeurs NES (mots 16 bits petit boutiste vers des débuts d'instruction exécutés) résolvait seulement
NROM et UxROM. CNROM (mapper 3) suit désormais NROM, et les autres cartes sauf AxROM (MMC1, MMC3…) résolvent
`$C000+` vers la dernière banque de 16 Kio et `$8000-$BFFF` vers la banque de la table elle-même. Le mapping réel dépend de la
carte et de ses registres : c'est une hypothèse, gardée honnête par l'exigence que la cible soit un début d'instruction
exécuté. Les frères de copie (S5) restent sur NROM/UxROM. Fixture `fix_nes_mmc1table`, test `nes_table_mmc1`. Non vérifié :
que le test échoue sans ce changement (la résolution renvoyait -1 pour ces cartes).

## 0.249.16 (MD, D10 : `--pad-full6`)

`--pad-full6` (implique `--pad-full`) sonde, à chaque site de lecture de manette, les 256 masques du pad 3 boutons combinés
aux 16 états X/Y/Z/Mode du pad 6 boutons (4 096 forks par site, dédupliqués par (pc, masque, ext)). Hors défaut : coûteux.
Test `md_pad_full6` (4 352 sondes sur `fix_md_pad6`). Le multitap n'est toujours pas modélisé : D10 MD reste ◐.

## 0.249.17 (NES, C1 : verdict `speculative`)

Après la fermeture statique, NES crée une section par plage d'octets atteinte **uniquement** par le flot statique (jamais
exécutée ni forcée) avec le verdict `speculative`, défini dans `docs/COUVERTURE.fr.md` C1. Une observation ne devient jamais
spéculative. Les sections existantes et leurs numéros sont inchangés (les nouvelles sont ajoutées à la suite). Test
`nes_speculative`. C1 NES reste ◐ : pas de liveness (`ctx_mask`, pages lues, `io_read_mask`).

## 0.249.18 (PCE, C1 : verdict `speculative`)

Même définition que NES 0.249.17 : une plage atteinte uniquement par la fermeture statique (`DP_PCE_STATIC`, ni exécutée ni
forcée), dans une page de 8 Kio, devient une section `speculative` ajoutée après les autres. Test `pce_speculative`
(`fix_pce_slide`). C1 PCE reste ◐ : pas de liveness.

## 0.249.19 (MD, C1 : `speculative` compté)

Le rapport MD ajoute « N speculative (static closure only) » : plages maximales d'octets marquées par la fermeture statique
(`stat` 3) mais jamais exécutées ni forcées ; le `--summary` JSON gagne `sections_speculative`. MD n'a pas de liste de
sections typée : seul le compte est exposé. Test `md_speculative` (`fix_md_dma_fault`). C1 MD reste ◐ : pas de liveness.

## 0.249.20 (arbre de choix : `budget` devient `deferred`)

Conformément à `docs/COUVERTURE.fr.md` F2 / §6, l'issue d'un nœud de `--tree` nommée `budget` s'appelle désormais `deferred`
(`DP_ISSUE_DEFERRED`, valeur numérique 5 inchangée : les `tree.bin` existants se relisent). Elle désigne un état en attente non
exploré (file ou parking vidés à l'arrêt sans disque). Cellules F2 NES, PCE et MD passées ✅.

## 0.249.21 (second passage : plafond de racines levé si le pas total est illimité)

Le second passage (`--second-pass`) de NES, PCE et MD plafonnait à 256 racines statiques, le surplus étant seulement compté
(`capped`). Ce plafond est levé quand `max_total` vaut `UINT64_MAX` (aujourd'hui seulement MD `--unbounded` le positionne ;
NES et PCE n'ont pas encore d'option `--unbounded`). Les cellules G4 NES/PCE/MD restent ◐ : hors mode illimité, une racine
au-delà du plafond n'est pas reportée dans la frontière (§6), elle est comptée.

## 0.249.22 (second passage : plafond levé quand un lot disque existe)

Le plafond de 256 racines du second passage (NES, PCE, MD) est aussi levé dès qu'un lot disque (`--lot`) est ouvert : les
racines en excès sont parquées puis écrites dans le lot (reprise possible) au lieu d'être comptées `capped` et abandonnées.
Sans lot ni mode illimité, le plafond et le comptage restent ; G4 reste donc ◐ pour ces cas.

## 0.249.23 (S7 NES et MD ✅)

Les inférences de voisins de la carte NES et MD (padding ≥ 8 octets 00/FF ou ≥ 32, code ≤ 3 octets entre deux codes, données
≤ 1024 octets) utilisent les mêmes seuils et le même balisage « inférence » que PCE, déjà ✅ : cellules S7 NES et MD passées ✅.
Aucun changement de code.

## 0.249.24 (NES : liveness par section, C1)

Chaque section NES porte désormais `ram_pages` (masque des 8 pages de 256 octets de la RAM de travail lues par son code) et
`io_reads` (classes de registres lues : PPU `$2000-$3FFF`, pads `$4016/7`, autres `$4000-$5FFF`). Le rapport texte les
affiche (`ram-pages $XX io $X`). Les deux instances de `--jobs 2` fusionnent leurs masques. Test : `test_liveness`
(`nes`). Ce suivi n'alimente pas encore l'éviction par contexte (C2) : C1 NES reste ◐.

## 0.249.25 (PCE : liveness par section, C1)

Chaque section PCE porte `io_reads` : un bit par bloc matériel lu par son code (registre `>> 10` : VDC, VCE, PSG, timer, pad,
IRQ). Le rapport texte l'affiche (`io $XX`) ; `--jobs 2` fusionne les masques. Les pages de RAM lues ne sont pas suivies
(le cœur n'expose pas de hook de lecture RAM) : C1 PCE reste ◐. Test : `test_liveness` (`pce`). MD n'a pas de structure de
section par plage (seulement des compteurs) : son liveness reste à faire.

## 0.249.26 (NES : projection de contexte par pages lues, brique de C2)

`dp_nesmach_state_ctx_hash_ram(blob, pages)` ajoute au contexte grossier (registres, pc, mapper) seulement les pages de RAM
de 256 octets dont le bit est levé (test `nes` `test_ctx_projection`). Elle n'est **pas** branchée dans `--ctx-cap` : le test
`nes_ctx_cap` pose que ce mode exclut toute la RAM, et le brancher sur la liveness par instruction (0.249.24) le rendait plus
fin (plus d'éviction sur sa fixture). Le branchement sûr demande la liveness par section entière, calculée avant l'éviction
(les sections ne sont construites qu'à la fin du run) : C2 NES reste ◐.

## 0.249.27 (G3 MD ✅)

G3 MD : `--entry ADDR` est fait ; `--seed-banks` est sans objet tant que la machine MD ne modélise pas de commutation de banque
ROM du 68000 (SSF2 non modélisée, la fenêtre de banque du Z80 est suivie à part). La cellule passe ✅ sur ce périmètre ;
elle sera rouverte si SSF2 est modélisée. Aucun changement de code.

## 0.249.28 (MD : shoots du film, J2 ✅)

Pendant le rejeu d'un film MD (GMV/BK2), l'état de la machine est mis en file d'exploration tous les `--shoot-every N` cadres
(défaut 256, 0 = aucun) : le code atteint par fourche depuis ces points du chemin joué est exploré comme les autres fourches
(étiquette « forcé », jamais « naturel »). Le rapport affiche `GMV shoots: N checkpoints queued`. Test `md_movie_shoots`
(`--shoot-every 1`, 3 états pour 4 cadres). Cellule J2 MD passée ✅.

## 0.249.29 (J1 : générateur d'entrées commun, première tranche)

Le xorshift32 des sessions jouées de NES, PCE, MD et Sega/MSX n'existe plus qu'en un exemplaire, `dp_xorshift32`
(`src/core/rng.h`) ; les quatre copies locales y délèguent. Même suite de nombres qu'avant (les 170 tests passent, sorties
inchangées). Le `rnd` de `src/explore/sessions.c` (GB) reste à part : il re-sème un état nul, donc sa suite diffère
légèrement. J1 reste ◐ : la couche de sessions (pool, époques, inbox, mode humain) n'est toujours pas commune.

## 0.249.30 (J1 : pool de checkpoints commun)

Le pool de checkpoints des sessions jouées de NES, PCE et MD (`--play-pool`) est un composant unique, `DpCkptPool`
(`src/core/ckptpool.h` : `dp_ckpt_claim`, `dp_ckpt_pick`, `dp_ckpt_free`), au lieu de trois copies. Même consommation du
générateur qu'avant (tiré seulement au remplacement), sorties inchangées ; test unitaire dans `test_core`. J1 reste ◐ pour
ces plateformes : l'ordonnanceur d'époques, l'inbox vers le fork-explorer et le mode humain ne sont pas encore communs.

## 0.249.31 (J1 : croissance des files commune)

La politique de croissance des tableaux d'états en attente (file vivante bornée par `--queue-max`, zone de parking sans
borne) est `dp_vec_grow` (`src/core/growvec.h`), utilisée par NES, PCE et MD (parking) et NES, PCE (file) ; la file de MD
garde sa propre logique. Sorties inchangées, test unitaire dans `test_core`. J1 reste ◐ : l'enregistrement d'un état dans
l'inbox (`push_live`, dédup, éviction de contexte, déversement sur disque) reste propre à chaque cœur.

## 0.249.32 (C2 NES : contexte projeté sur les pages lues, `--ctx-live`)

NES : `--ctx-live` (avec `--ctx-cap N`) ajoute au contexte grossier les pages de RAM (256 octets) qu'un code a lues
jusque-là dans le run (union cumulée, alimentée par le hook de lecture de la liveness 0.249.24). Deux états qui ne
diffèrent que par une page jamais lue restent donc dans le même contexte et sont plafonnés ; une page lue sépare les
contextes. Sur `fix_nes_ctx` (boucle `INC $10`), `--ctx-cap 3` ferme 10 états, `--ctx-cap 3 --ctx-live` aucun (la page 0
est lue), test `nes_ctx_live`. Hors option, comportement inchangé. C2 NES reste ◐ : la liveness utilisée est globale au
run, pas par section, et l'option est opt-in. Observation de l'outil, pas une mesure de matériel.

## 0.249.33 (C2 NES : liveness par fenêtre PRG de 16 Kio)

`--ctx-live` projette le contexte sur les pages de RAM lues par du code de la **même fenêtre PRG de 16 Kio** que
l'instruction courante (et non plus l'union de tout le run). Fixture `fix_nes_ctx2` : la boucle de la fenêtre `$8000` ne lit
jamais la RAM, un sous-programme de la fenêtre `$C000` incrémente `$10` ; la RAM diffère à chaque passe mais le contexte de
la boucle ne la voit pas, `--ctx-cap 3 --ctx-live` ferme 4 états (test `nes_ctx_live_window`). Une boucle qui lit elle-même
la page (`fix_nes_ctx`) n'est toujours pas plafonnée. C2 NES reste ◐ : granularité = fenêtre, pas section ; opt-in.

## 0.249.34 (D12 NES : adresse de table gardée)

NES : le rapport « indirect jumps » compte aussi les adresses de table distinctes par site (`jmp (abs)` : l'opérande
d'où la cible est lue), `N table addresses`, gardées pour S4/§5. Il n'y a pas de plafond à 8 cibles (jusqu'à 65 536 paires
site-cible, débordement compté) ; test `nes_indirect` mis à jour. La cellule D12 reste ◐ pour PCE/MD (même ajout à faire).

## 0.249.35 (D12 PCE : adresse de table gardée)

PCE : même ajout que NES (0.249.34) pour `jmp (abs)` / `jmp (abs,X)` : `N table addresses` (l'opérande, indexé par X pour
`$7C`, d'où la cible est lue). Test `pce_indirect` mis à jour. MD n'a pas d'équivalent : `jmp/jsr (An)` lit un registre,
l'adresse de table est celle que S4 retrouve par `s4_table`. D12 reste ◐ pour PCE/MD : plafond et second passage à
confirmer cœur par cœur avant de cocher.

## 0.249.36 (D12 NES ✅)

D12 NES passe à ✅ : cibles observées sans plafond à 8 (jusqu'à 65 536 paires site-cible, débordement compté), adresse de
table gardée (0.249.34), cibles non exécutées semées par `--second-pass` (successeurs statiques, mots de table tracés par
provenance). PCE et MD restent ◐. Audit : 61 cellules ouvertes.

## 0.249.37 (D12 PCE et MD ✅)

D12 passe à ✅ pour PCE (sans plafond à 8, adresse de table gardée en 0.249.35, second passage et provenance) et MD (sans
plafond, l'adresse de table est celle que S4 retrouve, second passage). La ligne D12 est ✅ sur les six colonnes.

## 0.249.38 (MSX : débordement de pile dans le défilement texte)

Un run prolongé de Gradius 2 (MSX, ROM hors dépôt) mourait par `stack smashing detected` : le runtime MSX amont lit la
largeur de ligne dans `LINLEN` (RAM, modifiable par le jeu) et `text_scroll` la range dans un tampon de 40 octets.
`cmake/dp_msx_instrument.cmake` borne désormais cette largeur à 40 dans la copie instrumentée (la source m0boy reste
intacte). Les 172 tests passent ; le run de Gradius 2 repart sans le crash (observation de l'outil, non conclue ici).

## 0.249.39 (MD : reprise d'un spool dans un autre processus)

`dp_mdmach_load` recopiait l'état entier, pointeurs compris : après une reprise de `--spool` par un nouveau processus,
le Z80 sonore appelait un `read` mort et plantait (SIGSEGV observé sur Sonic). La charge conserve désormais les
pointeurs du processus courant (callbacks du bus Z80, `ym_log`, `coverage`, `screen`, hooks). 172 tests verts. La
reprise de Sonic tourne ensuite sans plantage (observation de l'outil ; fin du run non encore documentée).

## Entrée `.zip` et lot sur un romset (0.249.40)

- **ROM en zip** : `deepeel explore jeu.zip` (et toute commande qui charge une
  ROM GB, MD, libretro) lit directement l'archive : `dp_read_rom_file`
  (`src/core/zip.c`) prend la première entrée `.gb`/`.gbc`, sinon la première
  entrée, stored ou deflate. Test `zip_cli` : le rapport d'un zip (deflate et
  stored) est identique à celui de la ROM brute. Correction au passage : le nom
  d'entrée de `dp_zip_read` n'était copié que s'il faisait moins de 8
  caractères.
- **`--state-dir` crée son répertoire** : un répertoire absent désactivait le
  spool en silence, et `--unbounded` remplissait alors la RAM sans déversement
  possible (plusieurs Go en quelques secondes même sur une ROM de 32 Ko).
- **`summary.json`** : `map.methods_pct` (apport en % de la ROM de `tas`,
  `random`, `fork`, `attract`, `static`, jusque-là seulement dans le PNG) et
  `discovery[]` (par source : entrées, crédit, octets gagnés, dernier gain,
  `last_credit_frame` et `max_frame` de la lane).
- **`tools/romset_batch.py`** : lot sur un répertoire de zips. Copie chaque zip
  dans `roms/gb/<sous-dossier>/`, récupère les films TASVideos du jeu
  (soumissions de tous statuts, publiés ou rejetés, via l'API
  `submissions`), lance un `explore --play --unbounded` par ROM sur un stockage
  temporaire (`/run/media/m/Windows/claude/romset/tmp`) sous cgroup
  (`systemd-run --user --scope`, `MemoryMax`, sans swap, arrêt gracieux à
  80 %), puis conserve `map.png`, `summary.json`, `report.txt`, `run.log` et
  `meta.json` dans `results/gb/<sous-dossier>/<slug>/` et efface le temporaire.
  `results/gb/ROMSET.{log,csv,json}` cumulent les stats ; `index.json` rend le
  lot reprenable. `roms/` et `results/` sont ignorés par git.
- **Arrêt d'une ROM** : épuisement, `--stagnation` (aucune découverte pendant N
  frames de jeu) ou mur `--time-limit` (cap). Un run unique par ROM, pas de
  tranches : `--resume` recharge le pool, le spool et l'arbre mais pas la carte
  de couverture, les rapports d'une reprise ne décriraient que la dernière
  tranche (observé : 97,9 % puis 92,7 % sur Alleyway).
- **Verdict TAS (estimation, pas une mesure)** : deepeel n'a pas de détecteur
  de désynchronisation. Le lot compare les frames atteintes par le film
  (checkpoints × intervalle) et la dernière frame où il apporte encore du
  crédit : `complete`, `desync_est` (film joué jusqu'au bout mais plus rien
  après < 60 % de sa longueur), `cut_by_wall`, `stopped_early`, `rom_refused`
  (film d'une autre révision), `no_movie`. Les valeurs brutes sont gardées dans
  `meta.json` (`per_movie`).
- Les nombres produits sont des observations de l'outil (machine concrète, RAM
  à zéro), pas des mesures de matériel.

## 0.249.41 : carte de couverture persistante, recherche fork avec son propre budget (2026-11-05)

- **Bogue corrigé : `--resume` oubliait la couverture.** La carte (`exec_flags`, `data_flags`,
  `static_flags`, `src_flags`) n'était pas sauvegardée : une reprise repartait d'une carte vide, et comme
  le dédup d'états saute les états déjà vus, leurs octets n'étaient jamais redécouverts — un octet connu
  par la passe précédente redevenait inconnu. Elle est désormais écrite dans `<state-dir>/coverage.bin`
  à la sortie (écriture atomique) et réinjectée par OU au `--resume` (`resumed coverage map (N bytes
  known again)`). Test `resume_cov`. Limite connue : le journal de découvertes n'est pas persisté, donc
  `methods_pct` d'une reprise crédite le socle déjà connu à `static`.
- **`--search-limit S`.** Avec `--play`, la recherche à fourches (« total search ») ne tourne qu'après la
  phase de jeu ; quand `--time-limit` expirait pendant le jeu, l'alarme déjà déclenchée l'arrêtait avant
  son premier pas, d'où `fork` à 0 % sur les ROMs qui atteignent le plafond. `--search-limit S` lui
  donne S secondes de plus. Sans l'option, comportement inchangé.
- Lot (`tools/romset_batch.py`) : `--search` (600 s par défaut) transmet `--search-limit` ; `--guard`
  passe à 20 Go (le spool disque sert à décharger la RAM) ; un arrêt `mem-guard`/`state-guard` reprend
  maintenant avec `--resume` (3 fois au plus) au lieu de clore la ROM.
- Observations de l'outil sur Noobow (Japan), une exécution chacune : `explore` sans `--play` (fourches
  seules) 77,78 % en 911 s, 47 M d'états (11,5 Go) en spool ; jeu 300 s + recherche 300 s 82,39 % ;
  reprise de cet état (jeu 120 s + recherche 600 s) 84,11 %, sans perte. La recherche à fourches n'a
  rien ajouté après le jeu dans ces deux passes : 17,4 M d'états sont élagués « côtés déjà couverts ».
  Des 17,6 % encore inconnus, 55 % sont des franges de remplissage, 15 % des bords code/données.

## 0.249.42 : PNG de carte lisible sur les petites ROMs (2026-11-05)

- Le titre, la ligne d'identité et la ligne des parts par méthode passent à la ligne (mots entiers)
  au lieu de déborder sur les images étroites (ROMs de 32 Kio : 2 banques, 274 px de large).
- La grille de chaleur est dessinée à demi-résolution sur les deux axes (un pixel de chaleur = le plus
  chaud d'un bloc 2×2 de pixels de la carte), centrée sous chaque banque : elle montre où le jeu est
  passé, les données complètes restent dans les chunks du PNG. Sa légende (« chaleur (jeu, 1/4) ») passe
  sur sa propre ligne quand elle ne tient pas à côté de la signature.

### 0.249.42 (suite) : journal de découvertes persistant

- `<state-dir>/disc.bin` : le journal de découvertes (qui a trouvé combien, quand) est sauvegardé à la sortie
  et préfixé au `--resume`, à côté de `coverage.bin`. Les octets connus d'une passe antérieure restent
  crédités à la méthode qui les avait trouvés dans `methods_pct`, au lieu de tomber dans `static`.
- Observation sur Noobow (Japan) : la recherche à fourches après le jeu n'est pas « vide » mais bornée par
  `--search-limit` : 569 s, 13 965 chemins, 43 M de forks élagués « côtés déjà couverts » (ils ne coûtent
  pas de code mais du temps). Le gain mesuré vient surtout de la fermeture statique (+20 417 octets au
  round 1). Les passes de reprise ajoutent du temps de recherche sans repasser sur le jeu.

## 0.249.43 : coupure de tranche au milieu d'une instruction (2026-11-05)

- **Bug** : le vblank se déclenche dans la synchronisation PPU d'une lecture de registre (`ldh a,[$41]`,
  boucle d'attente du mode STAT). Une coupure de fin de tranche (`DP_CUT_SEGEND`, `run_epoch`) faite à cet
  endroit laissait `pc` après l'opcode ; à la reprise, l'opérande était exécuté comme un opcode
  (`$41` = `ld b,c`) et la pile recevait une adresse de retour décalée d'un octet. Le jeu partait en exécution
  hors ROM. Cas observé : Super Mario Land 2, film `dadinfinitum` (enregistré sur Gambatte), blocage vers
  la frame 41 750 (écran noir jusqu'à la fin).
- **Correctif** (`src/mach/m0boy.c`) : pendant le hook vblank, `SEGEND` est différé jusqu'au début de
  l'instruction suivante (`cut_pending`, pris dans `dp_gbl_hook_trace`) ; `QUIT` et le chien de garde de
  halt restent immédiats.
- **Vérification** (observation de l'outil) : sur le film complet (74 317 frames), le hash de la WRAM de
  deepeel est identique à celui de libgambatte (`deepeel bh`) à 74 316 frames sur 74 317 (seule la
  frame 2, pendant le démarrage, diffère). Les 174 tests passent (`lr_gambatte_parity` ignoré).
- Outils de diagnostic ajoutés : `deepeel bh … --dump FICHIER` (hash WRAM, registres et octets de pile par
  frame), `DEEPEEL_DEBUG_WATCHWR=lo-hi` (écritures sur une plage), et la trace `DEEPEEL_TAS_TRACEFRAME`
  affiche cycles, IME, IF et le haut de pile. GBHawk n'est pas exécutable ici (pas de BIOS DMG, `gbh.dll`
  non construit) : la référence est libgambatte, qui a enregistré ce film.

## 0.249.44 : tables de dispatch indexées prouvées par la provenance (2026-11-05)

- **Manque** (anatomie résiduelle de SML2, `code-code (dispatch non prouvé)`) : les `jp hl` dont l'index
  n'est pas statiquement connu laissaient leurs tables hors de la carte. SML2 dispatche par `rst $28` :
  le vecteur dépile l'adresse de retour, indexe la table placée juste après le site d'appel, puis saute.
- **`src/explore/provenance.c`** : deux preuves nouvelles, balisées statiques (inférence de l'outil, pas
  observation) et alimentant la fermeture :
  1. idiome `ld hl,TABLE … add hl,idx … ld h,(hl) … jp hl` : les mots de la table sont prouvés données et
     leurs cibles prouvées entrées de code ;
  2. vecteur `rst` reconnu par sa forme (`pop hl`, `add hl,…`, `jp hl`, sans retour entre) : la table en
     ligne après chaque site `rst` exécuté est parcourue de la même façon.
  Une table exige ≥ 2 entrées plausibles (≤ 128) : mots contigus dans la même vue de ROM, premier octet de
  la cible un opcode légal et non un remplissage `00`/`FF`, emplacement jamais exécuté par le flot naturel
  (les forks spéculatifs peuvent exécuter les octets d'une table ; seuls le flot naturel et un opcode
  illégal l'arrêtent). Compteur `prov_table_targets`.
- **Tests** : fixtures `dispatch` et `rstdispatch`, vérifiées dans `test_explorer` (entrées jamais prises
  prouvées code, mots de table prouvés données).
- **Observation de l'outil** (SML2, relecture du film `dadinfinitum` seul, sans fourches) : octets inconnus
  51 750 → 46 923, plages 1 101 → 1 050 ; `code-code` 10 388 → 8 765 octets, `code-data` 34 654 → 31 475.
  C'est de l'inférence statique : à croiser avec la couverture par fourches.

## 0.249.45 : inférence des bords mixtes code/donnée (2026-11-05)

Deux règles d'inférence ajoutées à `dp_map_build` (classes inférées, comptées dans « dont inféré ») :
- **Bord mixte** : une plage inconnue (≤ 2048 o) entre du code qui se termine par un transfert
  inconditionnel (`ret`, `reti`, `jp hl`, `jp nn`, `jr e`) et de la donnée connue, ou entre de la donnée
  connue et du code connu, est classée donnée. Le code ne peut pas tomber dedans ; l'entrée du code
  suivant borne la plage. Donnée courant jusqu'à la fin de banque : idem.
- **Frange de remplissage** : plage ≤ 64 o au contact d'un remplissage, dont ≥ 75 % d'octets `00`/`FF`.

Observation de l'outil (SML2, relecture du film seul) : inconnu 46 923 → 26 117 octets, `code-data`
31 475 → 10 770. C'est une inférence, pas une observation d'exécution : les plages `code-code`
(dispatch non prouvé, 8 765 o) restent inconnues, faute de preuve.

## 0.249.46 : dispatchs par `call`, tables copiées en WRAM, code probable (2026-11-05)

- **Dispatcheur par `call`** (`provenance.c`) : la reconnaissance de forme du vecteur `rst` (`pop hl`,
  `add hl,…`, `jp hl`, sans retour) s'applique à toute routine appelée par un `call` exécuté ; la table en
  ligne qui suit le site d'appel est prouvée comme pour `rst`.
- **Tables de pointeurs copiées ROM→WRAM** : une copie verbatim ROM→WRAM dont la source est une suite de
  ≥ 3 mots, tous cibles plausibles (opcode légal, ni `00` ni `FF`), dont au moins une est déjà une entrée de
  code exécutée, est une table de pointeurs de code : ses autres entrées sont du code (preuve par
  co-appartenance), ses mots de la donnée.
- **Classe `probable-code`** (couleur orange, « incertain ») : plage inconnue entre deux blocs de code qui se
  décode linéairement sans opcode illégal, se termine exactement sur un transfert inconditionnel, a des
  adresses de saut/chargement plausibles et des `jr` sur des frontières d'instruction. Heuristique : des
  données aléatoires se décodent parfois ; la classe est comptée en code et en « inféré » mais reste séparée
  (`summary.probable`, champ `probable-code` du JSON) et n'alimente jamais la fermeture.
- **Observation de l'outil** (SML2, relecture du film seul) : le dispatch par `call` et les tables copiées ne
  changent rien sur cette ROM (aucun site de ce type en plus) ; `probable-code` gagne 252 octets
  (inconnu 26 117 → 25 865). Les ~8 500 octets de `code-code` restants sont en grande partie des données
  graphiques bornées par du code : seule l'exécution (fourches) peut les trancher.

## 0.249.47 : mappers approchés (HuC1, HuC3, Pocket Camera, MBC6, MBC7) (2026-11-05)

Sur le romset de 1 659 ROMs, 15 portaient un mapper sans variante compilée (HuC1 ×10, Pocket Camera ×3, HuC3,
TAMA5) et étaient écartées comme `unsupported` sans aucune couverture. `dp_mach_pick_variant` leur
associe désormais une variante dont le banking ROM est compatible : HuC1 → MBC1 ; HuC3, Pocket Camera, MBC6
et MBC7 → MBC5. `dp_mach_mapper_note` émet un avertissement sur stderr : les extras de la cartouche
(port infrarouge, capteur, horloge, accéléromètre, EEPROM, seconde fenêtre MBC6) ne sont **pas** modélisés,
donc la couverture de ces ROMs est une observation sous approximation, pas une mesure du matériel.
Toujours non supportés : TAMA5 et MMM01 (registres par quartets / banque 0 mobile).
Écart restant du matériel : CGB non modélisé (seulement 8 ROMs « CGB enhanced » dans ce romset, aucune
CGB-only), RTC MBC3, liaison série, valeurs de LY/STAT/DIV/TIMA non fourchées. Essai : Nectaris GB (HuC1)
est maintenant exploré (`--redo` du batch pour les 15 ROMs).

## 0.249.48 : fourches de valeur sur les registres événementiels (2026-11-05)

`fork_event` ne lisait que `ldh a,(n)`, `ld a,(C)` et `ld a,(nn)`, et seulement LY, STAT et DIV avec
des valeurs fixes. Il couvre maintenant :
- **Formes de lecture** : `ld r,(hl)`, `ld a,(hl+/-)`, `ld a,(bc/de)`, `cp/and/or/xor (hl)` et
  `bit n,(hl)` dont l'adresse est `$FFxx` (un code qui charge `HL=$FF44` puis boucle n'était pas fourché).
- **Registres** : TIMA ($00, $80, $FF), SC (transfert fini / en cours : l'attente série sans partenaire),
  SB ($00, $FF), en plus de LY, STAT et DIV.
- **Constante du test qui suit** : si l'instruction suivante est `cp n` ou `sub n`, les valeurs `n-1`, `n`,
  `n+1` sont fourchées (une attente de la ligne 100 ou d'un compteur précis atteint enfin la bonne valeur,
  pas seulement 0/143/144/153).
- **STAT** : les quatre modes PPU × le bit de coïncidence LY=LYC (8 valeurs).
Plafonds par site et par registre inchangés ; les forks sont comptés dans `forks_event`. Tests : fixtures
`lywait` et `hlwait` (le second échoue avec l'ancien code). Les chiffres de couverture sur ROMs réelles
sont à refaire : les fourches de valeur augmentent le nombre de chemins, donc le coût.

Mesure A/B (même ROM, mêmes bornes `--jobs 1 --time-limit`, seul le code de `fork_event` change ; observations
de l'outil) : Puzznic 2 172 → 2 172 octets inconnus (120 → 161 variantes) ; Tetris Rev A 55 → 55
(438 → 1 273 variantes) ; Kirby's Dream Land 34 183 → 33 697 (219 → 684 variantes). Le gain est réel mais
faible : la plupart des attentes de ces ROMs sortaient déjà par les bornes de boucle ; le coût en variantes
est multiplié par 2 à 3.

## 0.249.49 — file en RAM compressée, spool par différences

- **File compressée.** Les images d'état en attente dans les files (anneau local et pool partagé) sont
  gardées compressées (deflate brut RLE, préfixe de longueur sur 4 octets, drapeau `packed` dans
  `DpPathState`), décompressées au dépilage ou au vidage vers le disque. La file prioritaire reste brute
  (petite, plafonnée). Une image qui ne rétrécit pas reste telle quelle.
- **Spool par différences (format v2).** Nouvel enregistrement `DPSD` : l'état est stocké en XOR contre un
  enregistrement « clé » complet récent (jusqu'à 256 différences par clé, puis nouvelle clé), le décalage
  de la clé précède le corps. Versionnage : les enregistrements `DPSR` (brut) et `DPSZ` (zlib) des anciens
  spools restent lus tels quels et `--resume` accepte un spool mixte ; un ancien binaire ne lit pas `DPSD`.
- **Mesure** (Puzznic, `--queue 4 --queue-max 4`, 120 s, observation de l'outil) : spool 10,4 Mo → 4,5 Mo
  pour 47 878 états écrits (≈217 → ≈94 octets par état), même couverture (1 570 octets inconnus).
  Reprise d'un spool écrit par l'ancien binaire : sans erreur.

## 0.249.50 — carte inférée monotone

Les classes inférées de la carte (`inferred-*`, `probable-code`) sont mémorisées par octet
(`DpResults.map_sticky`) : un octet une fois classé par inférence ne retombe plus à « inconnu » quand une
passe ultérieure déplace les voisins qui le justifiaient (les classes observées l'emportent toujours, et
elles viennent des drapeaux de couverture, monotones). Persisté à côté de `coverage.bin`
(`coverage.bin.sticky`, magic `DPMS`) et rechargé par `--resume` ; un fichier absent ou d'une autre
géométrie est ignoré (compatibilité ascendante). Essai Puzznic 40 s + reprise 40 s : 24 523 octets inférés
conservés, inconnu inchangé (2 172 octets).

Journal du romset (`results/gb/ROMSET.log`, observation) : 0 arrêt `mem-guard`, 0 `hard-kill` ; les 27
`reason=interrupted` datent du SIGINT du batch (03:39) ; l'unique `state-guard` (frogger-usa, 10:14) est le
garde-fou de **disque libre** (< 15 Gio sur le scratch), pas la RAM, et la ROM a ensuite fini sans arrêt
(`time budget reached`, 99,912 %). L'entrée « noobow-japan 24 344 Mo » n'existe plus dans le journal : le
dernier run se termine par `time budget reached` (7 875 Mo d'état).

## 0.249.51 — reprise du spool sans balayage complet

`--resume` vérifiait les enregistrements du spool depuis l'offset 0 : sur un spool de 53 Go (≈243 millions
d'enregistrements, état SML2 sans limite) cela relit tout le fichier à ≈150 Mo/s avant de démarrer (≈6 min,
observation). Le fichier curseur `<spool>.rd` contient déjà `wr` du dernier flush, une frontière
d'enregistrement : le contrôle des enregistrements entiers part maintenant de là et ne parcourt que la
queue écrite après. Sans curseur valide, le balayage complet reste le repli.

### Arbre sur disque (0.249.52)

L'arbre est maintenant stocké en blocs de 65 536 choix. Tout bloc plein (sauf le premier) est écrit dans
`<state>/tree.spill` et sa RAM libérée ; ses choix encore ouverts restent dans une table (`pin`). La RAM ne porte
plus que la frontière, quelle que soit la taille de l'arbre. (Première version « bloc entièrement fermé » :
inopérante, les 91 372 choix ouverts du `tree.bin` SML2 sont répartis dans les 4 300 blocs.) L'enregistrement
de `tree.bin` passe par un fichier temporaire renommé. Le rechargement de `tree.bin` fait de même bloc par bloc (le `tree.bin` SML2 de
282 millions de choix, 5,6 Go, se recharge sans les 5,6 Go de RAM). Le format de `tree.bin` est inchangé :
les anciens fichiers se lisent. Avant ce changement la croissance de l'arbre (20 octets par choix, sans plafond
ni repli disque) était la principale consommation de RAM d'une exploration longue. Un `tree.bin` illisible reste
mis de côté (`tree.bin.prev`) avec un message au lieu d'être écrasé.

### Diagnostic mémoire (0.249.53)

`kill -USR1 <pid>` d'un `deepeel explore` en cours écrit sur stderr la taille de chaque structure en RAM :
malloc total, file, pool partagé (images), différés, ensemble de dédup exact, arbre (blocs résidents, choix
épinglés). Sert à savoir ce qui grossit sans arrêter le run (gdb est bloqué par ptrace_scope).

### File d'exploration bornée en octets (0.249.54)

Mesuré par `kill -USR1` sur SML2 (reprise, `--queue 60000`, `--jobs 8`) : file en RAM = 239 999 états de
25 193 octets = 5,9 Go, soit 4 × `--queue` (croissance adaptative `queue_max`, qui n'était freinée que par
`--mem-cap`, absent). La file est maintenant bornée aussi en octets : `--queue-mem MB` (défaut 1536). Au-delà,
les états vont dans le spool disque (jamais jetés) et reviennent quand la RAM le permet.

### Écriture du spool en parallèle (0.249.55)

Mesuré par `perf record` sur le drain parallèle de SML2 (`--jobs 15`) : 3 threads sur 13 actifs, les autres
en attente de verrou, et plus de la moitié des échantillons dans `deflate`. Chaque état déversé était
compressé deux fois, la seconde (delta XOR contre l'image clé) **sous le verrou du spool** : tous les workers
passaient par un seul compresseur. Le delta est maintenant calculé hors verrou sur une copie de la clé (une clé
périmée reste valide : les clés ne sont jamais effacées du fichier). Le format du spool est inchangé.

### Drain en profondeur et arrêt dans un chemin (0.249.56)

Mesuré sur SML2 (`--unbounded --jobs 15`) : le spool grossissait de 0,8 Go par minute sans fin (76 Go). Cause :
le pool partagé dépilait le plus ancien (parcours en largeur, frontière = une couche entière de l'espace
d'états) et `spool_feed` le remplissait de vieux enregistrements à chaque pas, repoussant les fourches neuves
sur le disque. Avec `cfg.dfs` (recherche totale), le pool dépile maintenant le plus récent (frontière = pile,
profondeur × fan-out) et le spool n'est plus qu'une réserve, rechargée jusqu'à un plancher de 256 états quand
le pool se vide.

Autre mesure : SIGINT n'était testé qu'entre deux pas d'exploration, jamais dans un chemin ; sans mur de pas
(`--unbounded`), un chemin qui pousse des fourches sans fin ne s'arrêtait plus (arrêt > 30 min, disque qui se
remplit). Tous les 1024 pas, un chemin voit l'interruption et reste en attente comme continuation, comme au
mur de pas : l'arrêt est prompt et ne perd rien.

### SIGINT pendant la recherche totale (0.249.57)

Cause trouvée du « SIGINT sans effet » : avec `--play`, `sessions.c` installe un gestionnaire coopératif qui
ne touche que les sessions de jeu, et il n'était jamais remplacé après la phase de jeu. Pendant le drain, le
signal n'atteignait donc jamais `dp_explore_interrupt` (runs de 30 min à 3 h sans arrêt, disque plein). Le
gestionnaire de l'explorateur est réinstallé à l'entrée de la recherche totale ; en plus, une demande d'arrêt
est honorée avant d'ouvrir un nouveau chemin et à l'intérieur d'un long chemin. Mesure sur un état neuf :
arrêt et rapport en 28 s. Les workers du drain s'adaptent aussi à la RAM libre (mise en pause par palier de
400 Mo sous 4 Go), et les continuations de chemin retournent dans le pool avant le spool.

### Élagage par destination sous --unbounded (0.249.58)

Cause de l'explosion du spool (5,8 Go en 10 min) : (1) les forks de branchement dont les deux côtés sont
déjà exécutés étaient *différés* au lieu d'être jetés, donc tous envoyés au disque (plafond infini) ;
(2) les forks d'événements (LY, STAT, DIV, TIMA, série), de port P1 et de touches n'avaient aucun plafond
sous `--unbounded` : chaque lecture dans une boucle d'attente relançait jusqu'à 12 variantes.
Les valeurs testées viennent de constantes écrites en dur au site (plus la constante du `cp` qui suit) :
un tour par (site, registre, constante) suffit. Sous `--unbounded`, un fork « deux côtés couverts » est
jeté (compté en `defer_final`), et les forks événement/P1/touches font un seul tour par site.
`--nocredit-cut N` ferme un chemin sans nouveau crédit depuis N pas (20000 par défaut en `--unbounded`).
Observation de l'outil sur SML2, état neuf, 20 min : RAM ~480 Mo, spool 325 Mo (au lieu de 5,8 Go),
inconnu 52 064 octets (74 355 avant).

### tree.bin v2 : seuls les choix ouverts (0.249.59)

Le `tree.bin` v1 stockait les 322 M de choix (6 Go) ; sa lecture réécrivait ces 6 Go dans `tree.spill`, et chaque
sauvegarde en écrivait 6 de plus. Sur le disque ntfs3 (écritures ~14 Mo/s, pas de fichier creux : un écrit à
6 Go de décalage alloue 6 Go de zéros) cela faisait ~7 min par chargement. Les lectures, elles, vont à 200-370 Mo/s.
Un choix fermé n'est plus jamais lu (son issue est dans les compteurs de l'en-tête, sa profondeur ne sert qu'à ses
enfants, tous issus de choix ouverts) : la v2 n'écrit que les choix ouverts et les compteurs. Mesure sur l'état
neuf : 358 Mo → 3,4 Mo, rechargé avec les mêmes compteurs. Une v1 se lit encore, compactée à la volée sans écrire
le spill, puis la prochaine sauvegarde est en v2. Le spill est indexé par un numéro de bloc relatif (`spill_skip`).

### Classe `probable-data` (0.249.60)

Les plus grosses plages inconnues de SML2 (20:64bc, 23:7820, 21:7203, 21:781d, 18:7401, 9:6da6) sont des données :
décodées en SM83 elles donnent ~4 % d'opcodes illégaux (le taux d'octets aléatoires), et 32-52 % de leurs octets sont des
octets de tramage (`aa/ab/ba/ee…`, ~5 % attendus au hasard). Les octets « code-forcé/statique » qui les bordent sont
du même type. La carte reçoit donc une classe `probable-data` (vert pâle, « incertain ») : une plage inconnue d'au moins
32 octets dont le score moyen d'un modèle de bigrammes d'octets, appris sur les octets que ce run a *observés* (code
naturel contre données lues/copiées/statiques/tables), atteint 0,5 nat/octet. Elle est signalée à part dans le résumé
(« data includes X% probable »), jamais réinjectée dans la fermeture et jamais mémorisée en sticky (recalculée à chaque
construction). Validation croisée par banques sur SML2 : 0,2 % des fenêtres de code observé sont jugées données, 85-92 %
des fenêtres de données observées le sont (fenêtres de 32-128 octets). Hors échantillon, avec l'ancienne carte
(`map.92.png`) : sur 20 639 octets prédits, aucun n'est devenu du code *naturel* ; ils sont devenus `code-static`
(3 763), `code-forced` (1 093), donnée (3 594) ou sont restés inconnus (12 178) : la passe statique produit du faux code
sur ces zones. Observation de l'outil : inconnu de SML2 21 937 → 10 261 octets sur une copie de l'état.

Pistes mesurées et **non retenues** : (a) « inconnu entre une donnée et un remplissage = donnée » est réfutée par les
classes observées (12 plages sur 12 dans cette situation sont du code) ; les 314 franges de remplissage ne sont pas du
remplissage (0-50 % de 00/FF) mais des tuiles, les courtes (< 32 octets, ~2,2 Ko) restent inconnues ; (b) l'amorçage
statique par tables de pointeurs (suites de ≥ 4 mots dont la moitié vise du code exécuté) ne donne que 23 racines à
décodage propre sur SML2, dont une seule au début d'une plage inconnue : rendement trop faible pour le coût.

## Segments aléatoires depuis les checkpoints TAS (0.249.61)

Par défaut, une session en touches aléatoires sur deux repart d'un snapshot TAS
(le moins déjà tiré) et joue un segment long (`3600 + [0, 7200]` images) : chaque
graine du run quasi parfait reçoit ainsi de nombreuses explorations aléatoires de
son voisinage. Réglages : `--tas-rng-share N` (1 départ sur N, défaut 2),
`--tas-rng-frames N` (défaut 3600), `--no-tas-rng` pour revenir à l'ancien
comportement. Les snapshots restent aussi des graines de fourches. Non mesuré sur
ROM réelle à ce stade : à comparer à temps égal (inconnu avant/après).

### 0.249.62 : snapshots TAS toutes les 5 s, plusieurs segments par snapshot, fenêtre `--mapwin` adaptée à l'écran

- `--tas-ckpt-secs` passe à 5 (défaut), `--tas-ckpt-max` à 4096. Chaque snapshot reçoit `--tas-rng-per` (8) segments
  aléatoires longs avant que les segments normaux reprennent (`--tas-rng-share` défaut 1).
- Mesure (SML2, 1 run chacun, 4 workers, charge machine variable — indicatif, pas une preuve) : inconnu 22 521 octets /
  676 plages sans (`--no-tas-rng`), 20 684 / 578 avec ; mais 1,62 M images jouées contre 1,34 M (confondant).
- `--mapwin` : la disposition suit la zone utilisable de l'écran ; 8 blocs par ligne si ça tient, sinon plus haut
  avant plus large ; `--mapwin N` est un plafond d'échelle.

### 0.249.63 : tous les films, tous rejoués (file d'attente de films)

Prendre tous les films possible est la norme : plus de plafond de 8 films
(`--tas`, `--tas-dir`, jusqu'à 1024). Quand il y a plus de films que de machines,
chaque worker dont le film se termine enchaîne sur le suivant non réclamé
(état d'allumage, profil de synchro propre au film) : « TAS queue: worker N
starts movie k/n ». Un film refusé (autre ROM) est sauté, déjà rapporté ;
la fin de `--tas-solo` attend la file vide. `tasvideos_fetch.py` lit la
Subs-List du jeu (tous statuts). Test : 20 copies d'un film sur 16 machines,
4 films en file, tous rejoués.

### 0.249.64 : echoRAMFix des VBM, outils de comparaison de replay

- VBM : le bit 6 de l'octet d'options (0x17) à 0 (echoRAMFix désactivé, cas des
  films VBA anciens) met `$E000-$FDFF` en mémoire séparée de la WRAM (knob
  `sync_echo_old`, patch m0boy). Sans effet mesuré sur Zelda LA (dumps identiques).
  `DEEPEEL_TAS_NOECHO=1` l'inhibe.
- `DEEPEEL_TAS_RAW=<fichier>` écrit les 0x2000 octets de WRAM à chaque vblank ;
  `tools/replay_compare.py` les compare à ceux de la référence GBHawk
  (`GBH_RAW`, `GBH_TRACE_FROM/TO` pour la trace d'instructions d'une frame).
- Observation (SML2, film GBHawk) : sur la frame LCD éteint 518/852, les traces
  d'instructions deepeel et GBHawk sont identiques (10 235 instructions, mêmes
  PC et registres, décalage de cycles constant). L'écart de WRAM aux frames
  519..524 n'est donc pas un écart d'exécution mais de phase d'échantillonnage :
  deepeel échantillonne à son vblank, GBHawk à la fin de son chunk de 70 224
  cycles (~14 481 cycles plus tard, le même décalage que `sync_pad_delay`).
  La comparaison de WRAM doit se faire hors des boucles de remplissage ou avec
  ce décalage.

## Fidélité VBM : motif d'allumage VBA-rr (0.249.65)

Un harnais VBA-rr sans tête (hors dépôt, construit à part) rejoue un VBM image
par image et sort la WRAM et les registres. Constats (observations de l'outil) :
- cœur V8 (VBM mineure ≥ 1) : WRAM initiale en motif `$FF`×8 / `$0F`×8 par blocs
  de `$800` alternés, HRAM à `$FF` ; deepeel l'applique désormais ;
- cœur V7 (mineure 0) : WRAM à zéro, aucun motif. Sur Zelda 2076, deepeel colle
  au cœur V7 jusqu'à l'image 23 ; le premier écart restant est `$C13D` (RNG qui
  lit LY), donc une différence de cadence cycle par cycle, à traiter ensuite.

## Fidélité VBM : lecture de LY du cœur VBA 1.7.2 (0.249.66)

Le cœur V7 de VBA-rr applique les cycles d'une instruction au LCD *après*
l'avoir exécutée : une lecture de `$FF44` voit donc l'état du début de
l'instruction. Pour les films VBA à mineure 0, le réglage `sync_ly_lag` (20
cycles, `DEEPEEL_TAS_LYLAG` pour expérimenter) fait lire la ligne de 20 cycles
plus tôt. Sur Zelda LA (film 2076), la première divergence de WRAM contre le
harnais VBA V7 passe de la frame 24 à la frame 43 (observation de l'outil).
Reste ouvert : la divergence de la frame 43 (`$C13D`, RNG), puis l'épée.

### Mise à jour 0.249.67 : lecture de LY au début d'instruction

Le décalage `sync_ly_lag` (profil `vba_old`, VBA V7) devient un décalage signé
appliqué à l'offset de ligne **au début de l'instruction** lectrice
(`gbl_insn_cyc`), défaut −16 T (`DEEPEEL_TAS_LYLAG`). Observation de l'outil
sur Zelda (bobmario2-zeldala.vbm, référence VBA V7 : harnais hors dépôt) : les
42 premières frames sont identiques à la référence (hors HRAM/OAM) ; la frame
43 diverge (RNG $C13D, compteurs $C1xx/$D00x). Un balayage coût de dispatch ×
lag montre que seules les combinaisons (20,−16), (16,0), (8,16) passent la
frame 43 ; aucune ne va au-delà : la cause restante n'est pas encore
localisée (un écart de découpage de frame apparaît dès la 43). Le replay
intégral de Zelda sans désynchro n'est donc **pas** atteint.

### Mise à jour 0.249.68 : entrée du film et dispatch d'IRQ (VBA V7)

- L'entrée de la frame 0 du film est appliquée dès le départ aussi pour les
  cœurs VBA 1.7.2 (le masque de la frame n+1 est alors présenté au vblank n) :
  l'ancien traitement retardait chaque entrée d'une frame (le Start de la
  frame 43 de Zelda était vu une frame trop tard).
- Le dispatch d'interruption ne coûte plus que 16 T au lieu de 20 sur ce
  profil (`DEEPEEL_TAS_IRQTRIM`, défaut 4) ; `sync_ly_lag` repasse à 0.
- Observation de l'outil (Zelda, bobmario2-zeldala.vbm, référence VBA V7) :
  première divergence à la frame 2088 au lieu de 43 ; 3269 des 4921 premières
  frames sont identiques. Les écarts suivants sont de courts décalages d'une
  frame ($C5D0, $D744) puis des différences de $C13D (RNG).

### Mise à jour 0.249.69 : réveil du HALT sur la grille sonore de VBA V7

- VBA 1.7.2 exécute une CPU en HALT par pas bornés par
  `min(gbLcdTicks, gbLcdLYIncrementTicks, soundTicks, timer)`. Le pas sonore
  vaut 24 ticks (96 cycles) : le réveil sur le vblank tombe donc sur une
  grille de 96 cycles (11316 des 11413 entrées vblank en HALT de Zelda sont
  alignées). Le profil V7 reproduit cette grille (`DEEPEEL_TAS_HALTSND`,
  phase en cycles, défaut 32 ; 0 = réveil matériel) ; elle ne s'applique
  qu'aux réveils par vblank LCD allumé (`sync_halt_snd`, `gbl_halt_wait`).
- Cas limite reproduit : quand le flip LY=144 tombe exactement sur la grille,
  `soundTicks` reste collé à 0, le pas suivant n'est plus borné par le son et
  la CPU dort jusqu'à l'incrément de LY suivant (+456 cycles).
- Diagnostic `DEEPEEL_TAS_HRAM` : vidage de la HRAM à chaque vblank.
- Observation de l'outil (Zelda, bobmario2-zeldala.vbm, référence VBA V7) :
  12873 des 13714 frames sont identiques à la référence (4335 avant). Restent
  des écarts isolés (frames 2088, 2090, 3579, 3669, 3765, 6252–9103) et une
  dérive finale à partir de la frame 13278 (sprites des crédits), pas encore
  expliquée. Ce n'est pas une mesure de matériel.

### Mise à jour 0.249.70 : attente d'IRQ VBlank, mode 3 fixe, EI (profil VBA V7)

Trois comportements de VBA-rr V7 reproduits dans le profil `sync_halt_snd` :

- **Attente d'IRQ (`gbInterruptWait`)** : posée à 6 ticks au passage LY=144,
  décrémentée à chaque instruction ; la VBL est servie à la fin de la première
  instruction qui démarre avec l'attente expirée. `EI` l'annule, `RETI` non.
- **Mode 3 de durée fixe** (43 ticks = 172 dots) : pas de pénalité sprite /
  fenêtre / SCX dans ce profil.
- **Écriture STAT** en mode LCD < 2 : lève IF STAT (quirk DMG de VBA).

Observation de l'outil sur le film Zelda `bobmario2-zeldala.vbm` (WRAM
comparée à VBA V7) : 13106 images identiques sur 13714 (12873 en 0.249.69),
première divergence à l'image 24. Cause ouverte : l'IRQ STAT de l'image 23 est
servie par VBA 2 ticks après le pc 476f, par deepeel ~44 cycles plus tard
(phase de la ligne 153 → LY=0 différente). Essai « LY 153→0 à 24 cycles » : sans
effet, retiré. Le replay intégral n'est pas encore sans désynchronisation.

Piste explorée, non retenue (observations de l'outil) : dans VBA V7, la
première ligne après un allumage du LCD dure 1140 ticks (mode 0, LY=0) : le
premier VBlank tombe 17443 ticks après l'écriture LCDC, soit ~4104 cycles de
plus que le matériel. Reproduit dans deepeel (ligne 0 allongée), l'effet fait
diverger plus tôt (première image 10 au lieu de 24 : `$DB97` apparaît une
image trop tôt) — le modèle reste à compléter avant d'être activé. De même,
LY 153→0 à 24 cycles n'a aucun effet mesuré.

### Outil de lockstep VBA V7 / deepeel (`tools/lockstep.py`)

Lance le harnais VBA V7 (`VBAH_PCT`, `VBAH_T0`, `VBAH_T1`) et deepeel
(`DEEPEEL_TAS_INSTR`) sur le même film, journalise chaque instruction des
deux côtés (fenêtre en ticks VBA, 1 tick = 4 cycles deepeel), aligne les
traces et indique chaque changement du décalage de temps (cycles deepeel −
4 × ticks VBA) puis la première instruction où pc ou registres diffèrent,
avec le contexte des deux côtés. `--mask-a LO-HI` ignore A/F sur une plage de
pc (lectures du joypad, bits 7-6 de `$FF00`), `--env K=V` passe les réglages
de profil à deepeel. Observations de l'outil, pas des mesures de matériel.

Constats avec cet outil (Zelda, `bobmario2-zeldala.vbm`) : avec
`DEEPEEL_TAS_IRQTRIM=20`, `DEEPEEL_TAS_HALTSND=0` et `LY153_CMP` à 24
cycles, les images 22 à 25 sont identiques instruction par instruction ; la
première divergence d'image passe de 24 à 25, mais le total d'images
identiques baisse (3740 contre 13106) : réglage non retenu par défaut. Reste
une phase des lignes visibles (LYC) en retard de quelques cycles dans la
trame 24 (le front LY32 arrive 2 cycles après la fin de l'instruction où VBA
l'a vu).

Constat du lockstep (SRAM) : le harnais VBA-rr V7 démarre avec la RAM de
sauvegarde à `$FF` (Zelda lit `$FF` en `$A100` à l'image ~14). deepeel
l'initialise désormais ainsi pour les films VBA mineur 0 (`DEEPEEL_TAS_NOINIT`
désactive). La première divergence d'instruction de la fenêtre 180k–592k ticks
passe du tick 246 490 au tick 372 306 (VBL prise une instruction plus tôt par
VBA). Observation de l'outil, pas une mesure matérielle.

Pistes lockstep suivantes (non retenues, observations de l'outil) :
- Le réveil du HALT par une requête VBL tombant sur la grille son (reste
  `(t−ph)%96` de 0 ou 6 selon la phase) attend jusqu'au prochain incrément de
  LY (+360 cycles), comme le cas déjà modélisé à 22.
- Avec `IRQTRIM=20`, `HALTSND=0`, `LYLAG=-3`, ce réveil et l'activation LCD
  décalée de −6 cycles, les 125 679 premières instructions de la fenêtre
  180k–700k ticks sont identiques, la première divergence d'image passe de 24
  à 38 et 13 143 images sur 13 714 sont identiques (13 106 par défaut). Le
  résidu est la phase des lignes : VBA réinitialise son compteur de LY sans
  report à chaque trame (quantifié à la fin d'instruction), alors que celle de
  deepeel est exacte ; un simple ancrage de la ligne 0 sur l'instant observé
  fait chuter le total (2 457 images) et n'est pas retenu.

### Mise à jour 0.249.71 : profil VBA V7 complet (replay Zelda identique)

Le film `bobmario2-zeldala.vbm` (VBM mineur 0, ROM Link's Awakening) se rejoue
sur les 13 714 images identiques à VBA-rr V7 (VRAM comparée image par image,
observation de l'outil), là où le profil précédent en donnait 13 106. Défauts
du profil VBA mineur 0 : `LYLAG=1`, `IRQTRIM=20`, `HALTSND=0` (variables
`DEEPEEL_TAS_*` toujours prioritaires). Règles ajoutées au runtime
(`cmake/m0boy-tas-compat.patch`), toutes actives seulement avec ce profil :

- SRAM à $FF au démarrage (0.249.70) ; activation LCD comptée depuis le début
  de l'instruction qui écrit LCDC (+2 cycles) ;
- lecture de LY : passage 153→0 vu 6 ticks après l'entrée en ligne 153
  (relatif à l'observation, `q153`) et événement de passage armé en
  permanence ;
- une correspondance LYC ne lève IF STAT que si IE bit 1 est positionné au
  moment de la comparaison (`gbCompareLYToLYC`) ; les sources de mode, non ;
- HALT réveillé par VBlank : marche pas à pas comme VBA (pas = min(prochaine
  frontière LCD, prochain tick son ; un pas qui démarre exactement sur la
  grille de 24 ticks n'est pas borné par le son) ; `gbInterruptWait` (6 ticks)
  est consommé par la durée des pas à partir de celui de l'entrée VBL, et
  l'IRQ est prise à la fin du premier pas qui démarre avec l'attente expirée.
  Remplace la règle empirique « +360 cycles » ;
- `tools/lockstep.py` ignore le quartet bas de F (VBA le conserve après
  `POP AF`, deepeel stocke les drapeaux décomposés).

## Mise à jour 0.249.72 : enchaînement des films et vitesse de rejeu

- **File de films** : un film lancé depuis la file (le « film suivant » d'un worker qui a fini) restait bloqué au boot (écran noir/blanc, boucle sur `LY=$91`). Le chargement de l'état de départ se faisait dans le hook de vblank, donc au milieu d'une instruction de lecture. Le worker réserve désormais le film dans le hook, demande une coupure, et recharge la machine dans `run_epoch`, hors de l'exécution (même chemin qu'au démarrage). Le patch `m0boy-tas-compat.patch` remet aussi à zéro, au chargement d'un état, les statiques hors état (attente VBL, verrou du pad, `q153`, `gam_*`, cycles absolus).
- **Vitesse** : tant qu'un film est en cours de rejeu ou en attente dans la file, le fork d'exploration ne fait plus de pas entre deux époques (priorité TAS) ; ses graines sont explorées après tous les films. Un film Zelda de 13 700 images se rejoue en une quinzaine de secondes sur cette machine (observation de l'outil, pas une mesure de matériel).
- `dp_gbl_hook_trace` ne relit plus `getenv` à chaque instruction.

## Mise à jour 0.249.73 : réveil de HALT, débit des sessions, libellé du watch

- **Réveil de HALT (VBA V7)** : quand un HALT démarre alors que le vblank est déjà levé mais retenu par `gbInterruptWait`, le patch compat marche le HALT par pas (grille sonore de 96 cycles, puis ligne LCD suivante) avec l'attente restante, au lieu d'un cas figé. Sur `bobmario511-zeldagb-playaround` (145 368 images), l'écart durable qui commençait à l'image 52691 disparaît ; le premier écart durable restant est à l'image 59601 (observation de l'outil face à VBA V7).
- **Débit** : `dp_section_touch` ne prend plus `sec_mtx` à chaque instruction exécutée (cache de compteurs par thread, vidé en fin d'époque), `steps_total` est mis à jour par lots de 1024, `dp_cov_mark` et `flags_or` n'écrivent plus quand les bits sont déjà posés. Neuf sessions sur huit films Zelda : 396 snapshots en 25 s contre 112 avant.
- **Watch** : le libellé affiche le numéro réel du film (parmi tous ceux en file) et non l'emplacement du worker ; le compteur de vitesse repart à zéro à chaque changement de film.

## Mise à jour 0.249.74 : watch en mosaïque

- `--watch` affiche tous les films rejoués en même temps (une vignette par worker, textures SDL mises à jour uniquement quand l'image change) au lieu d'un seul film sélectionné qui restait figé à sa fin. Chaque worker échantillonne son propre écran (`DpWorker.watch_img`, sous `watch_mtx`), exposé par `dp_play_watch_tiles`. La fenêtre de la carte (`--map`) garde le flux d'un seul film (`dp_play_watch_read`).
- `tools/lockstep.py` : le filtre de fenêtre et le calcul des décalages se font modulo 2³² (le compteur de cycles de deepeel est sur 32 bits) ; avant, une fenêtre au-delà du tick 2³⁰ donnait une trace deepeel vide.

## Mise à jour 0.249.75 : phase de la grille sonore de HALT, watch côte à côte avec VBA

- **Grille sonore de HALT (VBA V7)** : la grille de 96 cycles de VBA court sur un compteur 64 bits ; `gbr.cyc` est sur 32 bits et 2³² mod 96 = 64, donc après chaque repli la grille se décale de 32 cycles (modulo 96). Le patch compat compte les replis vus pendant les HALT (`halt_track`, bits hauts de `gbl_sync_halt_snd`) et corrige la phase (`halt_grid_phase`). Sur `bobmario511-zeldagb-playaround`, l'écart RNG de l'image 59601 (réveil de HALT 32 cycles trop tôt) et ceux qui en découlaient (67887, 82068, ~90k) disparaissent ; le premier écart durable est maintenant à l'image 123131 (`$C13D`). Limite : un état rechargé en cours de film (reprise) perd le compte de replis. Observation de l'outil face à VBA V7, pas une mesure de matériel.
- **Comparaison en direct** : `DEEPEEL_WATCH_REF=<fichier>` ajoute à la mosaïque une vignette « VBA 1.7.2 (reference) » alimentée par un harnais VBA (`VBAH_LIVE=<fichier>`, écran partagé par mmap, `src/explore/ref_feed.h`). Le rejeu du premier film ne dépasse pas la référence de plus d'une image : les deux vignettes montrent la même image. Le harnais VBA reste hors du dépôt.
- `DEEPEEL_TAS_INSTR_IF=1` ajoute IF/IE/LY (ligne `#IF=…`) à la trace d'instructions `DEEPEEL_TAS_INSTR` (diagnostic, incompatible avec `tools/lockstep.py`).

## Mise à jour 0.249.76

Profil VBA (`cmake/m0boy-tas-compat.patch`, hors m0boy : comportements de VBA, pas du matériel) :

- VBA ne verrouille jamais la VRAM ni l'OAM : `gbl_ppu_blocked` renvoie 0 sous le profil VBA. Le film
  bobmario 2021 lit la VRAM en mode 3 (frame 15858 : `ld a,(hl)` en `$8217`) ; avec le verrou, deepeel lisait
  `$FF` et l'ordre des sprites divergeait.
- Lecture de `$FF02`, `$FF03`, `$FF08-$FF0E` : l'octet brut, comme la mémoire de VBA.
- Résultat sur la bobmario 2021 : WRAM identique à VBA sur 145 366 frames sur 145 368 (écarts d'un octet aux
  frames 74207 et 135700), OAM et tilemaps identiques. Il reste des octets de tuiles recopiés depuis des
  registres d'E/S (DIV, IF, son), sans effet sur le jeu.
- Outils : `DEEPEEL_TAS_VRAM=<fichier>` (VRAM bank 0, OAM, LCDC/SCY/SCX par vblank) et `tools/vram_compare.py`
  pour comparer à la référence (`VBAH_VRAM` du harnais VBA). Observations de l'outil, pas des mesures de matériel.

## Mise à jour 0.249.77

Carte PNG (`map.png`) :

- **Chaleur tassée** : la grille de chaleur est au quart de la surface de la grille ROM ; ses tuiles sont
  maintenant collées bord à bord (4 par emplacement d'une tuile ROM, 16 par ligne, 3 px d'écart) au lieu d'être
  centrées une par emplacement. L'image est plus basse.
- **Destination des données copiées** : un octet de ROM qu'une boucle de copie ou un DMA a déplacé est coloré
  selon sa destination (tuiles VRAM `$8000-$97FF`, carte VRAM `$9800-$9FFF`, sprites OAM, WRAM) et la légende
  ajoute une entrée « copie vers … » avec sa part de la ROM. Le texte `.map` porte la ligne
  `# data copied by destination` avec les octets par destination.
- **Limite** : l'audio n'est pas distingué. Seule une copie a une destination ; une donnée lue par le moteur
  son n'en a pas. Typer l'audio demanderait d'observer les écritures vers `$FF10-$FF3F` et de les rattacher à
  la source en ROM. Observations de l'outil, pas des mesures de matériel.

## Mise à jour 0.249.78 — rafales aléatoires par snapshot TAS

Chaque snapshot du tas (captures du film) reçoit obligatoirement `--tas-rng-per` = 10 rafales :
touches totalement aléatoires (tirage uniforme, **START exclu**), au minimum `--tas-rng-frames`
= 2000 frames (~30 s de jeu), prolongées tant que la rafale découvre, terminées après 2000 frames
sans découverte propre (plafond 216000 frames). Le mur de stagnation globale attend que tous les
snapshots aient eu leurs rafales (le budget de temps et SIGINT arrêtent toujours). Le tas étant un
chemin quasi optimal, ces rafales balaient les alentours de chaque checkpoint à peu de frais ; elles
se distinguent du jeu aléatoire classique (départ au power-on ou checkpoint du pool, touches à priors).
Valeurs observées de l'outil, pas des mesures de matériel.

Les couleurs de destination des données copiées sont retirées de la carte PNG (0.249.78) ; le
décompte par destination reste dans le texte `.map`.

## Mise à jour 0.249.79 — timeline de banques bornée

La timeline de banques ne garde plus que la **première occurrence de chaque commutation distincte**
(site, fenêtre, ancienne et nouvelle banque) ; le total des commutations reste compté. Le rapport
affiche `bank switches (N distinct of M)`. Les compteurs d'accès registres et le crédit de banques
comptent toujours chaque commutation. Avant ce changement le tableau atteignait 8 Go sur Zelda
(observation de l'outil, pas une mesure matérielle).

## Mise à jour 0.249.80 — ensembles saturables bornés (audit RAM)

Suite de l'audit RAM : trois ensembles qui restaient à croissance libre sont
bornés, chaque saturation comptée (jamais silencieuse) :

- **ensemble des commutations de banque distinctes** : plafond absolu de 4 M
  d'entrées (32 Mo) ; au-delà, les occurrences continuent de compter dans le
  total (`bank switches`) mais la timeline cesse de croître — compteur
  `bank_seen_full` (rapport texte, JSON) ;
- **ensembles de contexte par section** (éviction « même contexte ») : un
  ensemble plein ne jette plus rien (le chemin est exécuté, comme avant), mais
  le cas est compté — compteur `ctx_set_full` ;
- **journal de découvertes** : plafond de 65 536 entrées ; au-delà, compaction
  par moitié (la courbe garde sa forme, la résolution temporelle est divisée
  par deux à chaque compaction) — compteur `disc_dropped`.

Ce sont des observations de l'outil ; les plafonds ne sont pas atteignables
par les fixtures (aucun test ne les déclenche), les compteurs sont vérifiés à
zéro sur les runs normaux.

## 0.251.38 — SMS/GG : pool de workers et plafond mémoire partagé

SMS/GG acceptent maintenant `--jobs N|auto` jusqu'à 16 instances runtime isolées
(ou moins si la machine expose moins de cœurs). Les budgets globaux de pas,
d'inputs aléatoires, de closure indirecte, de jeu et de chemins sont répartis
entre les workers ; les seeds du premier passage sont distribuées à la demande
et les bursts aléatoires issus des checkpoints TAS sont eux aussi répartis entre
les workers. Le nombre de runs par checkpoint reste celui demandé.
La somme des budgets globaux reste bornée par les valeurs demandées.
`--mem-cap-mb N` surveille le pic RSS du processus et arrête la croissance des
files de tous les workers à cette limite. `--queue-max` fixe leur plafond de
croissance ; `--spool-cap-mb` borne le spool du worker principal. Le moteur
MSX/SG reste limité aux deux instances runtime disponibles.

## Noyau générique (0.302.5)

`deepeel explore rom.gb --core` utilise le noyau `dp_core_explore` : une instance m0boy par worker
(`clone` = autre copie compilée, statiques propres), `--jobs N|auto`, sondes d'événements (`$FF00`
manette), fourche d'IRQ vblank, session attract unique (`--attract-frames`, défaut 6000 ;
`--attract-stall`). Les options du noyau passent avant celles de l'ancien moteur.
Bornes de temps communes à toutes les plateformes : `--min-time`, `--max-time`, `--stall-time`.
Observation de l'outil (pas une mesure matérielle) : Super Mario Land, 50 M de pas, 1 job : natural 8945.
Parité avec l'ancien moteur non encore mesurée ; l'ancien moteur reste le défaut.

## Rejeu TAS sur le noyau (0.303.2)

`deepeel explore rom.gb --core --tas film` rejoue un film (VBM, BK2, log) dans le noyau : le compteur de trames
est porté par l'état sauvegardé (les forks reprennent à leur trame) et le profil de synchro de l'enregistreur
(`dp_tas_tune_machine`, partagé avec l'ancien moteur) est appliqué à l'instance et à ses clones. Le chemin du
film s'arrête à sa fin ; ses forks poursuivent l'exploration. Observation de l'outil sur SML (muggv3, 20 M de
pas) : natural 7086 octets ; fidélité image par image au film non vérifiée côté noyau.

### Parité GB, seconde ROM (0.303.7)

Super Mario Land 2, 40 M de pas, 8 workers (observations de l'outil). Noyau : 13 720 octets d'exécution naturels, 33 181
au total (forcé inclus). Ancien moteur : son `--cov` marque tout flag 4 (spéculatif) et ne distingue pas le naturel
(0 octet naturel), mais 141 459 octets exécutés au total — l'ancien moteur couvre beaucoup plus en spéculatif sur GB
(forks à budget plus large). Le critère « naturel ≥ ancien » tient ; la couverture totale spéculative du noyau GB reste
inférieure. Cause identifiée en partie : l'ancien moteur GB ajoute la **fermeture statique** (tour 1 : +39 007 octets
statiques, 2 535 entrées semées) dont les octets sont du code décodé, pas exécuté ; le noyau n'a pas cette boucle
fermeture→graines. Écart non résorbé : à porter (ou à écarter explicitement) avant de retirer l'ancien moteur GB.

### Rejeu TAS GB sur le noyau : vérification incomplète (0.303.7)

`--core --tas muggv3-supermarioland.vbm` (SML Rev A) applique le profil de synchro VBA et rejoue un chemin TAS
(`tas replay paths 1`, 8 234 octets d'exécution non spéculatifs sur 20–60 M de pas). La preuve que le film atteint le
monde 4 passe par les lectures de données (tuiles bank 1 $47F2–$4FB1), que le `--cov` du noyau n'exporte pas (pas de
drapeau 8) : la fidélité reste **non vérifiée**. À faire : exporter les lectures de données du noyau, ou journaliser
le dernier frame du film atteint.

### `--cov` du noyau : lectures de données (0.303.8)

Le `--cov` du noyau marque désormais le drapeau 8 (octet ROM lu comme donnée par un chemin naturel ; `DP_CORE_DATA`),
sans que cela compte comme couverture de code. Attention aux colonnes : l'ancien `--cov` GB donne l'**adresse CPU**
(`$4000`–`$7FFF`), celui du noyau l'**offset dans la banque** (`$0000`–`$3FFF`).

Mesure SML Rev A + muggv3, `--tas-solo` (le film va au bout dans les deux moteurs), observations de l'outil :
- ancien moteur : 339 octets lus dans les tuiles du monde 4 (bank 1 $47F2–$4FB1) et 22 dans le flux de niveau
  ($5832–$5965) — il atteint le monde 4 ;
- noyau : **aucune** ligne de la bank 1 (ni exécution, ni donnée) ; les banques actives sont 0, 2 et 3 seulement.

Verdict : la fidélité du rejeu GB sur le noyau **échoue** (le film se désynchronise ou la banque est mal vue au
rechargement d'état ; cause non trouvée). Piste : comparer `dp_gbl_bank_of` après `gb_load`/`gb_clone` et le profil
de synchro entre l'ancien moteur et le noyau, frame par frame.
