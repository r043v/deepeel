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
  des changements de banque Z80 (la passe linéaire journalise, elle, toutes
  les banques ; l'explorateur à fourches ne trace que les banques Z80).

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
refusés, pas silencieusement tronqués. Le format BK2 est pris en charge
(`src/platform/md/movie.c`, identité MD5 du film vérifiée avant rejeu).
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
avec saturation rapportée ; la déduplication par contexte existe mais reste
opt-in (`--ctx-cap`, `--ctx-live`, dédup exacte `--exact-dedup`), sans objet
en défaut sur les passes courantes.
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
Chaque site ROM de lecture de la manette 1 peut aussi créer des instantanés
bornés : 14 masques couvrant les boutons 3 et 6 boutons. La touche est appliquée après l'instruction
de lecture : elle agit sur les lectures suivantes, typiquement au prochain tour
de polling, et non sur la valeur déjà lue. Le rapport compte ces sondes et les
refus de file.
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

Sondes de manette limitées à la manette 1 (14 masques, voir plus haut), pas
de GUI ; les limites historiques de cette entrée sont levées : fermeture
statique avec tables `jmp (An)` (S4) et second passage, Z80 du MD exploré
(copies et exécution en Z80 RAM), sessions jouées avec pool de checkpoints,
TAS aux formats GMV et BK2 ; la validation DMA en direct reste non vérifiée.

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

Essai complémentaire : `DP_MD_CLOWN_CLOCK=1` avec la même longueur de ligne dans le compteur HV / la ligne VDP / le délai DMA de `md_full_bus.c` (et plus seulement dans la durée des tranches) donne le même résultat négatif (trame 6000 : X joueur `08f4`/`093b`, 2244 octets de WRAM). La période V-int→V-int de la référence est 895780 horloges maîtres (3418/3419 selon la ligne : deux demi-lignes de `cycles_per_scanline/2` pour les lignes actives, synchronisation finale sur la trame) ; reproduire cette période seule ne suffit pas. Défaut inchangé (3420), 3 octets de pile morte toujours différents.

Balayage de la longueur de ligne (`DP_MD_CLOWN_CLOCK=<n>`, trame 3000, octets WRAM différents) : 3414 → 1, 3416 → 10, 3418 → 2, 3419 → 8, 3420 (défaut) → 1, 3421 → 17, 3422 → 18, 3424 → 19. Aucune valeur ne donne 0 : la longueur de ligne seule n'explique pas le dernier octet (trame 3000) ni les 3 de la trame 6000. 3420 reste le meilleur compromis avec 3414.

Comptabilité exacte de la référence (`DP_MD_CLOWN_CLOCK=exact`, option de m0boy, désactivée par défaut) : lecture de `Sync_Update`/`SyncM68kCallbackIterate` — la référence compte en horloges maîtres, arrondit chaque synchronisation au cycle 68000 supérieur et reporte le dépassement (`cycles_available`) d'une trame à l'autre ; trame de 895780 horloges maîtres (`53693175/60 + 53693175/60000`), lignes de blanking à 3419 et lignes actives à 3418 (deux demi-lignes de `3419/2`), synchronisation finale sur la trame. `md_line_end`/`md_frame_wrap` reproduisent cela (le 68000 tourne tant que `7·t + report < cible`). Résultat : toujours négatif (trame 6000 : X joueur `08f4`/`093b`, 2239 octets) ; le défaut 3420 reste le plus proche. La phase de démarrage (marqueur d'initialisation, `DP_MD_PENALTY` trame 52) a été calibrée sur l'ancienne horloge et n'est pas réajustée pour ce mode : l'essai n'est donc pas concluant sur la période elle-même.

Recalage du démarrage du mode `exact` (`DP_MD_PENALTY` 0, 10, 20, 40, 80, 160, 320, 640 ; trame 3000, octets WRAM différents) : 6, 9, 11, 11, 11, 11, 11, 9. Aucun réglage ne descend sous 6, contre 1 pour le défaut 3420. Le mode `exact` n'est donc pas une amélioration, même recalé ; il reste une option d'investigation.

Lecture fine du mode `exact` (`DP_MD_STACK_DIFF`, trames 0–3000) : l'écart de pile se réduit à un octet, `$FDF7` (octet bas de l'adresse de retour du V-int), `b0` côté référence contre `ac` côté m0boy, SP égaux. Le V-int interrompt la boucle d'attente `$29AC–$29B0` à une phase différente de 4 octets (un instruction de la boucle) ; la logique de jeu ne diverge pas. Les 6 à 11 octets du balayage `DP_MD_PENALTY` venaient de cette phase d'interruption répétée, pas d'un écart d'exécution. Le défaut (3420) ne diffère, lui, que dans la routine `$17E8–$1842` à partir de la trame 673. Les deux modes ne diffèrent donc que par la phase exacte où l'interruption tombe, à l'intérieur d'une instruction ou d'une boucle ; le décalage est dans le cumul sous-instruction de l'horloge, qu'aucun réglage de ligne/pénalité ne reproduit.

Balayage de la phase sous-cycle du mode `exact` (`DP_MD_CLOCK_OFF` 0 à 6, trame 3000, octets WRAM différents) : 6, 6, 11, 8, 11, 3, 11. Aucune valeur n'atteint 0 ; le meilleur (5) donne 3, comme le défaut à la trame 6000. Le décalage n'est donc pas une simple phase initiale en horloges maîtres.

Blocage DMA : arrondi de la référence (`DP_MD_DMA_REFROUND=1`, option de m0boy, désactivée par défaut). La référence compte le gel du 68000 en horloges maîtres (`cycles_per_scanline * lectures / débit`) puis arrondit au cycle 68000 supérieur ; m0boy calculait `(ligne * lectures / 7) / débit` au plancher. Avec l'arrondi de la référence, la WRAM de m0boy est identique à ClownMDEmu de la trame 60 à la trame 3300 (`diff=0` aux trames 900, 1500, 3000 ; les 3 octets de pile morte dès la trame 673 disparaissent), mais une divergence de jeu apparaît ensuite : 1 octet de pile à la trame 3600, puis une table de 4 en 4 octets en `$CB31…` et X du joueur `02bf`/`02b9` à la trame 3714 (642 octets), 2235 octets à la trame 6000. Le défaut reste donc plus proche sur 6000 trames (3 octets). La divergence à partir de ~3650 n'est pas encore expliquée (hypothèses : autre durée de gel DMA, test « vblank » : `DP_MD_DMA_VBLANK37=1` n'y change rien). Piste suivante : la première trame divergente entre 3600 et 3714.

Référence Gens-rr (v11c, sous wine) : `tools/gens_dump.lua` rejoue le film et écrit les 64 Kio de WRAM après chaque trame (`wine Gens.exe ROM.md -lua gens_dump.lua`) ; `DP_MD_GENS_DUMP=wram.bin [DP_MD_GENS_SKIP=N]` les compare dans `md_compare_reference`. Sonic 1, `s6k.gmv` : écart de 1 à 2 octets aux trames 120–420, puis des centaines d'octets (628 à la trame 1200, ~3500 à 6000), identiques à 2 octets près pour ClownMDEmu et m0boy — Gens est une troisième horloge (488 cycles/ligne), les deux autres s'accordent entre eux. Observation de l'outil, pas mesure matérielle.

### Re-mesure Gens-rr (13 novembre 2026) : film 1843S (Sonic 1, GMV 6 boutons)

`tools/tasvideos_fetch.py` a re-téléchargé les 27 films Sonic 1 (`tas/sonic1-refetch`) : les liens de
`tas/games/sonic-md-all` étaient cassés. `md_compare_reference` acceptait seulement les GMV à 2 manettes 3
boutons (`33`) ; il accepte maintenant `3` ou `6` par port (les films « 66 » dont les octets d'entrée sont
`FF FF` en dehors du premier). Observations de l'outil (pas des mesures matérielles), ROM USA/Europe, 3000 trames :

- ClownMDEmu et m0boy s'accordent à 1–2 octets près jusqu'à la trame ~1466 ; à cet endroit la position X du
  joueur diverge entre eux (« player X differs for 20 frames »), puis ClownMDEmu reste plus proche de Gens-rr
  que m0boy (trame 1560 : 645 contre 2123 octets d'écart ; trame 1800 : 486 contre 3085).
- Contre Gens-rr (référence des GMV, sous wine, `tools/gens_dump.lua`) les deux divergent dès la trame ~540
  (53 octets) puis de centaines d'octets (840 : 579) : Gens a sa propre horloge (488 cycles/ligne), connue.
- La première divergence propre à m0boy est donc ≈ trame 1466 (hors piles `$FDF6/7`, déjà différentes dès la
  trame 54) ; sa cause n'est pas encore isolée. Les autres films incomplets du tableau (`3994S`, `1437S`,
  `5906S`, …) restent à mesurer par la même chaîne.

Isolation de la divergence 1843S (`DP_MD_FIRSTLOW=1`, RAM des objets `$D000–$EFFF`) : premier écart réel à la
trame 1423 (position X du joueur, `$D008/9`) ; ensuite m0boy vaut exactement la valeur de ClownMDEmu de la
trame précédente (X : 0f→0e, 0b→05, 11→0b, 17→11, pas de 6). C'est un retard d'une trame (une trame de lag
manquée ou ajoutée côté m0boy), pas une erreur de logique de jeu. Prochaine étape : tracer la trame 1422–1423
(`DP_MD_TRACE_RANGE=1420:1424`) pour trouver quel événement (fin de vblank, lecture manette) décale la boucle principale.

Trace `DP_MD_TRACE_RANGE=1420:1424` (1843S) : trames 1420–1422 identiques ; à 1423 ClownMDEmu avance le joueur
(X `0eff`→`0f05`) tandis que m0boy ne bouge pas (`status` reste 08) : la boucle de jeu n'a pas tourné dans cette
trame côté m0boy. Même entrée manette (`pad=18`). Cause probable : une trame de lag due à un écart de cycles
68000 à la limite de la trame (boucle principale finie ou non avant le vblank). À borner par comparaison des
compteurs de cycles par trame entre les deux émulateurs.

Autres films (même chaîne, `DP_MD_FIRSTLOW=1`, ROM USA/Europe) : `1437S` diverge à la même trame 1423 que `1843S`,
avec les mêmes valeurs (X `0f`/`0e`) : l'événement est déterministe et dépend du jeu, pas d'un bruit de film.
`3994S` reste identique à ClownMDEmu jusqu'à la trame 29275 (premier écart d'objets à 1481, 2 octets). `5906S`
n'a pas fini dans le temps imparti. Ces nombres sont des observations de l'outil.

`5906S` : pas de divergence soutenue de la position du joueur ; deux écarts d'objets isolés (trame 4076 : 11 octets
`$D841–$D873` ; trame 10497 : 3 octets `$D009–$D00D`). Observation de l'outil.

Balayage `DP_MD_PENALTY` (1843S) : 0, +400, +1200 cycles laissent l'écart à la trame 1423 inchangé ; −400 et −1200 le
ramènent à la trame 336 (mauvais). Le lag de 1423 n'est donc pas un décalage de phase initial : c'est le coût en
cycles d'une trame précise (GHZ1) qui fait dépasser le vblank à m0boy mais pas à ClownMDEmu. Piste : comparer
les cycles par instruction 68000 (accès bus, MULU/DIVU) sur la trame 1422 avec `DP_MD_TRACE_RANGE`.

État CPU en fin de trame (`DP_MD_ENDSTATE=1 DP_MD_TRACE_RANGE=1419:1424`, 1843S) : à la fin de la trame 1422,
ClownMDEmu est déjà dans la boucle d'attente du vblank (`pc=$29B0`, `a7=$FFFDF8`), m0boy encore dans la boucle de jeu
(`pc=$3B34`, `a7=$FFFDFC`, `SR=$2309`) : `$3B34` est le dernier `bsr` de la boucle principale, à quelques
instructions du `cmpi.b #8,($F600).w` et du retour à l'attente. Le vblank arrive donc à m0boy avec la logique de la trame
inachevée : la trame 1423 est « perdue » (le joueur n'avance pas), d'où le retard d'une trame. L'écart est de quelques
dizaines de cycles 68000 sur une trame chargée de GHZ1 ; il ne dépend pas de la phase initiale (balayage
`DP_MD_PENALTY`). Piste suivante : sommer les cycles par routine de la boucle (jsr/bsr de `$3B10–$3B40`) des deux
côtés ; le cœur de référence n'expose pas de compteur, il faut donc un hook de cycles dans ClownMDEmu.

### Horloge de référence : la cause du lag de la trame 1423 (13 novembre 2026)

Balayage des modèles de timing de m0boy contre ClownMDEmu (`1843S`, `1437S`, `s6k`) : aucune bascule DMA
(`DP_MD_DMA_REFROUND`, `DP_MD_DMA_VBLANK37`) ne bouge l'écart ; en revanche l'**horloge** le fait.

| réglage m0boy | `s6k` (6000) | `1843S` | `1437S` |
|---|---|---|---|
| défaut (ligne de 3420 horloges) | identique | écart X à 1423 | écart X à 1423 |
| `DP_MD_CLOWN_CLOCK=exact` | écart X à 5936 | identique 13 500 trames | identique 13 800 |
| `exact` + `DP_MD_CLOCK_OFF=3` | identique | identique 13 500 | identique 13 800 |

L'horloge `exact` à phase 3 suit ClownMDEmu sur les trois films (X du joueur égal, 5 octets d'écart de WRAM) ;
`MD_IRQ_IMMEDIATE=1` donne un résultat voisin sur deux films (non retenu : latence d'IRQ de la référence).
`md_compare_reference` la prend désormais par défaut (`setenv(…, 0)`, surchargeable par l'environnement). Avec ce
réglage : `5906S` 39 900 trames sans écart de X (25 octets), `3994S` garde sa divergence à 29275 (autre cause).
**Non fait** : la changer dans les défauts de m0boy lui-même (dépôt externe, accord nécessaire) et donc dans
`deepeel md` ; les films GMV réels ne rejouent pas encore tous avec l'horloge par défaut de la plateforme.
Observations de l'outil, pas des mesures matérielles.

`3994S` avec l'horloge `exact` : premier écart de RAM d'objets à la trame 1479 (`$DB01`, bit 7 d'état d'objet : `b5`
contre `35`) quelle que soit la phase `DP_MD_CLOCK_OFF` 0–6 ; un écart plus large (X du joueur, 127 octets) à la trame
1497 pour les phases 3 et 4, repoussé après la trame 1499 pour les phases 0, 1, 2 et 6. Il n'existe donc pas de phase
unique qui tienne tous les films : `1843S`/`1437S` veulent 3, `3994S` préfère 0–2 et 6. Le modèle de timing
sous-cycle de m0boy garde un défaut résiduel (l'ordre exact des accès bus/DMA dans la trame) ; le réglage par
défaut de l'outil n'est qu'un compromis mesuré sur trois films. Observation de l'outil.

**0.320.0 : horloge `exact` phase 3 par défaut dans `deepeel md`** (`dp_mdmach_reset` : `setenv(…, 0)` avant
`md_full_reset`, sans modifier m0boy ; `DP_MD_CLOWN_CLOCK` et `DP_MD_CLOCK_OFF` du processus l'emportent). Les 69 tests
`md|sega|…` restent verts. Mesure : voir le tableau ci-dessus ; `3994S` garde son écart à 1479/1497.

`3994S`, états de fin de trame (`DP_MD_ENDSTATE`, horloge `exact` phase 3) : trame 1479, les deux sont en milieu de
routine (`$D70A` référence, `$D7C8` m0boy, m0boy *plus avancé*), revenus à l'attente à la trame 1480 sans conséquence ;
trame 1496, la référence est à l'attente (`$29A8`, compteur de trame avancé) alors que m0boy est encore en
`$4206` : une trame perdue, comme à 1423 pour `1843S`. Le signe du décalage change d'une trame à l'autre : le coût
en cycles de certaines instructions ou séquences de bus de m0boy s'écarte de ClownMDEmu dans les deux sens, ce qui
exclut un réglage d'horloge unique et demande la sommation de cycles par routine (hook de cycles côté référence,
dépôt externe). Observation de l'outil.

**S2 : mesure du raffinement « callee à retour observé » (0.322.0)** sur Sonic 1 (`explore`, `--max-total 20000`) :
appels à confiance perdue 76 → 38, octets de code statiques 2 180 → 2 256 (+76). Observation de l'outil.

**3994S : trace de cycles par instruction (frame 1496), `DP_MD_REF_CYCLE=1`.** `tools/md_compare_reference.sh` construit
alors ClownMDEmu avec le crochet local `DEEPEEL_C68_TRACE` (un `#ifdef` dans `clown68000.c`, non publié, par défaut
inactif) et le harnais écrit `cyc_ref.txt` / `cyc_dut.txt` (`DP_MD_CYCLE_TRACE=<trame>` ; DUT par les crochets
`md_trace`). Sur la trame 1496 de `3994S`, les deux flux d'instructions sont identiques sur les 12 285 instructions
communes. Écarts de coût hors DMA : `addq.w #n,An` (`$5849`, `$5449`, `$584F`) coûte 10 cycles dans ClownMDEmu et 8 dans
m0boy (5 occurrences) ; tout le reste est égal. L'écart de trame vient du gel DMA : m0boy ajoute 3 830 cycles de blocage
aux écritures qui lancent un transfert (`move.w ($…).w,(a5)` : 169, 1 548, 2 161 cycles) alors que ClownMDEmu laisse
au 68000 66 cycles de plus (37 instructions de plus avant la fin de trame, dont `$4206`, l'instruction qui manque à m0boy
à la frame 1496). La correction est donc dans le coût DMA par mot de m0boy (`DP_MD_DMA_*`), pas dans l'horloge.
Observation de l'outil ; la vérité matérielle de `addq.w` sur An (8 ou 10 cycles) reste à trancher.

**3994S et `DP_MD_DMA_REFROUND=1`.** Avec l'arrondi de coût DMA de référence (horloge `exact` phase 3), `3994S` tient les
30 000 frames (X du joueur identique, 19 octets de RAM d'objets différents en fin de film) : le premier écart passe de
1479/1497 à 1506 (5 octets). Mais `1843S` et `1437S` divergent alors (frame 13 483 ; phases 0 et 1 testées : 3 500 octets),
alors qu'elles tenaient 13 500/13 800 frames sans cette option. Aucun réglage `DMA_REFROUND` × `CLOCK_OFF` testé ne tient
les trois films : le coût DMA par mot de m0boy est proche de la vérité sans l'être, la valeur manquante est entre les deux
modèles. Défaut inchangé. Observation de l'outil.

**Balayage de l'arrondi DMA (`DP_MD_DMA_ROUNDK=k`, m0boy local, non publié).** Coût = `(lm·mots/débit + k) / 7`. Pour k = 0…5,
`1843S` (13 500 frames) et `1437S` (13 800) tiennent (5 octets de RAM d'écart) mais `3994S` diverge (zone différente, 2 906
octets). Pour k = 6 (`DMA_REFROUND`), `3994S` tient 30 000 frames et les deux autres divergent. Aucun k entier ne tient les
trois : l'écart tient à un seul cycle sur un transfert précis, donc à un terme du modèle (phase d'horloge maître au lancement
du DMA ?) et non à l'arrondi. À suivre avec la trace `DP_MD_REF_CYCLE` sur les frames 13 483 (`1843S`). Observation de l'outil.

**Report du reste d'horloge maître entre transferts DMA (`DP_MD_DMA_CARRY=1`, m0boy local, non publié).** Même résultat que k = 0…5 :
`1843S` (13 500) et `1437S` (13 800) tiennent (5 octets), `3994S` diverge (2 906 octets). L'hypothèse « la référence
conserve le reste entre transferts » est donc écartée. Le transfert qui sépare `3994S` des deux autres reste à identifier
(source ROM ou RAM, VRAM ou CRAM, H32 ou H40). Observation de l'outil.

**Arrondi DMA par classe (`DP_MD_DMA_KACT` / `DP_MD_DMA_KBLANK`, m0boy local, non publié).** k = 6 seulement sur les transferts en
affichage actif, ou seulement en V-blank : `3994S` diverge dans les deux cas (2 906 octets), `1843S` et `1437S` tiennent.
`3994S` n'est donc fermé que si les deux classes sont arrondies au plafond. La séparation active / blanking n'explique pas
l'écart ; les classes restantes à tester sont la destination (VRAM, CRAM, VSRAM) et la largeur (H32 / H40). Observation de l'outil.

**Arrondi DMA par destination et largeur (`DP_MD_DMA_KCLASS`, m0boy local, non publié).** k = 6 limité aux écritures VRAM, aux
écritures hors VRAM (CRAM/VSRAM) ou au mode H40 : `3994S` diverge dans les trois cas (2 906 octets). Aucune classe isolée
(actif/blanking, VRAM/autre, H40) ne reproduit le comportement « tout au plafond » : l'écart de `3994S` n'est pas un arrondi par
type de transfert mais une combinaison, ou un terme de phase (position dans la ligne au lancement du DMA) que l'arrondi fixe
imite sans le modéliser. Piste suivante : modéliser la phase de la ligne au lancement. Observation de l'outil.

**Gel DMA en phase d'horloge exacte (m0boy, défaut avec `DP_MD_CLOWN_CLOCK=exact`).** La référence gèle le 68000 pendant
`floor(horloges maîtres)` et le reste sous-cycle décale la phase des instructions suivantes ; m0boy l'ajoute désormais à
`clock_off` au lieu de l'arrondir (`DP_MD_DMA_NOPHASE` rétablit l'ancien calcul). Mesure : `3994S` 30 000 frames, `1843S`
13 500 et `1437S` 13 800 tiennent ensemble (X du joueur identique ; 19 / 14 / 14 octets de RAM d'objets différents en fin
de film). Remplace les pistes `DMA_REFROUND` / arrondi par classe, toutes écartées plus haut. Observation de l'outil.

## Fourches d'interruption du noyau (0.342.0)

La fourche IRQ du noyau (`irq_alt`) offre désormais les **deux sources** : `delay` 1
synthétise la V-int à ce site (le jeu a les interruptions de trame armées, bit 5 du
registre VDP 1), `delay` 2 la **H-int** (bit 4, le compteur de lignes du registre VDP 10) —
le runtime lève les deux naturellement sur ses propres lignes, la fourche ajoute la
variante où l'interruption tombe ici ; le masque IPL (`SR >= 6`) vette tout. Les sources
sèment le handler dans des contextes que le flux naturel ne produit pas (l'interruption
entre deux instructions arbitraires du site), bornées par les caps de fourches existants.

## Index musical (0.340.0 — sans objet sur MD)

`--music-index` n'existe pas sur MD : aucun format rip de chip-musique 68000 n'est
porté par l'index DPMDX002 (gbs→GB, nsf→NES, hes→PCE, kss→MSX et Sega 8 bits, et le
kss ne couvre pas le 68000). La voie MD reste le replay TAS/moteur Z80 documenté
ailleurs ; ajouter un format MD (VGM avec table d'entry points) est une ouverture,
pas une promesse.

## Typage data (0.343.0 — côté noyau)

La connaissance de destination (DP_DEST_* : où la donnée lue en ROM atterrit — VDP
`$C00000-$C00003`, PSG `$C00011`, SRAM `$200000-$20FFFF`) est câblée dans la machine
(`DpMachine.data_dest`) et consommée par le noyau, dont la carte PNG teinte les
octets typés. Les moteurs legacy ne portent pas ce tableau : ils convergent vers le
noyau plutôt que de dupliquer la plomberie (`--core` a la carte typée).

### Gens-rr contre m0boy, avec décalage de trames (19 novembre 2026) : film 1843S, 3000 trames

Les écarts de « centaines d'octets » ci-dessus comparaient la trame N de Gens-rr à la trame N de m0boy. Or les deux ne
comptent pas les trames de la même façon au démarrage (la marque `init` de `$FFFC` apparaît dès la trame 52 côté m0boy,
plus tard côté Gens ; les compteurs `$F001`/`$FE0F` diffèrent de 1–2). En cherchant, pour chaque trame Gens, la trame
m0boy la plus proche parmi N−2…N+2 (RAM `$0000–$EFFF`, hors pile) : l'écart tombe à **0–12 octets** sur les 2700
trames (décalage −2 ou −1 selon le moment ; 0 octet aux trames 40–600, 1100 et 2100). Le reste (≤ 12 octets) est la
position et la vitesse du joueur (`$D009–$D015`) et un octet de variable (`$AD01`) : un écart de quelques pixels, pas
une divergence de logique de jeu. Observation de l'outil sur dumps Gens-rr, pas une mesure matérielle.

Conséquence : l'écart Gens/m0boy est un décalage de départ + de petites dérives de position, pas une horloge différente
sur toute la durée. Prochaine étape : fixer le décalage de départ (trames de boot de Gens) et traquer la dérive de X.

### Modèle DMA de Gens lu dans le source (`gens/src/gens/gens_core/vdp/vdp_io.asm`, 19 novembre 2026)

`Update_DMA` est appelée à chaque début de ligne (`gens.c` : `Cycles_M68K += CPL_M68K` puis `main68k_addCycles(Update_DMA())`)
avec `CPL_M68K = round(CLOCK_NTSC/7/60/262) = 488`. Table de mots par ligne (`DMA_Timing_Table`, H32 ; H40 : +1 colonne) :
bloc 68000→VRAM 83 (vblank) / 8 (affichage), 68000→CRAM/VSRAM 167 / 16, remplissage 166 / 15, copie 83 / 8 ;
H40 : 102/205/204/102 et 9/18/17/9. Le choix vblank/affichage est refait **à chaque ligne** (ligne ≥ lignes visibles ou
affichage coupé → débit vblank). Tant que le transfert dure, le 68000 est gelé pendant la ligne **entière** (488 cycles) ;
à la dernière ligne le gel est proportionnel (`CPL * restant / débit`, virgule fixe 16.16). Remplissage et copie
(`DMAT_Type` bit 1) ne gèlent jamais le 68000. Le gel démarre à la **ligne suivante**, pas à l'instruction qui lance le DMA.

Comparaison avec `vdp_dma` de m0boy : mêmes débits (m0boy double les lectures VRAM : 166/2 = 83, 204/2 = 102) à 1 mot près
(Gens 167/205 pour CRAM/VSRAM en vblank, m0boy 166/204), même durée totale à ≈ 0,1 % près (488 contre 488,57 cycles par
ligne). Différences réelles : (1) m0boy fige le débit à la ligne de départ du DMA, Gens le recalcule par ligne
(un DMA commencé à l'affichage et fini en vblank accélère chez Gens seulement) ; (2) le gel de Gens commence à la ligne
suivante. Elles n'expliquent pas à elles seules l'avance d'une trame de m0boy après chaque chargement (trames 293, 336,
419, 468 du film 1843S, compteur vblank `$FE0C` : écart 0→3), qui reste à isoler par trace de cycles côté Gens.

### Chaîne DSY1 Gens-rr ↔ m0boy, sans l'exploration (19 novembre 2026)

- Côté m0boy : `DP_MD_DUT_DSY=m0.dsy` dans `tools/md_compare_reference` (rejeu du film seul, pas d'exploration ; `deepeel --dsy`
  lance l'exploration complète après le film et n'écrit que la RAM) écrit par image `wram` (64 Kio) et `regs` (D0-7, A0-7, PC).
- Côté Gens-rr : `tools/gens_dsy.lua` (sous wine, seul hôte possible de Gens-rr ; adapter les deux chemins `Z:\…`) écrit le même
  format DSY1 (`memory.getregister("main.d0")`…).
- Comparaison : `deepeel desync gens.dsy m0.dsy --ignore wram:fc00-ffff --ignore regs:0-67 --offset-a N --window 4 --ring DIR`.
  Les registres ne se comparent pas à la frontière de trame (les deux émulateurs rendent la main à des points différents de la
  trame) : les ignorer. Film 1843S : première différence `wram` à la trame 59 (`$F641`/`$F645`, posés une trame plus tôt par
  m0boy qu'avec Gens), c'est-à-dire le décalage de boot de 1–2 trames ; le chercheur s'arrête à la première différence,
  d'où le recours à des `--ignore` pour les compteurs de phase (`$F600-$F62F`, `$F000-$F00F`, `$FE0C-$FE0F`).

### Premier écart localisé : la frame 292 de 1843S

Mesure de l'outil (flux DSY1 Gens-rr ↔ m0boy, décalage de démarrage de 1 frame
déjà absorbé : `init` à l'indice 52 côté Gens, 51 côté m0boy) :

- les frames 280 à 291 sont identiques octet pour octet (WRAM hors pile) ;
- à la frame 292, `$FE0C–$FE0F` (compteur de V-int) vaut 98 chez Gens et 100 chez
  m0boy ; à la frame 293 les deux valent 101. Même nombre total de V-int, répartis
  autrement entre les deux frames : la frontière de frame tombe à un instant
  différent d'un traitement long (chargement), pas de V-int manquante ;
- retarder la V-int de 128 cycles dans la ligne (comme `main68k_exec(Cycles_M68K - 360)`
  de Gens) ne change rien à la dérive (+2 final identique) ;
- le modèle DMA par ligne de Gens ne la change pas non plus.

Reste à départager par une trace PC/cycles instruction par instruction autour de la
frame 292 (`memory.registerexec` côté Gens-rr, `DP_MD_CYCLE_TRACE` côté m0boy).

### Cadence de la boucle d'attente `$71FC4` (frames 270-291 de 1843S)

Boucle `nop` + `dbf d0` (14 cycles par tour), `d0` décompté d'une frame à l'autre :

- Gens-rr : 9133 tours par frame, constant ;
- m0boy (horloge exacte du cœur de référence, 895780/7 cycles par frame) : 9140-9141,
  soit ≈ 112 cycles de plus par frame ;
- m0boy avec `DP_MD_CLOWN_CLOCK=3416` (lignes de 488 cycles, 127856 par frame, comme
  `CPL_M68K` de Gens) : 9132-9133, la cadence colle, mais la fidélité d'ensemble
  s'effondre (écart médian de 1173 octets par frame contre 15 avec l'horloge exacte,
  meilleur décalage −1/0/+1, WRAM hors pile, 3000 frames).

La longueur de frame seule n'explique donc pas l'écart : environ 112 cycles par frame
sont consommés chez Gens par autre chose que le balayage (coût d'entrée/sortie de la
V-int, arbitrage de bus Z80 ou DMA) pendant cette boucle. À mesurer par une trace PC/cycles.

### Lignes de Gens avec l'arrondi exact (essai retiré)

Essai : horloge exacte conservée (arrondi du DMA, phase), mais lignes uniformes de
3416 horloges maîtres (488 cycles, 127856 par frame, la frame exacte de Gens dans la
boucle `$71FC4`). Même film 1843S, 3000 frames, meilleur décalage −8..+8 par frame :

- frames 100, 300, 500 : identiques à la base ;
- à partir de ≈ 1000-1500 la partie diverge franchement (≈ 1000 à 2800 octets par
  frame, décalage jusqu'à −8) alors que la base reste à 1-30 octets, décalage 0/−1.

L'horloge exacte du cœur de référence reste donc la meilleure approximation moyenne,
bien que la boucle d'attente de la frame 270-291 montre 112 cycles de moins par frame
chez Gens. Cet écart local n'est pas une longueur de frame constante : il faut la
trace PC/cycles de Gens-rr pour savoir où ces cycles sont consommés. Essai non
conservé dans m0boy.

### Correction de l'essai « lignes de Gens » et état du démarrage

L'échec de l'essai précédent venait d'un défaut de l'essai : `line_master` (3419 en mode
exact) servait encore au compteur HV et au statut VDP alors que les lignes valaient 3416.
Refait avec les deux alignés (`DP_MD_EXACT_LINE=3416`, expérience non committée dans
m0boy : lignes uniformes de 488 cycles, comptabilité exacte conservée) :

- la boucle `$71FC4` suit Gens : 9132-9133 tours par frame (9140-9141 avec la base) ;
- l'ensemble reste aussi fidèle que la base : écart médian de 15 octets par frame,
  334 frames identiques, 48 frames à plus de 50 octets (base : 47) ;
- les marches de dérive du compteur de V-int tombent aux mêmes frames (292-295
  transitoires, puis 336, 419, 470 durables) : la longueur de frame n'en est pas la cause.

Démarrage (`regs`, boucle de checksum `$32C-$330`) : les deux machines sortent de la
boucle à la même frame (indice 52, PC `$370`/`$372`) ; `a0` diffère de 14 à 18 octets
(7 à 9 tours, moins de 250 cycles) sur 49 frames. Le « décalage d'une frame » du marqueur
`init` n'est donc pas un décalage d'exécution. Les marches durables sont à chercher aux
phases de chargement (frames 335, 418, 467), par trace PC/cycles de ces frames.

### Cause trouvée : l'instant d'échantillonnage n'était pas le même

Les deux flux DSY1 n'étaient pas pris au même instant d'une frame :

- une frame de Gens-rr va de la ligne 0 de l'écran actif à la ligne 261 ; la V-int
  tombe à la ligne 224 (+128 cycles) et l'échantillon est pris **38 lignes après** ;
- une frame de m0boy commence à la V-int et l'échantillon était pris **262 lignes
  après**, juste avant la suivante.

Les états comparés étaient donc distants de 224 lignes d'émulation. Les « marches » de
dérive du compteur de V-int, la boucle `$71FC4`, le « décalage de démarrage » étaient
en grande partie cet artefact (sur une attente stable les deux coïncident, d'où
l'impression d'un bon alignement).

Correction : `DP_MD_DSY_PHASE=<cycles>` (`md_compare_reference`, avec le crochet
`phase_hook` de m0boy appelé `phase_cycle` cycles après le début de la ligne 37 ;
360 = 38 lignes moins 128 cycles). Mesures de l'outil, 1843S, 3000 frames, WRAM hors pile :

| échantillon m0boy | frames identiques | médiane d'octets | frames > 50 octets | première différence |
|---|---|---|---|---|
| fin de frame (avant) | 334 | 15 | 47 | frame 59 |
| 38 lignes après la V-int | 600-616 | 10 | 17-18 | frame 87 |

Le réglage fin (0 à 488 cycles) ne change presque rien. Restent, aux frames 87, 292,
320-335 : une palette (`$FB00`) ou le tampon de décompression Nemesis, écrits un peu plus
tôt chez m0boy. Frame 87 (trace m0boy) : la V-int n'est prise qu'à ≈ 14 100 cycles du
début de frame (section masquée) et le gestionnaire s'exécute de 14 158 à ≈ 20 400,
l'échantillon (18 416) tombe en plein dedans : une différence de quelques centaines de
cycles dans la section masquée suffit. Le modèle DMA par ligne de Gens, re-testé à la
bonne phase, dégrade (351 frames identiques) : non retenu.

### Frontières de frame : ce que le comptage de tours dit

Comptage des passages de la boucle masquée `$1432` avant la V-int de la frame 87
(registerexec côté Gens-rr, trace PC/cycles côté m0boy) : **1312 tours dans les deux
machines** (m0boy 752 + 560, Gens 687 + 625). L'exécution est la même ; seule la
frontière de frame diffère (65 tours, ≈ 1430 cycles, ≈ 3 lignes), et elle glisse puisque
la frame de m0boy dure 112 cycles de plus (127 968 contre 127 856).

Essais d'échantillonnage (`DP_MD_DSY_PHASE=ligne:cycle`, 700 premières frames) : la
phase « 38 lignes après la V-int » reste la meilleure (première différence frame 87) ;
des phases de 0 à 261 lignes, avec indexation directe ou décalée d'une frame, et avec
des lignes de Gens (3416), ne font pas mieux. La différence de la frame 87 (palette
`$FB00` écrite à l'échantillon chez m0boy, pas encore chez Gens) est un décalage de
frontière de ≈ 3 lignes dans un gestionnaire de V-int, pas un défaut d'exécution.
Pour la lever il faut caler la grille de frames de m0boy sur celle de Gens (frame de
127 856 cycles ET frontière à la bonne phase) avec une comptabilité DMA/V-int qui reste
fidèle ; essayé séparément, chaque moitié seule ne suffit pas.

### Grille absolue de 127 856 cycles (essai retiré)

Échantillonner m0boy tous les 127 856 cycles absolus (la période de Gens) au lieu d'une
phase fixe par frame, avec huit origines de 12 000 à 135 000 cycles (700 frames) : au
mieux 377-395 frames différentes, contre 368 pour « 38 lignes après la V-int ». Aucune
origine ne lève les frames 87, 292, 320-335. Le glissement de 112 cycles par frame n'est
donc pas le facteur limitant ; ce qui reste est de la comptabilité fine à l'intérieur
de la frame (durée de la section masquée, entrée de V-int, DMA) que seule une trace
cycle par cycle côté Gens-rr peut départager. Code d'essai non conservé.

### Échantillonnage à l'événement : la V-int (`DP_MD_DSY_VINT`)

L'instant d'échantillonnage n'est plus un temps mais un événement : la première
instruction du vecteur de V-int (`memory.registerexec` côté Gens-rr, `tools/gens_dsy_vint.lua` ;
sortie de `interrupt()` côté m0boy, `DP_MD_DSY_VINT=1`). Les frontières de frame sont
alors les mêmes dans les deux machines par construction. Sur 1843S (2694 V-int côté Gens,
2696 côté m0boy) : **124 frames identiques d'affilée** (87 avec la phase fixe), première
divergence à la frame 124.

Frame 124 : `$FFF62A` (`v_vbla_routine`) vaut 18 chez Gens et 0 chez m0boy, et le fondu de
palette (`$FB04…`) démarre une frame plus tôt chez Gens. La boucle principale de Gens a donc
posé la routine `$12` avant la V-int 124 et celle de m0boy non : m0boy subit une frame
« lag » (V-int pendant le travail de la boucle) que Gens n'a pas. Décaler l'entrée de ±1
frame (`DP_MD_DUT_PAD_SHIFT`) ne change pas la première divergence (146 frames identiques
pour −1, 124 pour +1) : ce n'est pas un décalage d'entrée mais un travail de boucle principale
qui dépasse de quelques cycles le budget de la frame chez m0boy (ou que la V-int de m0boy
tombe trop tôt dans ce travail). Piste suivante : compter les instructions et cycles du
travail entre les V-int 122 et 124 des deux côtés.

## Synchronisation avec Gens : mode « Gens » de m0boy (observations de l'outil)

Réglages m0boy qui alignent la rejouée sur Gens-rr v11c (échantillon DSY1 pris à
l'entrée du vecteur V-int, comme le hook d'exécution de Gens ; fichier `.k` à
côté) :

- `DP_MD_POWERON=gens` (démarrage à la ligne 38, pas de V-int au tour 0),
  `DP_MD_CLOWN_CLOCK=3416`, `MD_IRQ_IMMEDIATE=1` (V-int livré tout de suite
  après l'instruction qui le démasque), `DP_MD_DUT_PAD_SHIFT=1` (manette décalée
  d'une trame) ;
- `DP_MD_GENS_DMA=1` : modèle DMA de Gens (copie instantanée, `Update_DMA` par
  ligne, bit 1 « busy » du statut tant que `DMAT_Lenght > 0`, y compris fill) ;
- `DP_MD_VDBG=1` / `DP_MD_VDBG2=1` : trace des V-int, DMA et de l'état à chaque
  début de trame (diagnostic).

- Pas de bit busy YM2612 (`$A04000` bit 7) en mode Gens : Gens ne le lève jamais
  (`YM2612.status` ne porte que les drapeaux de timer). Les boucles d'attente du
  pilote sonore de Sonic (`$7272E…`) durent donc moins longtemps que sur m0boy
  « matériel » ; c'était la cause de la divergence à l'échantillon 217 (la trame
  523 consommait ≈125 000 des 127 856 cycles, le V-int arrivait avant la fin du
  travail principal).

Résultat observé : Castle of Illusion 233S, 2579 échantillons sur 2580 identiques
(WRAM + séquence de V-int). Sonic 1 1843S : 2991 échantillons sur 2993 identiques
sur tout le film ; seuls les échantillons 168 et 386 diffèrent (tampons de
sprites/hscroll, 26 et 183 octets, transitoires : l'échantillon suivant
concorde). Ce sont des observations de l'outil, pas des mesures de matériel.

`DP_MD_GENS_MODE=1 sh tools/md_compare_reference.sh ROM film.gmv N` positionne
d'un coup tous ces réglages (DMA, horloge, power-on, IRQ immédiate, décalage de
manette, échantillonnage à la V-int, horloge de référence Gens).

## Tables de code : deux trous trouvés en comparant aux dasm (0.409.0, observations de l'outil)

Chaque essai de couverture est comparé au désassemblage de référence ; les écarts ont désigné deux limites du suivi des tables de code (`code_table_targets`, `isa_code_table`).

- **Plafond de 128 entrées** : la table d'objets de Sonic 1 (`Obj_Index`, pointeurs longs) a des ids jusqu'à `$8C` ; au-delà de 128 entrées, ses objets (ex. `ScrapEggman`) n'étaient jamais suivis. Le plafond est `DP_CODE_TABLE_MAX` = 256 (ids sur un octet). Sonic 1, film 1843S, 10 min : code de référence « exécuté ou prouvé » 77,2 % → 96,4 % (la part exécutée, 52 %, ne change pas : le film couvre le début du jeu).
- **Mot haut posé par le dispatcheur** : Streets of Rage appelle `lea Table(pc),a1 ; jmp $15848(pc)` ; le dispatcheur fait `moveq #1,d0 ; swap d0 ; move.w 0(a1,d0.w),d0 ; movea.l d0,a1 ; jmp (a1)`, donc les entrées 16 bits sont des adresses `$1xxxx`. Le suivi lisait `$750A` au lieu de `$1750A`. `DpCodeTable.hi` reprend le `moveq #k,Dn ; swap Dn` qui précède la lecture. Streets of Rage, exploration autonome 10 min : code de référence manquant 22,0 % → 15,3 % (prouvé sans exécution 23 840 → 40 038 octets ; la part exécutée est plus basse qu'à 47 min).

Ces chiffres sont des observations de l'outil (pas des mesures de matériel). - **Base de table posée par l'appelant** (`lea T(pc),a5 ; jmp helper`, Streets of Rage) : trois causes cumulées. `jmp` effaçait tous les registres connus ; les ALU vers Dn (`andi.w #$7F,d0`…) aussi, alors qu'elles n'écrivent qu'un registre : `reg_effect` du 68000 n'efface plus que le registre écrit. Un helper déjà connu est reparcouru une fois par contexte de registres (adresses ROM tenues). Le budget d'un parcours croît avec le nombre d'entrées de table (borné à 16 × `CLOSURE_INSTR`). Streets of Rage, exploration autonome 10 min : code de référence manquant 15,3 % → 10,0 % (prouvé sans exécution 39 288 → 43 952 octets).
- **Tables reconnues aussi en parcours non fiable** (après un `jsr`, dont la suite n'est pas de confiance) : cibles provisoires, confirmées par un parcours fiable ou l'exécution. Streets of Rage, 10 min : manquant 10,0 % → 4,8 % (prouvé sans exécution 43 952 → 49 124 octets).
- **Base de saut décalée** (`tdelta`) : quand le `jmp 2(pc,d0.w)` du dispatcheur a sa base quelques octets après celle de la lecture (`move.w 4(pc,d0.w),d0`), `isa_code_table` conserve l'écart (champ `tdelta`, pairs, ±8 octets) et `code_table_targets` l'ajoute à chaque cible (Streets of Rage, dispatcheur `$293C-$2944`).
- **Relecture TAS sans plafond** : `DP_WATCH_FPS=0` lève le plafond de cadence du watch. Constat : sur Sonic 1 1843S, la WRAM du noyau est identique, trame par trame, à celle du harnais m0boy en réglages par défaut (3600 trames) ; l'écart avec Gens (ressort vers la trame 3400) vient donc des réglages de l'horloge (Clown exact / mode Gens) et non du noyau d'exploration.
- **Échantillon à la V-int côté noyau** (`DP_MD_DSY_VINT=<fichier>`, diagnostic) : le noyau écrit un DSY1 (WRAM + registres) à l'entrée du vecteur de V-int, pour la seule relecture principale (les forks restaurés sur la même machine rejouent des trames déjà vues et sont écartés : sans ce filtre, les échantillons sont pollués et l'on croit à tort le noyau décalé). Observation de l'outil, Sonic 1 1843S, réglages Gens (`DP_MD_CLOCK_OFF=0`, `DP_TAS_PAD_SHIFT=1`) : le noyau est identique au harnais m0boy en mode Gens (WRAM hors pile) sur les échantillons 0 à 123 ; la première différence est l'échantillon 124 (`$F62A` : 18 chez Gens et le harnais de référence, 0 chez le noyau), exactement la divergence m0boy/Gens déjà décrite. Cette divergence au V-int 124 était en réalité un défaut du noyau (voir ci-dessous), pas de m0boy.
- **Cause de l'écart noyau/harnais (corrigée, 0.411.0)** : le noyau découpe l'exécution en budgets de pas ; une reprise en milieu de ligne rejouait le travail de début de ligne, dont `md_full_gens_dma_line` (avance du DMA à la manière de Gens). Le DMA finissait trop tôt, la section masquée du chargement de niveau était plus courte (V-int 123 à la trame 334 ligne 258 au lieu de 336 ligne 144) et le V-int 124 arrivait trop tôt (`$F62A` = 0 au lieu de 18). Le travail de début de ligne n'est plus rejoué à la reprise (adaptation CMake). Observation de l'outil, Sonic 1 1843S, réglages Gens : le noyau est identique au harnais m0boy sur 2900 échantillons de V-int, et à Gens-rr sauf les échantillons 168 et 386 (transitoires, déjà connus).
- Options de relecture : `DP_TAS_PAD_SHIFT=k` (le film voit son entrée k trames plus tard, films Gens-rr : 1) et lissage du watch (tampon de 360 images, `DP_WATCH_SMOOTH=0` le coupe).
- **Entrée verrouillée à la première ligne active** (`DP_MD_PAD_LINE0=1`, avec `DP_TAS_PAD_SHIFT=0`) : Gens-rr applique l'entrée de la trame n dès la première ligne active, alors que le noyau l'applique au V-int qui ouvre la trame. Les lectures faites pendant l'affichage actif voyaient donc l'entrée précédente (décalage d'une trame qu'aucun `DP_TAS_PAD_SHIFT` entier ne corrige). Avec cette option le noyau applique l'entrée en attente à la ligne 38 de sa trame. Observation de l'outil, Streets of Rage film 1064M, réglages Gens : identique à Gens-rr sur les 556 échantillons de V-int du dump, alors que le réglage `DP_TAS_PAD_SHIFT=1` divergeait à l'échantillon 92 (Start lu une trame plus tard). Castle of Illusion film 233S : identique à Gens-rr sur les 2581 échantillons avec cette option (le réglage `DP_TAS_PAD_SHIFT=1` donnait un écart dès l'échantillon 78 sur ce dump). SoR 130M : sans changement, première différence à l'échantillon 4760 (`$DB9D`, octet de phase, hors entrée). Sonic 1 non revérifié avec cette option (mode par défaut inchangé).

## Technique B10 : adresse de routine immédiate parquée en RAM (0.412.0)

Un `move.l #routine,<case RAM>` exécuté (taille mot long, source immédiate, destination en RAM `$FF0000+` ou Z80)
dépose une adresse de code dans une variable que le jeu appelle plus tard par `movea.l`/`jsr (a1)` — c'est le
dispatch par état de Ristar et de Streets of Rage. Le noyau marche désormais cette adresse comme entrée non
fiable (confiance 0) de la fermeture statique ; le compteur « RAM-parked code pointers » l'indique dans le résumé.
Observations de l'outil face aux dasm : Ristar 49,9 → 66,5 % du code IDA couvert (34 393 pointeurs parqués) ;
Streets of Rage, code de référence manquant 9,1 % → 4,8 % (sans film). Restes : blocs atteints par des tables de
sauts indexées ($294E, $1727A, $113C8 en SoR ; $397EC en Ristar).

## Reprise d'un état de frontière de trame (0.413.0)

`dp_mdmach_load` repartait toujours en mode reprise (`resume_frame = 1`). Un état sauvé entre deux trames (trame terminée,
`line` encore à 261, `frame_cycles` revenu à ~400) rejouait donc la ligne 261 de la trame suivante et sautait le travail
de début de trame, dont la demande de V-int de la ligne 0 : le V-int de la trame était perdu. Sur Ristar (ROM JP, film
1506S), le handler 0 se terminait sur une trame sans V-int en attente, alors que Gens livre le V-int suivant au `rte`
(`$4370`) ; le thread principal du noyau avait une trame d'avance (RNG `$EA22` d'un pas, `$EA01`, décompression).
La reprise ne se fait plus qu'à l'intérieur d'une trame (`line == 261 && frame_cycles < 100000` = frontière : départ en
ligne 0). Observation de l'outil, Ristar 1506S, réglages Gens : 3398 échantillons de V-int, WRAM identique à Gens-rr
(hors pile) sur tous sauf 9 (écarts de 21 à 28 octets, transitoires) ; avant : écart dès l'échantillon 1, centaines
d'octets dès le 6.

Même correctif, Castle of Illusion film 233S, réglages Gens (`DP_TAS_PAD_SHIFT=1`) : 2578 échantillons de V-int du noyau, aucun écart de plus de 30 octets de WRAM (hors pile) face à Gens-rr. Observation de l'outil.

## Technique B11 : tables d'adresses de routines reconnues par leur forme (0.414.0)

Streets of Rage range ses objets dans une table de mots `$1D40-$1E0E` (`$2140 $2140 … $291E $291E …`) dont le lecteur n'est
jamais exécuté par l'exploration sans film : aucune lecture de la table n'est observée, donc aucune cible suivie (`$294E`,
`$284A`, `$291E`, premier trou de la comparaison au dasm). Au démarrage, le noyau cherche dans la ROM (68000, adresses < 4 Mio)
les suites d'au moins 6 entrées alignées de 16 ou 32 bits dont chaque valeur pointe une adresse ROM paire précédée d'un
terminateur (`rts`, `rte`, `rtr`, `bra`, `jmp`) ; les zéros sont tolérés dans la suite, un mot isolé aussi en 16 bits. Chaque
entrée est marchée comme entrée non fiable (code provisoire, jamais « exécuté » ; compteur « RAM-parked code pointers »
partagé). Observation de l'outil, SoR sans film, 150 s, 4 fils : code de référence manquant 12,1 % → 9,0 % (prouvé sans
exécution 77 587 → 80 601 octets). Ristar, 150 s : statique/spéculatif 55,6 → 60,7 % du code IDA (les parts exécutées
diffèrent d'un run à l'autre). Limite : une table dont les cibles ne suivent pas un terminateur n'est pas vue.

Extension de B11 (même version) : les entrées 16 bits sont aussi complétées par un numéro de banque `k` (`moveq #k,Dn ; swap Dn`
du dispatcheur, tables d'états de SoR vers `$1xxxx`), une valeur répétée prolonge la suite (état au repos répété), et une cible
est plausible quand elle suit un terminateur **ou** démarre une chaîne d'au moins 3 instructions légales finissant par un retour
ou un saut (les routines d'une table ne suivent pas toujours un `rts`) ; la suite doit alors compter au moins 12 entrées.
Observation de l'outil, SoR sans film, 150 s : code de référence manquant 9,0 % → 3,2 % (prouvé sans exécution 80 601 →
86 295 octets) ; contrepartie : 10 457 octets provisoires hors du code de référence (49 % de précision sur le gain), tous en
code provisoire (jamais « exécuté »). Avec un seuil de 6 entrées : 0,8 % manquant mais 16 455 octets hors référence.

Ristar avec B11 étendu (seuil 12), sans film, 150 s, 4 fils : code IDA couvert 66,1 % → 99,7 % (exécuté 11,8 % + statique/spéculatif 87,9 %), 1 463 octets exécutés hors du code IDA. Observation de l'outil ; la part statique est provisoire (non exécutée).

Mesures finales de la session (B11, seuil 12, observations de l'outil) : Streets of Rage sans film, 600 s, 4 fils : code de référence manquant 1,3 % (exécuté 42,7 %, prouvé sans exécution 54 329 octets) ; plus grands trous restants `$1893A` (82 octets), `$1884A` (78), `$188F2` (64). Ristar : quatre adresses du dasm IDA restent non couvertes (`$397EC`, `$3BDBA`, `$3B134`, `$DA9A`) : elles suivent des données (pas de terminateur) et `$DA9A` est une entrée de table longue mêlée de cases partagées ; assouplir la forme (cases ROM quelconques) n'a rien gagné (99,6 %), essai retiré.

Films Gens anciens (`TEST6` et avant, ex. Castle 250S) : sans octets de manettes dans l'en-tête, ils étaient refusés (« unsupported controllers ») et le watch tournait sans entrées. Un champ vide sur une version < `A` vaut deux manettes à 3 boutons. Castle 250S, réglages Gens : 2258 échantillons de V-int, WRAM identique à Gens-rr (0 échantillon à plus de 30 octets d'écart). Observation de l'outil.

## 0.415.0 — films 2 joueurs, fin de film, vitesse et mémoire

- Les films Gens à deux manettes (SoR 1064M) sont lus : le port 2 est branché (bit 31 du masque d'entrée) et décodé comme manette 3 boutons.
- Fin de film : une session « attract » reste sur l'état final (avec détection de stagnation).
- Vitesse : accès direct à la RAM de travail (`DpMachine.ram_view`) pour `cell_key`/`proj_key` (hachage par mots de 8 octets sur les pages vivantes), balayage des zéros par blocs de 32 octets, tas glibc sans `mmap` pour les états. `DP_MD_DSY_STRIDE=N` n'échantillonne qu'une V-int sur N.
- Mémoire : la file d'états en RAM est bornée à 12 % de la RAM disponible (1,5 Gio au plus) ; le reste va au spool disque.
- Les clés `cell_key`/`proj_key` ont changé de fonction de hachage : les anciens fichiers d'état ne sont pas réutilisables.

B11 (enregistrements) et suite d'un appel indirect (0.417.0) : deux techniques ajoutées après comparaison au dasm IDA de Ristar. (1) Les tables d'enregistrements de 6, 8 ou 10 octets dont le premier champ est une adresse de routine sur 32 bits (table d'états/scripts : pointeur, durée, remplissage) sont reconnues dès 4 enregistrements consécutifs dont chaque pointeur suit un terminateur ; chaque pointeur est marché comme entrée non fiable (provisoire, jamais « exécuté »). (2) La suite d'un `jsr (An)` / `jsr d(pc,Dn)` est marchée comme celle d'un appel direct (non fiable, point 3) : le décodeur marquait déjà ces appels `call` + `fall`, mais le parcours les traitait comme un saut. Observation de l'outil, Ristar, 200 s, 4 fils, sans film : plages du dasm IDA non couvertes 90 → 47, octets 990 → 472 ; octets exécutés hors du code IDA 1 721 → 1 781 (provisoire). `ctest` : `kernel_explore` échoue aussi sans ce changement (test de limite d'écriture, indépendant) ; `rtcode_sms_source` et `nes_watch_dump` sont sensibles à la charge (verts isolément).

B11 (tables relatives, 0.417.0) : un `lea d16(pc),An` suivi dans les 24 octets d'un `jmp (An)` / `jsr (An)` donne la base d'une table de mots signés relatifs à son propre début (dispatcheur `move.w -2(a1,d0.w),d0 ; adda.l d0,a1 ; jmp (a1)`). La table se termine à sa plus basse cible ; chaque cible dont une chaîne d'instructions légales finit par un terminateur (une seule instruction avant lui suffit : stubs `move.b x,d0 ; bra y`) est marchée comme entrée non fiable (provisoire). Observation de l'outil, Streets of Rage sans film, 330 s, 4 fils, face au dasm de référence : code manquant 1 245 → 959 octets (1,3 → 1,0 %), exécuté 38,1 → 41,6 % ; Ristar inchangé (472 octets manquants). Plus grands trous SoR restants : `$1409A`, `$26AE`, `$4FC4`, `$12AAE`.

**0.418.0 — pas forcés : les deux côtés d'un transfert sont marchés.** Un pas forcé qui exécute un `bcc`/`bra`/`jsr` lance désormais `closure_walk_untrusted` sur sa cible et sur la suite (parcours non fiable, provisoire). Sonic 1 (film 1843S, 330 s) : code du dasm manquant 756 → 622 octets (le corps de `$1130`, `VBlank` palette, est retrouvé). Les octets « données du dasm » marqués exécutés (230 → 310) ne viennent pas de ce changement : le parcours non fiable ne pose que `DP_CORE_STATIC`, jamais « naturel » ; c'est la variance d'exploration entre deux runs de durées différentes (observation de l'outil).

**Limite technique : le code mort sans appelant.** Les restes du dasm qui subsistent après toutes les techniques (Sonic 1 `$14C4E`, 80 octets, routine de retournement de tuiles placée après un `rts` ; SoR `$1409A` ; Ristar `$6374`, 78 octets) n'ont ni appelant direct, ni pointeur dans une table, ni exécution dans les films : aucun chemin ne les atteint, ni dynamique ni statique. Seule leur forme (suite d'instructions légales se terminant par un retour) les désigne ; c'est le rôle de B8, qui reste volontairement hors du `.cov` (jamais « connu »). Élargir B8 (suivi des cibles en avant) a été essayé : +40 % d'octets spéculatifs, non mesurable contre le dasm, retiré.

**0.419.0 — B11 : terminateurs à mot d'extension.** La validité d'une entrée de table de pointeurs ne regardait que le mot juste avant l'entrée ; or `jmp d16(pc)`, `jmp abs.w`, `bra.w` (opcode 4 octets avant) et `jmp abs.l` (6 octets avant) terminent une routine avec un mot d'extension. Les tables d'états de SoR (`$1D80…`, mots absolus répétés deux fois) étaient coupées en séries courtes ; elles sont maintenant reconnues, et les stubs `lea table(pc),a1 ; jmp répartiteur` y sont acceptés (`chain_short`). SoR (run 600 s, 4 fils) : code du dasm manquant 961 → 843 octets (0,9 %) ; `$1409A` et `$26AE` retrouvés (observation de l'outil).

Ristar avec 0.419.0 (420 s, 4 fils) : code IDA manquant 472 → 442 octets (47 trous, 99,9 % exécuté ou prouvé) ; SoR 961 → 843. Les trous restants sont des routines sans appelant ni pointeur (voir la limite ci-dessus).

## Base TASVideos locale (`deepeel tasdb`, 0.420.0)

`deepeel tasdb sync [--systems A,B]` interroge l'API TASVideos (`/api/v1/systems`, `games`, `publications`, `submissions`) et range des TSV dans `tasdb/` à côté de l'exécutable (`DEEPEEL_TASDB` ou `--dir` pour changer) : jeux avec les hachages MD5/SHA-1 de chaque version de ROM, publications, soumissions (tous statuts). `deepeel tasdb find ROM [--fetch]` hache la ROM, retrouve le jeu et sa version, liste publications ET soumissions (date, émulateur, ROM de l'enregistrement) et télécharge les films dans `tasdb/movies/<jeu>/<id>M|S/`. `deepeel tasdb fetch GAME_ID` fait de même par identifiant. Exemple : la ROM `Streets of Rage (W) (REV00)` (SHA-1 `3d74dbc8…`) est le jeu 115 ; ses films sont 130M/67S (SprintGod, Gens 9, 2004) et 1064M/1845S (Mitjitsu & Mukki, GENS, 2008).

### SoR 1064M, échantillon 4690 : le V-int arrive 2 instructions plus tôt dans le noyau (20 novembre 2026)

Diagnostics ajoutés (désactivés par défaut) : `DP_MD_WLOG=lo:hi` + `DP_MD_WLOG_V=a:b` (journal des écritures CPU : numéro de
V-int, trame, ligne, PC de l'instruction), `DP_MD_PCLOG=lo:hi` + `DP_MD_PCLOG_V=a:b` (PC, ligne et cycles de chaque
instruction), côté Gens-rr `tools/gens_wlog.lua`. Observation de l'outil (film 1064M, réglages Gens, `DP_MD_CLOWN_CLOCK=3416`) :
l'écart de 8 octets de l'échantillon 4690 (`$C0A9` : Gens `ec`, noyau `ee`, puis `$DC10-15`, `$E281`, `$FA40`) vient du
constructeur de table de sprites (`$AFFA-$B048`) : Gens l'écrit avant le V-int 4690, le noyau après (échantillon 4691, ligne 76).
Le PC interrompu à l'échantillon 4690 est `$B0E8` chez Gens (dans le sous-programme appelé en `$AFF6`) et `$AFF4` chez le
noyau : Gens exécute deux instructions de plus (`addq` 8 cycles + `jsr` 18 cycles) avant de prendre l'IRQ, soit un retard de
prise d'IRQ compris entre 8 et 26 cycles dans ce cas. Les échantillons 4684-4689 sont identiques (PC et registres). Même
classe que SoR 130M (échantillon 4760) : un décalage global de `DP_MD_VINT_DELAY` ne convient pas (9-10 cycles cassent dès
4500) ; il faut modéliser la quantification de la prise d'IRQ de Gens (fin de ligne à l'instruction près).

### Sync Gens / m0boy : coûts d'instructions comparés à Starscream (20 novembre 2026)

Les coûts statiques de `md_m68k_cycles` ont été comparés à la table extraite de `Star.c` (Gens-rr, compilé sous Linux). Divergences relevées : `ADDQ/SUBQ.W #,An` (Starscream 4, matériel 8 : le noyau garde 8, une première « correction » à 4 faisait diverger Sonic 1843S dès la trame 168, contre 1628 sans elle), `MOVEP` (long = bit 6, corrigé par défaut), et des écarts propres à Starscream (BTST/BSET/BCLR registre, CLR mémoire, TAS, Scc/DBcc) regroupés derrière `DP_MD_STAR_TIME=1` (md_star_adjust, opt-in). Délai d'octroi du bus Z80 de Gens (16 cycles, `DP_MD_GENS_BUSREQ=1`, opt-in) : implémenté, sans effet sur l'écart de SoR 1064M à l'échantillon 4690 (15 octets, PC `$B0E8` Gens contre `$B9F6` noyau) ; piste écartée. Observations de l'outil, pas des mesures de matériel. Reste : coûts dynamiques (pénalités de bus/VDP de Gens).

### Sonic 1 (film 1843S) : watch et desync de la trame 37 3xx (20 novembre 2026)

Observations de l'outil (pas des mesures matérielles).

- **Watch** : le rendu se fait ligne par ligne (`m->screen`, palette par ligne : l'eau de Labyrinth Zone) ; le rejeu principal du
  watch ne fait plus d'exploration (`DP_WATCH_LEAN=0` la rétablit) : ni fork, ni snapshot, ni checkpoint TAS, ni file/spool LZ4
  tant que le film joue (1,4 M pas/s au lieu de 1,1). Le film (60 644 trames) se rejoue en entier en ~5 min ; la session
  d'attract qui suit explore (forks, spool) et reste plus lente. `isa_flow` est mémoïsé, `fetch_n` lit la fenêtre d'instruction
  en un appel.
- **Vecteurs d'exception 68000** : `DpMachine.vector_seeds` (MD : vecteurs 2..63 de la table, gestionnaires dans la ROM) ;
  chaque gestionnaire est une graine forcée, prise avant les sessions (hors film). Avant : `BusError`, `AddressError`, `ZeroDivide`
  du listing s1disasm restaient hors couverture.
- **Desync 37 3xx vs Gens-rr** (échantillonné aux V-int, `DP_MD_DSY_FROM/TO` pour une fenêtre à pas 1) : identique jusqu'à la
  trame 37 300 ; première différence hors pile en 37 312 (buffer de scroll `$F000-$F2CF`), l'objet de Sonic (`$D000`) diverge à
  37 318 ; à 37 317 Gens est encore dans `WaitForVBlank` (`$29AC`) alors que le noyau est interrompu en pleine logique de jeu
  (`$63C6`) : la logique de cette trame est plus lente dans le noyau, une trame de lag apparaît d'un seul côté (plateforme
  ratée). Écartés : `DP_MD_STAR_TIME` (sans `ADDQ.W An`, qui casse dès la trame 168-180), `DP_MD_STAR_DIV`, leur combinaison.

**0.421.0 — graines de vecteurs 68000 bornées.** Les handlers de la table de vecteurs (2..63) sont des départs forcés ; ils passent après les sessions longues (jamais avant : un handler comme `$200` de SoR court dans le jeu en chemin forcé et pollue le chemin naturel) et sont bornés à 4 096 pas (propre aux machines qui fournissent `vector_seeds`, le NES garde son comportement). Mesures de l'outil, 600 s, 4 fils, contre le dasm : SoR 845 octets manquants (0,9 %, inchangé ; sans la borne 1 983 à 3 851 et plus aucune session de jeu), Ristar 442 (inchangé), Sonic 1 : exécuté 42 302 → 42 742 octets, `BusError`/`AddressError`/`ZeroDivide` atteints, 99,4 % exécuté ou prouvé. Restes de Sonic : code de debug (`Debug_Init`, cheat) et code mort.

**Restes de couverture MD après 0.421.0 (observations de l'outil).** SoR avec le film 1064M (600 s, 4 fils) : 64,4 % du code du dasm exécuté, 708 octets manquants (0,7 %) ; sans film 846. Les restes de SoR examinés (`$4FC4` : remplissage `ori.b #0` que le listing classe en code ; `$12AAE`, `$178A6`, `$ACA6`, `$74DE`, `$17858` : aucun appelant, et les mots de la ROM égaux à ces adresses tombent dans du code ou des constantes, pas dans des tables) sont des routines sans accès statique ni dynamique : même limite technique que Sonic 1 `$14C4E` et Ristar `$6374`. Castle of Illusion n'a pas de dasm : sa couverture n'est mesurée que par l'exécution (59 910 octets naturels en 222 min, 24 734 en 300 s) et ne peut pas être comparée à une référence ; le nouveau noyau n'y change rien (graines de vecteurs : 2).

### Sonic 1 (film 1843S) : slack de la boucle d'attente autour de la trame 37 316 (20 novembre 2026)

Mesure : exécutions de `$29AC` (`WaitForVBlank`) par V-int, Gens (Lua, `slack.lua`) contre noyau
(`DP_MD_PCLOG=29ac:29ac`, `DP_MD_PCLOG_V=37289:37320`). Observations de l'outil, pas du matériel.

- Jusqu'à 37 314, les deux émulateurs ont un slack stable (Gens ≈ 2 300, noyau ≈ 210 : unités différentes, rapport ≈ 10).
- Le slack s'effondre dans les deux : Gens 1 632 → 188 → 887 → 2 110 (V-int 37 315..37 318) ; noyau 208 → 154 → 2 → 404 (même suite, décalée d'un V-int, décalage connu depuis 37 312).
- Le minimum du noyau (≈ 2 itérations) est plus bas que celui de Gens (≈ 19 en échelle noyau) : la logique de cette trame dure ≈ 17 itérations (~ 450 cycles) de plus dans le noyau, suffisant pour franchir le V-int suivant et retarder les écritures de logique d'un V-int.
- DMA de la trame (noyau) : 64 + 448 + 320 (+ 368) mots ; la cause exacte du surcoût (arrondi de DMA, coût d'instruction) n'est pas encore isolée. Pistes écartées : `DP_MD_STAR_TIME/DIV/ADDQ`.

Complément (même jour) : la trame lourde est la routine `$72500..$72DFE` (≈ 25 000 cycles dans le noyau). Instructions exécutées dans cette plage, par V-int : noyau 1 934 puis 6 832 (V-int 37 316, 37 317) ; Gens 932 puis 3 416 (37 315, 37 316), soit exactement la moitié (le hook Gens compte une fois par mot d'adresse paire) — le travail est identique, seul le point de coupure par le V-int diffère de quelques dizaines d'instructions. Le décalage vient donc d'un écart de temps inférieur à 0,5 % de trame, pas d'une divergence logique. Gens n'expose pas de compteur de cycles : sans référence cycle-exacte, aucun réglage de coût ne peut être validé ; limite technique de la référence, pas du noyau. `ctest` complet : 184 passent (`rtcode_sms_source` est instable sous `-j2`, passe seul).

### Restes Ristar (442 octets) : vérification des références (20 novembre 2026)

Pour les entrées de blocs manquants (`$5FC4A`, `$BB88`, `$4DB98`, `$6374`, `$519CA`, `$297D8`, `$346E2`, `$4D97C`, `$4DA14`, `$5CC12`), recherche dans la ROM de longs absolus et de `jsr/jmp/bsr` en mot absolu : aucune référence, sauf `$BB88` (pointeur en `$BB02` et `$6BF1A`, jamais suivi par le jeu dans le film). Les autres sont atteignables seulement par branchement relatif ou table relative depuis du code non exécuté ; `$12B6E` est du désassemblage de données. Aucune technique de l'outil n'ajoute ces entrées sans exécution réelle : limite d'atteignabilité, pas de technique manquante. Observation de l'outil sur une ROM et un film donnés.

### Diagnostic : captures PPM au V-int (`DP_MD_SHOT`) (20 novembre 2026)

`DP_MD_SHOT=<préfixe> DP_MD_SHOT_FROM/TO=<fenêtre de V-int> [DP_MD_SHOT_STEP=N]` écrit `<préfixe><n>.ppm` (rendu fin de trame, 320×224) ; il faut aussi `DP_MD_DSY_VINT`, `DP_MD_DSY_FROM/TO` couvrant la même fenêtre (même point d'échantillonnage). Sonic 1 film 1843S, V-int 32 600-33 400 (pas de 10 à 100) et 33 400-37 400 (pas de 100) : aucun tile corrompu observé (moniteur détruit en 33 080-33 130, HUD, décor). Le bug « tiles de la TV vers 33k » vu dans la fenêtre `--watch` n'est donc pas reproduit par le rendu fin de trame ; la piste restante est le rendu ligne par ligne de la fenêtre (`screen`), non utilisé par ces captures. Observation de l'outil.

### Restes SoR (846 octets) : vérification des entrées (20 novembre 2026)

Les 12 plus grands blocs manquants (`$4FC4`, `$12AAE`, `$178A6`, `$25FE`, `$256C`, `$ACA6`, `$74DE`, `$17858`, `$5904`, `$140B4`, `$196B4`, `$13FE4`) n'ont aucune référence en long absolu, `jsr/jmp` mot absolu ni `bsr/bra.w` dans la ROM. `$4FC4` est du remplissage nul (`ori.b #0,d0` ×n, 46 octets) classé code par la référence : non exécutable par construction. `$12AAE`, `$178A6`, `$25FE` sont des états d'objets derrière un `rts`/`jmp` : atteints seulement par table d'états relative quand le jeu y entre ; absents du film joué. Limite d'atteignabilité de la ROM, observation de l'outil (film 1064M + exploration).

### Palette de l'eau LZ : H-int de la dernière ligne (20 novembre 2026)

- Cause : quand la surface de l'eau est hors écran, `v_hblank_line` vaut 223 (`LZWaterFeatures.asm`) et le H-int tombe sur la dernière ligne active.
  Le noyau le servait après le V-int suivant, dont la copie de palette sèche était alors écrasée par le handler (palette d'eau). Gens lève le H-int
  404 cycles avant la fin de la ligne. Correctif dans m0boy `md_full_frame` (`DP_MD_HINT_LATE=1` rétablit l'ancien comportement).
- Observation de l'outil : aux trames de film 33 100 et 33 110, aucune différence de pixel avec Gens hors zone OSD ; à 33 920, 16 pixels sur une
  seule ligne (bord eau/sec). La « TV » aux tuiles bizarres de 33 920 est identique chez Gens (pas un défaut du noyau).
- Effet sur l'état du jeu (A/B 1843S, stride 14) : 5 octets de RAM en échantillons 2085-2086 puis identique ; pas de comparaison à Gens faute de dump
  de référence aligné pour cet environnement.
- Outils : `--replay-only --save-frames N,A-B,… --save-dir D` (rejeu allégé, état complet + RAM + écran des trames choisies, arrêt après la dernière) ;
  `DP_MD_LINESCREEN=1` (écran ligne par ligne) ; sélecteur `N,A-B,…` pour `DP_MD_VDBG`, `DP_MD_VDBG2`, `DP_MD_CRLOG`, `DP_MD_SHOT_FRAMES`.

#### Desynchro 37 316 : corrigée par le H-int de la dernière ligne (20 novembre 2026)

Mesure avec un dump Gens-rr régénéré (`tools/gens_dsy_vint.lua`, 1 échantillon sur 14 V-int, 38 100 trames, film 1843S) contre le noyau en
`--replay-only` (mêmes variables d'environnement), RAM `$0000-$FBFF` hors compteurs de phase (`aa00-abff`, `3500-35ff`, `f600-f62f`, `f000-f00f`,
`fe0c-fe0f`) : sans le correctif, 46 échantillons diffèrent dont la désynchro durable à partir de l'échantillon 37 324 (600 à 2 000 octets) ;
avec le correctif, 8 échantillons seulement (8 372, 15 750, 15 792, 18 214, 18 228, 28 490, 28 504, 36 428), tous isolés, l'état se
recale à l'échantillon suivant. Observation de l'outil, pas une mesure de matériel. La cause réelle n'était donc pas la coupure du V-int
mais le service du H-int de la ligne 223 (handler d'eau LZ qui exécute `VBlank_UpdateScreen` différé, `f_doupdatesinhblank`) après le V-int suivant.

Les 8 échantillons isolés restants (V-int 8 372 … 36 428) ont été examinés octet par octet : 1 à 42 octets, groupés dans des zones de travail
(`$AD01-AD06` minuteurs voisins des compteurs ignorés, `$D841-D859` et `$928E-92A6` tampons de sprites, `$07C5-07CD`, `$8216-823D` listes en cours de
remplissage, `$F921`, `$04AF`). Dans chaque cas l'état est identique à l'échantillon suivant : c'est la phase du thread principal à l'intérieur de la
trame (un tampon vu avant/après son remplissage, un minuteur à ±2), pas un décalage de livraison d'IRQ ni une dérive. Reste de la classe « glissement
de phase sous-trame » ; aucune action sur la logique des V-int/H-int n'est justifiée par ces échantillons.

#### SoR 1064M après le H-int de la dernière ligne (20 novembre 2026)

Les 556 échantillons du dump Gens `sor2pf_gens.dsy` (V-int 62-701, film `aka,mukki` 1064M, RAM `$0000-$FAFF`) retrouvent chacun un échantillon du
noyau (`--replay-only`, `DP_TAS_PAD_SHIFT=1`) avec **0 octet de différence**, y compris la trame 92 (livraison de Start) qui divergeait avant le
correctif. Observation de l'outil sur cette plage ; la suite du film n'a pas de dump Gens.

#### Ristar 1506S après le H-int de la dernière ligne (20 novembre 2026)

Dump Gens-rr `gens_ristar.dsy` (3 398 V-int, film `UpthornRistarV2`, RAM `$0000-$FAFF`) contre le noyau en `--replay-only` (réglages Gens), appariement
glissant : 3 065 échantillons sur 3 398 identiques à l'octet, aucun écart au-dessus de 200 octets, aucune dérive durable ; les écarts (1 à 100 octets, par
paquets : 37-54, 389, 429-435…) se résorbent d'eux-mêmes (phase sous-trame du thread principal). Observation de l'outil.

#### SoR 2 joueurs (1845S) : début de la désynchro vers le V-int 20 270 (20 novembre 2026)

Dumps Gens-rr (`gens_dsy_vint.lua`, fenêtre à 1 échantillon par V-int) contre le noyau (`--replay-only`, `DP_TAS_PAD_SHIFT=1`) : identiques à l'octet (au décalage
constant de numérotation près, 3 V-int entre les deux compteurs) du V-int 20 100 au V-int 20 266. À l'échantillon Gens 20 267, un seul octet diffère : le drapeau
`$FFFA00` (Gens 1, noyau 0), le même que dans la trame 92 de 1064M. L'échantillon suivant montre le noyau en retard d'un V-int sur les compteurs de `$F04E-$F1FE` (écart
d'un pas) puis les différences grandissent (16 octets, 100, 250 en 30 V-int) : le thread principal du noyau finit son travail de la trame après le V-int
alors que Gens l'a fini avant (trame en dépassement). Même classe de défaut que Ristar/Castle ; non corrigé. Observation de l'outil.

Complément (PC interrompus, mêmes fenêtres) : jusqu'à l'échantillon Gens 59 les deux machines interrompent le thread principal aux mêmes PC de la boucle
d'attente (`$1050C/$10510/$1051E/$10522`, compteur `d0` identique 48, 45, 42, 3F…). À l'échantillon 60 Gens est encore dans la boucle d'attente (`d0`=3F)
tandis que le noyau est à `$17FC6` (`tst.b $FA5A`, tête de la routine par trame, `d0`=13C) : le noyau a fini le travail de la trame **quelques cycles plus tard**
que Gens, juste après le V-int au lieu d'avant. C'est une marge de fin de trame (slack) trop courte côté noyau, la même signature que Sonic 1843S (slack
minimal 2 contre 19). Il faut une comparaison du nombre de cycles de la trame (trace d'instructions de la trame 20 266 dans les deux émulateurs) pour trouver
l'instruction ou le temps d'attente (VDP, DMA, rafraîchissement) qui diffère ; non fait.

Essais de réglages sur la fenêtre SoR 1845S (V-int 20 205-20 305, `DP_MD_GENS_BUSREQ`, `DP_MD_DMA_REFROUND`, `DP_MD_DMA_NOPHASE`) : résultat identique
à la référence (premier écart > 8 octets au même échantillon, 25 PC sur 94 égaux) : ces réglages ne touchent pas la cause. Elle est donc ailleurs que dans le calage DMA
(coût d'instruction ou attente de bus du travail de la trame) ; piste suivante : trace d'instructions de la trame avec cycles cumulés des deux côtés.

Marge de fin de trame SoR 1845S (itérations de la boucle d'attente `$10510` par V-int, Gens `registerexec` contre `DP_MD_PCLOG`, échelle ×20 côté Gens) : du V-int 20 255
à 20 266 les deux séries concordent à une itération près (≈ 27, 34, 38, 43 côté Gens ; 28, 34, 38, 44 côté noyau, noyau décalé de 1 V-int), puis Gens tombe à ≈ 0,65 itération
au V-int 20 267 où le noyau tombe à 0 : le noyau a une marge inférieure d'au plus une itération (≈ 22 cycles) à ce moment critique, ce qui suffit à renvoyer la
fin du travail après le V-int. `MD_IRQ_IMMEDIATE=0` n'y change rien ; `DP_MD_VINT_DELAY` (12/24/40) casse la numérotation des échantillons (dsy vide), à ne pas utiliser pour ce test.
Le décalage de l'ordre de 20 cycles par trame est du niveau du coût d'une instruction ou d'une attente de bus isolée : il faut les cycles cumulés par instruction de la trame 20 266 des deux côtés.

Sonde de durées instruction par instruction (Gens `-lua` contre noyau, ROM de test générée, boucle `instruction + addq.l + bra.w`, nombre d'itérations par trame après 90 trames ; cycles = 127 900 / itérations − 18).
Identiques des deux côtés : `nop` 4, `lea` 8, `btst #n,Dn` 10, `btst Dn,Dn` 6, MULU/MULS (valeurs dépendant des données). Seul écart relevé jusqu'ici : `BCLR/BSET/BCHG` sur Dn, Gens
(Starscream) paye toujours le prix « bit long » (+2) même pour un bit < 16 (`bclr #3,d2` 14 contre 12, `bset`/`bchg` 12 contre 10, `bclr Dn,Dn` 10 contre 8, `bset Dn,Dn` 8 contre 6) ;
c'est ce que couvre `DP_MD_STAR_TIME`, donc compatibilité Gens et non fidélité matérielle (m0boy suit le manuel : +2 seulement pour un bit ≥ 16). La base de la boucle diffère
de 5 itérations par trame (≈ 110 cycles : 5 813 noyau, 5 808 Gens), à expliquer (coût d'entrée de V-int / découpage par ligne). Sonic 1 REV00 contre s1disasm après le correctif H-int : 75 740 / 89 880 octets (84,3 %) exécutés, contre 56 676 (63,1 %).

Sondes CLR/TAS et synchro SoR 1845S (observations de l'outil) : sans quirks, `clr.b/w abs.w` coûte 16 côté noyau contre 14 chez Gens, `clr.l Dn` 6 contre 8, `tas abs.w` 18 contre 22 ;
`DP_MD_STAR_TIME` couvre ces trois écarts (compatibilité Starscream, pas fidélité matérielle). Avec `DP_MD_STAR_TIME/DIV/ADDQ`, `DP_MD_PAD_LINE0=1`, `DP_TAS_PAD_SHIFT=1` les trames 3 à ~120 sont
identiques à Gens (0 octet de diff, étiquette noyau = étiquette Gens + 1). Le diagnostic `DP_TAS_PAD_SHIFT_AT=F` + `DP_TAS_PAD_SHIFT2=k` (décalage d'entrée changé à la trame F) donne 0 octet de diff
de la V-int 8 414 à ~10 864 (≈ 2 450 V-ints) avec F=9000, k=0 ; la règle physique du décalage reste à expliquer. Nouvelle rupture aux V-int 10 866-10 878 (drapeau `$FFFA00`, marge de la boucle d'attente) :
cause présumée la durée de ligne (Gens 488 cycles, noyau 488,57). Expérience négative : déplacer la ligne de latch d'entrée (0, 2, 15) casse la scène entière.

#### SoR 1845S : profil `DP_MD_GENS=1`, ligne de 488 cycles, ADDQ matériel (21 novembre 2026)

Observations de l'outil (comparaison au dump Gens-rr `s20.dsy`, un échantillon tous les 14 V-int, RAM `$0000-$FAFF`, depuis le premier V-int). Trois corrections cumulées font passer la
synchro exacte de ~25 V-int à ~10 850 V-int : (1) ligne de 488 cycles (`DP_MD_CLOWN_CLOCK=3416`, 3416/7 = 488 exactement ; Gens arrondit la ligne à 488 là où le matériel fait 488,57 :
compatibilité Gens, pas fidélité matérielle) ; (2) `DP_TAS_PAD_SHIFT=0` (le décalage d'entrée de 1 trame compensait la durée de ligne ; il n'est plus nécessaire, le diagnostic
`DP_TAS_PAD_SHIFT_AT` n'est plus utile) ; (3) **sans** `DP_MD_STAR_ADDQ` (ADDQ.W An à 8 cycles, valeur matérielle) : avec ADDQ=4 la synchro s'arrête à ~6 290 V-int. Les anciens
réglages faisaient donc compenser des coûts faux les uns par les autres. Le profil `DP_MD_GENS=1` (dans `dp_mdmach_reset`, chaque réglage reste surchargeable par l'environnement) pose :
ligne 3416, `CLOCK_OFF=0`, `PAD_LINE0`, `GENS_DMA`, `POWERON=gens`, `MD_IRQ_IMMEDIATE`, `STAR_TIME`, `STAR_DIV`. Sonic 1843S garde le même premier écart (trame 8 360) avec ou sans ligne à 488.
Résultat SoR avec le profil : échantillons exacts de 0 à 774 (V-int ~10 850) avec des écarts isolés de 1 à 39 octets sur un échantillon (phase sous-instruction du thread principal), puis rupture
au V-int 10 865 : le noyau est encore dans une routine appelée par la boucle d'attente (`$03B2`) quand Gens est déjà à `$10510` (écart de phase de quelques cycles à l'arrivée du V-int,
drapeau `$FFFA00` 1 chez Gens, 0 chez le noyau). Piste en cours : sonde de durées étendue (`probe/gen2.py`, ~530 instructions × modes d'adressage).

Balayage de durées étendu (`probe/gen2.py`, 533 sondes exploitables : MOVE/ADD/SUB/AND/OR/CMP/EOR, MULU/MULS, DIVU, CLR/NEG/NEGX/NOT/TST, ADDQ/SUBQ, ADDI…CMPI, décalages mémoire, BTST/BSET, Scc, TAS, LEA/PEA, MOVEQ, MOVEA, LINK/UNLK, ABCD/SBCD/NBCD, décalages registre, MOVEM, en .W/.L × les 12 modes d'adressage), noyau contre Gens, observations de l'outil :
seuls 24 écarts de plus de 0,6 cycle, **tous déjà classés** : `CLR.W` mémoire −2 (Gens), `TAS` mémoire +4, `DIVU` constant 133, `DBT` 12 contre 16 (Starscream : `DP_MD_STAR_TIME/DIV`) ; `MULU/MULS (d8,An,Xn)` et `(d8,PC,Xn)` varient avec la donnée lue
(adresse différente d'un côté à l'autre dans la sonde, pas un écart de coût) ; `BSET/Scc (An)+` : sonde invalide côté Gens (compensation `lea` à revoir). Aucun écart matériel : le coût des instructions n'explique donc pas la rupture au V-int 10 865 de SoR 1845S ; il reste les
attentes de bus (VDP, Z80, DMA), l'entrée/sortie d'interruption et le découpage par ligne.

Ablation des compensations du profil `DP_MD_GENS=1` (SoR 1845S, 840 échantillons contre `s20.dsy`, observations de l'outil ; les réglages `DP_MD_*` sont testés par `getenv != NULL`, donc `=0` ne les désactive pas : il faut les retirer de l'environnement) :
référence 78 échantillons écartés, rupture soutenue à l'échantillon 775 ; sans `GENS_DMA` : écart dès l'échantillon 1 (indispensable) ; sans `PAD_LINE0` : écart dès l'échantillon 1 (indispensable) ; sans `STAR_TIME` : 395 écartés, rupture soutenue à 450 (nécessaire) ;
sans `STAR_DIV` : 77 écartés (aucun effet mesurable sur SoR, conservé pour Sonic) ; `CLOCK_OFF` sans effet avec la ligne à 3416. Aucune compensation n'est donc devenue inutile : la ligne de 488 n'a pas remplacé le modèle DMA ni le verrou d'entrée de Gens.

Contrôle Ristar JP 1506S sous `DP_MD_GENS=1` (3 398 échantillons contre `gens_ristar.dsy`, recherche de l'échantillon noyau le plus proche dans une fenêtre de 60 ; observations de l'outil) : ancien réglage (ligne 3420) 2 887 échantillons écartés, premier écart de plus de 20 octets à l'échantillon 37 ; profil Gens (ligne 3416) 2 870 écartés, premier écart de plus de 20 octets à l'échantillon 501. Le profil ne régresse donc pas Ristar et repousse le premier écart important ; la dérive de Ristar (RNG, phase des trames) reste à traiter séparément.

Vérification du balayage de durées sous le profil (`runn.sh` utilise désormais `DP_MD_GENS=1`, décalage 0) : `nop` 5 808 itérations (= Gens), `CLR.W (An)` 4 563 / 4 564, `TAS (An)` 3 550 / 3 549, `DBT` 4 260 / 4 259, `DIVU Dn` 846 / 847, `DIVU` avec dépassement 825 itérations soit 137 cycles (= Gens, qui n'abrège pas le dépassement) : les écarts du balayage sont tous annulés par le profil, à ±1 itération près. Les sondes `MULU/MULS (d8,An,Xn)` signalées plus haut lisaient une adresse impaire (artefact de sonde).
Interrupteurs DMA/bus sans effet sur SoR 1845S (runs identiques à la référence, 78 échantillons écartés, rupture soutenue à 775) : `DP_MD_GENS_BUSREQ`, `DP_MD_DMA_REFROUND`, `DP_MD_DMA_NOPHASE`, `DP_MD_DMA_VBLANK37`. Les écarts isolés avant la rupture portent sur les compteurs de marge `$FFFA02/03` (±1 à 3 itérations de la boucle d'attente, de signe variable) ; à l'échantillon 776 le drapeau `$FFFA00` diffère (1 chez Gens, 0 chez le noyau), puis la RAM diverge. Le coût des instructions étant épuisé, la piste restante est le travail de fin de trame lié aux accès VDP/DMA et à la livraison de l'interruption.

Contrôle SoR 1064M (film `aka,mukki`, 556 échantillons Gens `sor2pf_gens.dsy`, recherche dans une fenêtre de 60 ; observation de l'outil) : ancien réglage, `DP_MD_GENS=1` décalage 0 et `DP_MD_GENS=1` décalage 1 sont tous exacts sur les 556 échantillons (0 écart). Pas de régression ; le dump Gens est trop court pour départager les réglages (il s'arrête avant le V-int ~700 où l'on bute d'ordinaire).

Piste DMA écartée (SoR 1845S, décalage de ±1, 2 ou 4 cycles ajouté au gel du 68000 à chaque début de DMA, expérience retirée ensuite) : ±1 et ±2 dégradent nettement (rupture soutenue à 682 et 450 contre 775 en référence), ±4 ne change pas la rupture ; la durée du gel DMA du modèle Gens est donc au bon cycle près. Trace d'instructions des V-int 10 862 à 10 865 (noyau, profil Gens) : l'image 10 864 est une image lourde (24 313 instructions, 127 799 cycles) qui se termine encore dans une routine (`rts` en `$07EE`, appelée depuis la boucle principale `$03B2`) quand le V-int suivant arrive, alors que Gens a déjà posé le drapeau `$FFFA00` et attend en `$10510` : le travail de cette image est au moins ~70 cycles plus long côté noyau, sans cause isolée à ce stade.

Sondes complémentaires (`probe/gen3.py`, `gen4.py`, noyau sous `DP_MD_GENS=1` contre Gens ; observations de l'outil) : `Scc` (vrai/faux, Dn et mémoire), `DBcc` (expiré, pris, condition vraie), boucle `DBRA`, `MOVEM` (1, 2, 4, 8, 15 registres, mots et longs, stockage, prédécrément, chargement) hors les chargements qui écrasent `d6`/`a1` (sondes invalides), `BSET/BCLR/BCHG/BTST` (registre et immédiat, bit 3 et bit 20), décalages par registre (compte 20), `MULS/MULU` ($AAAA, $8000), et les accès VDP (données, contrôle, HV), RAM et ROM Z80, YM, PSG, manettes, `busreq` : toutes les durées sont identiques à Gens (au plus ±0,2 cycle). Les durées d'instructions et d'accès ne sont donc pas la cause de la rupture de SoR 1845S au V-int 10 865.
Balayage de `DP_MD_VINT_DELAY` (2, 4, 6, 8, 12, 16 cycles de retard à la levée du V-int) : 0 et 4 donnent le même résultat (rupture soutenue à l'échantillon 775), 2, 6, 8 la dégradent (450, 606, 489) et 12, 16 cassent dès le début : le point d'entrée du V-int du modèle est un optimum local. La cause de la rupture reste à trouver hors des coûts d'instructions, du gel DMA et de l'instant de levée du V-int ; la piste suivante est la granularité des tranches d'exécution (reprise mi-ligne) dans une image lourde.

Sondes mémoire vers mémoire (`probe/gen5.py`, `gen5b.py`) : `MOVE.b/w/l` entre `(An)`, `(An)+`, `-(An)`, `d16(An)`, `d8(An,Xn)`, absolu court/long et immédiat, plus `ADDX/SUBX` (`-(Ay),-(Ax)` et `Dy,Dx`), `CMPM`, `MOVEP`, `MOVE` depuis SR/vers CCR/USP, `ANDI/ORI/EORI` vers SR et CCR, `CHK`, `TRAPV`, `EXT`, `ROXL/ROXR`, `ASL/ROL` mémoire, `MOVEM` en `d16`/absolu/PC, `Bcc` court/long pris et non pris, `CMPA/ADDA/SUBA`, `ADDQ/SUBQ` sur Dn/An : toutes les durées du noyau sous `DP_MD_GENS=1` sont celles de Gens (au plus ±0,6 cycle sur 203 sondes), à l'exception des 16 sondes dont la destination est `-(An)` : Gens donne 0 (le registre de destination dérive à travers le miroir RAM jusqu'au journal de la sonde), elles sont donc invalides et non concluantes. Les coûts d'instructions ne sont toujours pas la cause de la rupture de SoR 1845S au V-int 10 865.

Image lourde 10 864 de SoR 1845S, instructions à temps dépendant de la donnée : la trace du noyau montre que 10 864 est la première image qui exécute des `DIVU` (3), avec 23 `MULS`, 6 `MULU` et 216 `MOVEM` (10 862 et 10 863 n'en ont aucun). `DP_MD_STAR_DIV=n` règle désormais le coût de `DIVU` (défaut 133, `DIVS` n+17). Avec n = 125, 133, 145 et 173 le drapeau `$FFFA00` à 10 864 reste à 0 (Gens : 1) et les 45 octets écartés de 10 865 sont les mêmes : la division n'est pas la cause. Le noyau est dans la fourchette ±1 itération de la marge de Gens sur les images légères 10 830–10 863 (les 45 octets de 10 865 sont les premiers écarts). Piste suivante : MULS/MULU sur des opérandes de cette image, ou la durée des `MOVEM` à 216 occurrences (registres larges).

Départ de DMA 68000→VDP dans Gens-rr (lecture de `vdp_io.asm`, fin de boucle DMA) : la routine appelle `Update_DMA` en ignorant le coût renvoyé, puis `main68k_releaseCycles`. Deux traductions testées dans le noyau contre `s20.dsy` (première image différant de plus de 8 octets) : (1) premier tronçon gratuit et reste de la tranche consommé : échantillon 7 (image 98) ; (2) premier tronçon gratuit sans libération : échantillon 7 aussi ; le modèle actuel (premier tronçon facturé tout de suite) tient jusqu'à l'échantillon 335 sous ce même critère. Les deux variantes sont retirées : la lecture du source ne se transpose pas telle quelle (le comportement réel de `releaseCycles` de Starscream, binaire, n'est pas lisible).

Lecture du drapeau `$FFFA00` à 10 864 : Gens a `01` (le fil principal a fini son travail avant le V-int et attend), le noyau `00` (encore occupé), puis le cycle `0, 1, 2` du drapeau est décalé d'une image dans le noyau (10 865 : `01`, 10 866 : `02`, 10 867 : `00` ; Gens : 10 865 `02`, 10 866 `00`). C'est donc une course serrée : la marge du fil de Gens sur cette image lourde est positive mais faible et le noyau la perd. Elle n'est pas mesurable directement : `memory.readword(0xC00008)` de Lua lit la valeur brute (0) et non le compteur HV du VDP, et Gens-rr n'expose ni compteur de cycles ni deuxième point d'arrêt de mesure. Les instructions nouvelles de cette image (187 opcodes absents de 10 862–10 863, dont 3 `DIVU`, 23 `MULS`, 6 `MULU`, ~65 `BSR`) sont toutes dans les classes déjà sondées, sauf `BSR`/`RTS`/`LINK` qui n'ont pas de sonde propre.

Sondes `BSR.b`, `BSR.w`, `JSR d16(PC)`, `JMP d16(PC)`, `PEA`+`RTS` (`probe/gen6.py`) : durées identiques à Gens (±0,1 cycle). La sonde `LINK`/`UNLK` est invalide (le noyau renvoie 0). Les classes d'instructions de l'image lourde 10 864 de SoR 1845S sont désormais toutes sondées et concordantes : la rupture ne vient pas d'un coût d'instruction.

Sondes `gen7` (`probe/gen7.py`) : `LINK`/`UNLK` (corrigée : An = a3, la première sonde écrasait le journal en a5), `MULU`/`MULS` sur sept opérandes (`$0001`, `$00FF`, `$0F0F`, `$1234`, `$AAAA`, `$8000`, `$FFFE`), `MOVEM` écriture (2, 8, 15 registres, mot/long, pré-décrément) : durées identiques à Gens (±0,2 cycle). Les sondes `MOVEM` lecture restent invalides (le chargement écrase le compteur). Les coûts d'instructions de l'image 10 864 sont donc tous concordants : la cause restante est dans l'ordonnancement (IRQ/DMA/tranches), pas dans les durées. Observations de l'outil.

Sondes `gen8` : `MOVEM` lecture (2 et 10 registres, mot/long, post-incrément, `d16(An)`, `d16(PC)`) valides cette fois (masque sans d6/a1/a4/a5) : durées identiques à Gens (±0,1 cycle). Piste IRQ : l'acquittement `Int_Ack` de Gens (`H_Ack`) efface tout `VDP_Int`, y compris un V-int en attente quand le registre 1 le désactive ; l'essai dans le noyau (`irq_pending = 0` à l'acquittement d'un niveau 4 avec V désactivé) ne change rien (premier écart à l'échantillon 335, identique à la base), retiré. Les coûts d'instructions et cette sémantique d'acquittement sont donc écartés pour 10 864. Observations de l'outil.

#### SoR 1845S : première divergence de flux d'instructions à 10 864 (21 novembre 2026)

Trace des PC par instruction côté Gens-rr (script Lua `registerexec` sur les 2 454 adresses vues dans la trace du noyau, une fermeture par adresse : `main.pc` lu dans le hook est faux, il renvoie une adresse voisine) comparée à la trace du noyau (`DP_MD_PCLOG_V`), image lourde (V-int noyau 10 864 = trame Gens 10 861, décalage de numérotation 3). Le flux du fil principal est identique pendant 199 instructions après la sortie du V-int, puis diverge en `$DCC2` (`btst #1,$31(a0)` ; bit à 1 chez Gens, 0 chez le noyau) ; avant cela, le gestionnaire de V-int lui-même diverge à l'instruction 273 : en `$72A60` (`move.b d(a3),d0 ; cmp.b d(a3),d0 ; beq`, pilote sonore, `a0=$FFF00D`) le noyau prend le saut, Gens non. C'est donc une lecture d'état RAM du pilote sonore (octets comparés de a3) qui diffère au moment du V-int : une écriture du fil principal ou du gestionnaire précédent est vue par un côté et pas par l'autre. Piste suivante : identifier qui écrit ces deux octets de la structure a3 et à quelle instruction du flux (Gens contre noyau). Observation de l'outil ; trace Gens dans `sor-sync/gpc.txt`.

Correction (même jour) : l'appariement des gestionnaires de V-int entre la trace Gens et celle du noyau est incertain (meilleurs rapports de séquence 0,83–0,93 seulement, la trace Gens ne couvre que les 2 454 adresses vues côté noyau). Le compteur du pilote sonore (`a3 = $FFF130`, octets `$F13D`/`$F13E` : 1 / 10, 9, …, 1, `$24`, `$23`…) est **identique** dans les dumps Gens et noyau à tous les V-int 10 855–10 870 : la divergence en `$72A60` ne vient pas de cet état. La conclusion « le pilote sonore diverge » est donc à ne pas retenir ; seule la divergence du fil principal (`$DCC2`, bit 1 de `$31(a0)` sur l'objet) reste une observation, à rapporter au bon V-int. La trace `DP_MD_PCLOG` affiche désormais aussi `a3`.

Trace Gens-rr complète (points d'arrêt sur toutes les adresses paires de `$0–$1FFFF` et `$72000–$73FFF`, 10 trames, ~11 600 instructions par trame) comparée à la trace du noyau sans filtre : pour chaque V-int du noyau (10 863, 10 864) le V-int Gens qui partage le plus long préfixe d'instructions diverge dans le pilote sonore, au V-int 10 863 en `$72C10` (`bne` : le noyau ne saute pas, Gens saute, au bout de 662 instructions) et au 10 864 en `$72A60` (`beq` : le noyau saute, Gens non, au bout de 273). Les deux sont des branches d'état du pilote (compteur de canal `$FFF13E` contre `$FFF13D`) alors que les dumps de WRAM aux mêmes V-int sont identiques : soit la numérotation des V-int comparés est décalée d'un tick du pilote entre les deux machines, soit les codes de condition au moment du branchement diffèrent. À trancher en traçant le CCR (SR) du noyau aux branches `$72A60`/`$72C10` et en lisant `$F13E` côté Gens à l'entrée de chaque gestionnaire de la trace. Observation de l'outil.

Précision (même jour) : côté Gens-rr le numéro de V-int du dump vaut le numéro de trame de `gens.emulateframe` moins 12 (le compteur du pilote sonore `$FFF13E` décroît d'un par V-int : valeur 13 à la trame 10 863, 1 à la trame 10 875 environ), de sorte que les fenêtres de trace Gens 10 858–10 867 et 10 870–10 879 ne correspondent pas aux V-int noyau 10 862–10 865 par leur numéro. L'appariement par plus long préfixe commun retombe sur la période de 3 des trames lourdes et ne désigne pas le bon V-int. En revanche la première branche du gestionnaire (`$19D2A` : `move.b $FFFA00,d1 ; beq`, puis `cmpi.b #2,d1 ; beq` en `$19D3A`) diverge dès l'instruction 3 à 9 selon le V-int apparié, avec la valeur de `$FFFA00` à l'entrée (0, 1, 2) : c'est la même course de phase du fil principal déjà observée, pas une divergence du pilote sonore. Pour trancher il faut apparier par la valeur de `$FFF13E` à l'entrée du gestionnaire (côté noyau en ajoutant la lecture d'octet à la trace `DP_MD_PCLOG`). Observation de l'outil.

Appariement par `$FFF13E` (trace `DP_MD_PCLOG` du noyau enrichie de `e=` = octet `$FFF13E` à l'entrée du gestionnaire). Le compteur décroît d'un par V-int dans les deux machines, mais son rechargement diffère : noyau 0x0B → 0x01 au V-int 10 865 puis rechargé à **0x24 (36)** ; Gens 1 (trame 10 858) puis rechargé à **18 (0x12)**. Le pilote sonore recharge donc une durée de note/tempo différente : l'état de lecture du pilote (séquence, valeur lue dans les données de la musique, ou réponse du Z80/YM lue avant) a déjà divergé avant 10 864. À remonter : tracer côté noyau et côté Gens l'écriture de `$FFF13E` (rechargement) et la valeur source lue. Observation de l'outil, non une mesure matérielle.

Trace des écritures du pilote sonore (noyau `DP_MD_WLOG=fff13e:fff13e`, Gens `memory.registerwrite`) : les deux lisent le même flux de séquence en ROM (`$7ED50` : `fb d8 8b f0 01 24 00 12 00 f0`), mais pas au même moment. Le noyau recharge `$FFF13E` à `0x24` (octet `$7ED55`) au V-int 10 865 (PC `$72AB2`, `move.b d5,$e(a3)`, d5 lu par `move.b (a4)+,d5`) ; Gens recharge à 18 (`$7ED57`) à la trame 10 858, soit une note plus loin dans le flux et ~19 ticks plus tôt. Le pointeur de flux `$FFF136` du noyau est à `$ED53` au V-int 10 854 alors que Gens est déjà à `$ED57` à la trame 10 850 : le pilote sonore du noyau est en retard d'environ une note sur Gens, accumulé avant 10 840. La cause est donc plus ancienne (début de la note/piste, déclenchée par le fil principal ou la commande Z80) ; il faut remonter les écritures de `$FFF134/$FFF136` vers l'arrière jusqu'à la première divergence de pointeur. Observation de l'outil.

Pointeur de flux `$FFF136` côté noyau, V-int 9 000–10 840 (`DP_MD_WLOG=fff134:fff137`) : la séquence musicale boucle avec une période exacte de 144 V-int (`$ED45` aux V-int 9 083, 9 227, … 10 523, 10 667, 10 811 ; `$ED57` à 9 137, 9 281, …), toutes les écritures venant de `$72BC8`/`$72A64`. Gens franchit `$ED57→$ED59` à la trame 10 858 (V-int ≈ 10 846 après le décalage 12) alors que le noyau le franchit à 10 757 et 10 901 : la phase de boucle diffère de 55 (ou 89) V-int. Or les comptes de `$F13E` concordaient aux dumps antérieurs : l'appariement trame↔V-int Gens utilisé pour comparer les pointeurs reste à vérifier (la trame Gens peut compter les trames lourdes autrement). Prochaine mesure : écrire côté Gens le pointeur `$FFF136` sur la même plage 9 000–10 840 (`memory.registerwrite` sur `$FFF136`) et comparer les instants de franchissement de `$ED45`. Observation de l'outil.

Correction et mesure (Gens `memory.registerwrite` sur `$FFF136`, trames 9 000–10 860) : la séquence sonore est en phase entre Gens et le noyau. Les instants de franchissement des mêmes positions du flux diffèrent d'un décalage constant noyau − trame Gens = 42 (soit −102 modulo la période 144), à ±1 près (ED45 : 42 sur les six dernières boucles ; ED47 : 42, 42, 42, 42, 43, 43 ; ED59 : −102 … −102, −101, −101). Le compteur a donc glissé d'un tick sonore dans les deux dernières boucles (après ≈ V-int 10 500), pas d'une note : l'hypothèse « noyau en retard d'une note » de la section précédente est fausse (elle comparait des positions d'un autre point de la boucle). Le décalage numérique 42 (et non 12) vient du comptage des V-int noyau depuis le début du film, trames légères comprises. Le glissement d'un tick coïncide avec la course de phase du fil principal déjà documentée ; il reste à repérer le V-int exact où ED47 passe de 42 à 43 (entre 10 541 et 10 685 côté noyau) en comparant les instants de franchissement un par un. Observation de l'outil.

Localisation du glissement d'un tick : décalage noyau − Gens à ED47 = 42 jusqu'au V-int 10 541 puis 43 à 10 685 ; à ED59 = −102 jusqu'à 10 469 puis −101 à 10 613. Le glissement se produit donc dans la boucle musicale qui commence au V-int noyau 10 523 (entre 10 541 et 10 613, soit trames Gens ≈ 10 499–10 571), environ 300 V-int avant la rupture de FA00 à 10 864. Les franchissements intermédiaires (ED53) oscillent de ±1 d'une boucle à l'autre (jitter d'échantillonnage de la trame lourde), donc seule la comparaison aux points ED47/ED59 est fiable. Prochaine mesure : tracer dans cette fenêtre le nombre de V-int livrés par trame (noyau) contre `gens.emulateframe` pour voir si le noyau livre un V-int en moins/plus. Observation de l'outil.

Écritures de `$FFF13E` dans la fenêtre du glissement (noyau V-int 10 520–10 620 ; Gens trames 10 481–10 575) : les rechargements successifs du compteur sont, côté noyau, 18, 13, 5, 6, 11, 36 (avec un maintien à 1 sur deux V-int en 10 558–10 559) ; côté Gens 12, 5, 6, 11, 35/36. Les intervalles entre rechargements (13, 6, 6, 11 contre 12, 6, 6, 12) ont la même somme (36) : un seul bord de tick est décalé d'un V-int à l'intérieur d'une séquence qui se referme à l'identique, ce n'est pas un V-int perdu ou ajouté. Le hook Gens-rr `registerwrite` laisse tomber des trames (10 492, 10 498, 10 510, 10 516, 10 528, 10 534, 10 546 manquent), donc la valeur observée à une trame manquante est inférée ±1 : le « glissement d'un tick » de la section précédente est du même ordre que cette incertitude d'échantillonnage. Conclusion : le pilote sonore n'est pas démontré divergent avant 10 864 ; la rupture reste la course de phase du fil principal (`$FFFA00`) déjà documentée. Observation de l'outil.

Écritures de `$FFFA00` autour de la rupture (noyau V-int 10 850–10 870 par `DP_MD_WLOG=fffa00:fffa00` ; Gens `memory.registerwrite`, trames 10 800–10 830) : les deux machines montrent le même cycle de trois trames. Le fil principal écrit 1 (`$10502`) dans la première trame, le gestionnaire de V-int l'efface (`$19D22`), le fil principal écrit 2 (`$10514`) dans la trame suivante, nouvel effacement, puis une trame sans écriture ni effacement (côté noyau : V-int 10 854, 10 857, 10 860, 10 863… sans ligne `$19D22`). Côté Gens la même séquence 1,0,2,0,(rien) se répète sans exception de 10 800 à 10 830 (une trame sur trois muette : 10 802, 10 805, …). Aucune rupture de ce cycle n'apparaît dans la fenêtre noyau 10 850–10 870 : la « rupture à 10 864 » (noyau FA00=0, Gens=1 dans les dumps) correspond à un instant du cycle, pas à une écriture manquante, et elle peut venir de la phase du cycle de trois (dump pris sur une trame muette d'un côté et pas de l'autre : décalage 42 ≡ 0 mod 3, mais 12 ≡ 0 mod 3 aussi, ce qui ne les distingue pas). À faire : comparer le contenu complet de la WRAM, trame Gens et V-int noyau appariés par le décalage 42, plutôt que par 12 comme dans les dumps précédents. Observation de l'outil.

Comparaison WRAM complète, V-int par V-int (dump Gens `e1083.dsy` contre dump noyau `DP_MD_DSY_FROM/TO=10830:10880`, profil `DP_MD_GENS=1`) : la numérotation des deux dumps est la même (échantillon au vecteur de V-int, f = V-int ; le décalage 42 ne concerne que les trames `gens.emulateframe`, pas ces dumps). De 10 830 à 10 863 la seule différence est la zone de pile/volatile `$FFFE74…` et `$FFFF44` (état propre à chaque machine, y compris les images « lourdes » sans écart d'objets). **Le premier octet de jeu qui diffère est `$FFFA00` à l'entrée du V-int 10 864 : Gens 1, noyau 0.** À l'entrée du V-int 10 863 les deux valent 2. Le fil principal de Gens a donc exécuté son `move.b #1,$FA00` (`$10502`) avant le V-int 10 864, celui du noyau seulement après (écriture à v=10 865 ligne 82 d'après `DP_MD_WLOG`) : à ce V-int, le fil principal du noyau a une trame de retard sur celui de Gens. Les états RAM des objets (`$B801…`) divergent ensuite (126 octets à 10 865, ~300–500 ensuite) comme conséquence. Le décalage est donc de moins d'une trame de travail du fil principal entre deux V-int (marge/slack du noyau plus faible que celle de Gens, déjà mesurée 2 contre 19 à 37 316 sur Sonic) : c'est la cause directe de la rupture, la source précise (quelle section du fil principal consomme plus de cycles côté noyau, ou quelle interruption plus longue) reste à isoler en comparant les compteurs de cycles par V-int entre `pa3.txt` (noyau : 127 854 cycles/V-int, `cyc=`) et la durée de la trame Gens. Observation de l'outil.

Cycles du fil principal autour de `$FFFA00` (noyau, `DP_MD_WLOG` avec `cyc=`, profil Gens, V-int 10 860–10 867) : entre l'effacement par le V-int (`$19D22`) et l'écriture suivante de 1 (`$10502`), le fil principal consomme 139 839 cycles pour le cycle commençant au V-int 10 861 (écriture au V-int 10 862, ligne 24) et 167 771 cycles pour celui du V-int 10 864 (écriture au V-int 10 865, ligne 82), contre 127 854 cycles par V-int. Dans le second cycle l'écriture passe donc 40 000 cycles après le début de la trame suivante, alors que Gens la fait avant le V-int 10 865 (dump `$FFFA00` = 1 à f=10 864). Le fil principal de Gens termine ce travail en moins d'une trame, celui du noyau en consomme une trame et demie : un écart d'au moins 40 000 cycles (environ 80 lignes) sur ce travail, beaucoup plus que les erreurs de durée d'instruction mesurées (±0,2 cycle). Le travail correspondant est une image lourde (copie/DMA) : la différence de temps vient donc de la prise en compte du DMA (blocage du 68000 par la copie), ou d'une attente (busy VDP, port manette) plus longue côté noyau. Essai : retirer `DP_MD_GENS_DMA` seul du profil Gens décale toute la chronologie (frames 11 049 au lieu de 11 105 à v=10 860, écriture 1 à chaque trame) et ne se compare plus ; le réglage reste nécessaire. À faire : journaliser les transferts DMA (taille, source, cycles de blocage) du noyau entre les V-int 10 864 et 10 865 et les comparer au temps de blocage de Gens (`Update_DMA`, formule de cycles de gelage). Observation de l'outil.

Journal des DMA du noyau (`DP_MD_VDBG=11106-11111`, trames 11 106–11 111 = V-int 10 861–10 866) : trois transferts seulement, tous dans la trame 11 108 (V-int 10 863, lignes 2 à 13, blanking) : 544 mots ROM→VRAM (`$5588A`), 512 mots ROM→VRAM (`$5E1CA`), 320 mots WRAM→VRAM (`$FFDC80`), soit 1 376 mots ≈ 13 à 17 lignes de blocage (~6 à 8 k cycles avec la formule Gens). **Aucun DMA dans les trames 11 109–11 110, où tombe l'écart de 40 000 cycles** (V-int 10 864 → écriture de 1 à 10 865 ligne 82). L'hypothèse « le temps de blocage DMA explique le retard du fil principal » est donc réfutée pour ce cycle : le travail du fil principal entre l'écriture de 2 (V-int 10 863 ligne 92, cyc 1 420 251 056) et l'écriture de 1 (V-int 10 865 ligne 82, cyc 1 420 501 743), soit 250 687 cycles (≈ 1,96 trame), se déroule hors DMA. Restent : durée de ce travail (code copié/décompressé, boucles de remplissage), interruptions plus longues dans ce couple de V-int, ou attentes de matériel (busy VDP/YM, manette). À faire : trace d'instructions du noyau entre ces deux écritures avec cycles cumulés par région de code (`$1xxxx` fil principal, `$19D16` V-int, `$7xxxx` pilote sonore), à comparer à la même découpe côté Gens (`gpc3`/`gpc4` : instructions paires `$0–$1FFFF`, `$72000–$73FFF`). Observation de l'outil.

Découpe des cycles du noyau par région (`DP_MD_PCLOG_V=10863:10865 DP_MD_PCLOG=0:ffffff`, 40 301 instructions) : V-int 10 863 : 117 k cycles de fil principal, 8 k de pilote sonore, 2,5 k de gestionnaire ; V-int 10 864 : 119 k de fil principal, 8 k de son ; **V-int 10 865 : 39 696 cycles de pilote sonore (8 048 instructions) contre 8 k aux V-int voisins**, et seulement 87 k de fil principal. Ces 39,7 k cycles de pilote sonore sont l'ordre de grandeur de l'écart de 40 000 cycles mesuré sur l'écriture de `$FFFA00`. Le code est la routine d'écriture YM2612 (`$73206` : `lea $A04000,a2 ; btst #7,(a2) ; bne ; move.b d0,(a2) ; nop×… ; btst ; bne ; move.b d1,1(a2)`), exécutée **156 fois** dans le seul V-int 10 865 (20 au 10 863, 18 au 10 864) et **sans boucle d'attente** (autant de `btst` que d'entrées : le bit busy n'est jamais vu). Côté Gens (hook `registerexec` sur `$73206`, une valeur par trame) la routine tourne à chaque trame par rafales de 8 à 60 (trames 10 821–10 823 : 8, 60, 60 ; pics 37, 42, 41 ailleurs) : la rafale de 156 écritures YM d'un seul tenant n'existe pas côté Gens, ce qui indique que le pilote sonore du noyau charge en un V-int (envoi d'un instrument complet) ce que celui de Gens répartit sur plusieurs trames, ou que les trames Gens ne s'apparient pas aux V-int (décalage 42 déduit de la musique). C'est la première différence de comportement de code localisée avec une grandeur compatible avec l'écart de cycles. À faire : compter côté noyau les exécutions de `$73206` par V-int de 10 800 à 10 870 et comparer terme à terme avec `gy.txt` (Gens) avec le décalage 42 ; si la rafale de 156 est répartie différemment, remonter à la commande sonore (`$F13D` / commande Z80) qui la déclenche. Observation de l'outil.

Exécutions de `$73206` par V-int, noyau (`DP_MD_PCLOG=73206:73206`, V-int 10 790–10 870) contre Gens (`gy.txt`, trame + 42). Noyau : base 16–22 par V-int et rafales de 94–160 à 10 793 (102), 10 811 (160), 10 815 (106), 10 829 (98), 10 836 (106), 10 841 (94), 10 859 (94), **10 865 (156)**. Gens : base 8–18 et rafales à 10 835+42=10 877 non concernées ; dans la même fenêtre 10 858 (37), 10 840 (42), 10 844 (41), 10 864 (60) et 10 865 (60). Les rafales se suivent à la même cadence (≈ 18 V-int, période du compteur `$FFF13E`) et la plupart tombent sur le même V-int (noyau 10 859 / Gens 10 858+42=10 900 non, voir ci-dessous) ; le rapport noyau/Gens est stable d'environ 2,3 à 2,6 (94 contre 37–42 ; 156 contre 60), ce qui indique une différence de comptage du hook Gens (une exécution sur deux/trois n'est pas déclenchée) plutôt qu'un comportement de pilote différent. **Différence de structure retenue : à 10 864–10 865 Gens a deux rafales consécutives (60 puis 60), le noyau une seule (18 puis 156).** Les 60+60 correspondent à ≈ 156 écritures du noyau à ce rapport, mais réparties sur deux V-int : le pilote de Gens écrit sa première moitié un V-int plus tôt et fait donc tenir le fil principal dans la trame, alors que le noyau fait tout au V-int 10 865 et repousse l'écriture de `$FFFA00` d'une trame. Le glissement d'un tick mesuré sur `$FFF13E` (ED47 : 42 puis 43) est la cause plausible d'un tel décalage d'une rafale. Observation de l'outil ; reste à confirmer que le déclencheur (`$F13D`) change d'un V-int entre les deux machines.

Écritures de `$FFF13D` (noyau `DP_MD_WLOG=fff13d:fff13d`, V-int 10 840–10 870 ; Gens `memory.registerwrite`, trames 10 790–10 830) : **aucune écriture** des deux côtés dans ces fenêtres, l'octet reste à 1 (comme dans les dumps). Le déclencheur de la rafale d'écritures YM n'est donc pas un changement de `$F13D` : la rafale est pilotée par le flux de séquence (octets lus par `move.b (a4)+,d5` en `$72AB2`, pointeur `$FFF136`) et par le compteur `$FFF13E`, c'est-à-dire par la phase de la musique, déjà observée avec un glissement de ±1 tick entre les deux machines. Hypothèse retenue, non démontrée : ce glissement d'un tick déplace d'un V-int l'instant où le pilote lit la commande d'instrument (rafale de 156 écritures) et, comme cette rafale coûte ≈ 40 000 cycles, fait basculer l'écriture de `$FFFA00` d'une trame. Pour la démontrer il faut aligner les instants de la rafale des deux machines à la trame près (hook Gens fiable : points d'arrêt à la place de `registerexec` dans la boucle, qui perd des trames) ou provoquer dans le noyau le décalage d'un tick et vérifier que `$FFFA00` suit. Observation de l'outil.

Correction des comptes `$73206` : la trace `DP_MD_PCLOG` du noyau journalise chaque instruction deux fois (même `cyc`, ligne identique ; vérifié sur le V-int 10 864 : 18 lignes pour 9 exécutions). Les comptes noyau de la section précédente sont donc à diviser par deux : base 8–11 par V-int (Gens 8–18, compatible), rafales 47 (10 859, 10 841), 53 (10 836, 10 815), 49 (10 829), 51 (10 793), 80 (10 811) et **78 à 10 865**. Avec le décalage 43 (et non 42) les rafales noyau/Gens s'apparient une à une : Gens 10 835/10 840/10 858 (37, 42, 37) ↔ noyau 10 836/10 841/10 859 (53, 47, 47) ; à la fin, Gens 10 864 = 60 et 10 865 = 60 contre noyau 10 865 = 78 et 10 866 = 9. **Le calendrier des rafales est donc identique (décalage constant 43), l'hypothèse « glissement d'un tick déplace la rafale » est réfutée.** Ce qui diffère est la forme de la dernière rafale : deux fois 60 côté Gens, une fois 78 côté noyau, c'est-à-dire un travail de pilote sonore (nombre d'écritures YM par V-int) qui n'est pas le même, la rafale « double » de Gens totalisant plus d'exécutions (120 contre 78) que celle du noyau. Un pilote qui fait plus de travail côté Gens est incompatible avec un fil principal plus rapide côté Gens : l'écart de cycles du fil principal ne vient donc pas de ces écritures. Reste à localiser la différence dans le fil principal lui-même (les 22 779 instructions du fil principal au V-int 10 864 contre 5 847 au 10 863). Observation de l'outil ; la trace PCLOG à doublon est un défaut de l'outil de diagnostic, pas du noyau.

Trace des PC du fil principal, Gens (k 10 819–10 824, `gpc5.txt`) contre noyau (V-int 10 863–10 865, `pw10.txt` dédoublonné). Les profils d'instructions s'apparient avec un décalage de 41, non 42 : Gens k=10 823 (396 exécutions de la boucle `$3B98–$3BA8`) ↔ noyau V-int 10 864 (396 exécutions identiques) ; Gens k=10 822 (126 de la même boucle) ↔ noyau 10 863 (132). Le travail du fil principal est donc le même ; la différence est le temps d'attente : Gens k=10 821 passe 4 850 tours dans la boucle d'attente `$1050C–$10510` et k=10 822 3 957 tours en `$1051E/$10522`, alors que le noyau en fait 171 à 10 863 et 181 à 10 865. Le fil principal de Gens est donc largement en avance sur le V-int (il attend), celui du noyau est occupé et rattrape à peine : l'écart de ≈ 40 000 cycles se traduit par une attente dans Gens, pas par un travail supplémentaire dans le noyau. Piste suivante : chercher en amont (k 10 810–10 821) où le noyau cesse d'attendre, c'est-à-dire la première trame où le nombre de tours d'attente diverge. Observation de l'outil.

Tours de la boucle d'attente `$10510`, trame par trame (Gens `gpc6.txt` k 10 700–10 830 + 41 ; noyau `pw13.txt` V-int 10 740–10 872). Les deux machines montrent la même périodicité (un pic toutes les 3 trames, le cycle de `$FFFA00`), mais **les comptes ne sont pas comparables** : Gens 3 000–5 000 tours par pic, noyau 54–133. Cause : `md_full_cpu.c` détecte une boucle d'attente et « dépense le reste du tour d'un coup » (saut d'inactivité), si bien que le PCLOG du noyau ne voit qu'une fraction des tours. La comparaison correcte de l'attente se fait donc en cycles consommés dans la boucle (somme des `cyc` entre l'entrée et la sortie), non en nombre d'exécutions. Le pic du noyau croît lentement avec la trame (≈ 70 → 130 de 10 740 à 10 872) alors que la durée d'attente de Gens reste ≈ constante : à comparer en cycles. Observation de l'outil ; le saut d'inactivité du noyau est un candidat à examiner (il doit rendre exactement le nombre de cycles que la boucle aurait consommés, sinon le fil principal sort de la boucle trop tôt ou trop tard par rapport au V-int).

Essai `MD_NO_IDLE=1` (nouveau commutateur de diagnostic de m0boy : les boucles d'attente sont exécutées instruction par instruction, sans le saut `md_full_idle`) : écritures de `$FFFA00` aux V-int 10 858–10 868 **identiques** à l'exécution normale (10 864 : seul l'effacement à ligne 0, l'écriture de 1 tombe à 10 865 ligne 82). Le saut d'inactivité et son arrondi sont donc **écartés** comme cause de la rupture. Restent comme causes possibles du retard de ≥ 40 000 cycles du fil principal avant 10 864 : un travail du fil principal plus coûteux en cycles dans le noyau (instructions au mauvais coût de cycles, cf. la piste de l'utilisateur sur les anciens correctifs masquants), ou une différence de répartition des cycles entre fil principal et V-int (coût du handler). Observation de l'outil.

Incohérence de décalage relevée (à lever avant toute conclusion) : l'appariement des rafales YM donne un décalage k→V-int de 43, celui des profils de PC du fil principal 41, l'appariement musical 42. Trois décalages pour une même relation trame Gens → V-int noyau signifient qu'au moins un des hooks Gens (`registerexec` sous-échantillonné, perte de trames) ne compte pas les trames comme `gens.emulateframe`. Tant que le décalage n'est pas unique, « Gens fait 60+60 à 10 864/10 865 » et « le noyau 78 à 10 865 » ne se comparent pas à la trame près. Mesure à faire : un seul run Gens qui journalise, par k, à la fois les exécutions de `$73206` et celles de `$1050C` (même trame, même hook), puis comparer avec un seul run noyau au même V-int. Observation de l'outil.

Run Gens unifié (`gpc7.lua`, un seul hook par adresse, k 10 780–10 840) : le vecteur de V-int `$19D16` est exécuté exactement une fois par trame k (aucune trame perdue : la numérotation k est fiable pour ces hooks). Écritures `$FFFA00`=1 (`$10502`) aux k ≡ 10 821 (mod 3) : 10 821, 10 824, … ; rafales YM `$73206` à k = 10 786 (41), 10 788 (40), 10 793 (37), 10 798 (42), 10 802 (41), 10 816 (37), **10 822 (60), 10 823 (60)**, 10 840 (37). Côté noyau, `$FFFA00`=1 aux V-int 10 859, 10 862, 10 865 et rafales à 10 836, 10 841, 10 859, 10 865. En appariant les écritures de `$FFFA00` (k 10 824 ↔ V-int 10 865, décalage 41) les rafales de Gens à k 10 822–10 823 tombent aux V-int 10 863–10 864, soit **1 à 2 V-int avant** la rafale du noyau (10 865) ; en appariant les rafales de la période précédente (k 10 793/10 798/10 816 ↔ 10 836/10 841/10 859, décalage 43) le phasage rafale/`$FFFA00` diffère de 2 V-int entre les machines. Les deux phases (sonore, 144 V-int ; `$FFFA00`, 3 V-int) ne sont donc pas dans la même relation côté noyau et côté Gens, bien que les dumps RAM soient identiques jusqu'à 10 863 : la phase sonore relative à la boucle principale est le point à instrumenter (valeur de `$FFF13E` et de `$FFFA00` relevées au vecteur de V-int dans un seul run Gens, comparées aux dumps DSY `e1083.dsy` qui ont une numérotation f indépendante de k). Observation de l'outil.

Rectification (dumps DSY, numérotation f commune, `e1083.dsy` Gens contre `n6.dsy` noyau, f 10 855–10 872) : `$FFF13E` (compteur de canal) et `$FFF136` (pointeur de flux) sont **identiques à chaque f** : 10 → 1 de 10 855 à 10 864, puis rechargement à 36 et `$F136` = `$ED53` → `$ED57` à f=10 865 **dans les deux machines**. La phase sonore est donc la même au V-int près ; la « phase sonore décalée de 2 V-int » de la section précédente est un artefact du recalage k→f (hooks Gens) et est **retirée**. Seule la rupture `$FFFA00` à f=10 864 subsiste (Gens 1 : le fil principal a déjà écrit avant le V-int ; noyau 0), puis, à partir de 10 865, des divergences de WRAM (126 octets à 10 865, 477 à 10 866) qui en découlent (régions objets `$B8xx`). Reste donc uniquement : pourquoi le fil principal de Gens atteint `$10502` avant le V-int 10 864 alors que celui du noyau l'atteint 82 lignes après le V-int 10 864. Observation de l'outil.

`$FFFA00` à l'entrée de chaque V-int (dumps DSY, Gens/noyau, f 10 850–10 871) : 10 850 0/0, 10 851 1/1, 10 852 2/2, 10 853 0/0, … 10 855 2/2, **10 856 1/1** (le 0 est sauté : le fil principal a réécrit 1 avant le vecteur), 10 857 2/2, 10 858 0/0, 10 859 1/1, 10 860 2/2, 10 861 0/0, 10 862 1/1, 10 863 2/2, **10 864 Gens 1 / noyau 0**, puis 10 865 2/1, 10 866 0/2, … La séquence 0,1,2 saute donc son « 0 » quand le fil principal est en avance sur le V-int ; cela se produit à 10 856 dans les deux machines et à 10 864 dans Gens seulement. Les deux fils principaux sont donc proches de la limite « finir avant le vecteur » : une différence de quelques centaines de cycles dans la trame précédente (10 863, où le noyau écrit 2 à la ligne 92) suffit à faire basculer le saut. Voie suivante : mesurer, trame 10 863, la ligne du V-int à laquelle le fil principal écrit `$FFFA00`=2 (noyau : ligne 92) et la comparer à Gens ; une marge de ligne différente indiquerait un fil principal plus lent dans le noyau avant 10 863. Observation de l'outil.

Sondes de timing (`probe/res.txt`, profil Gens, nombres de cycles par instruction, noyau contre Gens) : DIVU `divu3` 126 / 137 (+11), DIVS `divs3` 140,1 / 154,1 (+14), débordement `divu_big`/`divu_ovf` 14 / 137 (+123) ; BCLR/BSET/BCHG #imm et reg +2 côté Gens, CLR.B/W abs.W −2, CLR.L Dn +2, TAS abs.W +4, ADDQ.W/L An −4 (Gens 4, noyau 8, l'inverse de la note Sonic 1843S : STAR_ADDQ). Essais SoR 1845S, `$FFFA00` aux V-int 10 858–10 868 : `DP_MD_STAR_DIV=147` donne la même chronologie que 133 (rupture à 10 864 inchangée) ; `DP_MD_STAR_DIV=144` change toute l'histoire (plus aucune écriture de 2, `$FFFA00` à 1 à la ligne 21 de chaque V-int) : la valeur de DIVU entre 133 et 144 fait donc basculer le jeu dans un autre régime, ce qui montre que le coût de DIV est un levier fort mais pas encore la bonne valeur (le jeu ne passe pas par 10 858 avec le même état). Pistes : (1) dans le noyau, DIVU/DIVS Starscream = 144/161+3, avec le débordement à 137 (compat Gens, `DP_MD_STAR_*`) ; (2) vérifier d'abord, par dump DSY entre 0 et 10 000, que la chronologie reste alignée sur Gens avant de juger 10 864. Observation de l'outil.

Alignement des dumps DSY (Gens `e1083.dsy` contre noyau, première différence de WRAM hors `$FE70–$FE90`, f 10 830) avec `DP_MD_STAR_DIV` : **144 → 20 325 octets différents dès f=10 830** (l'histoire du jeu diverge bien avant 10 858) ; 140 → 49 octets différents dès 10 830 (zone `$FE91+`) ; 133 (défaut) → seules les ~14 octets de la zone `$FE74–$FE85` avant 10 864. La valeur 133 est donc la meilleure des trois pour la chronologie globale de SoR 1845S, et les +11/+14 cycles des sondes DIVU/DIVS ne doivent pas être corrigés en relevant `STAR_DIV` : l'écart de la sonde vient probablement de l'ossature de la sonde (instructions autour du DIV) ou du fait que le coût Starscream dépend de l'opérande (la sonde utilise un diviseur fixe). Observation de l'outil ; le point DIV est clos pour ce film, la rupture `$FFFA00` à 10 864 reste ouverte.

Correction des sondes de timing : la colonne « noyau » de `probe/res.txt` était **périmée** (mesurée avant `STAR_TIME`/`STAR_DIV`). Relancées avec le binaire actuel (profil Gens), les sondes BCLR/BSET/BCHG (immédiat et registre), CLR.B/W abs.W, CLR.L Dn, TAS abs.W, DIVU/DIVS et débordement DIVU donnent **exactement** les cycles de Gens (12/12, 14/14, 8/8, 22/22, 137,2/137, 154,1/154,1). Seul écart restant : ADDQ.W/L #n,An (noyau 8, Gens 4). L'essai `DP_MD_STAR_ADDQ=1` sur SoR 1845S fait diverger tout le jeu dès f=10 830 (20 322 octets, contre ~14 sans) : le coût de 8 cycles (matériel) est correct pour SoR, et l'écart de la sonde ADDQ n'est pas à compenser dans le noyau. Les écarts de cycles d'instruction connus sont donc **tous écartés** comme cause de la rupture `$FFFA00` à f=10 864. Observation de l'outil.

**Marge réelle de la rupture (PCLOG noyau, V-int 10 864–10 865).** Au V-int 10 864 le fil principal du noyau exécute son dernier `rts` (`$07EE`, fin du travail lourd de 396 tours de `$3B98–$3BA8`) à la ligne 261, cycle …603 ; le vecteur de V-int est pris au cycle …663, **60 cycles plus tard**. L'écriture `$FFFA00`=1 (`$10502`, après `jsr` en `$3B2`) tombe ≈ 56 cycles après ce `rts` : il manque au fil principal une ou deux instructions pour écrire avant l'interruption. Le noyau écrit finalement à la ligne 82 de 10 865, après la rafale YM du handler (≈ 40 000 cycles) : **les « 40 000 cycles de retard » sont la rafale sonore, pas un retard du fil principal**. La rupture se joue donc sur une marge de quelques dizaines de cycles (l'instant exact où Gens accepte le V-int, la latence d'interruption, ou l'accumulation de quelques cycles dans la trame 10 864), et non sur un coût d'instruction ou un travail différent. Pistes restantes à tester : instant du V-int dans la ligne 0 (`DP_MD_VINT_DELAY`, commentaire du code : « Gens raises the V-int 128 cycles into its line »), latence d'interruption (`MD_IRQ_IMMEDIATE`). Essai `DP_MD_VINT_DELAY=40/100/128` : les runs ne produisent aucun journal (`vint_n` n'avance pas : l'outil de diagnostic WLOG/DSY dépend de l'interruption prise sans passer par `run_slice` à la ligne 0, incompatible avec ce délai) ; l'essai est à refaire après adaptation de l'outil. Observation de l'outil.

`DP_MD_VINT_DELAY` : essai de cause de l'échec. Avec `DP_MD_VINT_DELAY=100`, le 1000ᵉ V-int est pris à `frames=1801` contre `frames=1197` sans délai (V-int 100 : 194 contre 212) : l'option fait **perdre des V-int** (≈ 0,6 par trame), le jeu ne suit plus la même histoire, d'où l'absence de journal à la hauteur 10 858. Le délai de V-int du noyau (`md_full_frame`, ligne 0 : `irq_pending` remis à 0, `run_slice(vd)`, puis remis à 1) est donc défectueux dans ce chemin de rejeu ; il ne permet pas, en l'état, de tester l'hypothèse « Gens prend le V-int ≈ 128 cycles après le début de la ligne 0 ». À corriger dans m0boy (`md_full_cpu.c`, `md_full_frame`) avant de refaire l'essai sur SoR 1845S. Observation de l'outil.

Réserve sur le paragraphe précédent : « perd des V-int » est une interprétation, non vérifiée. Avec `DP_MD_VINT_DELAY=100` le premier V-int arrive déjà à `frames=80 line=0` (contre `frames=61 line=166`) : l'amorçage du jeu diverge dès le démarrage, et un jeu qui rate sa fenêtre de trame peut légitimement tourner à ≈ 1 V-int pour 1,8 trames sans qu'aucun V-int ne soit « perdu » par le noyau. Il faut compter les `irq_pending` mis à 1 et pris à chaque trame pour trancher entre un défaut du délai et une divergence réelle du jeu ; non fait.

`DP_MD_VINT_DELAY` n'est pas défectueux, le jeu y est très sensible : `=1` reproduit exactement la chronologie de base ; `=16` décale déjà le V-int 99 de 212 à 239 trames. Comparaison aux dumps DSY de Gens (f 10 830 / 10 863 / 10 864 / 10 865) : `=2` → 20 323 octets différents dès 10 830 (histoire divergente) ; **`=4` → 7 / 3 / 8 / 114 octets, aligné comme la base, mais la rupture `$FFFA00` à 10 864 subsiste (Gens 1, noyau 0)** ; `=8` → 1 454 octets différents dès 10 830. Le résultat n'est pas monotone en fonction du délai (2 et 8 divergent, 4 non) : la chronologie du film est fragile à quelques cycles près, et un décalage fixe du V-int ne corrige pas la rupture. Cette piste est écartée dans sa forme simple ; reste la latence d'interruption (`MD_IRQ_IMMEDIATE`) et l'accumulation de cycles dans la trame 10 864, deux leviers qui touchent aussi à tout le film. Observation de l'outil.

Balayage `DP_MD_VINT_DELAY` (octets de WRAM différents de Gens à f=10 830 / 10 864 ; `$FFFA00` Gens/noyau à 10 864) : 3 → 6 / 8 ; 1/0 (rupture inchangée) ; 4 → 7 / 8 ; 1/0 (idem) ; 5 → 307 / 477 ; 6 → 565 / 653 ; 7 → 20 280 / 20 169 (histoire divergente) ; 12 → aucun journal. **Aucun délai de V-int ne garde l'alignement tout en corrigeant `$FFFA00` à 10 864** : 3 et 4 restent alignés mais gardent la rupture, 5 à 7 désalignent le film plus tôt. Le levier « instant du V-int dans la ligne 0 » est donc écarté ; restent la latence d'interruption et l'accumulation de cycles dans la trame 10 864. Observation de l'outil.

Latence d'interruption : profil Gens sans `MD_IRQ_IMMEDIATE` (latence de référence, une instruction de plus avant la prise du V-int) → 20 412 octets de WRAM différents de Gens dès f=10 830 : le film diverge entièrement. `MD_IRQ_IMMEDIATE=1` est donc indispensable à l'alignement SoR 1845S et n'est pas un levier pour 10 864. Bilan des leviers testés contre la rupture `$FFFA00` à f=10 864 : DMA, `$F13D`, phase sonore, saut d'inactivité, coûts d'instruction (DIV, ADDQ, bit/CLR/TAS), instant du V-int (`VINT_DELAY` 1–12), latence d'interruption — tous soit sans effet sur la rupture, soit destructeurs pour l'alignement du film. Reste l'accumulation d'un écart de quelques dizaines de cycles dans la trame 10 864 elle-même (60 cycles de marge) : à chercher dans les coûts d'accès bus (WRAM, ROM, VDP, Z80) de la boucle `$3B98–$3BA8` et du handler, avec un profil de cycles Gens qui n'existe pas encore (Gens-rr ne l'expose pas). Observation de l'outil.

Profil de cycles côté Gens : faisabilité (21 novembre 2026). Les sources de Gens-rr sont dans `~/dev/gens-rerecording`. Le Lua n'expose que les registres de `m68kPointerMap` (`luascript.cpp:2154`, d0-d7, a0-a7, pc, sr) ; le compteur de cycles de Starscream (`main68k_context.odometer`, `Star_68k.h:76`) n'y figure pas. Le correctif est une ligne (`RPM_ENTRY("odometer", main68k_context.odometer)`), qui donnerait par `memory.getregister("main.odometer")` le temps exact de chaque instruction de la trame 10 864 via `registerexec`, donc la comparaison de cycles que le noyau ne peut pas faire seul. Obstacle : la construction est un projet Visual Studio (`Compile.txt`, nasm + DirectX) ; la machine n'a ni `msbuild` ni SDK, seulement `mingw-w64`, `nasm` et `wine`. Un portage mingw de la construction est à tenter ; aucun essai fait. Observation de l'outil.

Profil de cycles Gens : essai du compteur Starscream (21 novembre 2026). Le `Gens.exe` de travail est compressé UPX ; décompressé par un `upx.exe` Windows (wine) en copie séparée (`gensrr/odo/`), puis la table `m68kPointerMap` (entrée `sr`, `.data` à `0x38b1bc`) a été re-pointée sur `main68k_context.odometer` (`0x76b68c`, taille 4) : `memory.getregister("main.sr")` renvoie alors le compteur. Mesure sur SoR 1845S (hooks `registerexec` aux adresses du cycle `$FFFA00`) : la valeur est une borne par trame (≈109 440–109 450 au vecteur V-int, soit la ligne 224 + 128 cycles, ce qui valide l'unité) mais **n'avance que par à-coups** (valeurs identiques sur des dizaines d'instructions consécutives de la boucle `$3B98–$3BA8`, sauts de 88 à 480 cycles) : Starscream ne met l'odometer à jour qu'aux fins de tranche et aux accès par fonctions, pas à chaque instruction. Il ne sert donc pas de profil de cycles par instruction. Seul résultat exploitable : dans Gens, l'écriture de 1 en `$10502` précède le V-int 10 865 d'environ 529 cycles (odometer 108 913 contre 109 442), alors que le noyau l'écrit ligne 82 de la trame suivante : la marge de Gens est donc très supérieure aux 60 cycles estimés côté noyau, ce qui oriente vers un travail effectué plus tôt dans Gens (un écart de plusieurs centaines de cycles accumulés sur la trame, pas de quelques cycles). Observation de l'outil, odometer grossier.

Rupture `$FFFA00` à f=10 864 : localisation au cycle près (21 novembre 2026, SoR 1845S ; Gens via l'odometer, noyau via `DP_MD_PCLOG`, `MD_NO_IDLE=1`). La trame de travail est un seul appel de la routine `$AE00` (retour `$AEEC`), qui contient la boucle `$3B98–$3BA8` (396 tours dans les deux machines) et sept passages de `$10400` (mêmes lignes dans les deux). Durée de `$AE00` à `$AEEC` : Gens 118 023 cycles, noyau 118 017 : **le travail est identique à six cycles près, le noyau n'accumule aucune dérive de coût d'instruction.** Après `$AEEC`, le fil principal enchaîne 28 instructions (`$108B8…$7EE`, 344 cycles dans le noyau) puis `jsr $10502` qui écrit 1. Dans le noyau, `$AEEC` tombe 404 cycles avant le vecteur de V-int ; le V-int est pris juste après le `rts` de `$7EE` (cycle 461 619), il manque environ 44 cycles (le `jsr` et le `move.b`) : l'écriture passe après le handler (ligne 82 de la trame suivante). Dans Gens, toute la chaîne `$AEEC…$10502` s'exécute dans une même tranche Starscream (odometer figé à 108 913, vecteur à 109 447, soit 534 cycles de tranche) : Gens prend donc le V-int au moins ~44 cycles plus tard que le noyau dans cette trame, sans que le travail diffère. Conclusion : la cause est l'instant de prise du V-int (granularité de tranche de Starscream : l'interruption n'est vue qu'à la fin de la tranche, pas à la première limite d'instruction suivant la ligne 224 + 128), pas le coût des instructions. Les essais `VINT_DELAY` fixe ne marchent pas parce que le délai nécessaire dépend de la phase de l'instruction en cours à l'échéance. Piste suivante : modéliser dans le noyau l'acceptation en fin de tranche de Starscream (tranche d'une ligne ?) dans le profil `DP_MD_GENS`. L'odometer lu par Lua est grossier (début de tranche) ; l'outil de mesure est une copie UPX-décompressée de `Gens.exe` dont l'entrée `sr` est re-pointée sur le compteur. Observation de l'outil.

PC interrompu à chaque V-int, Gens contre noyau (SoR 1845S, f 10 830–10 880, dumps DSY, adresse de retour empilée à `A7+2`) : les deux machines sont interrompues au même PC dans 16 trames sur 51 seulement. Les autres diffèrent de quelques instructions (par exemple 10 832 : Gens `$AFE2`, noyau `$B0CC` ; 10 861 : `$AF58` contre `$B0C8`), dans les deux sens, alors que la RAM reste identique. À 10 864 le noyau est interrompu en `$3B2` (avant le `jsr $10502`), Gens en `$10510` (après l'écriture). La phase entre le travail principal et le V-int est donc décalée dans une majorité de trames, de quelques dizaines de cycles, signe non monotone : le défaut n'est pas propre à la trame 10 864, celle-ci est la première où un décalage change l'état visible (`$FFFA00`). Cela explique l'échec des délais fixes de V-int (un décalage constant corrige certaines trames et en dégrade d'autres) et désigne un effet de phase variable côté noyau, à chercher dans ce qui varie d'une trame à l'autre (écritures DMA et leur `Update_DMA`, accès VDP/Z80, tranches `main68k_exec(Cycles - 404)` de Gens). Le Gens source confirme le schéma : par ligne, `main68k_exec(Cycles_M68K - 404)`, rendu, `main68k_exec(Cycles_M68K)` ; à la ligne 224, `main68k_exec(Cycles_M68K - 360)` puis levée du V-int. Observation de l'outil.

Calage des repères, Gens contre noyau (21 novembre 2026, SoR 1845S) : le noyau prend le V-int à la limite de ligne (vecteur à 109 315 modulo la trame, instruction de vecteur à 109 359 après les 44 cycles d'entrée), Gens à `ligne 224 + 128` (odometer 109 442–109 450 au crochet du vecteur, valeur lue avant l'entrée d'exception) : décalage constant de 128 cycles entre les deux repères (le film prend le même nombre de trames que Gens : V-int 99 à la trame 211/212, V-int 10 864 à la trame 11 109 dans les deux). Dans le repère de Gens, `$AEEC` tombe à ≈ 109 083 dans le noyau, alors que Gens l'exécute avant la limite de ligne 109 312 (la chaîne `$AEEC…$10502`, ≈ 410 cycles, s'exécute dans la tranche qui part de 108 913) : **à la fin du travail, le noyau est en retard d'environ 160 à 170 cycles sur Gens**. La durée de `$AE00` à `$AEEC` est la même (118 017 contre 118 023) ; le retard se forme donc avant `$AE00`, c'est-à-dire dans le handler de V-int de la trame (partie pilote sonore : ≈ 9 440 cycles du vecteur à `$AE00` dans le noyau, entre 9 300 et 9 630 dans Gens, l'odometer ne donnant que le début de tranche). Piste : une différence de coût des accès par instruction dans le handler (écritures YM `$A04000`, bus Z80, VDP), hors du jeu de sondes `probe/` actuel ; à mesurer en isolant les accès du handler un à un. Observation de l'outil, estimation par repères grossiers.

Dérive du noyau avant `$AEEC` (21 novembre 2026, SoR 1845S, f 10 864) : en accrochant dans Gens les 579 adresses distinctes que le noyau exécute entre la fin de la boucle `$3B98` et `$AEEC` (6 770 exécutions, même suite d'adresses dans les deux machines, 0 écart d'ordre), l'odometer de Gens (début de tranche, donc borne inférieure du temps réel) donne une borne supérieure du retard `noyau − Gens` en tout point. Cette borne croît régulièrement le long du segment : 43 cycles au début, ≤ 60 à l'événement 1 597, ≤ 80 à 2 875, ≤ 100 à 3 536, ≤ 120 à 4 578, ≤ 150 à 5 691, et ≥ 154 à `$AEEC` (fenêtre serrée par la tranche de 108 913). **Le noyau accumule donc environ 1 cycle de retard par 50 instructions dans le code `$AD00–$B120` de la trame, soit ~130 cycles sur 6 770 instructions**, et non un écart du handler ni de l'instant du V-int (corrige l'hypothèse précédente). Instructions les plus fréquentes du segment : `cmpi.w #,d0` (308), `rts` (301), `move.b (a1)+,d0` (292), `move.l d1,(a1)+` (224), `move.w d0,(a2)+` (212), `jsr d16(pc)` (193), `btst #,d16(a0)` (161), `dbf` (157), `ext.w` (146), `bcs`/`bcc` (146 chacun), `moveq` (139+95). Toutes les classes déjà sondées (`probe/`) concordent à ±2 cycles par boucle entière ; la classe fautive est donc parmi les non sondées (`add`/`sub`/`or`/`and` registre-registre, `move` vers `(An)+` de tailles mixtes, décalages par registre, `mulu`/`muls` à données variables, accès à des modes d'adressage rares). Prochaine étape : sondes pour ces classes, ou expérience de pénalité −2 cycles par classe dans le noyau en observant la baisse de la borne. Observation de l'outil.

Dérive de 130 cycles et ADDQ.W An (21 novembre 2026, SoR 1845S) : la courbe du retard noyau − Gens, relevée à chaque mise à jour de l'odometer, est une marche d'escalier de pas 4 (43 → 175 cycles en 6 770 instructions, aucune dérive sur les 1 050 premières, puis environ 4 cycles tous les 130 instructions dans `$AD00–$B120`). Le segment contient 33 `addq.w #n,An` (`$5449`, `$544C`, `$5249`) : 33 × 4 = 132, soit la dérive. Essai `DP_MD_STAR_ADDQ=1` (coût 4 comme Starscream) sur f 10 830–10 880 : l'alignement est perdu dès 10 830 (≈ 20 400 octets de WRAM différents), donc le coût 8 est juste ailleurs avant 10 830 ; la coïncidence 132 ≈ 130 n'est pas démontrée et l'hypothèse « ADDQ.W An = 4 partout » est réfutée. Reste à tester ADDQ.W An à 4 pour une seule forme (`#1` ou `#2`, registre d'adresse précis) ou à relever par PC les 33 occurrences et leur arrivée dans les paliers de +4. Observation de l'outil.

**Rupture `$FFFA00` de f 10 864 corrigée (0.423.0, SoR 1845S, 21 novembre 2026).** La dérive de ~130 cycles du segment `$3B98→$AEEC` venait des 33 `ADDQ.W #n,An` (Starscream 4 cycles, manuel 8) : 33 × 4 = 132. L'essai précédent de `DP_MD_STAR_ADDQ` échouait parce que l'option touchait aussi `SUBQ.W An`, que Gens garde à 8 (sondes `subq_w_a1` : 4919/4914). Restreint à ADDQ (mot et long, `addq_l_a1` : Gens 4), sur f 10 830–10 880 les octets de WRAM écartés (hors `$FE70–$FE90`) passent de 115 à 3 (f 10 865) et de 361 à 4 (f 10 880) ; restent 49 octets à 10 830 et 50 à 10 870. `DP_MD_STAR_ADDQ` entre désormais dans le profil `DP_MD_GENS` ; la valeur est un masque des immédiats 1–8 (bit n) pour isoler une forme. La note « casse Sonic 1843S à la trame 168 » datait de la version qui touchait aussi SUBQ : à revérifier. Observation de l'outil.

### SoR 1845S : rupture 19 559/19 564, boucle du bus Z80 (essais négatifs)

Après la correction ADDQ (0.423.0), le noyau reste aligné sur Gens jusqu'au V-int 19 558, puis la zone sonore `$FE9x` (19 559), `$DAB9`/`$FA00` (19 564) et les objets `$B8xx` (19 565+, ~100-150 octets) divergent durablement. Méthode : trace PC du noyau sur la trame 19 563 et odometer Starscream de Gens (hooks `registerexec`). Le premier écart de chemin (indice 5 372, ~60 000 cycles dans la trame) est la boucle `$73298` du pilote sonore : requête de bus (`move.w #$100,$A11100`), attente du grant, `btst #7,$A01FFD`, relâchement si le bit est à 1. Le noyau prend le `beq` en `$732B2` (octet Z80 `$1FFD` bit 7 à 0), Gens reste dans la boucle une itération de plus : l'état de la RAM Z80 vu par le 68000 diffère. Observations de l'outil, pas des mesures de matériel.

Essais sans effet ou négatifs :
- `DP_MD_GENS_BUSREQ=1` : sorties identiques trame par trame (le `move.w` coûte 20 cycles, déjà plus que les 16 du délai de grant).
- Z80 rattrapé seulement à la frontière de ligne précédente (Gens exécute le Z80 par ligne après le 68000), appliqué à lecture, écriture et requête de bus : le noyau perd ~540 V-ints sur 3 000 trames (2 262 contre 2 802), Gens n'en perd pas ; modèle faux, code retiré.
- Sondes `MULU/MULS` indexé et PC-relatif : écarts dus à l'opérande lu (registre d'index modifié), pas au coût ; `DIVU`, `CLR`, `TAS` égaux à Gens.

Piste restante : comment Gens fait avancer le Z80 pendant les relâchements courts du bus (le noyau le fait à l'instruction près), puis l'épisode 12 150-12 250.
