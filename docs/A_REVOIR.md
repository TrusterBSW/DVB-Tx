# Éléments à revoir

## Moteur arrêté quand une radio n'a plus de porteuse (3/11/2026)

Constaté sur l'i3 (10.5.1.23) pendant les tests DVB-S2X de dvbrx.

**Reproduction (API web)**
1. Config de départ : radio 0 `hackrf` (porteuse 0, désactivée), radio 1 `b210`
   (porteuses 1 et 2).
2. Supprimer les porteuses 2 et 1 (`DELETE /api/config/carrier/2` puis `/1`).
3. Passer la porteuse 0 sur la radio `b210` (`PATCH /api/config/carrier/0`,
   `"radio": "b210"`), puis `POST /api/config/apply`.

**Résultat**
- La validation accepte la config et l'apply réussit.
- `work/engine.ini` ne contient que `[radio 1]` : la radio `hackrf`, sans
  porteuse, est omise, mais la numérotation des radios est conservée.
- Le moteur refuse l'INI (« must be numbered 0..15 in order »), sort en code 1
  et redémarre en boucle (« engine exited (1), restart in 16 s / 30 s »). Plus
  rien n'est émis, sans erreur visible côté API (`pending` vide, apply `ok`).

**Pistes**
- Renuméroter les radios dans l'INI (0..n-1 sur les radios écrites, avec une
  table de correspondance pour `radio = ` des porteuses), ou écrire aussi les
  radios sans porteuse.
- Faire échouer l'apply (ou la validation) si l'INI produit est refusé par le
  moteur, au lieu de laisser une boucle de redémarrage silencieuse.

**Contournement utilisé** : supprimer la radio vide (`DELETE /api/config/radio/0`)
pour que la radio restante soit la radio 0.

## Validation : porteuses désactivées comptées dans la bande d'une radio

Avec les porteuses 1 et 2 désactivées (`enabled = false`), passer la porteuse 0
sur la radio `b210` à 1250 MHz est refusé : « radio b210: 3 carriers need 768.000
MS/s, more than uhd can do (61.44) ». Les porteuses désactivées ne sont pas
émises : elles ne devraient pas compter dans la bande (ou le message devrait
dire qu'elles comptent).

## État au 2026-11-06

Les deux points sont corrigés, mais pas encore commités. Ils sont déployés sur l'i3.

- **Radio sans porteuse.**
  - `engine.ini` numérote les radios 0..n-1 (`config.engine_radios`), et le superviseur retraduit les numéros dans les statistiques.
  - Un « Appliquer » que le moteur refuse remet l'ancienne configuration à l'antenne, renvoie l'erreur du moteur, et laisse les modifications en attente.
  - Tests : `tests/live_radio_empty_test.py` et `tests/planner_test.py`.
- **Porteuses arrêtées comptées dans la bande.** C'est voulu : Démarrer remet la porteuse sans redémarrer les autres. Le message d'erreur nomme maintenant les porteuses arrêtées et dit comment libérer la bande.
