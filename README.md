🔊 Real-Time Voice Gender Transformation (Teensy 4.0)
1. Audio acquisition & routing

Micro → ADC / I2S → Teensy Audio Library

Rôle :

Capturer la voix en temps réel

Acheminer le signal audio vers la chaîne de traitement DSP

Pourquoi c’est important :

Vérifier dès le début que l’audio entre et sort correctement

Garantir une latence faible et une base stable avant tout traitement

2. Pre-processing (nettoyage du signal)
2.1 Gain & normalisation

Rôle :

Ajuster le niveau d’entrée pour exploiter la dynamique sans saturation

Intérêt :

Un bon niveau d’entrée améliore la qualité de tous les traitements suivants

2.2 Filtre passe-haut (HPF)

Rôle :

Supprimer les basses fréquences inutiles (bruits, plosives, vibrations)

Intérêt :

Évite de traiter des composantes qui n’apportent rien à la voix

Améliore la stabilité du pitch shifting

2.3 Compresseur / limiteur léger

Rôle :

Réduire les variations excessives de niveau

Intérêt :

Rendu plus homogène

Protection contre la saturation et le larsen

3. Détection de voix (voisé / non-voisé)

Rôle :

Identifier si le signal correspond à une voix “voisée” (voyelles) ou non (s, f, ch)

Intérêt :

Appliquer le pitch shifting uniquement là où il est pertinent

Éviter des artefacts sur les consonnes bruitées

4. Pitch shifting (modification de la hauteur)

Rôle :

Augmenter la fréquence fondamentale (F₀) de la voix

Intérêt :

C’est l’étape principale qui fait passer la voix d’un registre masculin à un registre féminin

Les harmoniques sont déplacées vers le haut en fréquence

5. Correction / ajustement des formants

Rôle :

Modifier l’enveloppe spectrale (résonances du conduit vocal)

Décorréler le timbre de la hauteur

Intérêt :

Éviter l’effet “cartoon / Mickey”

Rendre la voix plus naturelle et crédible

6. Coloration timbrale (post-processing)
6.1 Égalisation (EQ)

Rôle :

Accentuer légèrement les médiums-aigus

Intérêt :

Les voix féminines sont perçues comme plus brillantes

Améliore la perception du genre sans modifier le pitch

6.2 De-esser (optionnel)

Rôle :

Atténuer les sifflantes excessives

Intérêt :

Évite une fatigue auditive après pitch/formant shifting

7. Sécurité audio en sortie
7.1 Limiteur final

Rôle :

Garantir qu’aucun échantillon ne dépasse le niveau maximal

Intérêt :

Protection des haut-parleurs

Sécurité indispensable pour une démo publique

7.2 Mix Dry / Wet (optionnel)

Rôle :

Mélanger le signal traité avec le signal original

Intérêt :

Permet de doser l’effet

Améliore le contrôle utilisateur et la flexibilité

8. Interface utilisateur & “produit”

Rôle :

Boutons, potentiomètres, presets, boite avec uniquement boutons et micro qui ressortent ?

Activation / désactivation de l’effet

Intérêt :

Rendre le système interactif

Donner une vraie dimension “produit embarqué”

9. Démonstration & évaluation

Rôle :

Présenter le fonctionnement, les choix techniques et les limites

Intérêt :

Montrer la maîtrise du temps réel, du DSP et de l’embarqué

Mettre en avant la démarche d’ingénierie

🧠 Résumé global (1 ligne)

Micro → nettoyage → détection voisé → pitch shifting → correction formants → EQ → limiteur → haut-parleurs