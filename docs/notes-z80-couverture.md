# Apports de la branche z80 au moteur GB — note pour la session moteur

La branche `origin/z80` (adaptée sur `origin/z80-adapt`, merge `e612895`,
25/25 tests verts) ajoute CPU Z80 + plateformes MSX/Sega. Trois techniques
qui y sont utilisées méritent d'être regardées côté moteur GB. Tout est
borné et saturé-rapporté, comme nos fourches.

## 1. Suivi de constantes de registres dans la fermeture statique

`src/cpu/z80/provenance.c` : le passage statique prouve des paires
complètes HL/IX/IY (chargements immédiats, mots ROM à adresse absolue,
`INC/DEC` 16 bits par calcul exact, `LD SP,HL`), et des octets A par
transfert statique pour recomposer une adresse en deux lectures ROM.
Ces constantes servent à :

- résoudre les sauts indirects `JP (HL)` → preuves `code-static` ;
- suivre les tables parcourues pointeur en main
  (`LD A,(HL); INC HL; LD H,(HL); LD L,A; JP (HL)`, variantes IX/IY,
  parcours à rebours) → preuves `data-static` sur les octets consultés ;
- résoudre l'adresse d'écritures indirectes (déplacement signé,
  bouclage 16 bits) pour n'invalider que les fenêtres réellement
  recouvertes ;
- des preuves de retour empilées par CALL/RST (jusqu'à 8 en WRAM),
  invalidées si une écriture peut écraser la pile (miroirs compris).

Équivalent GB : même suivi sur HL pour `jp hl` / `call hl`, et lectures
`ld a,(hl)` suivies d'incrément pour les tables. Toute preuve partielle
(octet haut inconnu) ne lit jamais la ROM « à zéro » — l'adresse reste
inconnue, rien n'est inventé.

## 2. Budgets en cascade (deuxième frontière)

Après la fermeture initiale, une recherche de cibles repart avec **ses
propres** nœuds/graines/vues (elle ne consomme pas celles de la première),
puis l'affinement des blocs reçoit le **solde** non consommé des cibles.
Une observation refusée faute de solde est comptée (graines/vues refusées),
jamais silencieuse. Limites : `--closure-nodes 262144`, `--closure-seeds
65536`, `--closure-views 256`, totaux plafonnés au double.

Pendant GB : notre closure tourne en `--closure-rounds 4` ; l'idée est
d'isoler les budgets par phase et de reporter le solde plutôt qu'un budget
plat partagé, pour éviter qu'une phase gourmande affame les suivantes.

## 3. Sondes d'entrées (fork sur lectures du port manette)

À chaque `IN` (lecture manette), avant exécution, l'exploration propose un
autre état matériel et fourque : 8 propositions initiales (relâché, chaque
bouton, Start/Pause/Reset…), jusqu'à 128 combinaisons ; chaque masque au
plus une fois par site/surface ; restauration par snapshot complet (pas
« relâcher le bouton », le NMI déjà en attente resterait) ; en boucle de
polling, les tentatives sans place en file attendent une consommation
(`deferred retry skips`) au lieu de recopier des snapshots. Branches
marquées forcées ; file pleine = refus explicite, compté.

Pendant GB : on fourque déjà sur l'IRQ joypad ; le pendant serait de
fourquer aussi sur les lectures directes de `$FF00` (P1 en polling, sans
IRQ armée), avec les combinaisons de boutons — ça ouvre les jeux qui
scrutent la manette sans interruption.

## Bonus : export `--evidence`

`--evidence fichier` sort le bitmap complet avec les preuves superposées
que le `.map` aplati : masques par octet `01` code-natural, `02`
code-forced, `04` data-read, `08` copié, `10` code-statique, `20`
donnée-statique (ex. `24` = lecture machine + donnée statique). Les index
restent compacts (`bank * 0x4000 + offset`). Un pendant GB devrait y
ajouter nos classes table/padding. Voir aussi la discussion en cours sur
ce que ça donnerait face au rendu `.map.png`.
