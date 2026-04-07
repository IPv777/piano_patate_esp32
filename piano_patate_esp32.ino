#include <Arduino.h>

/*
  ============================
  PIANO PATATE - ESP32
  ============================

  Starman de David Bowie : Mi mi mi fa sol la la la sol fa mi mi mi fa sol si si si la sol mi

  Ce programme fait 4 choses principales :

  1) Il lit 8 entrées tactiles de l'ESP32.
  2) Il calibre automatiquement le niveau "au repos" de chaque touche.
  3) Il détecte les appuis et relâchements.
  4) Il envoie ces événements sur le port série pour qu'une page HTML
     sur le PC puisse jouer les notes.

  En plus, il sait recevoir des commandes série venant du PC :
  - CALIBRATE  -> relance une calibration de 5 secondes
  - STATUS     -> renvoie les seuils/calibrations actuels
*/


/*
  ----------------------------
  1) Définition des broches tactiles
  ----------------------------

  On utilise ici 8 GPIO de l'ESP32 qui supportent la fonction touchRead().

  IMPORTANT :
  La position dans le tableau détermine l'index de la touche.
  Par exemple :
  - touchPins[0] = GPIO 4  -> touche 0
  - touchPins[1] = GPIO 33 -> touche 1
  etc.

  C'est cet index qui est envoyé au PC sous forme :
  "TOUCHE 0", "RELACHE 0", etc.
*/
const uint8_t touchPins[8] = {4, 33, 15, 13, 12, 14, 27, 32};

/*
  Tableau purement informatif.
  Il n'est pas indispensable au fonctionnement du code,
  mais il permet de savoir à quelle note correspond chaque touche.
*/
const char* noteNames[8] = {"Do", "Re", "Mi", "Fa", "Sol", "La", "Si", "Do+"};


/*
  ----------------------------
  2) Variables de calibration et d'état
  ----------------------------

  Pour chaque touche, on garde plusieurs informations :
  - baseline      : valeur moyenne mesurée au repos
  - thresholdLow  : seuil sous lequel on considère "appuyé"
  - thresholdHigh : seuil au-dessus duquel on considère "relâché"
  - filtered      : valeur filtrée pour lisser un peu le bruit
  - rawVals       : dernière valeur brute lue
  - pressed       : état logique actuel de la touche
*/
uint16_t baseline[8];
uint16_t thresholdLow[8];
uint16_t thresholdHigh[8];
uint16_t filtered[8];
uint16_t rawVals[8];
bool pressed[8];


/*
  ----------------------------
  3) Variables de timing
  ----------------------------

  On ne lit pas les touches en continu à chaque tour de loop(),
  sinon on ferait trop de lectures inutiles.

  On choisit donc un pas d'échantillonnage de 10 ms.
  Cela veut dire qu'environ toutes les 10 millisecondes,
  on relit les 8 touches.
*/
unsigned long lastSampleMs = 0;
const unsigned long samplePeriodMs = 10;

/*
  Durée de la calibration automatique.
  Pendant ces 5 secondes, il ne faut toucher aucune patate.
*/
const unsigned long calibrationDurationMs = 5000;


/*
  ----------------------------
  4) Paramètres de sensibilité
  ----------------------------

  touchRead() retourne une valeur qui varie selon le toucher.
  En général, sur ESP32, la valeur baisse quand on touche.

  On définit donc :
  - minDelta   : écart minimum entre le niveau de repos et le seuil
  - hysteresis : petite marge pour éviter qu'une touche oscille
                 trop vite entre appuyée / relâchée

  Pourquoi 2 seuils ?
  - thresholdLow  : pour détecter l'appui
  - thresholdHigh : pour détecter le relâchement

  Cette technique s'appelle l'hystérésis.
  Elle évite les faux déclenchements quand le signal "tremble"
  autour d'une seule valeur limite.
*/
const uint16_t minDelta = 8;
const uint16_t hysteresis = 6;


/*
  ----------------------------
  5) Gestion des commandes série
  ----------------------------

  serialLine sert à accumuler les caractères reçus depuis le PC,
  jusqu'à recevoir une fin de ligne (\n ou \r).

  isCalibrating indique si une calibration est en cours.
*/
String serialLine;
bool isCalibrating = false;


/*
  ============================================================
  FONCTION : calibrateTouches()
  ============================================================

  Cette fonction mesure le niveau de repos des 8 touches.

  Idée :
  - on lit chaque touche de nombreuses fois pendant 5 secondes
  - on fait une moyenne
  - cette moyenne devient la "baseline"
  - à partir de là, on calcule un seuil d'appui et un seuil de relâchement
*/
void calibrateTouches() {
  // On indique au PC / navigateur qu'une calibration commence.
  isCalibrating = true;
  Serial.println("CALIBRATION_START");
  Serial.println("Ne touche a rien pendant 5 secondes...");

  // Par sécurité, on remet tous les états logiques à false.
  // Pendant la calibration, aucune touche ne doit être vue comme appuyée.
  for (int i = 0; i < 8; i++) {
    pressed[i] = false;
  }

  /*
    sums[] va stocker la somme des mesures pour chaque touche.
    counts[] va compter combien de mesures ont été prises.
    Ensuite baseline = somme / nombre de mesures.
  */
  uint32_t sums[8] = {0};
  uint32_t counts[8] = {0};

  // On mémorise l'heure de départ de la calibration.
  unsigned long start = millis();

  /*
    Tant que 5 secondes ne sont pas écoulées,
    on continue à mesurer chaque touche.
  */
  while (millis() - start < calibrationDurationMs) {
    for (int i = 0; i < 8; i++) {
      uint16_t v = touchRead(touchPins[i]);  // lecture brute de la touche
      sums[i] += v;                          // on ajoute la mesure à la somme
      counts[i]++;                           // on incrémente le compteur
      rawVals[i] = v;                        // on garde la dernière mesure brute
    }

    // Petite pause pour éviter de lire inutilement trop vite.
    delay(5);
  }

  /*
    Maintenant qu'on a fini les mesures,
    on calcule les paramètres définitifs de chaque touche.
  */
  for (int i = 0; i < 8; i++) {
    // Si on a bien mesuré au moins une fois, on fait la moyenne.
    // Sinon, on relit une fois la broche comme secours.
    baseline[i] = counts[i] ? sums[i] / counts[i] : touchRead(touchPins[i]);

    // Au départ, la valeur filtrée est initialisée au niveau de repos.
    filtered[i] = baseline[i];

    /*
      On calcule un écart entre baseline et seuil.
      Ici on prend 20% de la baseline (baseline / 5).
      Si cet écart est trop petit, on impose un minimum avec minDelta.
    */
    uint16_t delta = baseline[i] / 5;
    if (delta < minDelta) delta = minDelta;

    /*
      thresholdLow : seuil d'appui
      thresholdHigh : seuil de relâchement

      Exemple :
      si baseline = 50
      delta = 10
      alors :
      - thresholdLow = 40
      - thresholdHigh = 46

      La touche devient "appuyée" sous 40,
      et redevient "relâchée" seulement au-dessus de 46.
    */
    thresholdLow[i] = baseline[i] - delta;
    thresholdHigh[i] = thresholdLow[i] + hysteresis;

    /*
      On envoie les paramètres de calibration au PC.
      La page HTML va lire ces lignes et afficher les valeurs.
    */
    Serial.print("CAL ");
    Serial.print(i);
    Serial.print(" GPIO ");
    Serial.print(touchPins[i]);
    Serial.print(" BASE ");
    Serial.print(baseline[i]);
    Serial.print(" LOW ");
    Serial.print(thresholdLow[i]);
    Serial.print(" HIGH ");
    Serial.println(thresholdHigh[i]);
  }

  // Calibration terminée.
  Serial.println("CALIBRATION_DONE");
  isCalibrating = false;
}


/*
  ============================================================
  FONCTION : sampleTouches()
  ============================================================

  Cette fonction lit les 8 touches et détecte les changements d'état.

  Elle compare la valeur filtrée de chaque touche aux seuils :
  - si la touche n'était pas appuyée et passe sous thresholdLow :
    on considère qu'elle vient d'être appuyée
  - si la touche était appuyée et repasse au-dessus de thresholdHigh :
    on considère qu'elle vient d'être relâchée
*/
void sampleTouches() {
  // Si on est en calibration, on ne doit pas faire de détection.
  if (isCalibrating) return;

  for (int i = 0; i < 8; i++) {
    // Lecture brute actuelle de la broche tactile.
    rawVals[i] = touchRead(touchPins[i]);

    /*
      Petit filtre IIR très simple :
      filtered = (ancienne_valeur * 2 + nouvelle_valeur) / 3

      Ce filtre réduit un peu le bruit sans rendre la lecture trop lente.
      En gros, il donne plus de poids à la valeur précédente.
    */
    filtered[i] = (filtered[i] * 2 + rawVals[i]) / 3;

    /*
      Détection de front descendant :
      La touche n'était pas pressée,
      et la valeur filtrée passe sous le seuil bas.
    */
    if (!pressed[i] && filtered[i] < thresholdLow[i]) {
      pressed[i] = true;

      // On envoie l'événement vers le PC.
      Serial.print("TOUCHE ");
      Serial.println(i);
    }

    /*
      Détection de relâchement :
      La touche était pressée,
      et la valeur repasse au-dessus du seuil haut.
    */
    else if (pressed[i] && filtered[i] > thresholdHigh[i]) {
      pressed[i] = false;

      // On envoie l'événement de relâchement.
      Serial.print("RELACHE ");
      Serial.println(i);
    }
  }
}


/*
  ============================================================
  FONCTION : handleSerialCommand()
  ============================================================

  Cette fonction traite une ligne de commande reçue depuis le PC.

  Exemples :
  - "CALIBRATE"
  - "STATUS"
*/
void handleSerialCommand(const String& cmdRaw) {
  // On copie la chaîne pour pouvoir la modifier localement.
  String cmd = cmdRaw;

  // trim() enlève les espaces et retours chariot superflus.
  cmd.trim();

  // toUpperCase() met tout en majuscules pour simplifier les comparaisons.
  cmd.toUpperCase();

  /*
    Si le PC demande une calibration,
    on relance la fonction complète de calibration.
  */
  if (cmd == "CALIBRATE") {
    calibrateTouches();
    return;
  }

  /*
    Si le PC demande STATUS,
    on renvoie l'état actuel de la calibration.

    Ici, on choisit de renvoyer :
    - l'état logique de calibration
    - puis les paramètres de seuil de chaque touche
  */
  if (cmd == "STATUS") {
    Serial.println(isCalibrating ? "CALIBRATION_START" : "CALIBRATION_DONE");

    for (int i = 0; i < 8; i++) {
      Serial.print("CAL ");
      Serial.print(i);
      Serial.print(" GPIO ");
      Serial.print(touchPins[i]);
      Serial.print(" BASE ");
      Serial.print(baseline[i]);
      Serial.print(" LOW ");
      Serial.print(thresholdLow[i]);
      Serial.print(" HIGH ");
      Serial.println(thresholdHigh[i]);
    }
    return;
  }
}


/*
  ============================================================
  FONCTION : readSerialCommands()
  ============================================================

  Cette fonction lit les caractères arrivant sur Serial,
  les accumule dans serialLine,
  et dès qu'une fin de ligne est reçue, elle envoie la commande complète
  à handleSerialCommand().

  C'est une méthode classique de parsing ligne par ligne
  sur un port série [web:148][web:152].
*/
void readSerialCommands() {
  while (Serial.available()) {
    char c = (char)Serial.read();

    /*
      Si on reçoit une fin de ligne,
      cela veut dire que la commande est complète.
    */
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        handleSerialCommand(serialLine);
        serialLine = "";
      }
    } else {
      // Sinon on ajoute simplement le caractère au buffer.
      serialLine += c;

      /*
        Sécurité :
        si une ligne devient beaucoup trop longue,
        on l'abandonne pour éviter de remplir la mémoire inutilement.
      */
      if (serialLine.length() > 80) {
        serialLine = "";
      }
    }
  }
}


/*
  ============================================================
  FONCTION : setup()
  ============================================================

  setup() s'exécute une seule fois au démarrage de l'ESP32.
*/
void setup() {
  // Démarre le port série à 115200 bauds.
  Serial.begin(115200);

  // Petite attente de confort.
  delay(1000);

  // Quelques messages d'information.
  Serial.println();
  Serial.println("ESP32 Piano Patate Serial");
  Serial.println("FORMAT: TOUCHE n / RELACHE n");
  Serial.println("COMMANDS: CALIBRATE / STATUS");

  // Calibration automatique au démarrage.
  calibrateTouches();
}


/*
  ============================================================
  FONCTION : loop()
  ============================================================

  loop() tourne en permanence.

  A chaque tour :
  1) on regarde si le PC a envoyé une commande série
  2) si c'est le moment, on échantillonne les touches
*/
void loop() {
  // Lecture des commandes série venant du navigateur / PC.
  readSerialCommands();

  // Échantillonnage périodique des touches toutes les 10 ms environ.
  if (millis() - lastSampleMs >= samplePeriodMs) {
    lastSampleMs = millis();
    sampleTouches();
  }
}