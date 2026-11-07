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
