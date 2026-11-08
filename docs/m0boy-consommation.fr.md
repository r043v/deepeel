# Interface de consommation deepeel → m0boy (axe 9)

Ce que m0boy lit de deepeel, avec les formats et leurs garanties de
stabilité. Objectif de l'axe 9 : m0boy ne fait plus sa propre découverte de
code — il consomme les cartes et rapports deepeel ; sa couverture propre
(harnais `m0_explore` partagé par les cœurs de couverture) est destinée à
être désactivée pour la découverte (les mesures sur binaire recompilé — ce
qui interpète encore — restent, elles servent à atteindre zéro interpréteur).

## Les quatre canaux

| Canal | Commande deepeel | Lecture m0boy | Contenu |
|---|---|---|---|
| `.cov` texte | `explore ROM --cov game.cov` | `gbrecomp --coverage game.cov` (format natif) | `bank addr flags(1=exec,2=entry,4=speculative,8=data) hits` ; plages de données prouvées en une ligne avec une 5ᵉ colonne de longueur |
| PNG de carte | `explore ROM --map game.map.png` | `gbrecomp --coverage game.map.png` | chunk `deepeel_cov` (couverture jouée, comptes de hits pour le budget de flash), chunk `deepeel_rom` (lectures jouées par page de 256 octets : blocs ROM résidents quand la ROM est streamée) |
| `.map` texte | `explore ROM --map game.map` (format texte) | `gbrecomp --coverage-map game.map` (SG-1000/GG/SMS/MSX) | plages de code par banque physique : chaque plage sème la marche du recompilateur (dispatch calculé compilé, pas interprété) |
| code en RAM | `explore ROM --rtcode game.rtcode` | `gbrecomp --ram-code game.rtcode` | variantes `(pc, octets)` du code exécuté hors ROM (WRAM/HRAM), compilées et entrées seulement quand la RAM tient les octets enregistrés |

## Garanties

- Le format `.cov` (« gbl coverage v1 ») est **le format natif des deux
  côtés** : deepeel l'écrit, le recompilateur m0boy le parse (`%x %x %x %x
  %x`, drapeaux 1/2/4/8, 5ᵉ colonne = longueur de plage de données). Toute
  évolution passe par un numéro de version dans la ligne d'en-tête `#`.
- Les chunks PNG sont versionnés par leur nom (`deepeel_cov`,
  `deepeel_rom`) ; un chunk inconnu est ignoré par le lecteur m0boy.
- Les compteurs de hits sont 32 bits (plus plafonnés à 0xFFFF).
- Les nombres sont des observations de l'outil (machine concrète, RAM à
  zéro) — jamais des mesures de matériel.

## Mesure bout-en-bout (validation de l'interface)

- Super Mario Land (World), `.cov` de la campagne TAS deepeel (42 706
  enregistrements) : 6 963 instructions compilées en 74 régions,
  **interpreter-only 0**, 21 125 enregistrements de données prouvées laissés
  en données.
- Blargg `09-op r,r` (ROM de test matériel, 8210 enregistrements) : 53
  instructions compilées, **interpreter-only 0** — la qualité de la
  recompilation suit la couverture du code, le repli ne réapparaît pas.

## État de la désactivation (axe 9, fait côté code)

- `--explore` (le joueur guidé par la couverture, la découverte propre) est
  **retiré de tous les hôtes de cœurs** : meta/GB, sg1000, gamegear, nes,
  pce, vectrex — chaque hôte refuse avec un renvoi vers deepeel. Le harnais
  `m0_explore` reste dans `targets/common` (PICO-8 reste scripté, d'autres
  usages peuvent subsister).
- **Reste actif volontairement** : l'enregistrement de couverture en jouant
  (mode humain) et la mesure incrémentale du binaire recompilé (ce qui
  interpète encore) — c'est la voie vers zéro interpréteur, pas de la
  découverte.
- Les cartes MD/PCE/NES consommées par les recompilateurs correspondants :
  le canal `.map` texte couvre déjà SG/GG/SMS/MSX (`--coverage-map`).
