# SoR 1845S : sync Gens-rr, découvertes (2026-11-22)

Film `SoR_2p.gmv`, référence Gens-rr, objectif : sync complet jusqu'à la trame 25650.
**État : non atteint.** Premier écart RAM à la V-int 17600 (5 octets `$FFFE90…`), puis 17625
(15 octets `$FFB8xx`). La suite jusqu'à 25650 n'a pas été vérifiée. Les nombres ci-dessous sont des
observations de l'outil, pas des mesures de matériel.

## Profil Gens

`DP_MD_GENS=1 DP_TAS_PAD_SHIFT=0 DP_MD_GENS_TRIP=18416 DP_MD_GENS_Z80TIME=1 DP_MD_GENS_Z80TAB=0
DP_MD_GENS_Z80PHASE=128 DP_MD_GENS_Z80IRQ=1 DP_MD_GENS_Z80LATCH=1 DP_MD_GENS_Z80LINE=1
DP_MD_STAR_TIME=1 DP_MD_STAR_DIV=1 DP_MD_STAR_ADDQ=1`. Le bloc `DP_MD_GENS` de `src/mach/mdmach.c`
active aussi `DP_MD_GENS_Z80START` et, désormais, `DP_MD_GENS_Z80LDIR`.

## Modèle Z80 de Gens (observé)

- `z80_Exec(n)` s'arrête quand cum ≥ n, avec `edi = n − cum − 1` ; chaque instruction est payée à sa
  sortie ; les interruptions ne sont testées qu'à l'entrée d'Exec.
- LDIR : `sub edi,21; jns Loop`, 21 cycles par itération et 16 pour la dernière.
- Préfixe DD/FD : 4 cycles plus le NEXT de l'instruction (LD IX,nn = 14, LD r,(IY+d) = 19,
  SBC HL,rr = 15, PUSH 11, POP 10).
- Cibles de chunk par ligne : 228·(L+1)−60.

## Règle LDIR (nouvelle, `DP_MD_GENS_Z80LDIR`)

Un LDIR (ED B0) qui traverse au moins 2 ré-entrées de chunk Z80 coûte 21 cycles de moins chez Gens
(−21 à l'itération finale, quand `z->pc != g->pc`). Empirique : le mécanisme exact n'est pas élucidé.
Elle lève la rupture 12766 (poignée de main `$0732A0` : 40 contre 39 passes) ; la première rupture
RAM passe d'environ 12600 à 17600. Surcharge d'expérience : `DP_MD_LDRE=min:delta`.

## Rupture 17600–17603 : ce qui est établi

- Trace PC du noyau (trame 17896) alignée instruction par instruction sur Gens (hooks `registerexec`
  Lua) : même séquence, 13 050 contre 13 055 entrées.
- Le décalage d'odomètre est nul jusqu'à la poignée de main Z80 de ROM `$732A0` (requête de bus à
  fc 44758, Z80 en `$016D`, flag `$A01FFD` clair → `beq` vers `$732C4`, accès YM bus tenu). Ensuite le
  noyau a ~154–266 cycles d'avance sur Gens, stable jusqu'à la fin de trame. Le V-int tombe chez Gens
  avant `$D7E`, alors que le noyau l'a déjà passé (marge ~240–350 cycles).
- Les frontières d'odomètre Gens sont des débuts de ligne (+~8) et +~90 (chunk Z80) ; le saut vient
  d'une ligne franchie ~11 instructions plus tôt chez Gens.
- Cause non identifiée.

## Hypothèses écartées

- DMA/fill : aucun DSTEP/FILL en ligne 54.
- Relâchement du bus : le `move.w #0,$A11100` n'est pas exécuté dans cette séquence.
- Pénalité par accès 68k à `$A04000-3` (`DP_MD_YMACC`, 20/40 cycles) : désynchronise tout dès 17600.
- Pénalité au premier accès YM après relâchement (`DP_MD_RELPEN`) : sans effet, condition jamais vraie.
- Injection de N cycles à la requête de bus fautive (trame 17896, fc 44758) : N = 154, 200, −154 sans
  effet sur les écarts ; N = 266 ne laisse que 4 octets d'écart à 17625 (17600 inchangé). Indice faible
  que l'écart est de l'ordre de 266 cycles à cet endroit, sans le localiser.
- Hooks Lua a4/a5/a6 : valides seulement dans le contexte 12766 ; compteur HV du VDP lu par Lua :
  renvoie 0. L'odomètre Lua est en grumeaux d'environ 400 cycles : il manque une horloge Gens plus fine.

## Outils et numérotation

- `m->frames` brut = vint_n + 294 vers 17600 (245 vers 12500) ; le compteur Lua `f` vaut vint_n.
  `--save-frames` doit dépasser n+294.
- Journaux noyau : `DP_MD_PCLOG=<m->frames>` (lignes `PC <frame_cycles> <pc>`), `DP_MD_FFDLOG=<trame>`
  (requêtes de bus), `DP_MD_FILLLOG=<n>` (DMA), `DP_MD_VINT_LOG=1`.
- Toute commande `deepeel` lancée à la main doit exporter le profil Gens complet, sinon le run est
  différent.
- Les modifs `meta/gbl_*` d'une autre session de m0boy cassent le patch `dp_gblmach` ; la compilation de
  travail se fait depuis un snapshot (HEAD de m0boy plus les `md_*` modifiés).

## Reste à faire

Trouver la cause du décalage de ~266 cycles en `$732C4` (instrumenter Gens pour une horloge plus fine),
poursuivre jusqu'à 25650 (rupture suivante vers 20407), retirer les diagnostics (ZADJ, BUS, FFDLOG,
ZTRACE, PCLOG…), documenter dans `docs/MD.fr.md`.
