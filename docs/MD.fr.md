# Mega Drive

Support en cours de construction (jalon 2, version 0.13.0). Les nombres
produits sont des observations de l'outil (machine concrète, RAM à zéro),
pas des mesures de matériel.

## Ce qui existe

- **Machine** : le 68000 et le bus de m0boy (`MdFull`, `M0BOY_DIR`), pont
  `src/mach/mdmach.{c,h}`. Compilé avec `-DMD_TRACE` : m0boy expose des hooks
  (pas d'instruction, lecture, écriture) dans `runtime/md_trace.h`.
- **Cartouche** (`src/core/cart_md.c`) : détection de l'en-tête `SEGA` à
  `$100`, nom, région, checksum (déclaré et calculé), vecteurs `$0–$FF`.
  Une ROM sans en-tête SEGA retombe sur le chemin Game Boy.
- **Registre** (`src/platform/md/md_registry.c`) : VDP, Z80, YM2612, pads,
  bus Z80, TMSS, SRAM, mapper SSF2, RAM.
- **Passe linéaire** (`src/explore/md_trace.c`) : exécution depuis le vecteur
  de reset, avec budget de pas. Journalise l'exécution par octet, les
  lectures de données ROM, les accès par registre, les changements de banque
  SSF2 et les copies DMA ROM→VRAM (décodées depuis les registres VDP 19–23).

- **Désassembleur 68000 statique** (`src/cpu/m68k/`) : longueur, classe
  (Bcc/DBcc/BRA/BSR/JMP/JSR/RTS/RTE/TRAP), cibles, texte.
- **Exploration à fourches** (`src/platform/md/explore.c`) : les deux issues
  de chaque Bcc/DBcc (snapshot de `MdFull`), forks d'IRQ V-int (niveau 6)
  quand le VDP les autorise, enregistrements de copies DMA 68k→VDP, journal
  des changements de banque.

## Utilisation

    deepeel md explore jeu.md [--max-total N] [--text rapport.txt] [--map f]
    deepeel md explore jeu.md --tas film.gmv --max-total 1000000000
    deepeel md dis jeu.md HEX [n]
    deepeel regs md
    deepeel explore jeu.md        # alias de `md explore`

Bornes par défaut : 20 000 000 pas au total, 2 000 000 par chemin, file de
256, 2 forks par site, 64 visites par instruction. Chaque saturation a son
compteur dans le rapport (budget total, budget de chemin, borne de boucle,
file pleine, cap par site, copies perdues) ; une ligne non nulle signifie une
exploration incomplète.
`--tas` accepte un film GMV Gens (brut ou ZIP), NTSC, départ à froid et deux
manettes trois boutons, seule la première utilisée. La première entrée est
appliquée avant le premier pas et les suivantes après chaque trame complète.
Le film est prioritaire sur la limite de pas par chemin et la borne de boucle,
mais reste soumis à `--max-total` ; le nombre de trames rejouées est rapporté.
Le format n'identifie pas la ROM et la synchronisation avec Gens n'est pas
garantie : vérifier version et progression avant de conclure. Les films PAL,
avec état initial, multitap, manette six boutons ou entrée sur le joueur 2 sont
refusés, pas silencieusement tronqués. BK2 Mega Drive n'est pas encore pris en charge.
Sur Sonic 1 World Rev 00, le GMV JXQ publié sous le numéro 783 a été rejoué sur
65 849 trames avec 466 921 654 pas au total, fourches comprises. Union observée
hors lectures du checksum `$032C` : 43,43 %. La progression et la synchronisation
avec Gens ne sont pas encore vérifiées ; ce résultat n'est pas une partie validée.
Un banc externe indépendant compare la RAM et quelques repères Sonic aux mêmes
entrées sur m0boy et `clownmdemu-core`, sans inclure le code du second dans
deepeel : `sh tools/md_compare_reference.sh ROM film.zip 6000`. Initialiser les
sous-modules du cœur de référence et définir `CLOWN_MD_DIR` si nécessaire ; le
build deepeel doit déjà exister. Les deux cœurs ne découpent pas les trames au
même endroit, donc un petit décalage n'est pas en soi une faute CPU. Sur le
film JXQ, les deux machines sont encore en Green Hill acte 1 à la trame 3000 ;
à 3600, la référence est en acte 2 tandis que m0boy est encore en acte 1 : le
rejeu a effectivement divergé avant ce point (observations des deux outils).
Le banc signale aussi les séquences de 20 trames où la position X du joueur
diffère même en tenant compte d'un décalage d'une trame ; un premier signal
apparaît vers la trame 2025. Le test compare deux émulateurs, pas la sortie
enregistrée par Gens : il reste à identifier lequel s'écarte du film et pourquoi.
La copie de travail de `clownmdemu-core` fournit un interrupteur de comparaison
`DEEPEEL_GENS_60` : `DP_MD_REF_GENS_CLOCK=1` construit le banc avec 488 cycles
68000 par ligne, l'arrondi observé dans Gens et utilisé par m0boy. Le PC des
premières trames coïncide alors, mais l'écart vers 2025 demeure. La variable
`DP_MD_TRACE_RANGE=1950:2100` détaille position, vitesses et inertie sur ces
trames ; ces repères sont spécifiques à Sonic 1 World Rev 00.
`--queue-max N` autorise la file à doubler jusqu'à ce plafond ; sans cette
option, `--queue` reste un plafond fixe. Le rapport compte les agrandissements
et les refus, et chaque instantané occupe la taille complète de la machine.

## Clés ROM

Espace plat : clé = `addr & 0x3FFFFF` (≤ 4 Mio). Le `dp_rom_index` GB
(`bank * 0x4000`) n'est pas utilisé. Exécution hors ROM (RAM, Z80) comptée
dans `exec_outside`.

## Observations de l'outil (4 ROMs réelles, hors dépôt)

Machine concrète, RAM à zéro, registres événementiels statiques : ce ne sont
pas des mesures de matériel. Chaque exploration dure moins d'une seconde.

| ROM | pas | chemins | octets exécutés | couverture |
|---|---|---|---|---|
| Castle of Illusion | 41684 | 46 | 2404 | 0,5 % |
| QuackShot | 22317 | 35 | 1600 | 0,3 % |
| Sonic 3 | 15227 | 46 | 1546 | 0,1 % |
| Sonic 1 | 7244 | 33 | 994 | 0,2 % |

La couverture est faible : la plupart des chemins s'arrêtent sur la borne de
boucle (attente de V-int/VDP) ou sur un fault machine ; le jeu proprement dit
n'est pas atteint.

## Limites

Les instantanés repris au milieu d'une trame reprennent à la ligne enregistrée,
sans rejouer le début de trame ni l'événement V-int de cette ligne. Une copie
du CPU m0boy adaptée est produite au build ; si sa structure change, CMake
refuse la construction au lieu d'appliquer silencieusement une adaptation
partielle. Une fixture compare l'état repris à l'exécution continue.

La carte dynamique marque les octets effectivement lus comme instruction par
le cœur, y compris les extensions lorsque le branchement est pris ; le rapport
compte les désaccords de longueur avec le décodeur statique. Les instantanés d'IRQ dont
le PC de reprise est hors ROM ne marquent pas d'octet dans la carte ROM.
Le compteur de visites accepte toute la plage de `--visit-cap` sans reboucler
à 256 ; il repart à zéro après chaque trame complète afin de ne pas couper
les attentes normales entre trames. Sa saturation reste signalée ; les
budgets par chemin et global bornent les boucles qui traversent des trames.
Les lectures CPU naturelles sont distinguées des lectures forcées. Les sources
ROM des commandes DMA ont un drapeau séparé : le bus DMA contourne le hook CPU
et une faute peut interrompre le transfert, donc ce sont des plages commandées,
pas une preuve que tous leurs octets ont été effectivement lus.
Un hook distinct sur le bus DMA de la copie du runtime construite par deepeel
marque aussi les mots ROM effectivement lus avant une éventuelle faute ; ces
octets et les plages commandées ont des drapeaux et comptes distincts.
Lorsque la SRAM est activée, les accès et l'exécution dans sa fenêtre sont
comptés séparément et ne marquent pas les octets ROM superposés.
Les `MOVE.W` qui lisent directement un mot ROM et l'écrivent au port VDP en
mode VRAM marquent séparément leur source, après contrôle de la valeur ; les
comptes distinguent mots copiés et octets source distincts. Les copies passant
par un registre sont reconnues lorsque le chargement ROM et l'écriture VRAM
sont deux `MOVE.W` consécutifs ; toute instruction intermédiaire ou interruption
interrompt ce suivi. Les copies transformant les données ne sont pas reliées.
Le rapport liste jusqu'à 64 sites de lecture ROM, leur intervalle d'adresses
et leurs nombres d'accès naturels et totaux ; les accès refusés sont comptés.
Un intervalle est une enveloppe, pas la preuve que tous ses octets ont été lus.
Sur Sonic 1, la boucle de checksum en `$32C` lit presque toute la cartouche :
cette lecture d'intégrité ne prouve pas l'utilisation du contenu par le jeu.
Le rapport donne l'union des octets exécutés, lus par le CPU ou le DMA, ou
copiés en VRAM, puis sépare les octets seulement lus par le CPU. La première
union inclut donc les checksums ; la seconde regroupe code et sources VRAM
observées. Aucune de ces deux valeurs n'est une couverture de jeu joué.
`--exclude-read-pc 0x32c` affiche en plus une union sans les lectures CPU issues
de ce site ; les autres accès au même octet restent comptés. La carte brute et
les comptes non filtrés sont conservés. Le choix du site est explicite et
doit être vérifié pour chaque révision de ROM.
Sur une passe bornée, le témoin de la faute fréquente en `$02F588` saute via
`a3` chargé en `$168E` depuis `$FFF6E0`. Le code écrit aussi cette case avec
`a3` en `$1628` et `$16C2` ; la routine `$16DC` décale des mots longs dans
la structure qui la contient. L'origine exacte de la valeur `$02F582` n'est
pas encore tracée : le seul désassemblage ne prouve pas quelle écriture a eu
lieu sur le chemin témoin. Ces observations décrivent la machine de l'outil.
Une cible déjà exécutée ne suffit pas à évincer une fourche : elle peut être
atteinte avec un autre contexte machine. Les fourches restent bornées par site,
avec saturation rapportée ; il n'y a pas encore de déduplication par contexte.
Sur une passe Sonic de 20 millions de pas, un essai de cache comparant les 64
derniers instantanés octet pour octet n'a trouvé aucun doublon ; les horloges
diffèrent notamment. Cet essai a été retiré : il faut une comparaison de
contexte tenant compte des composantes effectivement pertinentes.
Pour une fourche Bcc, le CCR de l'instantané est ajusté avec le minimum de bits
modifiés afin que la condition corresponde à l'issue forcée ; les instructions
suivantes qui relisent ces drapeaux ne voient plus l'issue opposée.
Les fourches DBcc reconstruisent aussi le compteur à partir de sa valeur avant
l'instruction : sortie par condition vraie sans décrément, ou branchement avec
décrément. DBF sort sur `$FFFF` ; DBT ne peut pas brancher et n'est pas fourché.
Les sondes d'interruption attendent que le masque CPU autorise leur niveau ;
une file pleine ne consomme plus définitivement la sonde, qui peut être
retentée lorsque de la place se libère, y compris pendant l'exécution en RAM.
Chaque site ROM de lecture de la manette 1 peut aussi créer quatre instantanés
bornés (Start, A, droite, A+droite). La touche est appliquée après l'instruction
de lecture : elle agit sur les lectures suivantes, typiquement au prochain tour
de polling, et non sur la valeur déjà lue. Le rapport compte ces sondes et les
refus de file ; les autres combinaisons ne sont pas encore explorées.
Les fautes machine sont regroupées par code et instruction, avec le PC fourni
par le cœur et les comptes. Un second journal donne le site de la dernière
fourche ; chacun est borné à 64 entrées et son débordement est compté.
Pour chaque site de faute, le rapport conserve aussi les huit dernières
instructions d'un chemin témoin ; ce n'est pas une trace de toutes les fautes.
Il indique également les registres d'adresse de ce témoin et la dernière
instruction qui a changé leur valeur, provenance conservée dans les fourches.
`--watch-write 0xFFF6E0` ajoute au témoin la dernière écriture CPU qui touche
cette adresse ; son origine suit les instantanés. Il s'agit d'une observation
par chemin, limitée à l'adresse exacte choisie, pas d'un suivi complet de RAM.
Les valeurs D0, A1 et A2 au moment du hook d'écriture sont aussi indiquées.
Codes du cœur actuel : 1 accès bus
inconnu, 2 opérande invalide, 3 accès mot impair, 4 opération VDP non prise en
charge, 6 limite interne d'instructions.

Sondes de manette limitées à quatre combinaisons sur la manette 1 (voir
plus haut), pas de fermeture statique (`jmp (An)`, tables de sauts), Z80 du
MD non exploré, pas de GUI ni de sessions jouées ; TAS limité aux films GMV
(pas de BK2) ; validation DMA en direct non vérifiée.

## Modifications côté m0boy

Hooks `MD_TRACE` commités dans m0boy (`md_trace.h`, `md_m68k_ops.h`,
`md_full_cpu.c`, commit 15b30f3, non poussé).

## Tests

`tests/test_md.c` (jalon 1) et `tests/test_md_explore.c` (fourches Bcc,
DBcc, boucle bornée, V-int, DMA), fixtures générées au build.

## 0.187.6 — `md explore --time-limit SEC`

Arrêt gracieux au mur d'horloge (alarme → drapeau vérifié par la boucle principale ; le rapport est écrit, comme
pour un mur de pas). Vérifié sur Sonic : `--time-limit 30` s'arrête à 30,6 s réelles, 62,5 M pas. Vérifié aussi avec `--jobs 2` (21 s réelles pour 20 demandées, 42,5 M pas ; temps utilisateur ≈ temps réel, donc un seul cœur : `--jobs 2` partage le travail par `--entry` (`jobs.c`) et retombe sur une instance, avec un message sur stderr, s'il y a moins de deux `--entry` ; sans `--entry`, il n'y a rien à répartir). `--unbounded` est ajouté en 0.187.8 (ci-dessous).

## 0.187.8 — `md explore --unbounded`

Exception décidée par le propriétaire (cf. `AGENTS.md`) : lève les plafonds de pas total, de pas par chemin, de forks
par site et de visites par instruction ; la file en RAM reste modeste (`--queue-max` ≥ 4096 par défaut) et l'excédent
va dans un lot disque `<texte>.lot` (ou `--lot`), repris par un run ultérieur. Le seul arrêt est l'épuisement,
`--time-limit` ou SIGINT. Le pruning « les deux côtés couverts » est conservé.

Pour que l'arrêt soit honoré sans borne de pas : l'exécution est découpée en tranches de 2^24 pas (`run_sliced`) et
`push` refuse tout nouveau travail une fois l'arrêt demandé. Vérifié sur Sonic : `--unbounded --time-limit 30` rend la
main à 43 s réelles (rc 0, rapport écrit) ; 16,7 M pas, 65 024 forks écrits au lot. Observation de l'outil, pas une
mesure de matériel. Attention : le lot grossit vite (≈ 3,4 Go en quelques minutes sur Sonic) — surveiller le disque.
Non vérifié : un run `--unbounded` jusqu'à épuisement.

## 0.249.82 — sections vivantes (C1 liveness)

Les sections (courses exécutées maximales) sont désormais construites en
direct pendant l'exploration et portent leur **liveness matérielle** : les
blocs que la section lit ou écrit réellement (video, sound, input, bank/mapper,
sram, sys), dérivés des accès registres attribués à l'instruction courante.
Le rapport liste les 64 premières sections avec leur verdict (required/forced)
et leurs blocs vivants ; les suivantes sont comptées. Avec `--jobs 2`, les
sections du secondaire rejoignent celles du primaire (union des courses, hits
au plus grand compte, liveness OU-ées) et toutes sont triées en ordre
canonique (premier octet) au moment de la finalisation. Test
`md_section_liveness`. Ces liveness préparent l'éviction par contexte C2 sur
le modèle GB (projection de l'état sur ce que la section lit réellement).

## 0.249.84 — C2 par liveness de section : `--ctx-live` (mesuré, rejeté par défaut)

`--ctx-cap N --ctx-live` remplace la clé grossière (pc, D0-D7, A0-A7, SR) par
l'identifiant de la section cible et l'état machine **projeté sur les blocs
que cette section touche** (`dp_mdmach_state_ctx_hash_live` : registres et SR
toujours ; VDP, son, bank/SRAM, interruptions selon la liveness apprise).
Quand la liveness d'une section grandit, son haché change et les admissions
se rouvrent sans réinitialiser l'ensemble vu.

Mesure (Sonic the Hedgehog, 3 M pas, cap 1) : la clé projetée **réduit le
travail de moitié mais aussi la couverture de moitié** (261 571 pas, 493
courses, 5 184 octets exécutés contre 410 513 pas, 932 courses, 10 794
octets sans éviction ; la clé grossière garde les 10 794 octets avec 419 761
pas). Cause : la projection ne porte ni la RAM de travail ni la VRAM (64 Kio
chacune, uniques à quasi chaque push — mesuré sur la fixture `md_ctx_cap` :
15 courses au lieu de 2) ni la manette (la différence même des sondes D10 :
chaque génération de boucle de lecture se ré-admet). Les états évincés
différaient donc par des composantes non modélisées que la section lit
réellement — même échec que le C2 NES par fenêtres 16 Kio. Le défaut reste
la clé grossière ; reprise quand les sections porteront leurs pages de RAM
lues (sémantique GB). Test `md_ctx_live` (la clé projetée évince et le
rapport dit « section-projected »).

## 0.249.92 — frontière partagée (§6) : `--defer-tiers` (couverture en hausse)

Comme NES et PCE (0.249.90-91) : l'admission des continuations coupées passe
par la politique partagée `src/explore/frontier.c`. Une course dont le
budget propre (`--path-steps`) s'épuise — sans mur global ni SIGINT — garde
son état machine (registres, writers, watch) et reprend avec un budget ×4,
jusqu'à `--defer-tiers N` (0 : coupes finales, défaut). Les fourches
héritent du palier de leur parent, les restes vont au lot disque, la raison
reste ouverte (issue `deferred` dans l'arbre). Mesure (fixture `fix_md_pad`,
`--path-steps 50`) : 28 continuations reportées et toutes reprises, 57
courses contre 29 sans déferral au même budget. Rapport : `frontier:
deferred steps/loop; resumed; final; dropped`. Test `md_defer_tiers`.

## Les 10 octets de WRAM d'écart avec ClownMDEmu (observation, 6000 trames Sonic 1)

`DP_MD_DIFF_LIST=1 sh tools/md_compare_reference.sh ROM s6k.gmv 6000` liste les adresses qui diffèrent (aux trames multiples de 3000). Observations de l'outil : trame 3000, 2 octets (`$F606` ref 00 / m0boy FF, `$FD81`) ; trame 6000, 10 octets : `$F606` plus `$FD7E–$FD85`, `$FD8C–$FD8D`. La zone `$FD7E–$FD8D` est la pile 68000 sous le pointeur de pile, que le V-int et sa trame d'exception réécrivent à chaque trame ; les deux cœurs sont dans la boucle d'attente du V-int (`pc` 0x29AC / 0x29B0 selon l'instant d'échantillonnage) et la valeur résiduelle dépend de l'instruction coupée par l'interruption. Mode de jeu, zone, X du joueur ($08F4), et toutes les variables de jeu sont égaux ; `fault=0`. Ce n'est donc pas un écart d'état de jeu mais un résidu de pile dépendant de la phase d'interruption au cycle près ; `$F606` reste à expliquer. Pas une mesure de matériel : une comparaison entre deux cœurs.

Suite : `$F606` est expliqué et corrigé. C'est `v_jpadhold2` de Sonic 1 (manette 2 maintenue). m0boy renvoyait 0 pour une lecture de `$A10005` (port 2, aucun cas dans `md_full_bus.c`), le jeu calculait `~0 = FF` ; ClownMDEmu lit « aucun bouton » et le jeu obtient 00. m0boy renvoie désormais `0x7F` (manette absente, boutons relâchés). Observation de l'outil, 6000 trames : 10 octets → 9, tous dans la pile 68000 (`$FD7E–$FD8D`), une zone sous le pointeur de pile : les octets de la référence apparaissent décalés de 4 dans m0boy (`ff fb` en `$FD7E` contre `$FD82`), soit une profondeur de pile différente d'un mot long à l'instant de l'échantillon, donc de la mémoire morte, pas de l'état de jeu.

Correction : l'explication « profondeur de pile différente à l'instant de l'échantillon » était fausse. `DP_MD_DIFF_LIST=1` affiche aussi A7 : `$00FFFDF8` des deux côtés aux trames 3000 et 6000. Les 9 octets (`$FD7E–$FD8D`) sont à plus de 0x60 octets sous le pointeur de pile : de la pile morte, résidu d'un appel plus profond d'une trame antérieure (contenu non nettoyé), jamais relu par le jeu tant que A7 ne redescend pas jusque-là. Ce n'est pas démontré qu'aucune lecture ne l'atteint ; l'état de jeu observé (mode, zone, X, variables) est égal. Origine exacte du résidu (quelle trame, quelle donnée) non déterminée.

Origine observée (`DP_MD_STACK_DIFF=1`) : les premières divergences de pile sont aux trames 55–59 et 93–97, à `$FDF6/$FDF7`, juste au-dessus de A7 : ce sont les octets de l'adresse de retour que l'interruption empile. Ils valent `ac` côté ClownMDEmu et `b0` côté m0boy (ou l'inverse) : le V-int coupe la boucle d'attente (`$29AC`/`$29B0`) à une instruction d'écart. Le résidu plus profond (`$FD7E–$FD8D`) est cette même différence de phase, propagée lors d'appels plus profonds. Donc : écart de phase d'interruption au niveau de l'instruction, pas d'état de jeu ; l'aligner au cycle près demande l'ordonnancement exact 68000/VDP de ClownMDEmu, non fait. Observation de l'outil sur Sonic 1, pas une mesure matérielle.

Essai négatif : `DP_MD_PENALTY` (cycles ajoutés à la trame 52) à 4, 8, 12, 16 et 24 ne réduit pas l'écart (6000 trames : 9 octets dans tous les cas ; 24 ajoute des octets à la trame 3000). Un décalage constant de l'horloge ne suffit donc pas ; le résidu de phase du V-int demande l'ordonnancement instruction par instruction de ClownMDEmu.

Octets profonds (`DP_MD_DEEP=1 DP_MD_STACK_DIFF=1`, fenêtre `$FD7E–$FD8D`) : première divergence à la trame 673, A7 égal (`$FFFDB8`), mais PC différents à l'échantillon (`$1812` contre `$17DC`, puis `$17DC`/`$1846`…) : le V-int tombe encore à des instructions différentes dans une autre routine (`$17DC–$1898`), et les octets empilés (adresse de retour, SR) diffèrent. Cela vérifie l'hypothèse pour les octets profonds : même mécanisme (phase d'interruption), pas un état de jeu. Reste non fait : l'ordonnancement instruction par instruction de ClownMDEmu.

Lecture comparée de l'ordonnancement du V-int (lecture de source, pas de modification) : ClownMDEmu lève le V-int à la fin de la ligne 223, après `SyncM68k` jusqu'au cycle maître de la ligne ; le 68000 s'exécute avec un budget de cycles et l'interruption est prise à la frontière d'instruction suivante. m0boy lève `irq_pending` au début de la tranche de la ligne 0 (`md_full_frame`) avec des tranches `(ligne+1)*3420/7`. Les deux frontières coïncident en principe ; la dérive de phase observée (trame 673, routine `$17DC–$1898`) vient donc plutôt d'un écart de durée d'une ou plusieurs instructions entre `md_m68k_time.h` et Clown68000, accumulé sur les trames. Piste concrète non faite : tracer les cycles par instruction des deux côtés sur les trames 660–673 et chercher la première instruction dont la durée diffère.

Sonde `DP_MD_PC_DIFF=1` (PC et SR des deux cœurs en fin de trame) : dès la trame 1 les PC diffèrent d'une instruction (`$0264/$0262`, `$0330/$032E`, `$032C/$0330`…) dans les boucles d'attente de l'initialisation, bien avant toute logique de jeu. Cela corrige la piste précédente : ce n'est pas (d'abord) une instruction à durée fausse apparue à la trame 673, c'est un décalage d'une instruction à la frontière de trame, présent depuis le début, que les boucles d'attente absorbent. Il devient visible dans la pile quand le V-int tombe sur des instructions différentes (trames 55, 93, 673…). La cause à traiter est donc la sémantique de la frontière de budget (l'instruction qui dépasse le budget de cycles et le report du dépassement) entre `run_slice` de m0boy et `SyncM68k`/Clown68000, pas la table de durées. Non fait : aligner cette sémantique.

Correction trouvée (m0boy, `md_full_cpu.c`) : Clown68000 verrouille le niveau d'interruption en attente AVANT l'instruction et l'entre APRÈS elle (une instruction de latence, commentaire « Sesame Street » dans sa source). m0boy testait l'interruption avant chaque instruction. `run_slice` reproduit maintenant la latence de la référence (voie interpréteur ; `MD_IRQ_IMMEDIATE=1` rétablit l'ancien comportement). Observation de l'outil sur Sonic 1, 6000 trames : l'écart WRAM passe de 9 à 3 octets (`$FD81`, `$FD8C`, `$FD8D`, toujours sous A7, A7 égal) et les PC des deux cœurs coïncident à la trame 6000 (avant : `$29AC/$29B0`). ctest 258/258. Restent 3 octets de pile morte ; leur origine n'est pas encore tracée (sonde `DP_MD_STACK_DIFF` avec `DP_MD_DEEP`).

Après la latence d'interruption : les 3 octets restants (`$FD81`, `$FD8C`, `$FD8D`) divergent à partir de la trame 673 (A7 égal `$FFFDB8`, PC `$1812` contre `$17DC`). `$17DC–$1820` est une routine de manipulation de bits (`add.b d1,d1` / `bcc` / `subq.w #1,d6` / `btst d6,d5` / `beq` / `ori.w`/`addi.w #…,d3`). Les durées de `btst Dn,Dn` (6 cycles dans `md_m68k_time.h` ; 4 + 2 pour un opérande long dans Clown68000) paraissent égales à la lecture ; l'écart de durée éventuel n'est pas localisé. Piste restante : une trace cycles-par-instruction côté Clown68000 (le cœur de référence n'expose pas ce compteur à l'extérieur ; il faudrait l'instrumenter dans une copie).

Trace cycles/instruction (copie instrumentée de Clown68000 hors du dépôt de référence, `DP_CYCLE_TRACE` dans l'outil, `DP_MD_CYCLE_TRACE=<trame>`) : sur la trame 673, les séquences de PC et les durées par instruction sont identiques des deux côtés jusqu'à la fin de la trame 672 (13837 instructions communes, aucune durée différente). Le V-int de la trame 673 ne tombe pas au même endroit de la routine `$17E8–$1842` (rte vers `$17F8` côté référence, `$1842` côté m0boy) : la trame 672 s'arrête à `$17F4` côté référence et à `$183C` côté m0boy, soit ~28 instructions (~200 cycles) plus tard. La table de durées n'est donc PAS en cause ; ce qui diffère est la longueur de la trame de la référence (60 Hz : 53693175/60 + /60000 par trame, ~3419 horloges maîtres par ligne) contre 3420 (59,92 Hz) pour m0boy. Essai `DP_MD_CLOWN_CLOCK=1` (m0boy à 3419 par ligne, option ajoutée, désactivée par défaut) : plus mauvais (trame 6000 : X joueur `08f4`/`093b`, 2244 octets de WRAM) ; la phase de la trame (V-int au début de la tranche vs. à la ligne 224 de la référence) doit être alignée avec la longueur, pas seulement la longueur. Non résolu : il reste 3 octets de pile morte (`$FD81`, `$FD8C`, `$FD8D`).
