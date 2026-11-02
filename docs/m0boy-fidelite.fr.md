# Fidélité DMG de m0boy — rapport de rétroportage

*Établi depuis deepeel (rejeu du TAS muggv3 de Super Mario Land World,
m0boy en interpréteur C). Patch complet : `docs/patches/m0boy-fidelite-dmg.patch`
(à appliquer dans `m0boy/targets/meta/runtime`). Observations d'émulateurs,
pas de mesures sur matériel.*

## Méthode

Trois traces par instruction (cycle relatif, PC, AF, BC, DE, HL, SP), alignées
sur la première extinction du LCD par le jeu, DIV recopié, mêmes changements
de manette au même cycle :

- **SameBoy** (copie bsnes `m0boy/output/deps/bsnes/bsnes/gb`, harnais
  instrumenté hors dépôt) ;
- **Gambatte** (`/home/m/dev/gambatte`, crochet `gambatte_trace_hook` ajouté
  dans `cpu.cpp`, accesseurs `Memory::peekIo/setDivCounter/divCounter`) ;
- **m0boy** via deepeel.

SameBoy et Gambatte sont **identiques** sur les 24,7 premiers millions
d'instructions (~9 750 frames), au cycle près : c'est la vérité retenue.
Chaque correctif ci-dessous fait avancer la première divergence de m0boy.

| étape | première divergence (instruction) |
|---|---|
| départ | 116 955 (~0,2 s de jeu) |
| après tous les correctifs | ~6 830 000 (~1 900 frames, ~32 s) |

## Correctifs (tous dans le chemin C ; l'assembleur ARM n'est pas touché)

1. **Instant des accès mémoire** (`gbl_cpu.c`, table `acc_last`) — l'interpréteur
   ajoutait les cycles de l'instruction *avant* de l'exécuter : tout accès
   avait lieu à l'horodatage de fin. Le bus voit un accès unique au début du
   dernier M-cycle (`LDH A,(n)` lit à +8 sur 12). Pour les opcodes à accès
   unique final (LD r,(HL), LD (HL),r/n, LDH, LD (C), LD (nn), ALU A,(HL),
   LD (BC)/(DE), LDI/LDD, INC/DEC (HL) côté écriture) : `cyc += total - 4`,
   exécution, `+4`. *C'est la cause des recalages artificiels : avec elle, les
   constantes d'origine MODE3_START=80 / MODE0_START=252 sont justes.*
   À reporter aussi dans le code **généré** (recompilateur) et l'interpréteur
   assembleur (`GBL_INTERP_ASM`), qui ont la même convention.
2. **STAT LYC à l'allumage** (`gbl_ppu_write`, LCDC) — LY=0 à l'allumage : une
   coïncidence LYC=0 avec la source LYC armée lève la ligne STAT
   immédiatement (avant : au premier front du mode 3, 144 cycles plus tard).
3. **Phase de la trame à l'allumage** — `line_start = cyc - 2` : le premier
   VBlank après allumage est demandé 65 662 cycles après le cycle de bus de
   l'écriture de LCDC.
4. **Sortie de HALT** (`gbl_halt_wait`) — modèle DMG : HALT finit en h (sur la
   grille de M-cycles, absolue depuis la mise sous tension) ; échantillons à
   h, puis h+6, h+10… ; reprise 2 cycles après l'échantillon qui voit la
   demande : déjà là à h → h+4 ; demande en t > h → max(h+8, (t+5)&~3).
   Avant : reprise à l'instant exact de l'événement (hors grille).
5. **Reprise hors grille** (`gbl_run`) — un hôte qui coupe la machine depuis un
   crochet pendant une attente (deepeel, toutes les 480 frames) reprenait
   hors grille ; `gbl_run` réaligne `cyc` au M-cycle suivant.
6. **Interruption timer** (`timer_sync`, `timer_next_event`) — le débordement de
   TIMA demande son interruption un M-cycle plus tard (champs
   `tima_irq_at/tima_irq_pending`, fin de `gbl_timer_state_t`).
7. **Ligne 153** (`gbl_ppu_advance`, `ppu_next_event`, `ly_visible`) — LY lit 0
   et la coïncidence LYC=0 se déclenche 8 cycles après le début de la ligne
   153 (une ligne avant la ligne 0) ; le planificateur prévoit cet événement.
8. **Durée variable du mode 3** (`mode3_end`, champ `m0_start`) — 172 points +
   (SCX & 6) + 2 en ligne 0 + 6 si la fenêtre démarre sur la ligne + par objet
   (≤ 10, ordre OAM puis X croissant) : 11 si X=0, rien si X ≥ 168, sinon 6 +
   max(0, pixels de la tuile à droite du pixel de gauche − 2) une fois par
   tuile (Pan Docs « Mode 3 length »).
9. **Blocage d'accès par mode** (`GBL_ACCESS_BLOCK`, défaut 0) — VRAM : lecture
   $FF / écriture ignorée en mode 3 ; OAM : idem en modes 2-3. Coûte la page
   VRAM directe ; activé par deepeel.

## Constaté, pas encore corrigé (prochaines étapes)

- **Lecture de STAT/LY au bord de ligne** (règles de Gambatte `getStat`,
  `getLyReg`, `getLycCmpLy`, ligne m0boy = changement de LY Gambatte) : LY
  lit LY+1 à 6 cycles de la fin de ligne (valeur transitoire `LY & (LY+1)` à
  6) ; la coïncidence compare LY tant qu'il reste > 4 cycles, rien entre 4 et
  2, LY+1 ensuite ; STAT annonce le mode 2 trois cycles avant une ligne
  visible et le mode 1 de 2 cycles avant la ligne 144 à 4 cycles avant la fin
  de trame. Un premier portage concorde sur la divergence n°6 830 000 mais en
  crée une à 290 000 (l'exception de la ligne 0 et l'écart mode 3 « 77 contre
  80 » restent à démêler) : retiré du patch, à reprendre avec les deux
  références.
- **Phase LCD et DIV à la mise sous tension** : Gambatte (état post-boot fidèle)
  démarre en ligne 153 à 396 cycles, DIV = 0x1ABCC ; au premier extinction
  du LCD par SML, son DIV vaut $B3D4 contre $B394 pour m0boy (64 cycles).
- **Durées variables restantes** : ~1 % des lignes gardent un écart de 2-6
  cycles de fin de mode 3 sans objet visible (mesure SameBoy bruitée par son
  PPU en paquets : à confirmer au niveau trace CPU avant d'en tirer une règle).

## Vérification

`ctest` deepeel 34/34 avec ce runtime (`GBL_ACCESS_BLOCK=1`). Les firmwares
(assembleur ARM, `GBL_TIMING_ASM`/`GBL_INTERP_ASM`) gardent l'ancien
comportement : rien n'y a été rétroporté.
