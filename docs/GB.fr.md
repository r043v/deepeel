# Plateforme Game Boy

## Registre matériel

`src/platform/gb/gb_plat.c` porte la table complète (`memscan regs gb` pour
la lister) : 69 entrées — les fenêtres de mapper (RAMG, ROMB0, ROMB1, RAMB,
MODE), les registres système/audio/vidéo (P1, SB/SC, DIV/TIMA/TMA/TAC, IF,
NR10…NR52 + WAVE, LCDC…WX, DMA), les registres CGB (KEY1, VBK, BOOTROM,
HDMA1-5, RP, BCPS/BCPD, OCPS/OCPD, OPRI, SVBK, FF72-77) et IE. Chaque entrée
porte sa catégorie, son sens lecture/écriture et ses **implications de
mapping** (`bank-lo`, `bank-hi`, `ram-bank`, `ramg`, `mode`, `dma`, `hdma`,
`irq`, `event`, `window`, `modelled`).

Les IRQ (VBlank $40, LCDSTAT $48, Timer $50, Serial $58, Joypad $60) ont
leur vecteur et leur bit IE/IF ; les points d'entrée graine l'reset $100,
les 8 RST et les 5 IRQ (un vecteur en padding n'est pas semé).

## Machine embarquée (0.3.0)

La machine n'est plus approximée : memscan embarque la **runtime GB de
m0boy** (`targets/meta/runtime/gbl_*`, dépendance `M0BOY_DIR`) —
l'interpréteur dont le run_hash/image_hash/audio_hash est tenu par la suite
m0boy. Le registre matériel ci-dessus reste la grille de lecture
(`memscan regs gb`) ; l'exécution, le PPU (rendu par ligne, 160×144
récupéré pour le GUI et les snapshots), l'APU, le timer et le joypad
matériels sont ceux de m0boy.

- **Variantes** : `GBL_MBC`/`GBL_ERAM_SIZE` sont compile-time — le build
  compile 17 variantes MBC×RAM (0:0 ; 1:0/0x800/0x2000 ; 2:0x200 ;
  3 et 5 : 0..0x20000) et `ms_mach_pick_variant` choisit d'après l'en-tête.
- **Threads = instances** : la runtime vit dans des statics ; chaque worker
  reçoit une copie des symboles préfixée par `objcopy --redefine-syms`
  (10 instances, `MS_MACHINES`). Les hooks (`trace`, lectures/écritures
  CPU interposées par `-Dgbl_rd/-Dgbl_wr`, vblank, lignes PPU) sont
  re-câblés par instance vers le moteur.
- **États** : une image d'état (`gbl_state_io`, ~17 Kio + RAM cartouche) se
  sauve/charge à frontière d'instruction ; fourche = sauver, retourner le
  drapeau Z/C (ou vider IF) dans la copie, restituer. Les banques de boot :
  MBC1-3 démarrent en banque 1, **MBC5 démarre en banque 0** (légal).
- **Garde-fous** : HALT que rien ne réveille (8 frames sans instruction) et
  glissade dans du padding (4096 pas) coupent la machine par `longjmp`.

## Modélisation (ce que memscan ajoute par-dessus)

- **Interposition** : chaque lecture CPU (IO y compris) et chaque écriture
  (mapper, VRAM, OAM, IO) passe par un hook memscan avant/autour de
  l'effet machine — c'est de là viennent la carte data, les stats de
  registres, les événements de banque et les canaux de copie.
- **OAM DMA** : l'écriture à $FF46 (canal 1) produit l'enregistrement
  `rom -> oam` 160 octets comme le ferait la console.
- **Interrupteurs** : à chaque conditionnel l'autre issue est engendrée en
  retournant le drapeau testé (Z via `gbr.zr`, C via `gbr.cf`) ; aux points
  IME && IE & IF, un fork « l'IRQ ne part pas » complète la dispatch
  naturelle que la machine fait elle-même.
- **Évictions** : dédup exact d'état (hash d'image hors cycle), éviction
  « même contexte » par section, budgets (pas/chemin, pas global, sites de
  fork, arrière-branches), tout compté dans le rapport.

## Limites (documentées, pas cachées)

- **Pas de PPU/APU/timer** : les registres marqués `event` (LY, STAT, TIMA,
  DIV, PCM…) restent statiques. Le code qui attend LY ne sort pas de sa
  boucle d'attente autrement que par les gardes ; les handlers d'IRQ sont
  eux atteints par les graines de vecteurs et les forks d'IRQ. C'est le
  compromis assumé : la carte cherchée est celle de la ROM, pas le timing.
- **RAM initiale à zéro**, registres au reset DMG ($01B0…) — l'exploration
  est déterministe ; deux exécutions donnent la même carte.
- **SVBK** (banques WRAM CGB) stocké mais une seule banque simulée ; VBK est
  lui réellement commuté (2 banques VRAM).
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
- **persistance** : `--state-dir D` écrit le pool (`pool.bin`) et les
  snapshots toutes les `--save-epochs` époques ; `--resume` recharge et
  continue. Un run coupé/repris est identique au run continu au sein de la
  session rejouée (testé).
- **arrêt** : `--frames` (budget), `--stagnation F` frames sans découverte,
  `--until-static` (carte statique entièrement expliquée), SIGINT gracieux ;
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
`MS_SF_PAD`) est un retour sauvage (RETI sur pile vide...) : les octets de
padding ne sont **jamais** marqués code, et la glissade est coupée après
quelques pas de suite (sessions) ou bornée (fourcheur, qui doit atteindre le
code derrière la plombée). Un pas isolé sur un octet plombé (le NOP du boot,
un vecteur) reste légal.

### Mode humain

`--human [slot]` (ou `H` dans la GUI) prend une lane : le pad/clavier la
joue, l'outil surveille le crédit et **sauve chaque avancée** comme snapshot
PNG (écran 160×144 + machine complète en chunk `tEXt` `memscan_snapshot`,
style cartouche PICO-8 — un seul fichier, visible et rechargeable avec
`--from-png f` ou `ms_play_load_png`). Les découvertes humaines alimentent
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
conditionnel. Ce qui est atteint ainsi est marqué `MS_XF_STATIC` — jamais
confondu avec `natural`/`forced`. Le rapport dit « static closure : … octets
atteints par flux direct seul ».

- Arrêts : opcode indéfini, padding, table de pointeurs, données prouvées
  verbatim. Comptés (`stopped by data/padding`, `by undefined opcodes`).
- Une arête de banque 0 vers `$4000+` n'est résolue que par les banques
  observées dans la section (ou la banque de boot) ; sinon elle est comptée
  `unresolved`, jamais devinée. Les `JP HL` observés sont listés avec leurs cibles
  (jusqu'à 8 par site, débordement compté).
- Boucle `--closure-rounds N` (défaut 4) : fermeture → graines (les entrées non
  exécutées) → recherche totale → fermeture… jusqu'à ce qu'un tour ne sème
  rien. Plafond de graines `--seed-cap`, rejets comptés (`dropped`).
- Export `.cov` : octets statiques = début d'instruction seul, drapeau 4
  (spéculatif), +2 si entrée, compte 0. Jamais le drapeau 1.
- Élagage : un site conditionnel dont les deux côtés ont déjà été exécutés ne
  reçoit plus que `--covered-keep` forks (défaut 2) ; `--no-covered-prune`
  rétablit l'ancien comportement. Compteur `forks pruned`. Les forks de touches
  sont limités à un tour par site une fois le gestionnaire joypad (`$60`) vu.
- Le mur global compte les pas du seul explorateur à forks : les pas des
  sessions jouées ne l'épuisent plus. Chaque tour de fermeture a son mur.
- TAS : tous les `--tas-ckpt` images (défaut 240), l'état de la machine est
  déposé dans la boîte de graines du fork — des états de jeu réels (RAM
  peuplée) d'où forker. Compteur dans le rapport de session.
- Un opcode indéfini n'est plus consigné qu'une fois par site.

Observations de l'outil (machine concrète, RAM à zéro), sans jeu, avant → après :
Super Mario Land 14 062 → 20 551 octets de code, Mystic Quest 19 341 → 28 575.
Ce sont des comptes de l'outil, statiques inclus, pas des mesures de matériel.
Aucun fichier TAS n'était disponible : le chemin TAS n'est couvert que par
les tests synthétiques.

## Données prouvées et pont .cov

Une copie ROM→VRAM dont les octets lus hachent exactement comme les octets
écrits (mêmes longueurs) est **verbatim** (`MS_DF_VERBATIM`, canaux OAM
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
  entre deux codes), `inferred-data` (≤ 256 octets entre deux data),
  `inferred-padding` (≥ 4 octets identiques 00/FF). Jamais à travers une banque.
- « Connu » inclut donc les octets déduits ; la part déduite est rapportée à part.
- `memscan explore ROM --map x.map` écrit le texte (`banque début fin classe`,
  fin incluse, adresses vues du CPU) ; `--map x.map.png` écrit une image
  (un carré par 64 octets, 16×16 par banque, rouge = inconnu, bleus = code,
  verts = data, ocre = table, gris = padding) qui **embarque le `.map`
  complet** (chunk tEXt `memscan_map`, comme une cartouche PICO-8).
  `memscan mapx x.map.png x.map` extrait le `.map` à l'identique.
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

`memscan explore ROM --play --tas film.vbm --watch [FPS]` ouvre une petite
fenêtre SDL2 (sans carte ni stats) qui montre l'écran du jeu rejoué par le
TAS, échantillonné à 1..30 images/s en temps humain (défaut 30). Une image
sur 1/FPS s est rendue par la machine du film, les autres ne le sont pas :
le TAS n'est pas ralenti. La fenêtre affiche le film, sa frame courante, et
la vitesse réelle du rejeu en frames/s (observation de l'outil). `Espace`,
`Tab` ou `→` passent au film suivant ; `Échap` ferme la fenêtre seule, le
run continue. Sans SDL2, l'option affiche un message et est ignorée.
