# Première exploration MSX1

Le registre matériel est accessible par `build/deepeel regs msx`. Le pont
`src/platform/msx/` réutilise les sources MSX/SG/TMS/AY de m0boy et le CPU
Z80 commun. Une exploration dynamique bornée produit des rapports et cartes
avec le moteur partagé Z80, suivie d’une fermeture statique sur les vues
de slots et de banques observées.
Il ne s’agit pas encore de la parité avec l’explorateur GB.

```sh
build/deepeel z80 explore msx jeu.rom --mapper auto --text rapport.txt --map carte.map
build/deepeel z80 explore msx jeu.rom --mapper ascii8 --max-total 20000000 --input-total 20000000
```

Les plafonds de pas, chemins, files, boucles, fourches et journaux sont ceux
décrits dans [Z80.fr.md](Z80.fr.md). Aucun en-tête de copieur Sega n’est retiré
d’une image MSX. Le mapper effectif est inscrit au rapport. Les graines de
vecteurs BIOS sont désactivées : les interruptions sont prises par la machine
concrète. La fermeture statique utilise les mêmes plafonds `--closure-*` que Sega ;
`--no-closure` désactive toute la fermeture ; `--no-target-closure` désactive
les cibles et leur affinement BC/DE. `--no-block-closure` ne désactive que
cet affinement, qui emploie le solde des budgets de nœuds, graines et vues
des cibles. Les plafonds et saturations
sont séparés par frontière, afin de conserver toutes les preuves initiales. Ses octets `code-static` et `data-static` restent distincts des
exécutions et lectures concrètes. `--evidence preuves.evidence` conserve
tous les bits de preuve, y compris les octets utilisés à la fois en code et
en donnée ; le format et les priorités sont décrits dans [Z80.fr.md](Z80.fr.md).

## Profil matériel

Le profil fixe comporte 64 Kio de RAM, un TMS9918A et 16 Kio de VRAM, un
AY-3-8910, un PPI 8255 et quatre slots primaires :

- slot 0 : BIOS simulé, visible dans la page CPU 0 ;
- slot 1 : cartouche, fenêtres `$4000-$BFFF` ;
- slot 2 : vide ;
- slot 3 : RAM, disponible dans chacune des quatre pages CPU.

Le port `$A8` sélectionne un slot par page de 16 Kio. Au démarrage simulé
usuel, `$F4` sélectionne BIOS, cartouche, RAM, RAM. Une cartouche de 32 Kio
n'apparaît donc pas automatiquement dans la deuxième page : le programme
doit sélectionner son slot, directement ou par un appel BIOS. Une petite
cartouche dont INIT est dans `$8000-$BFFF` reçoit la disposition `$D0`.
Pour une cartouche bancarisée dont INIT est dans cette page, le pont y
sélectionne aussi le slot de cartouche (`$D4`) avant de démarrer le CPU.
Une entrée INIT située dans le padding de fin d’image est refusée.

Les cartouches ont un en-tête `AB` et une entrée INIT non nulle. Le pont
accepte jusqu'à 1 Mio, choisit heuristiquement un mapper ou reçoit un choix
explicite via l’API ou `--mapper` : `plain`, `ascii8`, `ascii16`, `konami`, `konami-scc`.
Le profil plain est limité à 32 Kio. Les cartouches BASIC sans INIT, les
formats alternatifs et les cartes SRAM demandent encore un autre profil.

Les fenêtres des mappers font 8 Kio, sauf ASCII16 qui sélectionne une paire.
Les index ROM restent des **offsets physiques compacts en octets**. Les
lectures BIOS, RAM, slot vide et padding ajouté en fin d'image ne sont pas
des octets de la ROM. Les écritures de banque ne sont actives que lorsque
le slot de cartouche correspondant est visible. La fenêtre SCC active
`$9800-$9FFF` est exclue des index ROM : la lecture matérielle de ce
périphérique n'est pas intégralement modélisée par la runtime importée.

Les ports `$98/$99` pilotent données, adresse, registres et statut VDP ;
`$A0/$A1/$A2` sélectionnent, écrivent et lisent l'AY. Le PPI utilise
`$A8-$AB` pour slots, clavier, cassette, LED et clic. Les IRQ VDP sont de
niveau, activées par R1.5 et acquittées par lecture de statut. Le boot
simulé initialise IM1 et autorise les interruptions ; les hooks BIOS
H.KEYI et H.TIMI restent du code/mémoire extérieur à la cartouche.
La touche RETURN est une entrée clavier, **pas une NMI de Pause console**.

Référence matérielle : [MSX Technical Data Book](https://map.grauw.nl/resources/system/msxtech.pdf).
Le registre décrit le matériel ; il ne prouve pas l'implémentation de tous
les périphériques ou de chaque variante de machine.

## États, entrées et BIOS

La machine MSX possède ses propres symboles et sa propre instance ; elle
ne partage aucun état avec SG-1000, SMS/GG ou GB. Le build renomme aussi
les indicateurs globaux ASan, afin que la vérification simultanée des
instances ne dépende pas de collisions de symboles.

Les snapshots privés contiennent CPU, RAM, VRAM, état AY/SCC, entrées, slots,
banques, IRQ et latch VDP, puis reconstruisent les pointeurs de fenêtres au
chargement. Ils sont liés à une initialisation de cartouche précise par un
numéro de génération. Ils ne sont ni un format persistant ni échangeables
entre ROMs. Une taille incorrecte ou une ancienne génération est refusée.

Le clavier importé est partiel : directions, SPACE et RETURN, avec deux
pads joysticks via l'AY. Ce profil ne fournit pas encore toute la matrice
ni les périphériques spécialisés. Les bits de direction AY, le mode 8255,
les sous-slots, le mapper RAM `$FC-$FF`, les interfaces cassette, disque,
imprimante, série et les composants MSX2 demandent des modèles supplémentaires.

Aucune image de BIOS n'est ajoutée au dépôt. La runtime fournit de petits
stubs Z80 et des routines C simulant une partie des services BIOS. Les
accès internes des services mémoire/VDP passent désormais par les callbacks
du CPU. Les accès directs par slot produisent des événements distincts
avec index ROM ou RAM physique ; ils ne sont pas attribués à la vue CPU
courante. Les écritures AY ont leur événement de registre, y compris
lorsqu’elles viennent d’un service BIOS. Un début et une fin de trap
encadrent chaque appel, afin que l’explorateur puisse traiter ses effets
sur les registres. L’explorateur invalide leurs origines après un service C,
sauf pour le résultat pur reconnu de `RDSLT`. `WRSLT` transmet l’origine de E
à la cellule RAM physique, même cachée. `LDIRVM`, `WRTVRM` et `FILVRM`
consignent les copies pures vers la VRAM ; une lecture VRAM ou une donnée
transformée ne reçoit pas artificiellement une origine ROM. Les tags RAM
couvrent les 64 Kio et sont restaurés dans chaque fourche. Les ports privés
`$00-$3F` de ces traps sont un mécanisme logiciel de l'outil, pas des
registres physiques MSX. Le pont ne déclenche ces traps que depuis un stub
reconnu du BIOS visible ; un `OUT` bas depuis une cartouche ou la RAM ne
devient pas artificiellement un service BIOS. Les observations de cette machine ne constituent
pas des mesures de matériel réel.

## Exploration et journaux

Les deux issues des conditions et répétitions Z80 sont explorées sous budgets.
Les exécutions de BIOS ou de RAM incrémentent `exec_outside` sans marquer la
cartouche ; du code ROM dans `$8000-$BFFF` garde son index physique réel.
Le journal de banques distingue écriture mémoire, `OUT $A8` et modification
de slots par BIOS. Les changements de visibilité SCC sont également sondés.

Les commandes indirectes VDP et prises effectives INT IM0/1/2 utilisent les
journaux partagés. L’AY possède des compteurs par registre et un journal de
1 024 écritures, avec compteur de débordement ; la source BIOS ou port est
indiquée. Les appels BIOS sont comptés séparément par identifiant, sans faux
accès matériels aux ports privés `$00-$3F`. Le bootstrap simulé précédant le
point INIT ne fait pas partie de cette trace d’exploration.

Les sondes d’entrée ont leur deuxième phase, leurs propres files et budgets.
Elles reconnaissent AY R14 et la sélection de joystick R15.6, les lignes
clavier PPI 7/8, ainsi que les services BIOS `GTSTCK`, `GTTRIG`, `SNSMAT`,
`CHSNS` et `CHGET`. Les états choisis persistent jusqu’à une autre sonde.
RETURN est consignée comme touche ; un HALT MSX ne reçoit aucune sonde de
Pause/NMI Sega. Les autres lignes et périphériques restent à modéliser.

## Fermeture statique

Chaque vue conserve, par segment de 1 Kio, l’index ROM éventuel, la présence
de RAM et les fenêtres de cartouche visibles (y compris le padding). Deux
choix de slots montrant la même ROM mais une RAM différente sont des vues
distinctes. L’analyse ne fait tourner ni le CPU ni un service BIOS.

Les écritures mémoire invalident les fenêtres visées par les décodeurs
ASCII8, ASCII16, Konami ou SCC. Les plages hors décodeur et les écritures
dans un slot de cartouche caché ne déplacent pas la ROM. Le premier banc
fixe Konami reste connu après une écriture mémoire de cible inconnue ; une
cartouche plain ne change aucune banque par écriture mémoire. Un `OUT $A8`,
ou une sortie de port inconnu pouvant le viser, invalide la vue de ROM et
les preuves de retour en RAM. Les autres ports connus ne sont pas assimilés
à un mapper Sega. L’analyse reprend depuis une vue concrètement observée,
sans deviner la valeur d’une banque ou d’un slot.

Une preuve de retour créée par CALL/RST n’existe que si les deux cellules
de pile sont en RAM visible. Elle fonctionne dans les pages basses et à la
frontière `$FFFF/$0000`, sans miroir artificiel de 8 Kio. Une écriture qui
recouvre une cellule prouvée détruit la preuve ; une cible inconnue détruit
toutes les preuves. Huit preuves au maximum sont conservées par contexte,
avec compteur de saturation. Les retours depuis un mot ROM connu restent
possibles ; la RAM concrète n’est pas lue pour deviner un retour statique.

Les constantes HL/IX/IY observées ou établies par transferts purs et mots
ROM connus peuvent résoudre `JP (HL/IX/IY)` ; une lecture RAM, un mot
tronqué ou une transformation non suivie détruit cette preuve. `INC/DEC`
16 bits et les lectures d’octets ROM à adresse connue permettent aussi de
recomposer une table via A ; une paire partielle, un slot RAM/BIOS/SCC ou une
banque invalidée ne fournissent pas un octet ROM supposé. Les constantes
ROM réellement consultées par l’analyse portent `data-static`, sans lecture
machine fictive ; le décodage et les sondes de padding ne portent pas ce bit. Les règles de mapping
restent applicables à la cible. Une adresse d’écriture connue via HL/IX/IY
préserve les preuves de pile non recouvertes et ne touche que le décodeur
de mapper réellement visé. Le déplacement indexé est signé et boucle sur
16 bits ; un slot RAM à la place d’un registre cartouche ne change pas de
banque. Une paire entièrement connue peut aussi établir SP par `LD SP,HL/IX/IY`.

Les deux branches d’une condition sont suivies sous les plafonds de nœuds,
graines, vues et contextes. Un appel BIOS n’a pas de continuation supposée :
l’analyse s’arrête en dehors de la cartouche, sur HALT, saut indirect non
résolu, donnée copiée sans exécution naturelle, padding ou mapping ambigu.
La fenêtre SCC active et la fin partielle d’une image ne deviennent pas du
code ROM. Les saturations sont affichées dans les compteurs de fermeture.

## Instrumentation du BIOS simulé

Le build génère une copie instrumentée de `msx.c` ; aucune modification n'est
appliquée à m0boy. Les aides BIOS mémoire et VDP utilisent les callbacks
courants du CPU. Le décodeur matériel des ports appelle ensuite les fonctions
VDP natives, afin de ne pas réentrer dans le même callback. Les accès cachés
`RDSLT`/`WRSLT` sont observés dans leur slot réel ; la fenêtre SCC ne reçoit
aucune origine ROM quand le périphérique est activé.

Les remplacements générés vérifient que chaque point d'instrumentation existe
exactement une fois. Un déplacement ou une duplication dans une future source
m0boy fait échouer le build ; il demande une nouvelle inspection de la source.
Un contrôle local a vérifié les deux refus et la conservation d'une sortie
préexistante (`output/msx-bios/anchor-validation.json`, sortie ignorée).

`dp_msx_observe` enregistre un observateur en lecture seule après init. Ses
événements ne remplacent pas les callbacks mémoire/ports : ils complètent les
accès qui ciblent directement un slot et les écritures AY. L'observateur ne
fait pas partie d'un snapshot et est effacé à la fermeture/réinitialisation.
Il ne conserve aucun journal sans borne dans le pont.

## Vérification

`test_msx` crée ses cartouches en mémoire et vérifie : slots ROM/RAM/vides,
INIT dans les deux pages, ROM partielle, quatre mappers bancarisés, écritures
ignorées dans un slot caché, exclusion SCC, deux pads, clavier réduit,
BSR du PPI, VRAM/read-ahead/IRQ, un service BIOS de copie d'octet en VRAM,
restauration de banque/RAM/CPU/latch, rejet des anciens snapshots et
isolation avec les machines Sega présentes dans le même exécutable.
Les boucles de test sont bornées ; aucune ROM commerciale n'est nécessaire.
`test_msx_bios` vérifie les copies BIOS dans les deux sens, les remplissages,
les accès cachés ROM/RAM, les écritures AY et le cycle de vie de l’observateur.
`msx_bios_equivalence` compare la version instrumentée à la source BIOS
m0boy non instrumentée, dans deux exécutables : onze points de contrôle
comparent registres CPU, horloges, RAM visible/cachée, VRAM et état des
périphériques. Il s’agit d’un contrôle d’absence de changement dû aux traces,
pas d’une preuve de fidélité du BIOS simulé au matériel réel.
`test_msx_explore` vérifie branches, provenance CPU/BIOS, RAM cachée et
restauration des tags hauts dans les fourches, slots/mappers, VDP/AY,
sondes de clavier/pad sans NMI, IRQ IM2 et saturation des journaux.
`test_msx_closure` vérifie les piles basses/cachées et leur bouclage, les
vues à ROM identique mais RAM différente, les quatre décodeurs de mapper,
les ports PPI/VDP/AY, l’arrêt au BIOS/SCC/padding, la récursion bornée et
un gain de code statique après coupure du budget dynamique.
La suite complète compte vingt-deux tests CTest verts ; les seize tests
Z80/Sega/MSX passent aussi sous ASan/UBSan avec LeakSanitizer désactivé
(environnement sous ptrace). Aucune absence de fuite n’en est déduite.
Aucune ROM MSX réelle n’était présente dans le corpus local consulté.

Le contrôle CLI local utilise une cartouche synthétique générée uniquement dans
`build/`, de taille `$4200` (cas qui ne doit pas subir le retrait d’en-tête
Sega), avec copie BIOS et choix explicite ASCII8. Les sorties texte/carte sont
vérifiées dans `output/z80-msx-integration/validation.json` (ignoré). Dans le
même contrôle, rapports et cartes d’Action Fighter SMS et d’Aerial Assault GG
sont identiques aux références `output/z80-hardware/`, aux mêmes options
(`--copy-cap 0`, autres plafonds par défaut). Ce sont des observations de
l’outil sur ces deux images, sans conclusion de fidélité matérielle générale.

La validation de la fermeture (`output/msx-closure/validation.json`, sortie
locale ignorée) utilise une autre cartouche synthétique limitée à un seul
pas CPU : 3 octets sont connus sans fermeture, 9 avec elle, dont 6 octets
`code-static`. Les compteurs dynamiques restent identiques et aucune donnée
n’est lue ou copiée par l’analyse statique. Rapports et cartes des deux images
Sega ci-dessus restent également identiques à options constantes. Aucun gain
sur une ROM MSX commerciale n’est revendiqué faute d’image dans ce corpus.

La consultation statique des deux octets de cette table synthétique est
maintenant conservée en `data-static` : 14 octets connus au lieu de 12,
avec toujours un pas CPU et zéro lecture ROM dynamique. L’export `--evidence`
reconstruit les deux masques `20` ; les preuves superposées gardent tous
leurs bits. Ces données consultées par l’analyse ne sont pas des lectures
observées de la machine ni une mesure de matériel MSX.

L’affinement BC/DE suit leurs chargements ROM et transferts, les incréments,
`DJNZ` et les mises à jour des instructions de blocs. Il conserve les règles
de slots pour chaque lecture/écriture et distingue l’octet C d’un port des
16 bits d’une adresse mémoire BC. Ses contextes sont séparés, ses budgets
pris sur le solde des cibles ; il conserve les cartes et compteurs antérieurs.
Les tests positifs vérifient la cible dans une exécution CPU bornée ; les
résultats d’analyse statique restent distincts des observations de machine.

Le contrôle CLI synthétique BC/DE (`output/z80-blocks/validation.json`, ignoré)
atteint le HALT derrière `LD DE,target; EX DE,HL; JP (HL)` : neuf octets connus
contre huit lorsque l’affinement est désactivé, avec les mêmes compteurs
antérieurs, un pas CPU et zéro lecture ROM dynamique. Les tests de budgets
vérifient aussi le refus compté des observations après finalisation.
