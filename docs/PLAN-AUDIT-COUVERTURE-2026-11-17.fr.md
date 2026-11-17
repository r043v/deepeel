# Audit du code et plan d’augmentation de la couverture

Analyse du 17 novembre 2026, version 0.391.0, HEAD `9ca63cff`.
Analyse statique seulement : aucun programme du projet, test ou ROM exécuté ;
aucun code modifié. Les scénarios ci-dessous sont des fixtures **à écrire**,
pas des résultats expérimentaux. Les références de lignes correspondent à
l’arbre analysé. Les gains restent qualitatifs, sans pourcentage inventé.

Références : `README.md`, `docs/GB.fr.md` (notamment 0.389–0.391),
`docs/COUVERTURE.fr.md`, `docs/PLAN-COUVERTURE-MAX.fr.md`, `AGENTS.md`.
Cet audit complète le plan précédent sans le remplacer ni considérer ses
corrections annoncées comme des preuves de correction du code courant.

## 1. Conclusion sur la mutualisation

Le moteur dynamique est bien commun. Les commandes GB, Z80 (SMS/GG/SG/MSX),
NES, PCE et MD appellent `dp_core_explore_cli`, puis `dp_core_explore` :
`src/deepeel.c:767,1165,1292,1461`, `src/platform/sega/command.c:166`.
Le noyau porte les fourches, les sessions, le rejeu TAS, les checkpoints,
les sondes, le rejeu d’index, la résolution des conditions, les copies,
la fermeture statique, les cellules, la projection et la persistance.
Les cartes ont une projection commune (`src/explore/core_map.c:11`).

**Un moteur commun ne signifie cependant pas que toutes les méthodes sont
effectivement disponibles ou correctes sur chaque plateforme.**

| Méthode | État réel dans le code lu | Travail restant |
|---|---|---|
| Fourches conditionnelles | Politique commune ; dépend de `DpFlowInfo` | Appels/retours conditionnels mal raccordés (§2.4) |
| Sondes, RNG, signatures, index | Politique commune, décodeurs `DpIsa` propres aux cinq ISA | Réarmement incomplet (§2.5–6), lectures matérielles (§4) |
| Fermeture par constantes | `get_regs` + `reg_effect` communs | Sentinelle de plafond erronée, clés et plafonds (§2.3, §3) |
| Fermeture Z80 | Marcheur générique **et** `src/platform/sega/closure.c` | Deux chemins de couverture statique encore actifs |
| Tables de sauts | Aides communes mais tests d’opcodes et formats particuliers dans le noyau | Décrire les dispatchs dans `DpIsa` |
| Banques | Politique commune, capacité `bank_view` | Déclenchement incomplet et cap destructif (§2.7) |
| Copies et code RAM | Observateurs communs | Provenance non locale au chemin (§2.8) |
| État exact | GB/NES/PCE/MD ont `state_hash` | Aucun `state_hash` installé par `dp_machine_sega` |
| Entrées/sessions | Ordonnancement commun | Masques, catalogue et états périphériques restent spécifiques |
| Rapport/carte | Projection commune | Sources et hits incomplets sur certains producteurs (§3) |

La closure Sega est créée dans `command.c:61–75`, alimentée avant chaque pas
(`:78–90`), exécutée et fusionnée à la fin (`:92–103`). Elle n’est donc pas
simplement du code mort depuis le retrait des anciens moteurs. La retirer
avant d’avoir porté ses preuves utiles risquerait de perdre de la couverture.

Les tests d’opcodes `E9/DD/FD` et `6C/7C` de `core.c:1964–1970`, ainsi que
les signatures de mots `$4000–$7FFF` de `:446–460`, montrent que la séparation
ISA/politique reste partielle malgré la note « plus aucune ISA nommée ».

## 2. Défauts prioritaires établis par lecture

« Établi » désigne ici une contradiction ou un chemin erroné visible dans le
code ; son impact sur une ROM réelle n’a pas été mesuré.

### 2.1 P0 — Tampon de décodage insuffisant pour le 68000

Preuves : `src/explore/core.c:1804–1811`, mais aussi `:303`, `:530`, `:756`,
`:840`, `:869`, `:929` ; `src/mach/m68k_isa.c:18` ;
`src/cpu/m68k/m68k_disasm.c:16–24,125` et suite.

Le noyau fournit des tableaux de **8 octets** à `isa->flow`. Le wrapper
68000 annonce **10 octets disponibles** à `dp_m68k_decode`. Une instruction
de dix octets, par exemple `MOVE.L abs.l,abs.l`, fait lire les deux derniers
octets hors du tableau. Une longueur d’instruction de dix octets est ensuite
également utilisée pour l’archive RAM à partir du tampon de huit octets.
Le résultat peut être une mauvaise longueur, un opérande faux, une lecture
hors limites ou une couverture qui dépend du contenu de la pile hôte.

Action : un contrat explicite de taille maximale et de taille disponible
pour le décodage ; vérifier tous les appels dynamiques, statiques et RAM.
Fixture : instruction 68000 de dix octets suivie d’un transfert vers une
nouvelle plage ; vérification sous ASan, des longueurs et des octets archivés.

### 2.2 P0 — Signature d’entrée statique : fenêtre de huit octets dépassée

Preuve : `src/explore/core.c:446–460`, `closure_entry_is_data`.

`w` contient huit octets. Pour `o <= 4`, le test de trois mots consulte
`w[o + 2*q + 1]` ; avec `o=3,q=2`, il atteint `w[8]`, puis potentiellement
`w[9]` avec `o=4`. Le court-circuit ne garantit pas leur absence : une
fenêtre dont chaque paire chevauchante forme un mot de la plage attendue
peut maintenir `hit/ok` vrais jusque-là.

Action : borner les décalages par la taille réelle ou charger la fenêtre
nécessaire ; sortir la géométrie GB de cette règle générique.
Fixture : fenêtre de huit octets `$40` et appel de fermeture non fiable,
avec détection ASan. Vérifier ensuite les décisions de vraisemblance.

### 2.3 P1 — `closure_seed_cap == 0` refuse les amorces au lieu de lever le plafond

Preuves : `src/explore/core.c:80,103,1984–1996`, `core.h` : 0 = sans plafond.

La condition de refus commence par `!cfg->closure_seed_cap`. Ainsi
`--closure-seed-cap 0` refuse toutes les nouvelles amorces concrètes.
Le parseur de `--unbounded` pose précisément cette valeur à zéro : la CLI
coupe donc ce mécanisme au moment où elle promet d’en lever les plafonds.
L’appel direct à l’API avec `unbounded=1` et le plafond par défaut ne suit
pas exactement la même voie : normaliser la configuration une seule fois.

Action : ne refuser que si un plafond non nul et applicable est atteint.
Fixture : cible indirecte prouvable seulement depuis les registres concrets ;
elle reste atteinte avec plafond 0 et avec `--unbounded`, via CLI et API.

### 2.4 P1 — Les appels et retours conditionnels échappent au fork générique

Preuves : `src/cpu/sm83/sm83_isa.c:153–190`,
`src/cpu/z80/z80_isa.c:50–85`, `src/explore/core.c:2073–2080,2136–2140`.

Le noyau ne fourche que `kind == DP_FLOW_COND && target >= 0`.
Les `CALL cc` SM83/Z80 sont classés `DP_FLOW_CALL` : pas de fork de l’issue
non observée. Les `RET cc` sont conditionnels mais gardent `target=-1` :
pas de fork non plus. La fermeture peut marquer du statique, mais cela ne
remplace pas l’exécution de l’autre issue avec ses effets sur pile et données.

Action : représenter séparément la condition, la nature du transfert et sa
cible éventuellement calculée. Construire les deux états **avec les effets
de l’instruction** : changer seulement PC depuis un état avant CALL/RET ne
reproduit pas les push/pop. Garder l’identification des IRQ intervenantes.
Fixtures : CALL cc et RET cc pris/non pris sur SM83/Z80, avec données nouvelles
dans chaque issue, SP et adresse de retour vérifiés ; étendre le contrat aux
conditions composées DBcc et aux répétitions de blocs.

### 2.5 P1 — Réarmement : cache invalidé par les lectures, pas par les valeurs

Preuves : `src/explore/core.c:1018–1021,1177–1208` ;
`docs/GB.fr.md`, note A2 en 0.391.0.

`cell_key` hache les **valeurs** de RAM. `ctx_rearm` mémorise ce hash selon
`ctx_epoch`, incrémenté seulement lorsqu’un nouvel octet rejoint l’ensemble
lu. Un compteur de niveau, de chanson ou d’état peut changer sans nouvelle
adresse lue : le cache conserve alors l’ancienne cellule et supprime un
réarmement utile. La documentation reconnaît ce comportement en 0.390.0 ;
il s’agit d’une restriction volontaire existante, mais son caractère
« redondant » n’est pas justifié pour l’objectif de couverture maximale.

Action : conserver l’optimisation avec des versions des pages écrites,
une invalidation au chargement d’état, ou un recalcul aux tentatives
1/2/4/8. Une époque d’ensemble lu ne prouve pas l’égalité de ses valeurs.
Fixture : même site, mêmes adresses lues, valeur de mode différente ;
le second contexte doit réarmer et découvrir une plage distincte.

### 2.6 P1 — Un réarmement IRQ accepté est ensuite rejeté comme doublon

Preuves : `src/explore/core.c:2052–2063`.

Lorsque `ikey` est déjà connu, un succès de `ctx_rearm` pose `known=0`.
Après création du snapshot, `fresh = known || keyset_insert(..., ikey)`
retente l’insertion de **l’ancienne clé déjà présente** : résultat faux,
snapshot libéré. La nouvelle clé de contexte dépensée par `ctx_rearm`
n’autorise donc pas la nouvelle fourche IRQ.

Action : porter explicitement l’autorisation de réarmement jusqu’à la mise
en file, sans exiger une nouvelle insertion de la clé historique.
Fixture : même site/délai IRQ sous deux cellules, second handler dépendant
du nouveau contexte ; deux états gardés et une nouvelle plage découverte.

### 2.7 P1 — Fourches de banques : déclenchement incomplet et variantes perdues

Preuves : `src/explore/core.c:1091,2085–2111`,
`src/mach/sega_machine.c:173,322`, `src/mach/pce_machine.c:346`.

Trois problèmes distincts :

- Le déclencheur dynamique ne teste que `rom_index(addr) >= 0`. Il ignore
  `m->mapper_reg`, pourtant fourni pour les registres Sega `$FFFC–$FFFF`.
  TAM sur PCE et les changements de slots par ports ne sont pas des writes ROM.
- La clé `(site,banque)` est insérée **avant** de vérifier `bank_fork_cap`.
  La première variante refusée est consommée sans être mise en file ni différée.
  Le cap par défaut 16 reste actif dans la normalisation `--unbounded`.
- Après `bank_view` et sauvegarde, le code restaure `cur`, puis appelle
  `queue_push(sh,m,...)` : l’index `Pending.tri` est calculé dans l’ancien
  mapping, et non celui du snapshot. Cela fausse la priorité par nouveauté.

Action : événement générique de changement de mapping (écriture mémoire,
port ou instruction CPU), clé dépensée après admission, report des refus,
index résolu dans la vue sauvée, cap levé en non borné.
Fixtures : mapper Sega en RAM, TAM PCE, plus de 16 banques, reprise après
saturation, deux snapshots au même PC dans des banques différentes.

### 2.8 P1 — Provenance ROM→RAM partagée et périmée

Preuves : `src/explore/core.c:1083–1116,1821–1845,2577,2615–2617`.

`fed/fedsrc/fedprod` sont partagés entre workers et entre chemins. Ils ne
font pas partie des snapshots. Les écritures fixent la provenance, mais
une réécriture ultérieure sans lecture ROM récente peut sortir avant toute
invalidation (`last_rom < 0 || age > 4`). Une adresse RAM peut donc garder
la source d’un autre état, voire d’un autre worker, après remplacement.
L’exécution RAM crédite alors de faux octets ROM comme code naturel ; le
test de RAM « alimentée » d’un chemin forcé peut aussi devenir faux.
Les accès ordinaires concurrents à ces tableaux ajoutent un problème de
course mémoire ; des atomiques seuls ne résoudraient pas l’erreur de modèle.

Action : provenance locale à la machine et versionnée avec les snapshots,
invalidée à chaque écriture dont la source n’est pas prouvée ; distinguer
égalité d’octet et preuve de copie. Prévoir une forme creuse/delta pour le coût.
Fixtures : copie puis écrasement puis exécution ; deux états au même PC RAM
avec sources ROM différentes ; aller-retour spool ; jobs 1/2 ; SMC.

### 2.9 P1 — Le budget de chemin coupe encore définitivement `--unbounded`

Preuves : `src/explore/core.c:1257–1258,1587–1610,1782,2527–2532` ;
`tests/core_unbounded.py:13–16` ; `docs/GB.fr.md`, A8 0.389.0.

La conservation de `max_path_steps` est une décision d’accordage documentée.
Mais atteindre ce budget fait `break`, puis `defer_state` refuse précisément
de conserver le chemin en non borné. Une boucle finie au-delà du quantum
peut donc cacher définitivement son code de sortie. Ce n’est pas un
épuisement sémantique. Le commentaire du test assimilant cet arrêt à un
épuisement ne démontre pas le contrat demandé dans AGENTS.md.

Action : conserver les chemins courts comme **quantum d’ordonnancement**,
puis réenfiler la continuation au lieu de la détruire. Le rendement d’un
ancien régime borné ne justifie pas une perte définitive. Le mécanisme de
yield à 65 536 pas existe déjà ; préserver aussi les métadonnées d’analyse
nécessaires entre quanta. Étendre l’audit aux budgets `held_steps`, sondes
et plafonds de marche statique restant actifs.
Fixture : compteur multi-octets sortant après plusieurs quanta, seule sortie
vers une plage nouvelle ; couverture finale identique avec petits/grands
quanta, avec spool et interruption/reprise.

### 2.10 P1 — Transfert SM83 : ordre d’évaluation non garanti

Preuve : `src/cpu/sm83/sm83_isa.c:59–64`.

`sm_set_byte(out, sm_byte_slot(..., &by), by, b[1])` lit `by` dans un argument
et l’initialise dans un autre. C ne garantit pas cet ordre d’évaluation.
La valeur de l’octet haut/bas peut donc être lue avant son initialisation,
corrompant la propagation des constantes et les cibles indirectes prouvées.

Action : calculer d’abord le slot et `by`, puis appeler `sm_set_byte`.
Fixture : chaque `LD r,n`, puis formation BC/DE/HL et saut indirect ; mêmes
valeurs connues sous plusieurs compilateurs et niveaux d’optimisation.

## 3. Risques complémentaires à caractériser

Ces points ont un mécanisme suspect visible ; leur contre-exemple complet
doit être épinglé avant de choisir le correctif.

1. **Cellules trop pauvres.** `cell_key` (`core.c:1120`) inclut cible et au
   plus 4096 valeurs RAM, mais ni registres ni mapping. Deux états peuvent
   atteindre le même PC CPU avec un registre/une banque différent et être
   fusionnés avant le test de projection. Inclure l’identité ROM et les
   dépendances effectives, ou traiter cette éviction comme heuristique avec
   frontière conservée. Tester les états qui ne diffèrent que par un registre.
2. **Projection fondée sur une liveness incomplète.** `proj_key` (`:1134`)
   ne regarde que les pages ROM du site et de la cible, pas les dépendances
   transitives d’un callee. Une absence de lecture observée n’est pas une
   preuve de non-lecture future. Tester une sous-routine située sur une
   troisième page qui lit une RAM non encore apprise ; préserver les candidats
   évincés tant que la projection n’est pas justifiée.
3. **Dédup statique tronquée.** La clé `ri << 40 | hash40` (`:1986`)
   ne conserve que 24 bits d’index ROM dans 64 bits : alias structurel au-delà
   de 16 Mio. `regwalk_seen` (`:370`) confond aussi 64 sondages épuisés et
   état déjà vu ; `REG_SET_CEIL` reste fixe même en non borné (`:394`).
   Stocker une identité vérifiable, distinguer plein/doublon, faire croître
   ou différer. Fixture synthétique de grande image et forte collision.
4. **État dit exact.** Auditer quelles horloges et quel état périphérique
   sont exclus : deux états CPU/RAM égaux avec une IRQ plus proche peuvent
   avoir des suites différentes. Le hash GB n’inclut pas son suffixe wrapper
   `last_ei` (`gb_machine.c:43–69`). Tester égalité CPU/RAM mais ombre EI ou
   phase périphérique différente avant d’admettre une fusion définitive.
5. **Reprise multi-fichiers.** `frontier.bin` valide taille/magie mais pas
   identité de ROM/backend (`core.c:1302`), et les fichiers de couverture,
   cellules et exact sont séparés. Ajouter identité ROM, schéma d’état et
   génération cohérente ; tester deux ROMs de même taille et arrêt entre
   écritures. L’atomicité de la seule frontière ne suffit pas à prouver cela.
6. **Sources et résultats de la closure Sega.** La fusion `command.c:98–100`
   pose STATIC sans source CLOSURE, et ne transpose pas les autres preuves
   du moteur statique. Examiner ses données prouvées et ses saturations avant
   migration. Les annotations historiques de la norme ne reflètent plus
   toujours les producteurs réellement actifs.
7. **Chaleur et source RAM.** `dp_core_write_cov` écrit toujours `1` comme
   compteur (`core.c:3084`) malgré `r->hits`. L’attribution d’exécution RAM
   pose NATURAL sans mettre à jour les sources et les débuts d’instructions
   (`:1834–1843`). Clarifier ces contrats et vérifier tous les exports.

## 4. Possibilités d’augmenter la couverture après corrections

| Priorité | Technique proposée | Gain recherché | Critère de validation |
|---|---|---|---|
| Haute | Contrat de capacités machine/ISA inspectable | Détecter les méthodes absentes plutôt que les supposer acquises | Tableau automatique hooks requis/fournis pour les cinq ISA et profils machine ; absence explicitée |
| Haute | Lectures matérielles alternatives avec `arm_read`, ports compris | Débloquer les conditions sur statut VDP/PPU, timer, pad, IRQ | Même fixture logique par plateforme ; lecture alternative persiste dans un snapshot et ne modifie pas la branche témoin |
| Haute | Dispatchs décrits par l’ISA : mots, tables séparées bas/haut, offsets relatifs, RTS, banque+cible | Atteindre les routines jamais appelées dans le chemin concret | Provenance et bornes de chaque entrée ; une routine cachée exécutée, octets de table classés données |
| Haute | Rejeu d’index guidé par les bornes et dépendances | Explorer chansons, scènes, modes, listes d’objets au-delà de 0–15 | Valeur élevée seule productive ; limites prouvées et indices larges ; rejeu en conservant le contexte vivant |
| Haute | Checkpoints diversifiés par nouveauté de données, mapping et code RAM | Sortir des sessions qui reviennent toujours au même écran | Découverte exclusive depuis un état tardif ; coûts bornés en régime borné, états conservés en non borné |
| Moyenne | Résolution de conditions sur plusieurs octets et expressions | Débloquer seuils 16/32 bits, masques et validations couplées | Condition impossible en ne changeant qu’un octet, solution vérifiée par rejeu ; source fabriquée conservée |
| Moyenne | Provenance des transformations et des décompresseurs | Rattacher le code généré et découvrir les tables comprimées | Dépendances exactes ou explicitement approximatives ; pas de simple égalité prise comme preuve |
| Moyenne | État exact Z80 derrière le backend | Libérer du budget utile et rendre les boucles non bornées équitables | Sérialisation sans pointeurs hôte dans l’identité, sans supprimer les phases matérielles utiles |
| Moyenne | Exploration du processeur sonore MD | Couvrir les alternatives du driver Z80 et ses banques | Instance machine du Z80 rattachée à l’état MD, fourches synchronisées, provenance restaurée |
| Moyenne | Frontière commune par raison et vieillissement | Reprendre les variantes refusées et éviter la famine du spool LIFO | Chaque refus budgétaire est conservé ; une ancienne variante finit par être explorée |

Le solveur actuel essaye les 256 valeurs d’un octet, mais sa fenêtre suit
au plus quatre adresses et huit instructions (`core.c:1023,1393,2005`).
Étendre cette profondeur doit être guidé par des dépendances, pour éviter
une explosion cartésienne sans progrès. Les nouveaux états doivent conserver
leur provenance « fabriquée » : un gain de couverture n’est pas une preuve
d’accessibilité depuis le boot sans modification de l’état.

Pour la mutualisation statique, porter les **preuves et formats** de la
closure Sega dans `DpIsa`/le marcheur commun, puis comparer l’union des
preuves par octet. Ne pas copier une plage seulement parce que l’ancien
marqueur la disait code. Une limite de hook est un portage à faire, pas une
impossibilité matérielle.

## 5. Ordre de réalisation et acceptation

1. **Sécuriser le décodage (S).** §2.1, §2.2 et §2.10 ; fixtures minimales
   avec ASan/UBSan lorsque pertinent. Établir des résultats reproductibles
   avant de comparer les rendements de couverture.
2. **Restaurer les mécanismes promis (M).** §2.3, §2.5 et §2.6 ; comparer
   exactement les états acceptés, pas seulement les compteurs d’essais.
3. **Fermer les trous de flot (M/L).** §2.4 et §2.7 ; contrats communs pour
   transfert conditionnel et changement de mapping, fixtures par ISA.
4. **Préserver le travail (M/L).** §2.9 et risques cellules/projection ;
   quantum réenfilé, refus différés, reprise après SIGINT. Maintenir la règle
   autorisée « deux côtés déjà couverts » ; elle ne prouve pas l’équivalence
   des états et ne doit pas être étendue implicitement à d’autres évictions.
5. **Rendre le code RAM fiable (L).** §2.8, provenance dans les snapshots,
   SMC, cohérence jobs 1/2 et spool. Corriger avant de créditer des gains RAM.
6. **Terminer la mutualisation utile (L).** Capacités explicites, dispatchs,
   lectures matérielles, état Z80, puis retrait de la closure particulière
   seulement après comparaison des preuves utiles.
7. **Mesurer et élargir (continu).** Rejeu d’index, checkpoints, résolution
   multi-octets, processeurs auxiliaires et matériel manquant ; prioriser
   les techniques dont une ablation montre des découvertes exclusives.

S/M/L désignent ici un ordre de grandeur relatif, pas une estimation en jours.
Après chaque correction du noyau : `ctest --test-dir build --output-on-failure`,
incluant `test_core`, `test_coverage` et fixtures du noyau, conformément à
AGENTS.md. Les fixtures ROM sont générées au build, jamais ajoutées au dépôt.
Incrémenter la version ou documenter dans GB.fr.md tout changement visible.

### Avancement de l'implémentation

- Les tampons de décodage, la limite nulle des amorces, la propagation SM83,
  le réarmement des cellules et des IRQ, ainsi que les fourches CALL/RET
  conditionnelles SM83/Z80 ont reçu des corrections et des fixtures ciblées.
- Les écritures de mapper reconnues déclenchent les variantes de banques ;
  certaines variantes saturées sont différées. Un quantum non borné rend
  désormais la main au lieu de terminer le chemin lorsque du travail attend.
- La reprise vérifie l'identité de la ROM et du backend ; la cohérence
  transactionnelle de tous les fichiers de checkpoint reste à traiter.
- La fermeture Sega renseigne sa source et l'export de couverture donne les
  compteurs connus sur les débuts d'instructions. L'ombre EI entre dans le
  hachage Game Boy, sans que cela démontre l'exactitude des autres empreintes.
- La provenance ROM→RAM n'est **pas encore locale aux snapshots**. Les
  invalidations et vérifications ajoutées réduisent certains faux positifs,
  mais ne constituent pas une preuve de copie entre chemins concurrents.
- Les dispatchs ont un premier hook ISA ; le port complet de la fermeture
  Sega, les lectures matérielles supplémentaires, la frontière exhaustive,
  les techniques d'élargissement et les validations associées restent ouverts.
- Une campagne complète de 232 fixtures a dépassé 300 secondes sur
  `fix_irq.gb` ; le coût de l'exploration IRQ reste à traiter. Le test de
  campagne utilise maintenant un budget explicite réduit, sans modifier les
  réglages de la commande de campagne complète. Les tests d'options de
  moteurs retirés ont été supprimés ; les tests des mécanismes actuels de
  sessions et checkpoints restent actifs. Le test d'export PCE vérifie `.cov`,
  puisque `--evidence` n'est plus produit. Un avertissement signale désormais
  que `--evidence` et `--romdata` sont acceptés mais non produits.
- La suite `ctest --test-dir build --output-on-failure` a terminé avec
  179 tests réussis et `lr_gambatte_parity` ignoré. Le test de parité des
  références a pris 133 secondes. La campagne rapide et l'export `.cov` PCE
  ont également réussi dans un répertoire de compilation propre. Ces
  résultats ne valident pas la provenance RAM entre instantanés ni la
  couverture exhaustive en mode non borné.

### Mesure à préparer, sans l’exécuter dans cet audit

- Relever séparément code dynamique naturel, forcé, statique confirmé,
  provisoire, données lues, copies et inconnu. Une hausse d’inférence n’est
  pas un gain d’exécution ; corriger un faux positif peut réduire le total.
- Comparer les **ensembles d’index ROM** avant/après, les ajouts et les pertes,
  et pas uniquement un pourcentage global ou une couleur de carte.
- À budgets identiques, ablater une méthode à la fois ; mesurer découvertes
  exclusives, temps CPU, RAM, octets de spool, frontière et saturations.
- Tester petit budget + reprise contre exploration continue sur fixtures
  finies ; des compteurs de perte nuls ne prouvent pas l’absence de perte
  lorsque le code ne compte pas les états détruits.
- Comparer jobs 1/2, taille de file petite/grande, ordre des options CLI,
  mode borné et non borné. Une différence doit être attribuée au budget,
  à l’ordonnancement ou à une erreur, jamais masquée par l’union globale.
- Les campagnes futures sur ROMs réelles hors dépôt seront des observations
  de l’outil, sans les présenter comme des mesures de matériel.

## 6. Limites de cet audit

Lecture ciblée des méthodes de couverture du noyau, contrats machine/ISA,
adaptateurs, points d’entrée CLI, rapports, documentation récente et tests
associés. Il ne s’agit pas d’une validation exhaustive des émulateurs
embarqués, de tous les mappers ni du matériel de chaque console.
Aucune affirmation de suite verte, de reproduction effective ou de gain
mesuré ne découle de cette lecture seule.
