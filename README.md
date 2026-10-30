# memscan

Outil générique de découverte de la **memory map** des ROMs, plateforme par
plateforme. Il contient le registre matériel complet de chaque console
(registres IO, fenêtres de mapper, IRQ), il exécute la ROM dans une machine
concrète et consigne **tout** : chaque octet exécuté, chaque lecture de
donnée ROM, chaque accès registre, chaque copie ROM→VRAM/OAM (canal matériel
OAM DMA/HDMA **et** boucles de copie CPU), chaque changement de banque.

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

- **Game Boy** : complet (MBC0/1/2/3/5, OAM DMA, HDMA CGB, 5 IRQ, scan de
  pointeurs). Voir `docs/GB.fr.md` pour le registre et les limites.
- Autres plateformes : l'architecture est prête (`src/platform/platform.h`,
  `src/cpu/`) — une plateforme = une table de registres + des hooks ; un CPU
  = un décodeur/exécuteur qui journalise ses effets. Z80, 6502, HuC6280,
  68000, 65816 sont les suivants naturels.

## Layout

- `src/core/` — cartouche, machine concrète (espaces, banques, hachage de
  pages), routage mémoire.
- `src/cpu/sm83/` — exécuteur SM83 complet (drapeaux exacts, effets
  journalisés) + désassembleur.
- `src/platform/gb/` — registre matériel GB, mapper MBC, canaux de copie.
- `src/explore/` — moteur à fourches, dédup à deux niveaux, sections et
  stats de nécessité, suivi de copies.
- `src/report/` — texte, `map.json`, export `.cov`.
- `src/gui/` — visionneuse SDL2 temps réel (+ police 5×7 embarquée).
- `tests/` — fixtures GB synthétiques générées à la construction (aucune ROM
  dans le dépôt) + tests C via ctest.
