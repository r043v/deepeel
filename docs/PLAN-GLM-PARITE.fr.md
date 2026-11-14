# Plan GLM — audit de parité des moteurs d'exploration

## Mission et périmètre

Établir une matrice vérifiable des capacités des explorateurs historiques et
du noyau `--core` pour GB, SMS, GG, SG-1000, MSX, NES, PCE et MD. Distinguer
présence du code, activation par défaut, fixtures et mesures réelles. Les
pourcentages conventionnels du plan principal ne sont pas des mesures de
couverture. Lire d'abord `README.md`, `docs/GB.fr.md`,
`docs/COUVERTURE.fr.md`, `docs/PLAN-MUTUALISATION.fr.md` et
`docs/PLAN-SUPPORT-EXPLORATION.fr.md`.

## Livrables

1. Créer `docs/AUDIT-PARITE-MOTEURS.fr.md` : pour chaque plateforme et méthode,
   indiquer historique / noyau, option et défaut, preuve par fichier et test,
   limites et état `validé`, `partiel`, `absent` ou `à vérifier`. Couvrir au
   minimum branches, IRQ, sondes, graines, banques, provenance, copies, code
   RAM, fermeture, déduplication, frontières et reprise, sessions/TAS, sorties
   et parallélisme. Séparer SG des autres profils Z80.
2. Répertorier les contradictions entre documents historiques et code actuel,
   avec références précises et corrections proposées. Ne pas conclure qu'une
   technique est impossible parce qu'elle manque dans un moteur.
3. Proposer un banc de parité reproductible : mêmes fixtures générées, graines,
   options et budgets ; comparaison des ensembles et sources par octet, des
   états différés et compteurs, ainsi que des sorties et saturations. Distinguer
   les différences de politique attendues des pertes de couverture.
4. Classer les écarts par impact, dépendances et test minimal. Si possible,
   réaliser une petite preuve de concept de comparaison dans un nouveau fichier
   propre à l'audit, sans prétendre couvrir toutes les plateformes.

## Contraintes de coordination

Ne pas modifier `src/report/map.c`, `src/report/report.c`, `src/report/report.h`,
`tests/test_map_infer.c`, `docs/GB.fr.md` ou le plan principal : une autre tranche
les utilise. Respecter les modifications locales préexistantes. Aucune ROM dans
le dépôt ; présenter les essais réels comme observations de l'outil. Signaler
explicitement les cellules sans preuve et les tests non exécutés. Si du code
moteur est modifié, appliquer les règles de tests du dépôt et documenter tout
changement visible. Ne pas committer sans instruction ; tout commit éventuel
respecte la règle de co-auteur de la session.
