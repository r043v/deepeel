# memscan

Outil générique de découverte de la **memory map** des ROMs, plateforme par
plateforme. Il contient le registre matériel complet de chaque console
(registres IO, fenêtres de mapper, IRQ), il exécute la ROM dans la **machine
m0boy** (l'interpréteur GB dont le run_hash est tenu par la suite de tests
m0boy, embarquée comme dépendance `M0BOY_DIR`) et consigne **tout** : chaque
octet exécuté, chaque lecture de donnée ROM, chaque accès registre, chaque
copie ROM→VRAM/OAM, chaque changement de banque.

La recherche est **autonome** : à chaque branchement conditionnel les deux
issues sont testées (fork), à chaque point d'interruption activée un fork
d'IRQ est tenté. L'éviction interdit les ré-exploitations inutiles :

- dédup exact : un état (PC + registres + banques + RAM hachée par pages)
  déjà exploré n'est jamais ré-engendré ;
- éviction « même contexte » : une ré-entrée dans une section dont l'état
  d'entrée ne diffère que par des composantes que la section ne lit jamais
  (masse de liveness apprise pendant l'exécution) est évincée ;
- anti-boucles : budget de pas par chemin, compteur par arrière-branche,
  budget global de pas, budget de forks par site — la saturation est
  rapportée, jamais cachée.

Toute l'exploration vit dans un **arbre commun** (`src/explore/tree.c`) :
chaque graine, chaque fourche de branche/IRQ, chaque dépôt de session est un
choix relié à son parent ; quand un chemin se termine son choix se ferme avec
une issue (`done`, `merged` — retombe sur du connu sans issue, le cas
gameover —, `dead` — opcode indéfini —, `reset` — retour à $0100, `budget`,
`capped`). Après les sessions, une **recherche totale DFS** draine la
frontière en profondeur (`tree.bin` la persiste, `--resume` la reprend).

Des **stats de nécessité par section** sont tenues pendant l'exploration
(hits, flux naturel vs forcé, contextes, côtés de conditionnels couverts,
registres touchés, copies produites) et chaque section reçoit un verdict :
`required` (atteint par le flux de boot, gère une IRQ, ou produit des
copies), `forced` (uniquement atteint par exploration forcée — candidat
donnée-avec-code) ou `speculative`.

La fenêtre **SDL2** montre la carte de la ROM en temps réel pendant
l'exploration (une tuile par banque, 16×16 blocs de 64 octets, zoom/pan,
clic = détail de section avec désassemblage).

## Construction

```bash
cmake -G Ninja -B build . -DBUILD_TESTS=ON
ninja -C build
ctest --test-dir build --output-on-failure
```

SDL2 est optionnel (`-DBUILD_GUI=OFF` pour s'en passer).

## Usage

```bash
memscan explore rom.gb [--gui] [--json map.json] [--text report.txt] [--cov out.cov]
memscan explore rom.gb --max-total 50000000 --queue 8192
memscan explore rom.gb --entry 2:4000        # graine supplémentaire banque 2, $4000

# sessions jouées (m0boy : inputs aléatoires + attract, 10 threads, pool de
# checkpoints, scheduler d'époques, persistance --state-dir/--resume)
memscan explore rom.gb --play --frames 72000 --state-dir s --cov jeu.cov --romdata jeu.prof
memscan explore rom.gb --play --resume --state-dir s
# TAS : rejoue un movie tool-assisté (VBA .vmv, BizHawk .bk2, log texte) — un
# worker par fichier ; le TAS est prioritaire : prioritaire sur tout budget
# (frames, segments, stagnation), le film est joué en entier jusqu'au bout du
# jeu, puis le worker passe en jeu aléatoire
memscan explore rom.gb --play --tas supermarioland.bk2 --frames 18000
# export jouable : ecrit le checkpoint choisi (le plus productif, ou
# --export-slot N) en slot 0 du <SD>.STA du projet pc m0boy — le firmware
# pc le propose au menu (Etat -> Charger l'etat) et boote dessus avec
# M0_STATE=0 ; le .SAV batterie suit quand la cartouche a de la RAM
memscan explore rom.gb --play --frames 6000 --export-playable chemin/vers/projet_pc
# mode humain : une lane jouable au pad, chaque avancée sauvée en PNG (écran +
# machine complète, rechargeable --from-png) et explorée automatiquement
memscan explore rom.gb --play --gui --human
memscan map rom.gb                           # carte statique seule (pointeurs, padding)
memscan regs gb                              # dump du registre matériel GB
memscan dis rom.gb 2:4000 12                 # désassemblage banque 2, $4000
```

Options d'exploration : `--max-steps` (par chemin), `--max-paths`,
`--max-total` (mur global de pas), `--queue`, `--loop-cap`,
`--fork-cap` implicite via `--max-*`, `--seed-cap`, `--seed-banks`,
`--no-pointer-scan`, `--no-context-evict`, `--no-irq-fork`.

L'export `--cov` écrit le format de couverture de m0boy (`bank addr flags
count`) — pont optionnel vers le convertisseur.

## État

- **Game Boy** : la machine est celle de m0boy (MBC0/1/2/3/5, PPU/APU/timer
  réels, CPU interprété) — 17 variantes MBC×RAM compilées au build, un
  thread = une instance préfixée. Voir `docs/GB.fr.md` pour le registre
  matériel, l'arbre et les limites actuelles.
- Autres plateformes : l'architecture est prête (`src/platform/platform.h`,
  `src/cpu/`) — une plateforme = une table de registres + des hooks ; un CPU
  = un décodeur/exécuteur qui journalise ses effets. Z80, 6502, HuC6280,
  68000, 65816 sont les suivants naturels.

## Layout

- `src/core/` — cartouche, inflate/zip (les TAS `.bk2`), conteneur PNG.
- `src/cpu/sm83/` — décodeur SM83 statique (longueurs, conditions,
  désassemblage) ; l'exécution est celle de m0boy.
- `src/mach/` — le pont vers la machine m0boy embarquée (instances
  préfixées, hooks trace/lecture/écriture/vblank, images d'état, longjmp).
- `src/platform/gb/` — registre matériel GB (registres, IRQ, régions,
  entrées).
- `src/explore/` — moteur à fourches, dédup à deux niveaux, sections et
  stats de nécessité, suivi de copies, sessions jouées, fichiers TAS, arbre
  commun.
- `src/report/` — texte, `map.json`, export `.cov`.
- `src/gui/` — visionneuse SDL2 temps réel (+ police 5×7 embarquée).
- `tests/` — fixtures GB synthétiques générées à la construction (aucune ROM
  dans le dépôt) + tests C via ctest.
