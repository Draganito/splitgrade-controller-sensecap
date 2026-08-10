In der Dunkelkammer arbeiten wir mit den Augen, dem Gefühl für Licht und der Reaktion des Fotopapiers – nicht mit Algebra. Deshalb lass uns den gesamten Vorgang ganz intuitiv und **völlig ohne mathematische Formeln** betrachten.

*Hinweis: Dieses Dokument beschreibt das aktuelle Belichtungsmodell des Controllers. Es ersetzt eine frühere Version, die mit einem einzigen "Crossover"-Korrekturfaktor arbeitete. Das neue Modell braucht diesen Kompromiss nicht mehr, weil es Blau und Grün von Anfang an getrennt behandelt.*

---

### Warum wir jetzt zwei getrennte Messungen machen, statt eine mit Korrektur

Das Geheimnis von Multigrade-Fotopapier sind seine zwei verschiedenen Lichtempfindlichkeiten im gleichen Blatt:

* Die **harte Schicht** reagiert auf blaues Licht und erzeugt kräftige Kontraste und tiefes Schwarz.
* Die **weiche Schicht** reagiert auf grünes Licht und zaubert sanfte Grautöne und feine Details in die hellen Bildbereiche (die Lichter).

Der Controller misst dein Licht mit einem einfachen Sensor (TSL2591), der selbst **keine Farben unterscheiden kann** – er sieht nur "hell" oder "dunkel", egal welche Farbe. Damit eine Messung überhaupt etwas Sinnvolles über eine bestimmte Farbe aussagt, darf zum Zeitpunkt der Messung wirklich nur diese eine Farbe leuchten.

Genau deshalb misst der Controller Blau und Grün jetzt **nacheinander, als zwei komplett eigenständige Messungen** (`MEAS BLK` für Blau, `MEAS LIT` für Grün) – anstatt wie früher nur einmal zu messen und die zweite Zeit aus einem festen Korrekturfaktor zu *schätzen*. Jede der beiden Messungen sagt dir direkt und ohne Umwege: "So viel Sekunden brauchst du bei der Lichtstärke, die gerade an deinem Vergrößerer anliegt."

---

### So bringst du dem Controller dein Papier bei (mit dem Stufenkeil, nur einmal pro Papiersorte)

Diesen Kalibrier-Vorgang machst du **nur ein einziges Mal** pro Papiersorte (zum Beispiel einmal für dein Lieblings-Barytpapier) – und zwar für Blau und für Grün getrennt, aber nach demselben Rezept.

#### 1. Den Stufenkeil einlegen

Du legst deinen Stouffer-Stufenkeil (21 Stufen, ein halbes Blendenstufe Unterschied zwischen zwei benachbarten Feldern) in die Bildbühne deines Vergrößerers, genau wie sonst ein Negativ.

#### 2. Die Lichtstärke erfassen ("READ REF")

Auf dem versteckten Kalibrier-Bildschirm (durch 3 Sekunden Halten von `MEAS BLK` erreichbar) drückst du für die jeweilige Farbe **READ REF**. Der Controller misst kurz die aktuelle Lichtstärke dieser einen Farbe und merkt sich den Wert.

#### 3. Den echten Testabzug machen

Jetzt belichtest du an deinem Vergrößerer einen echten Probestreifen durch den Stufenkeil, mit einer festen, selbst gewählten Testzeit (Standard 10 Sekunden, änderbar über `TIME +/-`). Das macht der Controller heute noch nicht selbst, weil noch kein echter Leuchtkopf angeschlossen ist – du übernimmst diesen einen Handgriff.

#### 4. Entwickeln und die richtige Stufe suchen

Nach dem Entwickeln suchst du auf dem Papier die Stufe, die genau deinen gewünschten Referenzton zeigt:

* Für **Blau**: die Stufe, an der zum ersten Mal ein sattes, maximales Schwarz erreicht ist.
* Für **Grün**: die Stufe, an der zum ersten Mal ein sichtbarer, heller Grauton auftaucht (dein Lichter-/Tonwert-Referenzpunkt).

Die gefundene Stufennummer stellst du am Controller über `STEP +/-` ein.

#### 5. Speichern

Ein Druck auf **SAVE** genügt. Der Controller rechnet sich intern aus, welche Belichtungszeit bei *dieser* Lichtstärke nötig wäre, um exakt an Stufe 1 des Keils (der hellsten Referenzstufe) zu landen – das ist sein gemerkter Zielwert für diese Farbe. Diesen Vorgang wiederholst du für die andere Farbe.

---

### Was dein Controller damit im Laboralltag macht

Sobald beide Farben einmal kalibriert sind, ist der tägliche Ablauf denkbar einfach:

1. Du legst dein Negativ ein, fokussierst wie gewohnt.
2. Du misst mit **`MEAS BLK`** auf dem Filmsteg zwischen zwei Bildern – der Controller schaltet kurz nur blaues Licht ein, misst, und rechnet dir sofort eine passende harte Belichtungszeit für *genau diese* Lichtstärke aus.
3. Du misst mit **`MEAS LIT`** genauso – der Controller schaltet kurz nur grünes Licht ein, misst, und liefert dir die passende weiche Zeit.
4. Du belichtest mit **`EXPOSURE`**.

Es gibt dabei keine Korrekturrechnung mehr im Hintergrund und keinen Unterschied zwischen "AUTO" und "MANUAL" – jede Messung ist bereits eine vollständige, für sich stehende Antwort auf die Frage "wie viel Zeit brauche ich jetzt, bei diesem Licht, für dieses Papier". Ändert sich die Lichtstärke (zum Beispiel weil du die Höhe des Vergrößerers änderst), misst du einfach erneut – ganz ohne Nachdenken darüber, was "vorher" war.

**Das ist die ganze Idee:** zwei saubere, unabhängige Messungen statt einer Messung plus einer Schätzung für die zweite Farbe.
