# Support et progression des méthodes d'exploration

État de travail établi le 13 novembre 2026 pour deepeel 0.330.0. Cette
photographie reprend la matrice §2.1 de [COUVERTURE.fr.md](COUVERTURE.fr.md)
et le bilan discuté dans la session. Elle doit être révisée après audit du code :
la matrice normative mêle des mises à jour récentes et des descriptions historiques.
Elle concerne surtout les explorateurs propres aux plateformes ; le noyau générique
`--core` possède son propre état de parité, à mesurer séparément.
L'[audit par moteur](AUDIT-PARITE-MOTEURS.fr.md) apporte une première
matrice sourcée ; ses conclusions sont à vérifier sur le code actuel avant
implémentation. En particulier, l'hypothèse E1 sur le registre C doit être
confrontée au calcul `$FF00 + C`. Le travail proposé à GLM est détaillé dans
[son plan d'implémentation](PLAN-GLM-IMPLEMENTATION-PARITE.fr.md).

## Lecture des pourcentages

Ces pourcentages sont **des catégories conventionnelles de support**, pas des
mesures de couverture de ROM, de fiabilité ou de quantité de code. Ils traduisent
l'état documenté : 100 % = pris en charge dans le périmètre décrit ; 50 % =
implémentation partielle ou limitée ; — = sans objet ; ? = état non établi.
Une cellule à 100 % ne prouve ni la prise en charge de tous les périphériques,
ni l'activation par défaut, ni la parité entre moteurs. Ne pas calculer de moyenne
globale non pondérée. Les mesures sur ROMs réelles sont des observations de l'outil,
pas des mesures de matériel.

| Méthode | GB | SMS | GG | SG | MSX | NES | PCE | MD |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Fourches des deux issues conditionnelles | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Fourches IRQ/NMI | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Sondes de lecture des manettes | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Fourches d'instructions de répétition | — | 100 % | 100 % | 100 %* | 100 % | — | 100 % | 100 % |
| Observation des cibles indirectes | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Arrêt des chemins non pertinents et padding | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Scan statique : padding et pointeurs | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Fermeture statique du flot | 100 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 50 % | 50 % |
| Suivi de constantes et provenance | 50 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 50 % | 50 % |
| Tables de sauts par provenance | 50 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 100 % | 50 % |
| Recherche des frères de copie | 100 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 50 % | 50 % |
| Rejeu de décodeurs sur tables de flux | 50 % | 50 % | 50 % | 50 % | 50 % | 50 % | 50 % | 100 % |
| Inférence des zones voisines | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Graines reset et vecteurs d'interruption | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Dispatch par vues de banques | 100 % | 100 % | 100 % | ? | 100 % | 50 % | 50 % | 50 % |
| Graines manuelles et banques | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 %† |
| Semis indirect et budgets en cascade | 100 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 50 % | 50 % |
| Sections, liveness et verdicts | 100 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 50 % | 50 % |
| Éviction par contexte | 100 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 50 % | 50 % |
| Déduplication exacte | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| File, stockage disque et reprise | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Sessions jouées et pool de checkpoints | 100 % | 50 % | 50 % | 50 % | 50 % | 50 % | 50 % | 50 % |
| Rejeu TAS et exploration depuis le film | 100 % | 100 % | 100 % | 100 %* | 100 % | 100 % | 100 % | 100 % |
| Go-Explore / cellules de nouveauté | 50 % | 50 % | 50 % | 50 % | 50 % | 50 % | 100 % | 50 % |
| Parallélisme des explorateurs historiques | 100 % | 100 % | 100 % | 100 %* | 100 % | 50 % | 50 % | 50 % |

\* SG est déduit du backend Z80 partagé lorsque la matrice ne distingue pas
explicitement ce profil ; ce n'est pas un audit indépendant. † MD : entrée
manuelle prise en charge ; semis de banques sans objet pour la ROM 68000 plate
dans le périmètre considéré.

## Plan : établir une référence vérifiable

1. **Auditer chaque cellule et chaque moteur.** Créer une matrice structurée
   par plateforme, moteur historique/`--core`, technique et sous-cas : code,
   option et défaut, limites, fixtures, résultats réels et date. Vérifier SG
   séparément. Réconcilier les jalons historiques de la norme avec les ajouts
   0.253–0.330 ; corriger les contradictions. Une cellule sans preuve reste `?`.
2. **Construire un banc reproductible.** Générer les fixtures au build, jamais
   de ROM dans le dépôt. Pour chaque ISA, couvrir branches, IRQ, banques, copies,
   code RAM, états sauvegardés et reprise. Comparer ancien moteur et noyau à
   graines, options et budgets identiques ; comparer les ensembles d'octets et
   leurs sources, pas seulement les totaux. Ajouter des témoins négatifs pour
   les faux positifs et les chemins non équivalents.
3. **Mesurer les apports.** Pour chaque technique, lancer avec et sans celle-ci,
   même budget de pas puis même durée, plusieurs graines pour les sessions.
   Rapporter gains nets d'exécution naturelle, résolue, forcée, statique et
   données, recouvrements, saturation, temps et mémoire. Conserver commandes,
   version, empreintes et compteurs. Un octet classé par inférence ne devient
   pas un octet exécuté. Les ROMs réelles restent hors dépôt.

## Plan : améliorer les techniques existantes

Les travaux ci-dessous sont ordonnés par dépendances et gains attendus ; ils
doivent être réévalués avec les mesures du banc. Porter chaque technique sur
toutes les ISA où elle est possible, avec adaptation aux modes d'adressage et
au matériel. Une limite d'implémentation n'est pas une impossibilité technique.

1. **Parité et conservation de la frontière.** Auditer les branchements,
   sondes, graines, déduplication, attribution des sources et reprises de chaque
   moteur. Porter les manques dans le noyau commun. Vérifier les états différés,
   les lots, les saturations et la reprise ; aucun chemin perdu silencieusement.
   Vérifier `--unbounded` contre le contrat du dépôt. Critère : mêmes preuves
   attendues sur fixtures, et reprise équivalente à une exploration continue.
2. **Provenance et code en RAM.** Étendre le suivi aux registres, à la pile et
   à la RAM avec banques et invalidations correctes, puis aux copies non verbatim
   lorsque les dépendances sont prouvées. Généraliser le rattachement des octets
   ROM au code copié puis exécuté, y compris code modifié. Critère : fixtures
   positives et négatives par ISA ; aucune attribution source non justifiée.
3. **Cibles indirectes et banques.** Étendre S2–S4 et G2–G4 : vues observées,
   changements de mapping prouvés, tables scindées, index et formats propres
   aux ISA. Sur NES couvrir les modes de mapper restants ; sur PCE les TAM
   indirects ; sur MD les modes d'adressage indirects restants. Conserver une
   frontière lorsque la banque ne peut pas être prouvée, au lieu d'inventer
   une cible certaine. Critère : nouvelle cible atteinte sur fixture sans
   mélanger preuve statique et exécution.
4. **Copies, décodeurs et tables indexées.** Généraliser les frères de copie,
   le rejeu S6 et le rejeu par index aux conventions de chaque ISA, avec bornes
   prouvées ou hypothèses marquées. Tester plusieurs banques et flux compressés.
   Critère : données et routines supplémentaires découvertes, sources distinctes,
   aucun accès hors vue valide.
5. **Sections et éviction de contexte.** Suivre les composantes réellement lues
   par section : RAM, banques, IRQ et matériel. Réévaluer NES/PCE/MD avant tout
   changement de défaut. Critère : fixtures d'états presque identiques mais non
   équivalents préservées ; campagne à budget égal sans régression de couverture
   justifiée par une éviction abusive.
6. **Sessions, TAS et matériel.** Mutualiser pool, reprise et sélection des
   checkpoints ; comparer replay et snapshots aux références disponibles.
   Étendre les sondes d'événements et d'entrées et traiter en priorité les
   composants non émulés qui bloquent réellement une ROM. Pour MD, étudier les
   snapshots et fourches du Z80 sonore. Critère : fixture par surface et rapport
   clair des périphériques observés mais non émulés.
7. **Parallélisme.** Mesurer le partage effectif de frontière et les coûts de
   synchronisation ; préserver les états en cas de saturation et la fusion des
   preuves. Critère : union parallèle au moins égale au sériel sur fixtures,
   sans perte silencieuse ; débit et mémoire publiés.

## Plan : trouver et évaluer de nouvelles techniques

Ne pas réinventer comme nouveautés les neuf pistes déjà décrites dans
[PLAN-MUTUALISATION.fr.md](PLAN-MUTUALISATION.fr.md:61) : concolique et
contamination, sondes événementielles, sauvegardes, RNG, ordonnancement par
nouveauté, décompresseurs, index, cellules sémantiques et perturbation temporelle.
Leur existence et leur portée par ISA doivent d'abord être auditées : plusieurs
formes partielles existent déjà.

1. **Analyser le résidu.** Regrouper les octets inconnus et les frontières par
   cause observable : attente d'entrée ou de matériel, branche non résolue,
   cible indirecte, banque, données compressées, code RAM ou saturation. Choisir
   des exemples représentatifs par ISA et conserver un témoin reproductible.
2. **Explorer des hypothèses nouvelles**, à vérifier avant de les qualifier de
   nouveautés : synthèse de courtes séquences d'entrées guidée par les branches
   proches ; interpolation entre checkpoints compatibles pour rechercher des
   préconditions de progression ; analyse différentielle de traces entre états
   proches pour identifier des variables déterminantes ; inférence de protocoles
   de périphériques à partir des séquences d'accès ; recherche guidée par les
   dépendances entre copies, tables et routines de décodage. Toute combinaison
   artificielle d'états doit rester étiquetée hypothétique jusqu'à rejeu cohérent.
3. **Cycle expérimental.** Pour chaque hypothèse : rechercher d'abord le code et
   les travaux déjà documentés, formuler le mécanisme et son risque de faux
   positif, construire une fixture discriminante, puis une implémentation limitée
   et désactivable. Comparer avec/sans, à budgets identiques, sur plusieurs jeux
   et ISA ; mesurer gains uniques, coût, saturation et reproductibilité. Garder,
   ajuster ou retirer selon les résultats et consigner aussi les échecs dans
   [CATALOGUE.fr.md](CATALOGUE.fr.md).
4. **Promouvoir les réussites.** Généraliser dans le noyau et les adaptateurs,
   ajouter compteurs et provenance, tests par ISA, documentation des options et
   limites, puis mettre à jour la matrice. Ne pas présenter un chemin forcé ou
   une hypothèse comme une exécution naturelle prouvée.

## Programme détaillé de recherche de nouvelles ouvertures

### Cadre commun

Chaque piste commence par un inventaire du code, des tests et du catalogue afin
de distinguer nouveauté, extension et duplication. Elle reçoit une hypothèse,
une fixture positive et une négative par ISA pertinente, une option de contrôle,
des compteurs et un journal des hypothèses. Les états artificiels et les cibles
supposées conservent une source distincte ; seul un rejeu depuis un état cohérent
peut établir une exécution naturelle. Un résultat négatif est consigné avec ses
conditions. Les files et budgets restent soumis au contrat de frontière du dépôt.

### R1 — Diagnostic du résidu et remontée des adresses

**Hypothèse.** Une partie des zones inconnues est référencée indirectement par
des constantes séparées, un calcul d'offset, une table ou une sélection de banque.

1. Produire une liste de plages inconnues, de leurs voisins et des indices connus
   (lectures, pointeurs, vues de banques, copies, compression et frontières).
2. Rechercher dans le code analysé les producteurs possibles de leur adresse :
   immédiats, octets haut/bas, opérations arithmétiques, accès RAM et mapper.
3. Associer chaque candidat aux preuves, à la vue mémoire nécessaire et aux
   opérations non résolues ; créer une frontière ou une graine hypothétique,
   sans transformer la cible en code prouvé.
4. Vérifier par rejeu concret lorsque les préconditions sont réalisables.

**Fixtures.** Adresse construite en deux morceaux et banque sélectionnée ;
constantes similaires sans relation et banque incompatible. **Mesures :**
plages expliquées, candidats confirmés ou infirmés, faux positifs, coût et
nouveaux octets par source. **Fin :** rapport reproductible et aucune preuve
statique comptée comme exécution.

### R2 — Différences entre checkpoints

**Hypothèse.** Deux états au même site et dans des vues compatibles peuvent
révéler les variables responsables d'une divergence de flot.

1. Apparier des checkpoints selon PC, vue de banques et contexte matériel ;
   calculer les différences RAM et registres, filtrées par les lectures futures.
2. Tester les différences dans des clones isolés, par groupes puis par réduction,
   avec un budget et des compteurs explicites. Ne jamais supposer que la copie
   isolée d'un octet préserve les invariants du jeu.
3. Conserver les dépendances candidates et rechercher un chemin d'entrées ou
   une sauvegarde valide qui reproduit la transition depuis un état cohérent.

**Fixtures.** Un drapeau ouvre une branche ; deux octets liés ou une banque
différente rendent la substitution isolée invalide. **Mesures :** hypothèses
testées, transitions confirmées, divergences non confirmées et coût. **Fin :**
rejeu exact des confirmations et séparation des découvertes artificielles.

### R3 — Synthèse de séquences d'entrées

**Hypothèse.** Une recherche courte guidée par les branches et les variables
pertinentes atteint des états que les pressions aléatoires ratent.

1. Depuis un checkpoint, générer des séquences de boutons et de durées propres
   au protocole de la plateforme ; inclure relâchements et contraintes de timing.
2. Prioriser les séquences modifiant les lectures associées à une branche ou
   produisant une découverte ; garder les autres états selon la politique de
   frontière, sans les supprimer silencieusement.
3. Minimiser les séquences gagnantes et les rejouer depuis le checkpoint, puis
   si possible depuis le début ou un film attesté. Étendre ensuite aux positions
   proches d'un TAS, avec perturbations temporelles explicitement comptées.

**Fixtures.** Combinaison ordonnée de touches, maintien puis relâchement ;
séquence similaire qui ne doit pas ouvrir la branche. **Mesures :** branches
uniques, trames essayées, coût, taux de rejeu et apport hors sessions existantes.
**Fin :** séquence exportable et déterministe sur les fixtures ; comparaison
contre aléatoire et sondes à budgets de trames et de temps égaux.

### R4 — Inférence de protocoles matériels

**Hypothèse.** Les séquences d'accès observées permettent de proposer des états
ou réponses d'un périphérique qui débloquent des chemins encore inexplorés.

1. Journaliser ordre, valeurs, délais, IRQ et sites des accès par périphérique.
2. Construire des candidats de protocole limités, puis générer des réponses de
   statut compatibles avec les transitions observées ; borner les variantes.
3. Comparer à une référence disponible ou à des tests matériels connus ; les
   comportements non confirmés restent des sondes hypothétiques, sans prétendre
   émuler le périphérique. Prioriser selon les blocages observés sur les ROMs.

**Fixtures.** Commande, attente, réponse et acquittement ; réponse prématurée ou
séquence invalide. **Mesures :** attentes débloquées, accès nouveaux, hypothèses
rejetées et divergences de référence. **Fin :** aucune réponse hypothétique
présentée comme comportement matériel établi.

### R5 — Transitions entre phases de jeu

**Hypothèse.** Les transitions d'écran, de mapping ou de structure RAM sont de
bons points de départ pour explorer des phases peu représentées.

1. Définir des signaux mesurables et indépendants du jeu : changements d'écran,
   banques, sections actives et groupes RAM lus. Les comparer aux cellules de
   nouveauté déjà existantes pour identifier l'apport propre.
2. Archiver les états avant et après transition ; ordonnancer ceux qui précèdent
   une transition rare. Explorer entrées et IRQ autour de ces états.
3. Vérifier que la politique modifie seulement l'ordre et que les états moins
   prioritaires restent récupérables ; mesurer aussi le coût d'archivage.

**Fixtures.** Menu vers plusieurs phases et écran animé sans changement de
phase. **Mesures :** transitions distinctes, gains de code et données, coût et
faux signaux. **Fin :** avantage reproductible face aux cellules existantes.

### R6 — Relations entre tables de ressources

**Hypothèse.** Plusieurs tables partagent un index de ressource et décrivent
ensemble adresse, banque, taille, palette ou routine de traitement.

1. Associer les lectures de tables par index et contexte d'appel ; garder le
   pas, la largeur, la vue de banque et les bornes justifiées.
2. Proposer des groupes de tables et rejouer des entrées cohérentes avec leurs
   dépendances ; attribuer chaque découverte à sa preuve et à son mode de rejeu.
3. Généraliser aux décodeurs et aux ressources compressées en respectant les
   formats et bornes prouvés, avec compteur de candidats refusés.

**Fixtures.** Tables parallèles adresse/banque/taille et musique non jouée ;
tables voisines mais indépendantes. **Mesures :** entrées confirmées, octets
nouveaux et associations erronées. **Fin :** aucun accès hors vue ni typage
certain issu d'une simple proximité.

### R7 — Divergences entre machines

**Hypothèse.** Une divergence d'émulation explique certains plateaux de
couverture et peut être localisée par comparaison de traces reproductibles.

1. Inventorier les producteurs de traces et références déjà disponibles par
   plateforme ; définir les champs comparables et les différences de phase
   attendues au démarrage et aux frontières de trame.
2. Rejouer une même ROM autorisée, un même état et les mêmes entrées ; trouver
   la première divergence, puis réduire la séquence et classer la cause.
3. Corriger ou documenter la limite dans le composant concerné, ajouter une
   fixture et mesurer son effet sur l'exploration en aval.

**Fixtures :** divergence injectée de CPU, banque et périphérique ; décalage
de phase connu. **Mesures :** première divergence, trames comparées, causes
résolues et changement de couverture. **Fin :** aucune différence de trace
assimilée automatiquement à une faute sans alignement et diagnostic.

### R8 — Contraintes et regroupement des blocages

**Hypothèse.** Plusieurs frontières sont bloquées par une même précondition ;
les regrouper permet de choisir une expérience à fort rendement.

1. Enregistrer pour chaque frontière les contraintes observées et leur origine :
   comparaison, banque, entrée, IRQ, périphérique, budget ou cible indirecte.
2. Regrouper les contraintes compatibles, estimer le nombre de frontières
   concernées et proposer une action : résoudre, sonder, rejouer ou différer.
3. Vérifier les propositions par exécution concrète ; distinguer preuve
   d'inaccessibilité, contradiction locale et absence de preuve. Une frontière
   non résolue reste récupérable.

**Fixtures.** Plusieurs branches dépendant d'un même drapeau ; contraintes
apparemment semblables dans deux banques incompatibles. **Mesures :** groupes
utiles, frontières débloquées, erreurs de regroupement et surcharge. **Fin :**
diagnostic explicable sans écarter d'état sur une hypothèse non prouvée.

### Ordre et décisions

Commencer par le banc de référence et R1/R8, qui rendent les blocages visibles ;
enchaîner R2 puis R3 sur les mêmes témoins. Mener R7 dès qu'une divergence de
machine soupçonnée invalide une mesure ; R4 dépend d'observations de protocoles.
R5 et R6 suivent lorsque checkpoints et provenance sont suffisamment fiables.
Après chaque prototype, publier une comparaison avec et sans la technique :
ensemble d'octets uniques par source, branches, temps, pas, mémoire, saturation,
et reproductibilité sur fixtures puis plusieurs ROMs hors dépôt. Généraliser
seulement après gain ou diagnostic utile démontré et absence de régression
inexpliquée ; sinon ajuster ou retirer, en conservant les résultats.

## Critères de fin de chaque livraison

Fixtures générées au build, tests ciblés et `ctest --test-dir build
--output-on-failure` verts ; absence de ROM dans le dépôt ; compteurs de
saturation et de perte vérifiés ; version ou documentation mise à jour si le
résultat visible change. Publier les commandes et les écarts mesurés, y compris
les régressions et les limites non encore résolues.

## Avancement de la première tranche

- Le rapport distingue les classes voisines des plages inconnues et indique
  combien de plages sont omises. Le ciblage résiduel parcourt toutes les plages
  et compte celles qui dépassent sa capacité de sortie.
- Le noyau exporte les plages inconnues et les débuts d'instructions observés ;
  `tools/compare_residual.py` compare deux exports compatibles et
  `tools/residual_immediates.py` propose des pistes SM83 explicitement
  hypothétiques, y compris des chargements proches d'octets haut et bas de
  BC/DE/HL si demandé avec un résumé. Ces pistes ne constituent pas une preuve
  de dépendance ni d'exécution de la cible.
- La sauvegarde de la frontière GB historique conserve les snapshots compressés
  présents en file. Des fixtures vérifient également la reprise et les bornes
  des tables candidates.
- La reprise de la frontière du noyau respecte à son tour le seuil de file RAM
  et déborde sur disque, au lieu de charger toute la frontière en mémoire. Le
  fichier de frontière est validé avant reprise et remplacé seulement après
  une écriture complète ; les erreurs sont signalées. Le stockage temporaire du
  noyau reste à rendre durable pour les arrêts brutaux et les erreurs disque.
  Les graines d'entrée empruntent aussi cette file, et leur identité subsiste
  dans ses enregistrements ; en mode sans borne elles ne sont pas écartées
  simplement parce que leur adresse figure déjà dans la couverture.
- Les sources par octet du noyau sont sauvegardées avec sa carte et restaurées
  à la reprise ; une ancienne carte reste utilisable sans inventer les sources
  qu'elle ne contenait pas.
- En mode noyau GB, les entrées `--entry` sont transmises à l'explorateur,
  y compris les banques MBC5 au-delà de 255 ; le plafonnement du scan de
  pointeurs est compté et `--no-pointer-scan` est respecté.

Ces livraisons couvrent une partie de la conservation de frontière et du
diagnostic R1. Elles ne constituent pas encore une matrice auditée par moteur,
ni une comparaison de gains à budgets égaux sur plusieurs plateformes ; les
autres étapes et les expériences R2 à R8 restent à mener.

### État de validation et prochaines étapes

- Les tests ciblés du noyau couvrent la reprise avec débordement, les sources
  restaurées, les anciennes cartes, les frontières tronquées, les échecs
  d'écriture et la perte d'un enregistrement du stockage temporaire. Les tests
  d'intégration vérifient aussi les entrées GB, le résidu et la frontière CLI.
- Les anciens états sans extension ne contiennent pas de sources récupérables.
  Le stockage temporaire du noyau n'est pas durable après un arrêt brutal ; les
  chemins actifs et les erreurs de sauvegarde des autres fichiers d'état restent
  à traiter. Le contrat `--unbounded` doit encore être audité, notamment pour
  les plafonds de visites, les arrêts temporels et l'interruption propre.
- La campagne complète doit être relancée sur la construction définitive. Une
  exécution antérieure a échoué pendant une reconstruction simultanée ; une
  autre a signalé un échec du test P1 alors en cours de développement par GLM ;
  ce test passe après reconstruction des fixtures. Ne pas utiliser ces résultats
  pour déclarer la suite verte ou la parité acquise.
- Après stabilisation de la frontière, poursuivre le banc différentiel par
  ensembles d'octets et sources, puis les expériences R1 à R8 et leur mesure
  sur les plateformes concernées, comme détaillé ci-dessus.
