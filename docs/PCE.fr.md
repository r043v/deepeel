# PC Engine (HuCard)

deepeel explore les HuCards PC Engine / TurboGrafx-16 avec le cœur de
m0boy : le HuC6280 (`targets/pce/runtime/huc6280.c`), le VDC et le PSG sont
compilés tels quels. Le pont `src/platform/pce/pcemach.c` inclut `pce.c` de
m0boy et intercepte tout le bus hors WRAM : les fenêtres ROM directes du CPU
sont coupées et le pont projette lui-même les pages MPR, ce qui lui permet de
signaler chaque lecture de la cartouche.

## Utilisation

    deepeel pce explore jeu.pce [--text rapport.txt] [--map carte.txt] [--png carte.png]
    deepeel regs pce

L'en-tête copieur de 512 octets est retiré s'il est présent. Les images de
384 Kio suivent le découpage matériel (256 Kio en $00-$3F, la fin en
$40-$7F). Les autres tailles sont projetées modulo leur nombre de pages.

Les index ROM sont les offsets physiques, ce qui revient à
`banque16K * 0x4000 + (addr & 0x3FFF)`, comme sur les autres plateformes.

## Passes

1. **Fourches bornées.** Les deux issues de chaque conditionnel sont
   explorées. Pour BBR/BBS, c'est le bit en page zéro qui est inversé.
   S'y ajoutent les fourches de manette sur les lectures de $1000 et les
   fourches d'IRQ (vblank VDC, timer) aux points d'activation (CLI, PLP, RTI,
   écriture de $1402). Ces fourches sont plafonnées par site
   (`--fork-cap`, `--pad-fork-cap`, `--irq-fork-cap`), par chemin
   (`--max-steps`) et par arrière-branche (`--loop-cap`), avec un budget
   global (`--max-total`).
2. **Jeu.** `--play N --sessions K` lance des sessions depuis la mise sous
   tension avec des entrées aléatoires tenues. `--tas film.bk2` rejoue un
   film BizHawk, ou un texte d'un octet hexadécimal par trame (préfixe `R`
   pour un reset). Toutes les 300 trames, un point de reprise amorce la file
   des fourches.
3. **Recherche par nouveauté.** `--novelty PAS` fonctionne à la manière de
   Go-Explore. Une archive d'états compressés est entretenue ; chaque essai
   reprend l'état le plus productif (gain de cellules par sélection) et joue
   `--novelty-frames` trames d'entrées aléatoires. Les essais qui découvrent
   des cellules ajoutent un point de reprise à l'archive, plafonnée par
   `--novelty-archive`. En mode jeu, le pont exécute la boucle native de m0boy
   ligne par ligne, avec un crochet de trace par instruction.
   Le flux d'instructions est identique au pas à pas, ce qui a été vérifié
   sur Raiden : les mêmes cellules sont observées.
4. **Analyse statique.** Elle comprend :
   - une fermeture statique dans la fenêtre MPR observée de chaque page ;
   - la détection des tables `JMP (abs,X)` ;
   - le bourrage ;
   - l'inférence des petits trous entre du code et des données
     (`--no-static`, `--no-infer`).

Les copies ROM→VRAM sont consignées. Il s'agit des écritures sur le port de
données VDC (registre 2) d'une valeur lue juste avant en ROM, ou d'un
transfert de bloc (TII/TDD/TIN/TIA/TAI) vers ce port.

Toute saturation est comptée et rapportée dans la ligne `cuts:` du rapport.
Les instructions illégales coupent le chemin (`illegal`). L'exécution hors
ROM (WRAM) est comptée dans `exec outside ROM`.

`--novelty-cells` active une variante Go-Explore. Les cases de l'archive
sont indexées par une empreinte grossière de l'écran (zone visible de la
BAT, défilement vertical par pas de 32 pixels). L'octet de vies est appris
sans connaître le jeu : un octet ≤ 9 qui ne baisse que par pas de 1, espacés
d'au moins 200 trames, et ne remonte que rarement de 1. Les essais qui
perdent une vie sont rejetés, et la sélection favorise les cases profondes.
Sur Raiden, elle identifie bien le compteur (`$34C5`), mais ne dépasse pas
2 480 trames de jeu en un million de trames jouées. Elle reste donc
optionnelle ; la sélection par gain reste le défaut.

Les copies vers le VDC sont regroupées en flux, quel que soit le `pc`, avec
un trou d'au plus 8 octets (les octets de contrôle d'un décompresseur). Un
rejeu d'un flux déjà consigné n'ouvre pas de nouvel enregistrement.

## Observations sur Raiden (USA)

Ces chiffres sont des observations de l'outil (machine concrète, RAM à
zéro), pas des mesures du matériel.

| passe                                | observé | classé |
|--------------------------------------|--------:|-------:|
| fourches seules, 20 M pas            |  8,7 %  | 20,9 % |
| jeu aléatoire, 13 309 trames         | 17,6 %  | 31,8 % |
| nouveauté, 6 G pas (≈ 3 min)         | 23,6 %  | 39,5 % |

L'essentiel de la ROM est constitué de graphismes et de données de niveaux,
qui ne sont lus que lorsque le niveau correspondant est atteint. L'objectif
de 90 % suppose d'atteindre tous les niveaux. Aucun film TAS de Raiden
n'existe sur TASVideos ; un film BizHawk d'une partie complète, passé en
`--tas`, reste la voie la plus directe.

## 0.250.5 — fidélité PCE : banc de comparaison Beetle PCE

`tools/pce_compare_reference.sh ROM film trames` rejoue le même film
(.bk2 PCEHawk ou texte hexadécimal, chargeur `dp_pce_tas`) sur le cœur
libretro Beetle PCE / Mednafen (`DP_BEETLE_SO`, cloné et construit hors
dépôt) et sur le runtime m0boy, diff des 8 Kio de RAM de travail par trame
(la SYSTEM_RAM de Beetle porte les banques SuperGrafx/CD au-delà ; la
comparaison prend les 8 Kio partagés). Premiers résultats (Raiden USA,
300 trames d'un film synthétique sans TAS disponible : deux appuis sur
Run) : divergence dès la trame 1 sur un octet (`$0000` : Mednafen $EB,
m0boy $FF), qui s'élargit (~835 octets à la trame 300) — le boot des deux
machines diffère dès le premier passage ; à démêler octet par octet.
Observations de l'outil, pas des mesures de matériel.

## 0.249.85 — `--summary` (campagne JSON)

`pce explore --summary FILE` écrit le même profil JSON de campagne que MD
(deepeel, platform, rom_size, classes du map, known_pct, compteurs de pas,
courses, fourches, coupes, exécution hors ROM) : la matière première de
`tools/compare_platforms.py`, qui alimente la ligne PCE du tableau comparatif
`docs/COMPARATIF.fr.md`. Test `pce_summary`. Première observation (Raiden
USA, 5 M pas, borné 120 s) : 18,3 % connus — cohérent avec le tableau
ci-dessus (les données de niveaux ne se révèlent qu'en jouant).

## 0.249.91 — frontière partagée (§6) : `--defer-tiers` (opt-in)

Comme NES (0.249.90) : l'admission des continuations coupées (`step`,
`loop`) passe par la politique partagée `src/explore/frontier.c` — reprise
à budget ×4 jusqu'à `--defer-tiers N` (0 : coupes finales, défaut), raison
bruyante bornée au quart de la zone, fourches au palier du parent, restes
au lot disque. Rapport : `frontier: deferred steps/loop; resumed; final;
dropped`. Test `pce_defer_tiers`.

## m0boy

Correctif rétroporté : ST0/ST1/ST2 écrivent toujours sur le VDC, en
$1FE000-$1FE003 physique, quelle que soit la projection de MPR0.
L'interpréteur et l'émetteur du recompilateur sont corrigés.
