# Plan — mutualiser la recherche de couverture et ajouter de nouvelles techniques

## Contexte
Il existe quatre explorateurs écrits séparément, plus le moteur GB :
- `src/platform/sega/explore.c` (3043 lignes) ;
- `nes/explore.c` (2309) ;
- `pce/explore.c` (2081) ;
- `md/explore.c` (2315) ;
- le moteur GB dans `src/explore/` (forks, seeds, sections, closure, provenance, sessions, frontier, copies…).

Les interfaces existent déjà :
- `DpMachine` et `DpIsa` (`src/core/dpmachine.h`) ;
- les backends `src/mach/*_machine.c` pour toutes les plateformes ;
- les ISA `sm83`, `z80`, `m6502`, `huc6280` et `68000`.

Ces interfaces ne sont pourtant consommées que par le GB et par quelques modules : la politique de frontière et le conteneur bk2. Chaque technique est donc réécrite N fois. La matrice §2.1 de `docs/COUVERTURE.fr.md` reste pleine de ◐, et les corrections divergent : la session précédente l'a montré avec le partage de budget Sega, le gonflement du code forcé et la coupure des forcés en RAM.

Objectif : un seul moteur, `src/explore/`, qui pilote n'importe quel `DpMachine` + `DpIsa`. Toutes les techniques de la matrice sont appliquées partout. On y ajoute de nouvelles techniques de recherche, écrites une seule fois.

## Préalable (travail en cours, non commité)
Les changements Sega de la session précédente sont en cours : known hors forcé, `forced_ram_cuts`, budgets par worker par défaut, défauts orientés couverture, version 0.252.0.
1. Validation faite (0.252.0, défauts) :
   - Ristar GG sans TAS : 29,2 % connus, 1 450 octets forcés, 148 coupures RAM forcées, 57 s ;
   - Alex Kidd SMS avec TAS : 70,7 % connus (contre 89,4 % gonflé avant), forcé 29 620 → 11 689, natural 15 657 → 16 550, 2 912 coupures RAM forcées.
   Reste `exec_outside` à 9 M sur Alex Kidd : des chemins forcés qui exécutent du code copié en RAM. L'étape 3.1 (concolique) traite ce cas.
2. Deux tests sont en échec : `nes_speculative` et `pce_speculative`. Ils portent sur du code NES/PCE non modifié, mais le fait qu'ils échouaient déjà avant ces changements n'est pas vérifié. Les bissecter : suspect `59a3e60`, qui fait passer le fork-cap par défaut à 4, si bien que la section n'est plus spéculative. Corriger le test ou le défaut.
3. Commiter en deux commits séparés : le correctif Sega, puis le correctif des tests. Signature : `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
4. Supprimer le worktree temporaire : `git worktree remove --force /tmp/claude-1000/-home-m-dev-memscan/7e2a9d08-6729-4b9e-97b7-fd9262a0e52e/scratchpad/old/wt`.

## Étape 1 — Noyau générique `dp_core_explore`
Nouveau `src/explore/core.c` + `core.h` : la boucle pas-à-pas commune. Elle ne parle qu'à `DpMachine` (save/load/step/observe/force_branch/irq/set_input/rom_index/ram_index/bank_geometry) et à `DpIsa` (length/flow/cond/mem_operand/regs_effects).
- Elle réutilise tel quel, en les rendant indépendants du GB :
  - la frontière : `frontier.c`, `frontier_disk.c` et `dp_frontier_room` ;
  - `tree.c` ;
  - les sections et la liveness : `sections.c` ;
  - `copies.c`, avec toutes les destinations par `dest_type` ;
  - `forks.c`, pour D7–D11 et le cap par site ;
  - `seeds.c`, pour G1–G4 ;
  - `closure.c` et `provenance.c` sur `DpIsa`, en reprenant la version la plus complète, celle de `sega/closure.c` (vues) ;
  - `streams.c`, pour S6 ;
  - `residual.c` ;
  - `ramarch.c` ;
  - `sessions.c` et `tas.c`, pour J1–J4.
- Le modèle de couverture commun §4 (flags + `src_flags` + first_seen) est le seul stockage. Les flags locaux de chaque plateforme disparaissent.
- Le « known » des rapports exclut partout le code forcé, compté à part (`speculative`). La coupure « forcé exécutant de la RAM non alimentée par une copie ROM » est généralisée.
- Le parallélisme `--jobs` est commun, avec une file partagée par work-stealing : il remplace les `jobs.c` par plateforme. Budgets par worker par défaut, partagés seulement s'ils sont explicites.
- Les options sont dans une table unique (`cli_common.h` / `DP_CLI_COMMON`), avec des défauts communs (`coverage_defaults.h`) : jobs = cœurs, seed-banks, queue-max 65536, mem-cap = ½ RAM, fork-cap 4.

Les particularités de chaque plateforme deviennent des hooks optionnels du noyau, `DpPlatformHooks`. Exemples : traps BIOS MSX, `$3E` SMS, timing NES en milieu de trame, Z80 son MD, sondes manette, mappers.

## Étape 2 — Migration plateforme par plateforme (ordre §7.5)
Pour chaque plateforme :
1. Brancher le CLI de la plateforme sur `dp_core_explore`, derrière `--core`.
2. Comparer avec l'ancien moteur, à budgets égaux, sur les fixtures et sur 1 à 2 ROMs réelles. Critère : couverture natural ≥ l'ancienne et tests verts.
3. Passer le noyau par défaut et **désactiver** (sans supprimer) le code moteur de `platform/*/explore.c` une fois la parité atteinte : décision du propriétaire, le vieux moteur reste dans le dépôt, inactif, comme référence. Seuls les hooks, le registre et les catalogues de sondes sont partagés.

Ordre : Sega/MSX (le plus proche), puis NES, PCE et MD (avec les fourches du Z80 son).

Effet attendu : les ◐ de la matrice passent ✅ par construction. Sont concernés S2 à vues, S3/S4 provenance, S6, G2/G4, C1/C2 liveness, F3 file partagée, J1 pool, J4 Go-Explore, J5 export, O2.

## Étape 3 — Nouvelles techniques (dans le noyau, donc partout)
1. **Résolution concolique des branches.** Aujourd'hui, un fork forcé ment sur l'état. On utilise plutôt la provenance (S3) : si la condition dépend d'un octet RAM ou d'un port lu, on cherche une valeur de cet octet qui prend l'autre côté (petite énumération ou inversion de `cp n`/`and n`/`bit`), on l'écrit dans l'état et on relance. Le chemin obtenu est **natural-cohérent** : la source est `fork-solved`, et il est compté comme connu, pas comme spéculatif. On réduit ainsi le code forcé et on augmente la couverture prouvée. Le repli reste le fork forcé.
   Trois niveaux de vérité : `natural` (prouvé) > `solved` (valeur RAM/port écrite pour prendre légalement l'autre côté, consignée, comptée à part comme plausible) > `forced` (spéculatif). Résolution : remonter par la provenance au dernier écrivain du flag (`cp n`, `and n`, `bit n`, `dec`, `in`) ; écrire la valeur dans l'état restauré au moment de la lecture, puis rejouer depuis cette lecture. Repli : 1.b.
1.b **Forcé à contamination.** Quand la résolution échoue, la fourche forcée marque comme contaminés le flag inversé et tout ce qui en dérive (registres, octets RAM écrits depuis des valeurs contaminées, pile). Le chemin est coupé dès qu'une valeur contaminée sert d'adresse : saut ou appel indirect, `ret` ou `pop pc` sur pile contaminée, base ou index de lecture, numéro de banque, port de mapper. Compteur : `forced_taint_cuts`, par raison. Les lectures de données dont l'adresse est contaminée restent spéculatives. Le code exécuté avant la coupure est marqué `forced` (code réel, état partiellement faux). Ce mécanisme remplace la stagnation comme garde principale du forcé (la stagnation reste en filet). Il généralise la coupure « forced RAM » de 0.252.0. L'inversion seule (marquer le bloc cible sans poursuivre) est le cas limite où la contamination atteint une adresse immédiatement. Mesure attendue : sur Alex Kidd, `exec_outside` (9 M en 0.252.0) et le code forcé baissent, à code natural égal ou supérieur.
2. **Sondes de registres événementiels (§3.7).** Valeurs alternatives aux lectures de statut (VDP/PPU status, compteurs V/H, timers) aux sites qui les testent.
3. **Fourche « sauvegarde valide » (§10.2).** Une lecture SRAM comparée à une constante entraîne une fourche où la signature attendue est écrite.
4. **Octets RNG appris (§10.3).** Détection des octets WRAM mis à jour comme un LFSR à chaque trame ; fourches sur 2 à 4 valeurs aux sites qui les testent.
5. **Ordonnancement guidé par la nouveauté**, à la AFL. La frontière priorise les états dont le site parent est rare ou dont l'issue récente a découvert des octets. Il faut un score par site dans `frontier.c` et une courbe de rendement (§10.10). Rien n'est jeté ; seul l'ordre change.
6. **Détection générique des décompresseurs (§10.4).** Ratio de lecture ROM séquentielle / écriture ≠ 1. On rejoue la routine (S6/§5.4) sur chaque entrée de sa table de pointeurs ; le type `compressed` est posé sur les sources.
7. **Rejeu par index (§5.4)** de toute routine qui lit `base+index` : on énumère l'index sur la plage prouvée. Exemple : musique jamais jouée.
8. **Cellules Go-Explore par hash de RAM sémantique**, en plus des cellules écran. On hache les octets RAM que les sections vivantes lisent (liveness C1), ce qui ne demande aucune connaissance du jeu.
9. **Perturbation temporelle.** Décaler de ±1 à 3 trames l'entrée des pressions TAS/aléatoires, et les fourches d'IRQ à mi-trame, pour atteindre les branches qui dépendent du timing.

Chaque technique arrive avec :
- son option de désactivation ;
- son compteur, jamais silencieux ;
- sa fixture dans `tests/build_fixtures.py`, une par technique et par ISA (§10.6 : matrice exécutable).

## Fichiers critiques
- Nouveaux :
  - `src/explore/core.c` et `core.h` ;
  - `src/explore/solve.c` (concolique) ;
  - `src/explore/probes_evt.c`.
- Modifiés :
  - `src/core/dpmachine.h`, pour ajouter si besoin `write_ram` (concolique), `state_hash`, `framebuffer` et `clone` ;
  - `src/mach/*_machine.c` ;
  - `src/explore/{forks,seeds,sections,copies,closure,provenance,sessions,frontier}.c`, pour les rendre indépendants de l'ISA ;
  - `src/cli_common.h` ;
  - `src/explore/coverage_defaults.h` ;
  - `src/deepeel.c`.
- Réduits à des hooks : `src/platform/{sega,nes,pce,md}/explore.c` et `jobs.c`.
- Docs : `docs/COUVERTURE.fr.md` (matrice, §7, §10, §11), `docs/GB.fr.md` et `docs/Z80.fr.md`. Version incrémentée à chaque jalon.

## Vérification
- Après chaque étape : `ctest --test-dir build --output-on-failure`. Les 5 tests GB (`test_explorer`) restent identiques et tous les tests de plateforme restent verts.
- Parité de migration : pour chaque plateforme, mêmes classes de carte natural (≥) sur les fixtures et sur la campagne réelle (script `output/couverture-*/run_campaign.sh` réutilisé : Castle, Sonic, Alex Kidd, Ristar, plus une ROM NES, PCE et MD), à budgets égaux. Le tableau avant/après est présenté comme observation de l'outil.
- Nouvelles techniques : une fixture qui ne se couvre qu'avec la technique (par exemple une branche sur `cp $A5` d'un octet SRAM pour la sauvegarde valide). Le test vérifie la source `DP_SRC_*` et le compteur.
- Concolique : sur les ROMs de campagne, le code forcé baisse et le code natural monte, à budget égal.
