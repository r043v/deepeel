# NES (deepeel)

Le support NES mutualise le runtime NES de m0boy (`targets/nes/runtime` :
cœur 6502, PPU événementiel, APU, cartouches NROM et UxROM). Deepeel ne
porte plus sa propre machine NES : le même cœur joue les jeux recompilés
dans m0boy et explore les ROMs ici, comme pour la Game Boy (`gbl_*`) et la
Mega Drive (`md_full`).

## La machine

- **Cœur** : l'interpréteur 6502 de m0boy (6502.c), cycle-juste par
  instruction, NMI sur front, IRQ par niveau, OAM DMA ($4014, 513 cycles de
  stall). Les opcodes illégaux y tournent en NOP de longueur honnête.
- **Crochets d'observation** (`nes_trace.h`, `-DNES_TRACE`) : `step` (avant
  chaque instruction), `rd`/`wr` (accès données, fetch et pile exclus),
  `interrupt` (début d'une séquence NMI/IRQ/BRK), et `stop` — l'observateur
  fige le cœur à la frontière d'instruction suivante, cycles réellement
  consommés uniquement. Sans `-DNES_TRACE`, tout plie à rien (comme
  `MD_TRACE` du 68k). Avant chaque `step`, le cœur synchronise pc/s/cycles
  depuis ses registres locaux : un snapshot pris dans un crochet est exact.
- **Frame résumable** : `nes_frame()` court les 262 lignes,
  `nes_frame_rest()` reprend la frame courante (`machine.line` = prochaine
  ligne non exécutée). L'explorateur peut donc figer la machine au milieu
  d'une frame et reprendre.
- **Passerelle** (`src/mach/nesmach.c`) : iNES (mapper 0 et 2 seulement —
  les autres cartouches sont refusées avec un message), hooks, budgets,
  snapshots complets (`nes_state_io`, ~12,6 Kio, légaux au milieu d'une
  frame), hash d'état exact (horloges volatiles exclues) pour la dédup.
- **Espace de noms** : les globales du runtime (`machine`, `nes_*`,
  `apu_*`, `ppu_*`) sont préfixées `dp_nesrt_` par objcopy (technique
  MSX/Sega) — le runtime Game Gear exporte aussi un `machine`.
- Des gardes CMake épinglent les points de crochets du runtime : si m0boy
  change le corps de la boucle ou de la frame, la construction échoue avec
  un message au lieu de dériver silencieusement.

## L'explorateur

Même contrat que GB/Z80 : les deux côtés de chaque conditionnel (fourche
par force des drapeaux dans un snapshot), fourche sans-NMI quand le front
est armé, éviction par dédup d'état exact, budget de pas par chemin, cap
d'arrière-branche par site, cap de fourches par site, budget global —
chaque saturation tombe dans un compteur. Les cellules de la cartouche
sont les index compacts `banque * $4000 + (addr & $3FFF)` ; la fenêtre
fixe $C000-$FFFF est la dernière banque physique. L'exécution hors ROM
(trampolines RAM) est comptée `exec_outside`, le chemin continue.

## TAS

`--tas FICHIER` (répétable, 8 max) rejoue un film tool-assisté depuis la
mise sous tension avant la drain de fourches ; l'état final de chaque film
devient une graine de fourche. Formats :

- `.fm2` (FCEUX) : colonnes `R L D U S T B A` (S = Start, T = Select) ;
  l'ordre a été validé sur le TAS DuckTales (aglar/meshuggah 2441) ;
- `.bk2` (BizHawk, zip) : `Input Log.txt` piloté par son `LogKey`.

Le rejeu est **best-effort** : un film enregistré sur une autre version de
la ROM (ou une autre région — les TAS NES DuckTales sont NTSC/USA, la ROM
du dépôt d'essai est Europe/PAL) désynchronise à la première divergence ;
l'exploration par fourches prend le relais. `tools/tasvideos_fetch.py
ROM.nes DIR --system NES [--game "DuckTales"]` télécharge les films.

## Sessions de jeu

`--play N --sessions S --seed X` : S sessions de N frames à entrées
aléatoires (xorshift32), reprise de 8 frames en arrière sur blocage
(un blocage déterministe revient toujours au même état). Chaque session
termine par un état graine pour la drain de fourches.

## Usage

```
deepeel nes explore ROM.nes [--tas FILE]... [--play N] [--sessions N]
    [--seed N] [--mapper auto|nrom|uxrom] [--max-total N] [--max-steps N]
    [--max-paths N] [--queue N] [--loop-cap N] [--fork-cap N]
    [--no-irq-fork] [--text FILE] [--map FILE]
```

## DuckTales (observations de l'outil, pas des mesures de matériel)

Sur `DuckTales (Europe).nes` (UNROM, PRG 128 Kio, CHR RAM) :

- film TAS seul (2441, any %, 25 526 frames) : **21,5 %** du PRG touché ;
- TAS + sessions aléatoires + fourches (1238 chemins) : **23,5 %**.

Le plafond vient du désync du TAS à mi-jeu (ROM Europe PAL contre films
USA NTSC) : le premier niveau est bien couvert, les banques des niveaux
suivants ne sont atteintes que par fourches. Avec une ROM USA NTSC, le
rejeu complet du film devrait couvrir l'essentiel du jeu — à confirmer
quand une ROM NTSC est disponible hors dépôt.

Le cœur m0boy traite les opcodes illégaux en NOP : DuckTales passe
(trampoline de banque UNROM `STA $FFE5,X` avec table ROM), mais une
cartouche s'appuyant réellement sur la sémantique d'un opcode illégal
dériverait — à garder en tête.

## 0.249.81 — tables de pointeurs vers une autre banque (S1)

Un mot `$8000-$BFFF` d'une table lue en données peut viser une autre banque
commutable que celle de la table (le jeu sélectionne la banque cible avant le
saut indirect) : la résolution « banque propre » échoue alors alors même que
la cible est exécutée. Le scan accepte désormais cette résolution croisée sur
les cartes à fenêtre `$8000-$BFFF` commutable (UxROM et cartes « loose »,
MMC1/MMC3…) : la preuve reste le début d'instruction exécutée — dans une
banque commutable quelconque à l'offset visé — et le relâchement est compté
(`cross-bank words` dans le rapport). La classe reste une inférence (`table`).
Fixture `fix_nes_xbanktable.nes` (UxROM : table en banque 0, cibles exécutées
en banque 1), test `nes_table_xbank`.

Mesure sur DuckTales (Europe) (UNROM, 20 M pas, observations de l'outil) :
tables de pointeurs 18 → 26, octets classés `table` 90 → 129 (6 mots
cross-bank) ; les 39 octets gagnés sont reclassés depuis `data-read` et
`inf-data`, aucune autre classe ne régresse (`known_pct` identique).

## 0.250.4 — fidélité NES : banc de comparaison Nestopia

`tools/nes_compare_reference.sh ROM film.bk2|fm2 trames` rejoue le même film
sur le cœur libretro Nestopia (`DP_NESTOPIA_SO`, construit hors dépôt) et sur
le runtime m0boy, diff des 2 Kio de RAM par trame (`DP_NES_DUMP=préfixe`
décharge les RAM pour l'analyse d'alignement par corrélation croisée, comme
le banc GG).

Premiers résultats (DuckTales Europe, TAS jvuln33v2, 600 trames) : les 4
premières trames sont identiques ; divergence à la trame 6 (`$0080`), pas
un décalage de trame (corrélation plate) — le compteur de trames du jeu
avance côté m0boy (4 contre 1). À démêler : timing exact de \$2002 (la
boucle d'attente vblank du boot), frontière de trame interne de Nestopia.
Le banc a déjà trouvé et corrigé un vrai bug du runtime :
`dp_nesmach_frame` n'initialisait jamais `steps`/`budget` — le hook de pas
voyait `0 >= 0` et arrêtait la machine à sa première instruction ; une
instance pilotée par `dp_nesmach_frame` n'exécutait rien (reparé :
`dp_nesmach_cut` public + budget initialisé). Le warm-up PPU (~29658
cycles, écritures \$2000/\$2001/\$2005/\$2006 ignorées) est côté m0boy ;
DuckTales n'écrit pas dans la fenêtre (ramdiff inchangé), conservé pour la
conformité NESdev. Observations de l'outil, pas des mesures de matériel.

## 0.250.4 — fidélité NES : banc de comparaison Nestopia

`tools/nes_compare_reference.sh ROM film.bk2|fm2 trames` rejoue le même film
sur le cœur libretro Nestopia (`DP_NESTOPIA_SO`, cloné et construit hors
dépôt) et sur le runtime m0boy, diff des 2 Kio de RAM par trame
(`DP_NES_DUMP=préfixe` décharge les RAM pour la corrélation croisée, comme
le banc GG).

Premiers résultats (DuckTales Europe, TAS jvuln33v2, 600 trames) : les 4
premières trames identiques ; divergence à la trame 6 (`$0080`), pas un
décalage de trame (corrélation plate) — le compteur de trames du jeu avance
côté m0boy (4 contre 1). À démêler : timing exact de \$2002 (la boucle
d'attente vblank du boot), frontière de trame interne de Nestopia.
Avancées de l'enquête : le boot identifié (la boucle classique des 4
vblanks en \$FEF3-\$FEF9, les deux machines l'exécutent — trames 1-5
identiques) ; l'échantillon au début du NMI (ligne 241) ne change pas la
forme (524 → 490 octets, corrélation toujours plate) ; la machine d'état
\$0080 avance d'un pas par trame côté m0boy (écritures tracées avec leurs
pc : le NMI de la trame 6 et la réponse de la boucle principale tombent
dans la même trame) ; le buffer OAM \$0200+ cesse d'être mis à jour côté
m0boy après la trame 5 quand la référence continue. L'instrument est désormais **value-exact** (0.250.9) : le hook `rd` du
runtime m0boy porte la valeur **rendue** (le macro `rd` du 6502 calcule
l'octet puis appelle le hook — post-lecture, `DpNesHooks.on_read` porte la
valeur), et le handler NMI de DuckTales est décodé : `$92` est le compteur
de trames (`INC \$92` à \$C0AC), `\$90` reçoit `\$FF` (\$C0B0), la lecture
`\$2002` à \$C02C n'utilise pas sa valeur (effacement du drapeau/du loffset).
Le 0x90 vu à la lecture = vblank + open-bus bas (le `\$90` écrit dans
\$2000 juste avant) — conforme. Comparaison faite côté Nestopia par ses sources : les écritures PPU
rafraîchissent le latch (`io.latch = data`) et la lecture renvoie
`latch & 0x1F` — après le `\$90` écrit dans `\$2000`, Nestopia lit aussi
`0x90` : l'open-bus est **écarté**. Le chronométrage du compteur `\$92`
(dump par trame, 60 trames) donne l'arme : la référence incrémente `\$92`
**chaque trame** (1,2,3,…24), m0boy **une trame sur deux** — le handler
NMI de m0boy **sort tôt en alternance** (les trames sans `INC \$92` n'ont
pas le `STX \$90` final non plus : `\$90=255` n'apparaît qu'aux trames
impaires). La suite a **résolu l'alternance et caractérisé l'écart** :

- l'alternance 1,1,2,2 du compteur était un **artefact du harnais** : deux
  hooks coupaient la course (NMI ligne 241 et fin de trame ligne 262) et
  les itérations alternaient entre les deux points de coupe ; le comptage
  direct des `INC $92` montre que le handler s'exécute **chaque trame** des
  deux côtés. Corrigé : une seule coupe par itération, au NMI ;
- au nouveau régime les compteurs avancent chaque trame avec un **décalage
  constant de +5** (le boot de Nestopia dépense ~5 trames de plus avant la
  première NMI du jeu — son warm-up PPU ; `DP_NES_SHIFT=5` aligne \$92) ;
- mais au niveau \$92 la RAM diffère encore largement (~520 octets) avec
  des **valeurs différentes**, pas décalées : un écart comportemental semé
  à l'init (une lecture sensible au timing qui a nourri RNG ou un choix de
  mode) — la chasse continue avec l'instrument value-exact (côté m0boy les
  lectures \$2002/\$4016/\$4015 de la trame 6 sont traçables ; côté
  Nestopia par patch local de son `NstPpu.cpp`).

Le banc a déjà trouvé un vrai bug du runtime : `dp_nesmach_frame`
n'initialisait jamais `steps`/`budget` — le hook de pas voyait `0 >= 0` et
arrêtait la machine à sa première instruction ; une instance pilotée par
`dp_nesmach_frame` n'exécutait rien. Corrigé (`dp_nesmach_cut` public +
budget initialisé). Le warm-up PPU (~29658 cycles, NESdev) a été essayé et
**reverté** : ramdiff Nestopia inchangé sur DuckTales (le jeu n'écrit pas
dans la fenêtre) et les fixtures synthétiques de test écrivent pendant la
fenêtre — résultat négatif au CATALOGUE. Observations de l'outil, pas des
mesures de matériel.

## 0.249.83 — C2 : projection de contexte sur la liveness (mesure et retour arrière)

La projection de contexte de `--ctx-cap --ctx-live` est étendue : CPU, pc,
registres mapper et bord NMI, **pages de RAM et de PRG-RAM ($6000-$7FFF)
lues** par la fenêtre de code, et **état PPU + OAM, manette ou APU** quand la
fenêtre a été vue les lire (les classes de liveness C1). Les sondes de
manette (D10) ne sont jamais évincées par le contexte — ce sont des sondes
délibérées, comme les graines prioritaires GB.

**Résultat négatif mesuré et conservé** : rendre cette éviction *par défaut*
(à la GB) fait perdre 8 points de couverture sur DuckTales (Europe), 20 M pas
: 60,6 % → 52,4 % de octets connus, 623 états fermés, 1527 → 962 chemins. La
liveness par fenêtre de 16 Kio est plus grossière que les sections GB et
l'éviction à l'empilage tue des états qui divergent dans un état que la
fenêtre n'a pas encore lu. L'expérience est réverted ; la C2 par défaut sur
NES attendra des sections vivantes par course (comme GB), la projection reste
disponible en opt-in. Observations de l'outil, pas des mesures de matériel.

## 0.249.90 — frontière partagée (§6) : `--defer-tiers` (opt-in)

L'admission des continuations coupées passe par la politique partagée
`src/explore/frontier.c` (`dp_frontier_room`, raisons `DP_R_*`), comme sega
(0.249.89) : une coupe de budget de chemin (`step`) ou de rebouclage
(`loop`) garde son état et reprend avec un budget ×4, jusqu'à
`--defer-tiers N` (0 : coupes finales, défaut — mesuré **neutre** sur
DuckTales à 3 M pas : 16,96 % connus avec et sans, 2 reprises seulement) ;
la raison bruyante `fork-cap` est bornée à un quart de la zone d'attente,
les fourches héritent du palier de leur chemin parent, et les restes vont au
lot disque. Rapport : `frontier: deferred steps/loop; resumed; final;
dropped`. Test `nes_defer_tiers`.
