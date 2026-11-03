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

