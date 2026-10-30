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
