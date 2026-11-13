# PCE : modèle de boot Mednafen/TurboNyma, découvertes par traces

Tout ce qui suit est une **observation de l'outil** sur le TAS Tonma
(`10124S`, cœur TurboNyma de BizHawk) et sur le cœur Mednafen 1.32.1
patché hors dépôt avec des sondes — pas des mesures de matériel. Le but
est de faire jouer au pont deepeel (`src/platform/pce/pcemach.c`, machine
m0boy) exactement la chronologie du cœur de référence, pour rejouer les
films TAS frame par frame.

## La méthode

Trois sondes comparées, alignées par flux d'instructions :

- **côté référence** (Mednafen patché) : `WWM pc=… a=…=…` logue chaque
  écriture en RAM de travail (pc, adresse, valeur) ; `V VS/VSE/VD f=… ln=…
  pos=… ts=…` logue les fronts du compteur vblank VCE et du test VD du VDC
  avec la ligne et la position dans la ligne (`1365 - hblank_counter`) ;
- **côté pont** : les mêmes `WWM` (sonde `pce_trace_wram_write` du `wr`
  de l'interpréteur, activée par `-DPCE_TRACE`) et `V LFIN` (une ligne
  par fin de ligne du pont : lm, cycles, status, machine d'états, registres).
- `tools/trace_cmp.py --collapse` pour la première divergence de pc,
  le script de comparaison des flux d'écritures pour la première écriture
  qui diffère.

## Découverte 1 — le harness décalait le film d'une trame

Le compteur de trame du jeu (WRAM `$1e`, écrit `e0 e1 e2…` à chaque vblank)
valait un de plus côté pont à trame égale. Cause : la trame 0 de la
référence n'est pas une trame de 262 lignes. Mednafen coupe sa trame à
l'incrément scanline 254 ; au boot, la numérotation des scanlines ne
redémarre à zéro qu'après le VSE du boot (front vblank bas à 4125 maîtres),
et la trame 0 se termine donc au mur **351042** = 237 + 257×1365 : la
« trame de boot » de 237 maîtres de hsync **plus** 257 lignes pleines, soit
258 fins de ligne côté pont. Le pont faisait 262 lignes puis sautait cette
trame (`pre_frames=1`) : chaque trame du film était appliquée une trame
plus tard. Correction : `pre_frames=0` et première frame = 258 fins de
ligne.

## Découverte 2 — l'horloge ligne démarre 237 maîtres dans le hsync

`VCE::Reset` de Mednafen pose `hblank_counter = 237` (reset dans le hsync,
237 maîtres avant sa fin) et `vblank_counter = 4095 + 30` : le premier
cycle VCE dure 30 maîtres de plus qu'un cycle plein. Les frontières de
ligne du pont sont donc décalées de 237 maîtres pour coïncider avec les
bords de scanline de la référence : première ligne de `237/div` cycles CPU
(div = 3 en vitesse haute, 12 en basse), `master_carry = 237 % div`.
Vérifié au mur : les coupes de frame du pont tombent sur 351042, 708672,
1066302… exactement les fins de scanline 254 de la référence.

## Découverte 3 — le compteur vblank VCE et le réarmement du VDC

Mednafen a un compteur de vblank dans la VCE, absent du pont :

- vblank=1 pendant **4095** maîtres au boot (4095+30), puis vblank=0
  pendant `(lc263 ? 358995 : 357630) − 4095` — `lc263` est le bit 2 de
  `vce_cr`, **lu au rechargement** du compteur ;
- les fronts tombent toujours à la même position sub-ligne : **207 maîtres
  avant la fin de la période active** (pos 1158 ; l'arithmétique du
  compteur le prouve : VSE→VS = 207+237+259×1365+921 = 354900 exactement
  pour une frame 263 lignes) ;
- le front **VS** (vblank montant) appelle `VDC::VSync(1)` : la machine
  d'états verticale du VDC est réarmée **en pleine ligne** (VPhase=VSW,
  compteur VSW+1, registres échantillonnés sur le moment — la vraie puce
  échantillonne à chaque entrée en VSW aussi, donc une écriture en cours
  de cycle ne prend effet qu'au cycle suivant) et `NeedRCRInc` est posé à
  faux : le pas de phase de la ligne du front est sauté ;
- le front **VSE** (vblank descendant) ne fait rien côté VDC, mais il fixe
  la longueur de la trame suivante : le cycle scanline de la référence est
  figé au rechargement, pas lu ligne à ligne ;
- la trame finit à l'incrément scanline 254, soit 254 lignes après le reset
  de numérotation qui suit chaque VSE — d'où des trames de 262 ou 263
  lignes selon le cycle figé.

## Découverte 4 — la position du test VD (IRQ vblank) dans la ligne

Le test `VPhase != VDW && NeedVBIRQTest` se fait à l'**entrée HDW** de
chaque ligne, soit **~66 maîtres après le début de la période active**
(pos ≈ 303), avec la phase posée par l'IncRCR de la ligne précédente ;
l'IncRCR (avance de phase) a lieu plus tard dans la ligne, à la fin de la
fenêtre HDW (~822 maîtres). Le pont, lui, avance et teste à sa fin de
ligne — soit le début de la scanline suivante. L'écart net entre les deux
instants d'IRQ est de l'ordre de la quantification des boucles d'attente
du jeu (8 cycles CPU = 24 maîtres) ; `pce_irq_delay` (44) le compense en
partie, et la porte du délai était fausse : `!st_before` exigeait que
**tous** les bits de status soient à zéro alors qu'un bit DV résiduel du
DMA SATB suffit à le court-circuiter — corrigé en test VD seul.

## État de la convergence (observation, en cours)

Avec ces corrections, le flux d'écritures WRAM du boot est **octet-exact
sur f0–f5** (8236+0+0+0+0+200) et f6–f8 identiques en nombre ; f9 a 27
écritures de moins (la boucle audio `6f0e–6f45`, ~7 itérations de moins
dans la même fenêtre murale). Le premier VD tombe trame 6 des deux côtés.
Reste la position sub-ligne exacte de l'arrivée de l'IRQ dans le
décompresseur : elle déplace la frontière de tronçon au moment du cut de
la trame, d'où des divergences transitoires de dumps dès la trame 247
(contre 334 avant, quand le jeu était décalé d'une trame). Les valeurs par
défaut du profil TurboNyma ne sont pas encore recalées sur ce nouveau
modèle.

## Non-régression

- `tools/pce_dsy` reconstruit avec le runtime modifié : 6000/6000 trames
  identiques à la sortie Bonk 3 déjà vérifiée ; contre `bizhawk_dsy pce` :
  280 trames hors pile, aucune divergence soutenue (état documenté).
- Chemin PCEHawk du pont : médianes par tranche inchangées, aucune
  divergence soutenue — le nouveau chemin est derrière le drapeau
  `pce_vce_model` (profil TurboNyma seul).

Ce sont des observations de l'outil sur deux jeux, pas des mesures de
matériel.
