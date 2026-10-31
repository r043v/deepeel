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

## Modélisation

- **Mappers** : MBC0 (aucun registre), MBC1 (mode 0/1, banque 0 → 1),
  MBC2 (RAMG sur bit 8 d'adresse, banque 4 bits), MBC3 (banque 7 bits,
  RTC journalisée mais non simulée), MBC5 (9 bits, banque 0 légale, RAM 4
  bits). Mapper inconnu : heuristique générique. Chaque écriture qui change
  une fenêtre produit un événement de banque (PC, fenêtre, ancien → nouveau).
- **Canaux de copie** : OAM DMA ($FF46, 160 octets) et HDMA CGB ($FF51-55,
  mode général et H-blank, modèle instantané) produisent des enregistrements
  `rom → oam/vram` ; la source marquée lue + copiée dans la carte.
- **Boucles de copie CPU** : un corps de boucle (arrière-branche) qui lit une
  plage ROM et écrit une plage VRAM à pointeurs monotones produit un
  enregistrement coalescé (canal `cpu_loop`), avec PC du site de boucle.
- **Interrupteurs** : à chaque conditionnel, l'autre issue est engendrée en
  retournant le drapeau testé dans une copie d'état (bit 1 de l'index de
  condition choisit Z ou C). Aux points où IME est actif et IE & IF a un bit
  en attente, un fork d'IRQ empile le retour et saute au vecteur du bit de
  plus haute priorité.
- **Pile** : concrète — CALL/RET/PUSH/POP fonctionnent ; un RET sur une pile
  invalide termine le chemin (le retour vers des données est exploré comme
  n'importe quel chemin forcé, et les verdicts le classent).

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

## Horloge de session (0.2.0)

`src/platform/gb/gb_tick.c` ajoute une horloge approximative pour les
sessions jouées (moteur de fourches inchangé, il ne tick pas) :

- table de cycles SM83 (256 + CB) avec les corrections des conditionnels
  pris (JR/JP/CALL/RET +4/+12), 70224 T-cycles par frame ;
- DIV /64 T, TIMA aux périodes TAC (1024/16/64/256, overflow → IF bit 2 +
  TMA), LY 0..153 à ~456 T/ligne, IF bit 0 à LY=144, bits de mode STAT
  approximatifs (OAM 0-80, VRAM 80-252, HBlank 252-456), comparaison LYC ;
- **HALT** : avance de temps jusqu'au prochain événement qui lève un bit IF
  autorisé par IE (vblank même depuis le vblank, STAT mode-2/LYC si
  autorisés, timer, front de frame pour une nouvelle touche). Un HALT que
  rien ne peut réveiller est une impasse, signalée comme telle ;
- **IRQ naturelles** : au pas d'instruction (IME && IE && IF), la priorité
  la plus haute dépilant PC est servie ; les handlers sont marqués
  `MS_XF_IRQ` jusqu'à leur RETI ;
- **P1 réel** : matrice bits 4/5, 0 = touche pressée, IF bit 4 sur nouvelle
  pression. `m->buttons` porte l'état des touches.

Approximations assumées : modes STAT grossiers, pas d'APU, pas de serial,
pas de double vitesse CGB. C'est une horloge pour sortir des boucles
d'attente, pas un modèle de matériel.

## Sessions jouées (`explore --play`, 0.2.0)

Le moteur de fourches explore tout ; les sessions jouent le jeu. N workers
POSIX (défaut 10, `--sessions`), chacun sa machine (la SRAM cartouche vit
dans la machine, snapshotée avec elle) :

- **inputs** : le générateur de m0boy (m0x_move) — direction pondérée tenue
  8-63 frames, A 45 %, B 25 %, START 2 %, taps 4-11 ; une session **attract
  sans touches** reste toujours vivante ; `--input f.input` rejoue un script
  m0boy (dialecte held-named) ; `--fixed mask` (tests) force un masque.
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
  `--until-static` (carte statique entièrement expliquée), SIGINT gracieux.

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

### Déterminisme

Perdu entre threads (l'entrelacement dépend de l'ordonnanceur) ; par contre
chaque session reste rejouable individuellement (graine ou script), et un
run à masques fixes + `--fork-paths 0` est reproductible.

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
