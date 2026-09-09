# Bedienungsanleitung — Splitgrade-Controller

Eine praktische Anleitung für den echten Druckabend in der Dunkelkammer. Kein technisches Vorwissen nötig — nur dein Gefühl für Licht, Papier und Entwicklung.

---

## 1) Was das Gerät für dich tut

Der Controller nimmt dir die Rechnerei ab und gibt dir schnell einen guten Startpunkt für deinen Splitgrade-Abzug — Blau (hart, Schatten/Kontrast) und Grün (weich, Lichter/Zeichnung) werden getrennt gemessen und getrennt belichtet. Das letzte Wort über den Look des Bildes hast trotzdem immer du, ganz wie beim klassischen Drucken nach Auge.

Zwei Geräte arbeiten dabei zusammen:

- **Der Controller in deiner Hand** — das Touchscreen-Gerät, das du bedienst.
- **Der Vergrößererkopf** — die LED-Lampe in deinem Vergrößerer, mit der
  [darkroom-enlarger-head](https://github.com/Draganito/darkroom-enlarger-head)-Firmware
  (oder ein zweiter SenseCAP als Testgerät auf der Werkbank).

Beide reden drahtlos miteinander. In der Dunkelkammer musst du nichts verkabeln.

---

## 2) Der Hauptbildschirm auf einen Blick

| Taste | Bedeutung |
|---|---|
| **MEAS BLK** | Misst das blaue (harte) Licht, berechnet eine frische Hart-Zeit |
| **MEAS LIT** | Misst das grüne (weiche) Licht, berechnet eine frische Weich-Zeit |
| **+ / −** (mittlere Spalten) | `HARD`- bzw. `SOFT`-Zeit von Hand nachjustieren |
| **FOCUS** | Schärfe-/Rahmen-Licht einschalten |
| **EXPOSURE** | Den Druck starten — oder sofort abbrechen |
| Statusleiste oben | Zeigt `IDLE`, `FOCUS` oder den laufenden Countdown |

Es gibt keinen AUTO/MANUAL-Modus, den du dir merken musst — jede Messung ist immer frisch und eigenständig, die `+`/`−`-Tasten verändern einfach direkt die angezeigte Zeit.

---

## 3) Einmalig: dein Papier einmessen (Kalibrierung mit Stufenkeil)

Das machst du **nur einmal pro Papier-/Entwickler-Kombination** — danach musst du diesen Schritt nicht mehr wiederholen, bis du das Papier oder den Entwickler wechselst.

1. Stouffer-Stufenkeil in die Bildbühne einlegen, so wie sonst ein Negativ.
2. **MEAS BLK** ca. 3 Sekunden gedrückt halten → das versteckte Kalibriermenü öffnet sich.
3. Für die **blaue** Seite: **READ REF** antippen — der Controller macht die Messung komplett selbst: er schaltet den Vergrößererkopf auf Blau, lässt die LEDs 3 Sekunden warmlaufen, dunkelt den eigenen Bildschirm ab und mittelt 20 Sensormessungen (insgesamt ca. 7–8 Sekunden), dann geht das Licht wieder aus.
4. An deinem Vergrößerer einen echten Probestreifen durch den Stufenkeil belichten, mit der angezeigten **TEST TIME** (bei Bedarf mit `TIME +/-` anpassen).
5. Entwickeln. Die Stufe suchen, an der dein Zielton zum ersten Mal erreicht ist (für Blau z. B. das erste satte Schwarz) und diese Stufennummer mit `STEP +/-` einstellen.
6. **SAVE** auf der blauen Seite antippen — der Zielwert ist jetzt gespeichert.
7. Schritte 3–6 für die **grüne** Seite wiederholen (hier eher eine hellere Referenzstufe wählen, z. B. das erste sichtbare Grau, da Grün deine Lichter/Zeichnung bestimmt).
8. **BACK** antippen, um zum Hauptbildschirm zurückzukehren.

---

## 4) Der normale Ablauf an jedem Druckabend

1. Negativ einlegen, mit **FOCUS** scharfstellen und rahmen.
2. Auf dem **klaren Filmrand zwischen zwei Bildern messen**, nicht auf dem Motiv selbst — das gibt jedes Mal eine stabile, wiederholbare Messung.
3. **MEAS BLK** antippen (kurzer Tap).
   - Der Kopf schaltet auf Blau und läuft 3 Sekunden warm, dann wird der Bildschirm für einige Sekunden dunkel, während 20 Sensormessungen gemittelt werden — Absicht, damit das Panel-Licht die Messung nicht stört. Insgesamt dauert die Messung ca. 7–8 Sekunden.
   - Der Controller misst das blaue Licht und berechnet daraus eine frische Hart-Zeit.
4. **MEAS LIT** genauso antippen — misst Grün, berechnet die Weich-Zeit.
5. **EXPOSURE** antippen → dein erster Abzug wird gemacht.
6. Ergebnis beurteilen. Passt es? Fertig. Wenn nicht: von Hand nachjustieren (siehe nächster Abschnitt).

Beide Messungen sind immer absolute, frische Werte — es gibt keinen "Referenzwert", den du im Kopf behalten musst. Änderst du die Vergrößerungshöhe oder rahmst neu, einfach erneut messen — die neue Lichtstärke wird automatisch berücksichtigt.

---

## 5) Feinabstimmung von Hand

- **HARD +/−**: die blaue/harte Zeit direkt nachjustieren, z. B. nach Beurteilung eines Probeabzugs.
- **SOFT +/−**: die grüne/weiche Zeit direkt nachjustieren.
- Ein erneuter Tap auf **MEAS BLK**/**MEAS LIT** überschreibt deinen von Hand eingestellten Wert immer mit einer frischen Messung — feinjustiere also erst *nach* deiner letzten Messung für diesen Abzug, nicht davor.

**Tasten gedrückt halten** (jede `+`/`−`-Taste, auch im Kalibriermenü): kurz halten = einzelne Schritte, länger halten = die Schritte werden automatisch schneller (langsam → schnell → sehr schnell). So kommst du auch bei großen Anpassungen schnell zum Ziel.

---

## 6) Sicherheit

- **EXPOSURE** ist dein universeller Nothalt: läuft gerade eine Belichtung oder das Fokuslicht, bricht ein Tap auf `EXPOSURE` sofort und ohne Verzögerung ab — egal was sonst gerade passiert.
- Tippst du versehentlich **FOCUS** während eine Belichtung läuft, bricht der Controller die Belichtung ab, statt einfach ins Fokuslicht zu springen. Ein zweiter Tap auf `FOCUS` schaltet danach wirklich das Fokuslicht ein.
- `MEAS BLK`/`MEAS LIT` und alle versteckten Menüs sind gesperrt, solange eine Belichtung oder das Fokuslicht läuft — so kann keine verunreinigte Messung mitten im Druck passieren.

---

## 7) Töne zur Rückmeldung

Jeder Tastendruck gibt einen kurzen, leisen Klick — sofortige Bestätigung, auch im Dunkeln, ohne zu stören. Ein längerer, deutlich anderer Warnton bedeutet: eine Aktion ist fehlgeschlagen (z. B. eine Messung ohne gültigen Wert) — der Controller lässt dich nie stillschweigend mit einer falschen Zahl da stehen.

---

## 8) Die drei versteckten Menüs

Alle drei Menüs sind nur erreichbar, wenn gerade **nichts läuft** (Status `IDLE`) — sie können also nie versehentlich eine laufende Belichtung stören.

### 8.1 Kalibriermenü

**MEAS BLK** ca. 3 Sekunden halten → öffnet das in Abschnitt 3 beschriebene Kalibriermenü. `BACK` zum Zurückkehren.

### 8.2 Cross-Faktor-Menü ("XOVER TUNE") — optional, für Fortgeschrittene

Ein kleiner, optionaler Regler für eine ganz konkrete, in der Fachliteratur dokumentierte Eigenheit von Multigrade-Papier: die harte (blaue) Belichtung trifft nicht nur die Schatten, sondern legt auch etwas ungewollte Dichte in die Lichter — weil die "weiche" Schicht des Papiers ihre Blau-Empfindlichkeit nie ganz verliert. Das macht sich bemerkbar, wenn deine Lichter nach einer längeren Hart-Belichtung dunkler/flauer wirken, als es dein Weich-Probestreifen versprochen hat. Die meisten Abzüge brauchen das nicht — lass den Wert auf `+0.00`, solange du diesen Effekt nicht tatsächlich an einem echten Abzug siehst.

- **EXPOSURE** ca. 3 Sekunden halten, **während nichts läuft** → öffnet `XOVER TUNE`. Ein kurzer Tap auf `EXPOSURE` startet/stoppt weiterhin ganz normal deine Belichtung — das Halten ist komplett davon getrennt.
- Mit **+ / −** den **CROSS FACTOR** einstellen (Schritte von 0,01, Bereich −0,30 bis +0,30, mit Beschleunigung bei längerem Halten). Ein **positiver** Wert bedeutet: für jede Sekunde Hart-Belichtung wird dieser Anteil von der Weich-Zeit abgezogen (z. B. `+0.15` bei 10 s Hart-Zeit zieht 1,5 s von der Weich-Zeit ab) — das ist die dokumentierte, in der Praxis relevante Richtung. Der Bildschirm zeigt dir in Klartext, welche Richtung gerade aktiv ist.
- **BACK** zum Zurückkehren. Der Wert wird bei jeder Änderung automatisch gespeichert.

**So findest du den richtigen Wert für dein Papier:** Ein Negativ messen und drucken, das eine eher lange Hart-Belichtung braucht — einmal mit `+0.00`. Wirken die Lichter dunkler/flauer als erwartet, den Test mit `+0.10`, dann `+0.15` usw. wiederholen, bis die Lichter wieder passen. Einmal gefunden, bleibt der Wert für diese Papier-/Entwickler-Kombination stabil und reproduzierbar — du musst dieses Menü danach nicht mehr anfassen.

### 8.3 LED-Panel-Menü ("LEDCFG") — einmalige Einrichtung

Hat dein Vergrößererkopf ein anderes LED-Panel als das Referenzpanel (andere Pixelzahl oder die Datenleitung an einem anderen GPIO), stellst du das hier ein — ganz ohne neu zu flashen.

- **MEAS LIT** ca. 3 Sekunden halten, während nichts läuft → öffnet `LEDCFG`. Ein kurzer Tap auf `MEAS LIT` misst weiterhin ganz normal das grüne Licht.
- Mit **+ / −** die **Pixelzahl** und den **Daten-GPIO** passend zu deinem Panel einstellen (Referenzwerte: siehe [FLASH.md](https://github.com/Draganito/darkroom-enlarger-head/blob/main/FLASH.md) §4 des Kopfs).
- **SAVE** sendet die Einstellungen an den Kopf — der merkt sie sich selbst, auch über Stromausfälle hinweg. Das bloße Öffnen des Menüs sendet nichts; nur `SAVE` überträgt. Ein `SAVE` ohne Änderung eignet sich auch, um einen frisch geflashten Kopf neu zu synchronisieren.
- **BACK** zum Zurückkehren.

Das machst du normalerweise genau einmal, direkt nach dem Flashen — und danach nie wieder.

---

## 9) Schnellreferenz-Karte

| Du willst... | So geht's |
|---|---|
| Neues Papier/Entwickler einmessen | `MEAS BLK` ~3 s halten → pro Farbe: `READ REF` → echter Testabzug → `STEP` einstellen → `SAVE` |
| Frischen Startwert für einen Abzug | Filmrand zwischen den Bildern messen → `MEAS BLK`, dann `MEAS LIT` |
| Hart-Zeit von Hand nachjustieren | `HARD +/-` |
| Weich-Zeit von Hand nachjustieren | `SOFT +/-` |
| Bild scharfstellen/rahmen | `FOCUS` |
| Druck starten/stoppen | `EXPOSURE` (kurzer Tap) |
| Sofort alles abbrechen | `EXPOSURE` |
| Cross-Faktor einstellen (Lichter wirken nach langer Hart-Zeit zu dunkel) | `EXPOSURE` ~3 s halten (im Leerlauf) → `+`/`-` → `BACK` |
| LED-Panel einstellen (einmalig nach dem Flashen) | `MEAS LIT` ~3 s halten (im Leerlauf) → Werte setzen → `SAVE` |

---

## 10) Was dieser Controller *nicht* macht

Er trifft keine künstlerischen Entscheidungen für dich. Er gibt dir einen schnellen, wiederholbaren, technisch konsistenten Startpunkt — wie das Bild am Ende aussieht, entscheidest immer du, mit deinem Auge, genau wie beim klassischen Dunkelkammerdruck.
