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
