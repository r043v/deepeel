# Plan GLM — implémentation après l'audit de parité

## Mission

Transformer les conclusions de [l'audit](AUDIT-PARITE-MOTEURS.fr.md) en
corrections et en mesures reproductibles. Lire aussi `README.md`,
`docs/GB.fr.md`, `docs/COUVERTURE.fr.md` et le
[plan principal](PLAN-SUPPORT-EXPLORATION.fr.md). L'audit est une photographie :
vérifier le code et les changements récents avant chaque intervention ; ne pas
supposer qu'une observation ou une ligne est encore exacte.

## Tranche 1 — sonde P1 GB historique (E1)

1. Reproduire le défaut dans `fork_p1` avec une fixture synthétique exécutant
   `LD A,($FF00+C)` avec `C=$00`, puis avec `C=$FF`. L'adresse effective du
   SM83 est `$FF00 + C` : déterminer laquelle lit effectivement P1 avant de
   changer le code. L'audit propose `$FF`, mais cette proposition doit être
   vérifiée contre la sémantique de l'instruction et le backend ; ne pas
   appliquer une correction qui déplacerait la sonde vers `$FFFF`.
2. Ajouter un témoin positif et un témoin négatif vérifiant la lecture et le
   compteur de fourches ; corriger seulement si le défaut est confirmé.
3. Documenter le résultat, y compris si l'hypothèse de l'audit est infirmée.

## Tranche 2 — verdicts de sections GB (E3)

1. Comparer les conditions d'attribution GB à celles de NES/PCE/MD et définir
   précisément les preuves qui justifient `speculative`, sans requalifier une
   exécution naturelle ou une fourche forcée comme preuve supérieure.
2. Construire des fixtures distinctes : section naturelle, forcée atteignable
   par le flot et section issue uniquement d'une graine spéculative. Vérifier
   les priorités et le comportement après fusion/reprise.
3. Implémenter le verdict si la provenance existante permet une distinction
   correcte ; sinon ajouter le suivi nécessaire et expliquer la migration des
   états persistés. Ne pas attribuer `speculative` sur la seule absence de
   preuve naturelle. Exposer le résultat dans les rapports déjà existants.

## Tranche 3 — banc de parité exécutable

1. Étendre la preuve de concept `tests/test_audit_parity.c` en un banc
   reproductible, d'abord GB historique/noyau, puis au moins une autre ISA
   réellement prise en charge. Réutiliser les fixtures générées au build.
2. À options, graines et budgets comparables, comparer les ensembles d'octets
   physiques et leurs sources ; publier intersection, différences dans les
   deux sens et changements de classe. Ne pas imposer une égalité artificielle
   quand les politiques diffèrent : conserver chaque différence avec sa raison
   vérifiable et un témoin négatif, sans masquer les pertes inconnues.
3. Enregistrer version, commandes, empreintes, budgets, saturations et résultats
   en JSON. Tester la détection d'une perte injectée, d'une différence admise
   et d'un résultat tronqué ou incompatible. Une liste d'exceptions doit être
   étroite, explicable et contrôlée par les tests.
4. Étendre ensuite le banc aux autres plateformes, à la reprise et aux
   frontières, en conservant les lacunes explicitement ouvertes.

## Tranche 4 — tests manquants et documentation

Ajouter les tests indépendants de l'audit E7/E8 lorsque leurs fixtures
prouvent le comportement visé. Corriger les contradictions documentaires
Z80/NES/PCE/MD de l'audit après vérification du code ; préserver les notes
historiques en les datant. Ne pas modifier la matrice normative tant que les
cellules concernées n'ont pas chacune une preuve actuelle.

## Coordination et limites

- Réserver à l'autre tranche `src/deepeel.c`, `src/explore/core.c`,
  `src/explore/core.h`, `src/explore/core_gbseeds.c`, `src/explore/core_map.c`,
  `src/explore/frontier_disk.c`, `src/explore/residual.c`, `src/report/`,
  `tests/test_explorer.c`, `tests/test_kernel.c`, `tests/test_map_infer.c`,
  `docs/GB.fr.md` et le plan principal. Pour E1 et E3, créer des tests dans
  de nouveaux fichiers ; coordonner toute modification indispensable à un
  fichier réservé avant de l'entreprendre. Dans `tests/CMakeLists.txt`, ajouter
  seulement les déclarations nécessaires en fin de fichier et relire l'état
  courant avant édition.
- Respecter les autres modifications présentes ; aucune ROM dans le dépôt.
  Ne pas committer sans instruction. Ne pas promettre la parité globale à
  partir de deux fixtures ni assimiler des octets forcés à du naturel.
- Exécuter les tests ciblés et `ctest --test-dir build --output-on-failure` ;
  mentionner les tests ignorés, échecs préexistants et résultats de chaque
  plateforme. Tout changement visible doit être documenté ou versionné selon
  `AGENTS.md`. Livrer les commandes, les écarts réels et les limites restantes.
