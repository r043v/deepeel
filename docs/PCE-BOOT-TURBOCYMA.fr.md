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

## La fenêtre « mystery » du HuC6270 (dérivée du code)

Le comptage des wait-states des deux côtés sur le boot Tonma a levé le
dernier modèle : Mednafen ne bloque **jamais** le CPU sur le DMA SATB dans
ces trames (202 événements WS, tous `WS:mys`), alors que le pont posait
~1500 attentes SAT (~95000 maîtres). La fenêtre mys du HuC6270 est
dérivable de son code : `VDC_IS_BSY` n'est que `pending_read ||
pending_write` ; une écriture du port de données posée pendant la phase
busy — **16 dot-clocks après 48 dot-clocks depuis le début du hsync**
(x=1128), soit `[1128+48·r, 1128+64·r]` maîtres de la ligne — reste
en attente, et l'accès suivant bloque jusqu'à la sortie de phase. Le pont
utilise maintenant cette fenêtre dérivée à la place du calage
`[979, +170]` ajusté sur l'ancienne géométrie (`PCE_MYS`/`PCE_MYSW`
maintiennent l'ancien comportement), et les attentes SAT ont leur propre
drapeau (`PCE_SATWAIT`) : sur le boot Tonma, fermer les attentes SAT
rapproche beaucoup les flux (f9 passe de −27 à −8 écritures ; la
divergence dans f6 recule de l'écriture 1308 à 1326, l'arrivée de l'IRQ
s'aligne).

## La mesure au cut : indexer sur les trames du film

La comparaison des dumps DSY (une WRAM par trame) s'est révélée piégeuse :
le dump `fn=N` de la référence n'est pas l'état à la fin de la trame film
N−1 comme le suppose l'outil de comparaison. Vérification directe : le
dump `fn=5` de la référence contient déjà `wram[0]=01` (l'écriture
`pc=4006`) alors que cette écriture tombe à ts=164301 **dans la trame 5**
du film (position dans le fichier : après le front VS f=5, avant le VS
f=6). La frontière « un Emulate Mednafen » ne coïncide donc pas avec la
coupe scanline 254 du film. La comparaison par **flux d'écritures** (WWM
des deux côtés, alignés par fin d'Emulate côté pont et par marqueurs de
sonde côté référence) reste l'autorité : f0–f5 octet-exacts, f6 divergence
à l'écriture 1326 avec l'IRQ alignée, f9 à −8. La mesure « au cut » doit
être refaite sur des instants explicitement alignés (les frontières de
Emulate de chaque côté), pas sur l'index de dump.

## Non-régression

- `tools/pce_dsy` reconstruit avec le runtime modifié : 6000/6000 trames
  identiques à la sortie Bonk 3 déjà vérifiée ; contre `bizhawk_dsy pce` :
  280 trames hors pile, aucune divergence soutenue (état documenté).
- Chemin PCEHawk du pont : médianes par tranche inchangées, aucune
  divergence soutenue — le nouveau chemin est derrière le drapeau
  `pce_vce_model` (profil TurboNyma seul).

Un segfault transitoire a été observé deux fois (runs N=2000 du pont, un
test `nes` sous ctest parallèle) sans se reproduire depuis ; le test `nes`
passe isolé et n'est pas affecté par ces changements (aucun chemin PCE
dans son binaire).

Ce sont des observations de l'outil sur deux jeux, pas des mesures de
matériel.

## Mesures du 13 novembre 2026 : Mednafen 1.32.1 autonome contre la référence BizHawk (Tonma)

Observations de l'outil (machine concrète, RAM à zéro) — pas des mesures de matériel.

- **Numérotation** : le dump `fn=N` du Mednafen patché autonome est identique octet pour octet (hors pile
  `$100-$1FF` et `$6`) au dump `fn=N-1` de `ref/tonma_tn.dsy`, de N = 1 à 250. `tools/dsy_cmp.py` applique ce
  décalage par défaut.
- **m0boy contre Mednafen autonome** (pont `pce_bridge_dsy`, profil TurboNyma, décalage 0, 252 frames) : 247
  frames identiques, aucune divergence soutenue ; les écarts sont des tampons transitoires de 2 à 33 octets
  (`$1BC4-$1BE2`) qui disparaissent d'eux-mêmes. Les traces d'instructions des frames 250-252 sont identiques
  sur 43 936 instructions, coupes comprises (boucle d'attente `$4146` des deux côtés).
- **Référence BizHawk contre Mednafen autonome** : à la frame 251 elles divergent l'une de l'autre, pas seulement
  de m0boy. Le même code écrit `$0201 = 0` (pc `$6045`) un dump plus tard dans la référence. La « divergence
  soutenue 251 » n'est donc pas une erreur de timing de m0boy : m0boy suit Mednafen 1.32.1, la référence (TurboNyma
  de BizHawk) s'en écarte. Fermer l'écart demande une trace de la référence elle-même.
- **Balayages sans effet** sur la frame 240/251 : `PCE_IRQD` (0-100), `PCE_PHASE` (250-265), `PCE_BOOT0`
  (237-1400), largeur de la fenêtre mystère `PCE_MYSW` (64-120).
- **Durées d'attente (WS) de Mednafen** (sonde `STEAL` dans `VCE::WS_Hook`, Tonma frame 239) : 6 vols de cycles
  par frame, de 27 ou 30 cycles CPU, tous `WS:mys` ; ils sont déclenchés par l'accès à la position 1314-1317 de la
  ligne et se terminent à la position 270-276 de la ligne suivante (fin d'événement VDC, pas une durée fixe).
- **Mega Drive** (`md_compare_reference`, `dsy/sonic.gmv`, ClownMDEmu, 6000 frames) : 3 octets de RAM de travail
  diffèrent, mode/zone/position X identiques. Les 12 autres films Sonic 1 de `tas/games/sonic-md-all` sont des
  liens cassés vers `output/tas/…` : à retrouver pour re-mesurer les relectures « incomplètes ».
