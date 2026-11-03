# Plateforme Game Boy

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
  timer réels), mais les fourches ne portent pas sur les valeurs de LY,
  STAT, DIV ou TIMA : une boucle d'attente sort au rythme de la machine
  concrète, pas sur chaque valeur possible. Les handlers d'IRQ sont atteints
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
- `tools/tasvideos_fetch.py ROM.gb DIR [--max N] [--list] [--game "nom"]`
  télécharge les TAS TASVideos de la ROM (nom de jeu strict) ; `--tas`
  accepte jusqu'à 8 fichiers, tous joués en entier.
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
- `--tas-dir D` (répétable) : découvre les films du répertoire (8 max, triés
  par ressemblance de titre quand il y en a plus ; `.vmv`, `.vbm`, `.bk2`,
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
le TAS n'est pas ralenti. La fenêtre affiche le film, sa frame courante, et
la vitesse réelle du rejeu en frames/s (observation de l'outil). `Espace`,
`Tab` ou `→` passent au film suivant ; `Échap` ferme la fenêtre seule, le
run continue. Sans SDL2, l'option affiche un message et est ignorée.

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
