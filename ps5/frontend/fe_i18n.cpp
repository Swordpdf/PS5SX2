// PS5 port frontend: the shelf's and the notifications' text in the PS5's language (vk-285-110).
// See fe_i18n.h. First translations by the port's author; testers' fixes go in /data/PCSX2/lang/<code>.txt.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_i18n.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace fe
{
namespace
{
constexpr int kCount = static_cast<int>(Str::Count);

const char* const kKeys[kCount] = {
	"hint.play",
	"hint.browse",
	"hint.jump",
	"shelf.no_network",
	"shelf.no_games",
	"covers.downloading",
	"size.gb",
	"size.mb",
	"size.decimal",
	"notify.starting",
	"notify.starting_test",
	"notify.covers_one",
	"notify.covers_many",
	"notify.now_playing",
	"how.native",
	"how.software",
	"notify.no_game",
	"notify.not_started",
	"notify.menu_failed",
	"notify.stopped",
	"hint.settings",
	"hint.move",
	"hint.change",
	"hint.reset",
	"hint.back",
	"sheet.this_game",
	"sheet.all_games",
	"notify.no_bios",
	"notify.gs_failed",
	"sheet.controls",
	"hint.qr_code",
	"achievements.title",
	"achievements.all",
	"achievements.unlocked",
	"achievements.locked",
	"achievements.points",
	"achievements.refresh",
	"achievements.previous",
	"achievements.next",
	"achievements.loading",
	"achievements.images",
	"achievements.empty_filter",
	"achievements.empty",
	"achievements.select_running",
	"achievements.show_running",
	"achievements.start_game",
	"achievements.unavailable",
	"achievements.sign_in",
	"achievements.connecting",
	"achievements.no_data",
	"achievements.refresh_failed",
	"achievements.select_game",
	"achievements.pending",
	"achievements.triangle_refresh",
	"achievements.shelf_sign_in",
	"achievements.boot_failed",
	"achievements.secure_failed",
	"achievements.create_failed",
	"achievements.connect_failed",
	"achievements.set_failed",
	"achievements.list_failed",
	"achievements.account_missing",
	"achievements.unlock_failed",
	"achievements.main_failed",
	"achievements.cancelled",
	"achievements.start_failed",
	"achievements.status_unlocked",
	"achievements.status_locked",
	"account.intro",
	"account.username",
	"account.password",
	"account.username_hint",
	"account.password_hint",
	"account.sign_in",
	"account.signing_in",
	"account.sign_out",
	"account.sign_out_again",
	"account.signed_in",
	"account.signed_in_note",
	"hint.select",
	"hint.edit",
	"hint.show_password",
	"hint.hide_password",
	"hint.delete",
	"hint.done",
	"keyboard.space",
	"texpacks.shelf",
	"texpacks.ready",
	"texpacks.failed",
	"hint.download",
	"hint.cancel",
	"achievements.server_error",
	"achievements.unreachable",
	"achievements.unsent",
	"shelf.no_bios",
};

const char* const kEnglish[kCount] = {
	"Play",
	"Browse",
	"Jump",
	"Game settings: no network",
	"No games in /data/PCSX2/games",
	"Downloading covers  %d / %d",
	"%s GB",
	"%s MB",
	".",
	"PS5SX2: starting",
	"PS5SX2 (testing build): starting",
	"PS5SX2: downloading %d cover",
	"PS5SX2: downloading %d covers",
	"Now playing: %s\n%s · have fun!",
	"native %s",
	"software renderer",
	"PS5SX2: no game to start. Put your .iso or .chd files in /data/PCSX2/games/ or on a USB drive, then start PS5SX2 again.",
	"PS5SX2: the game didn't start.\n%s",
	"PS5SX2: couldn't open the menu, back to the game",
	"PS5SX2: the game stopped",
	"Settings",
	"Move",
	"Change",
	"Reset",
	"Back",
	"This game",
	"All games",
	"PS5SX2: no PS2 BIOS found. Copy your BIOS file (4 MB, e.g. SCPH-70012.bin) to %s and start the game again.",
	"PS5SX2: the game's graphics didn't start. Start it again; if it happens again, reinstall PS5SX2 with its installer.",
	"Controls",
	"QR code",
	"Achievements",
	"All",
	"Unlocked",
	"Locked",
	"points",
	"Refresh",
	"Previous",
	"Next",
	"Loading achievements...",
	"Loading badge images...",
	"No achievements in this filter.",
	"This game has no published achievements.",
	"Select the running game to view its achievements.",
	"Show running game",
	"Start a game to view its live achievements. Before playing, use the Achievements tab on the game shelf.",
	"Achievement browser is unavailable.",
	"Sign in to RetroAchievements and start a game.",
	"Connecting to RetroAchievements...",
	"No achievement data available for this game.",
	"Couldn't refresh achievements: ",
	"Select This game to view its achievements.",
	"Finishing the previous request. Press Triangle to load this game.",
	"Triangle: refresh",
	"Sign in to RetroAchievements from the game shelf.",
	"Could not identify this disc's boot executable.",
	"Could not initialize the secure connection.",
	"Could not create the achievement browser.",
	"Could not connect. Check your connection or sign in again.",
	"No achievement set found, or the service could not be reached.",
	"Could not read the achievement list.",
	"Game or account data is unavailable.",
	"Could not retrieve unlocked achievements. Press Triangle to retry.",
	"Could not read the main achievement set.",
	"Loading cancelled. Press Triangle to retry.",
	"Could not start the achievement browser.",
	"Unlocked",
	"Locked",
	"Sign in with your RetroAchievements account to earn achievements as you play.",
	"Username",
	"Password",
	"Your RetroAchievements username",
	"Your password",
	"Sign in",
	"Signing in...",
	"Sign out",
	"Press again to sign out",
	"Signed in as %s",
	"Achievements unlock as you play. Your password isn't kept on the console.",
	"Select",
	"Edit",
	"Show password",
	"Hide password",
	"Delete",
	"Done",
	"Space",
	"HD textures",
	"HD textures ready",
	"HD textures failed",
	"Download",
	"Cancel",
	"RetroAchievements: server error",
	"Couldn't reach RetroAchievements. You're still signed in: achievements come back with the next game.",
	"An unlock couldn't be sent to RetroAchievements before closing (no connection). It won't be on your account.",
	"No PS2 BIOS found: games can't start. Copy your BIOS file (4 MB, e.g. SCPH-70012.bin) to %s",
};

const char* const kSpanish[kCount] = {
	"Jugar",
	"Explorar",
	"Saltar",
	"Ajustes del juego: sin red",
	"No hay juegos en /data/PCSX2/games",
	"Descargando carátulas  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"PS5SX2: iniciando",
	"PS5SX2 (versión de prueba): iniciando",
	"PS5SX2: descargando %d carátula",
	"PS5SX2: descargando %d carátulas",
	"Jugando: %s\n%s · ¡que lo disfrutes!",
	"resolución nativa, %s",
	"renderizado por software",
	"PS5SX2: no hay ningún juego para iniciar. Pon tus archivos .iso o .chd en /data/PCSX2/games/ o en una unidad USB y vuelve a abrir PS5SX2.",
	"PS5SX2: el juego no se ha iniciado.\n%s",
	"PS5SX2: no se ha podido abrir el menú; vuelves al juego",
	"PS5SX2: el juego se ha detenido",
	"Ajustes",
	"Mover",
	"Cambiar",
	"Restablecer",
	"Atrás",
	"Este juego",
	"Todos los juegos",
	"PS5SX2: no se ha encontrado ninguna BIOS de PS2. Copia tu archivo de BIOS (4 MB, p. ej. SCPH-70012.bin) en %s y vuelve a iniciar el juego.",
	"PS5SX2: los gráficos del juego no se han iniciado. Vuelve a iniciarlo; si vuelve a pasar, reinstala PS5SX2 con su instalador.",
	"Controles",
	"Código QR",
	"Logros",
	"Todos",
	"Desbloqueados",
	"Bloqueados",
	"puntos",
	"Actualizar",
	"Anterior",
	"Siguiente",
	"Cargando logros...",
	"Cargando imágenes...",
	"No hay logros en este filtro.",
	"Este juego no tiene logros publicados.",
	"Selecciona el juego en ejecución para ver sus logros.",
	"Mostrar juego en ejecución",
	"Inicia un juego para ver sus logros en tiempo real. Antes de jugar, usa la pestaña Logros de la lista de juegos.",
	"La lista de logros no está disponible.",
	"Inicia sesión en RetroAchievements e inicia un juego.",
	"Conectando a RetroAchievements...",
	"No hay datos de logros disponibles para este juego.",
	"No se pudieron actualizar los logros: ",
	"Selecciona Este juego para ver sus logros.",
	"Terminando la solicitud anterior. Pulsa Triángulo para cargar este juego.",
	"Triángulo: actualizar",
	"Inicia sesión en RetroAchievements desde la lista de juegos.",
	"No se pudo identificar el ejecutable de arranque de este disco.",
	"No se pudo iniciar la conexión segura.",
	"No se pudo crear la lista de logros.",
	"No se pudo conectar. Comprueba tu conexión o vuelve a iniciar sesión.",
	"No se encontró un conjunto de logros o el servicio no está disponible.",
	"No se pudo leer la lista de logros.",
	"Los datos del juego o de la cuenta no están disponibles.",
	"No se pudieron recuperar los logros desbloqueados. Pulsa Triángulo para reintentar.",
	"No se pudo leer el conjunto principal de logros.",
	"Carga cancelada. Pulsa Triángulo para reintentar.",
	"No se pudo iniciar la lista de logros.",
	"Desbloqueado",
	"Bloqueado",
	"Inicia sesión con tu cuenta de RetroAchievements para conseguir logros mientras juegas.",
	"Nombre de usuario",
	"Contraseña",
	"Tu nombre de usuario de RetroAchievements",
	"Tu contraseña",
	"Iniciar sesión",
	"Iniciando sesión...",
	"Cerrar sesión",
	"Pulsa otra vez para cerrar sesión",
	"Sesión iniciada como %s",
	"Los logros se desbloquean mientras juegas. Tu contraseña no se guarda en la consola.",
	"Seleccionar",
	"Editar",
	"Mostrar contraseña",
	"Ocultar contraseña",
	"Borrar",
	"Listo",
	"Espacio",
	"Texturas HD",
	"Texturas HD listas",
	"Error en las texturas HD",
	"Descargar",
	"Cancelar",
	"RetroAchievements: error del servidor",
	"No se pudo conectar con RetroAchievements. Sigues con la sesión iniciada: los logros volverán en el próximo juego.",
	"No se pudo enviar un logro a RetroAchievements antes de cerrar (sin conexión). No aparecerá en tu cuenta.",
	"No se encontró BIOS de PS2: los juegos no pueden iniciarse. Copia tu archivo de BIOS (4 MB, p. ej. SCPH-70012.bin) en %s",
};

const char* const kSpanishLatAm[kCount] = {
	"Jugar",
	"Explorar",
	"Saltar",
	"Configuración del juego: sin red",
	"No hay juegos en /data/PCSX2/games",
	"Descargando portadas  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"PS5SX2: iniciando",
	"PS5SX2 (versión de prueba): iniciando",
	"PS5SX2: descargando %d portada",
	"PS5SX2: descargando %d portadas",
	"Jugando: %s\n%s · ¡que lo disfrutes!",
	"resolución nativa, %s",
	"renderizado por software",
	"PS5SX2: no hay ningún juego para iniciar. Coloca tus archivos .iso o .chd en /data/PCSX2/games/ o en una unidad USB y vuelve a abrir PS5SX2.",
	"PS5SX2: el juego no se inició.\n%s",
	"PS5SX2: no se pudo abrir el menú; vuelves al juego",
	"PS5SX2: el juego se detuvo",
	"Ajustes",
	"Mover",
	"Cambiar",
	"Restablecer",
	"Atrás",
	"Este juego",
	"Todos los juegos",
	"PS5SX2: no se encontró ninguna BIOS de PS2. Copia tu archivo de BIOS (4 MB, p. ej. SCPH-70012.bin) en %s y vuelve a iniciar el juego.",
	"PS5SX2: los gráficos del juego no se iniciaron. Vuelve a iniciarlo; si vuelve a pasar, reinstala PS5SX2 con su instalador.",
	"Controles",
	"Código QR",
	"Logros",
	"Todos",
	"Desbloqueados",
	"Bloqueados",
	"puntos",
	"Actualizar",
	"Anterior",
	"Siguiente",
	"Cargando logros...",
	"Cargando imágenes...",
	"No hay logros en este filtro.",
	"Este juego no tiene logros publicados.",
	"Selecciona el juego en ejecución para ver sus logros.",
	"Mostrar juego en ejecución",
	"Inicia un juego para ver sus logros en tiempo real. Antes de jugar, usa la pestaña Logros de la lista de juegos.",
	"La lista de logros no está disponible.",
	"Inicia sesión en RetroAchievements e inicia un juego.",
	"Conectando a RetroAchievements...",
	"No hay datos de logros disponibles para este juego.",
	"No se pudieron actualizar los logros: ",
	"Selecciona Este juego para ver sus logros.",
	"Terminando la solicitud anterior. Presiona Triángulo para cargar este juego.",
	"Triángulo: actualizar",
	"Inicia sesión en RetroAchievements desde la lista de juegos.",
	"No se pudo identificar el ejecutable de inicio de este disco.",
	"No se pudo iniciar la conexión segura.",
	"No se pudo crear la lista de logros.",
	"No se pudo conectar. Verifica tu conexión o vuelve a iniciar sesión.",
	"No se encontró un conjunto de logros o no se pudo acceder al servicio.",
	"No se pudo leer la lista de logros.",
	"Los datos del juego o de la cuenta no están disponibles.",
	"No se pudieron obtener los logros desbloqueados. Presiona Triángulo para volver a intentarlo.",
	"No se pudo leer el conjunto principal de logros.",
	"Carga cancelada. Presiona Triángulo para volver a intentarlo.",
	"No se pudo iniciar la lista de logros.",
	"Desbloqueado",
	"Bloqueado",
	"Inicia sesión con tu cuenta de RetroAchievements para conseguir logros mientras juegas.",
	"Nombre de usuario",
	"Contraseña",
	"Tu nombre de usuario de RetroAchievements",
	"Tu contraseña",
	"Iniciar sesión",
	"Iniciando sesión...",
	"Cerrar sesión",
	"Presiona otra vez para cerrar sesión",
	"Sesión iniciada como %s",
	"Los logros se desbloquean mientras juegas. Tu contraseña no se guarda en la consola.",
	"Seleccionar",
	"Editar",
	"Mostrar contraseña",
	"Ocultar contraseña",
	"Borrar",
	"Listo",
	"Espacio",
	"Texturas HD",
	"Texturas HD listas",
	"Error en las texturas HD",
	"Descargar",
	"Cancelar",
	"RetroAchievements: error del servidor",
	"No se pudo conectar con RetroAchievements. Tu sesión sigue iniciada: los logros volverán en el próximo juego.",
	"No se pudo enviar un logro a RetroAchievements antes de cerrar (sin conexión). No va a aparecer en tu cuenta.",
	"No se encontró un BIOS de PS2: los juegos no pueden iniciar. Copia tu archivo de BIOS (4 MB, p. ej. SCPH-70012.bin) a %s",
};

const char* const kFrench[kCount] = {
	"Jouer",
	"Parcourir",
	"Sauter",
	"Réglages du jeu : pas de réseau",
	"Aucun jeu dans /data/PCSX2/games",
	"Téléchargement des jaquettes  %d / %d",
	"%s Go",
	"%s Mo",
	",",
	"PS5SX2 : démarrage",
	"PS5SX2 (version de test) : démarrage",
	"PS5SX2 : téléchargement de %d jaquette",
	"PS5SX2 : téléchargement de %d jaquettes",
	"En jeu : %s\n%s · bon jeu !",
	"résolution native, %s",
	"rendu logiciel",
	"PS5SX2 : aucun jeu à lancer. Mets tes fichiers .iso ou .chd dans /data/PCSX2/games/ ou sur une clé USB, puis relance PS5SX2.",
	"PS5SX2 : le jeu n'a pas démarré.\n%s",
	"PS5SX2 : impossible d'ouvrir le menu, retour au jeu",
	"PS5SX2 : le jeu s'est arrêté",
	"Réglages",
	"Déplacer",
	"Modifier",
	"Réinitialiser",
	"Retour",
	"Ce jeu",
	"Tous les jeux",
	"PS5SX2 : aucun BIOS PS2 trouvé. Copie ton fichier BIOS (4 Mo, par ex. SCPH-70012.bin) dans %s, puis relance le jeu.",
	"PS5SX2 : les graphismes du jeu n'ont pas démarré. Relance-le ; si ça recommence, réinstalle PS5SX2 avec son installateur.",
	"Commandes",
	"Code QR",
	"Succès",
	"Tous",
	"Débloqués",
	"Verrouillés",
	"points",
	"Actualiser",
	"Précédent",
	"Suivant",
	"Chargement des succès...",
	"Chargement des images...",
	"Aucun succès dans ce filtre.",
	"Ce jeu ne propose aucun succès publié.",
	"Sélectionne le jeu en cours pour voir ses succès.",
	"Afficher le jeu en cours",
	"Lance un jeu pour voir ses succès en direct. Avant de jouer, utilise l'onglet Succès de la liste des jeux.",
	"La liste des succès est indisponible.",
	"Connecte-toi à RetroAchievements et lance un jeu.",
	"Connexion à RetroAchievements...",
	"Aucune donnée de succès disponible pour ce jeu.",
	"Impossible d'actualiser les succès : ",
	"Sélectionne Ce jeu pour voir ses succès.",
	"Fin de la requête précédente. Appuie sur Triangle pour charger ce jeu.",
	"Triangle : actualiser",
	"Connecte-toi à RetroAchievements depuis la liste des jeux.",
	"Impossible d'identifier l'exécutable de démarrage de ce disque.",
	"Impossible d'initialiser la connexion sécurisée.",
	"Impossible de créer la liste des succès.",
	"Connexion impossible. Vérifie ta connexion ou reconnecte-toi.",
	"Aucun ensemble de succès trouvé, ou le service est inaccessible.",
	"Impossible de lire la liste des succès.",
	"Les données du jeu ou du compte sont indisponibles.",
	"Impossible de récupérer les succès débloqués. Appuie sur Triangle pour réessayer.",
	"Impossible de lire l'ensemble principal des succès.",
	"Chargement annulé. Appuie sur Triangle pour réessayer.",
	"Impossible de lancer la liste des succès.",
	"Débloqué",
	"Verrouillé",
	"Connecte-toi avec ton compte RetroAchievements pour débloquer des succès en jouant.",
	"Nom d'utilisateur",
	"Mot de passe",
	"Ton nom d'utilisateur RetroAchievements",
	"Ton mot de passe",
	"Se connecter",
	"Connexion...",
	"Se déconnecter",
	"Appuie à nouveau pour te déconnecter",
	"Connecté en tant que %s",
	"Les succès se débloquent en jouant. Ton mot de passe n'est pas conservé sur la console.",
	"Sélectionner",
	"Modifier",
	"Afficher le mot de passe",
	"Masquer le mot de passe",
	"Effacer",
	"Terminé",
	"Espace",
	"Textures HD",
	"Textures HD prêtes",
	"Échec des textures HD",
	"Télécharger",
	"Annuler",
	"RetroAchievements : erreur du serveur",
	"Impossible de joindre RetroAchievements. Tu restes connecté : les succès reviendront au prochain jeu.",
	"Un succès n'a pas pu être envoyé à RetroAchievements avant la fermeture (pas de connexion). Il ne sera pas sur ton compte.",
	"Aucun BIOS PS2 trouvé : les jeux ne peuvent pas démarrer. Copiez votre fichier BIOS (4 Mo, p. ex. SCPH-70012.bin) dans %s",
};

const char* const kGerman[kCount] = {
	"Spielen",
	"Blättern",
	"Springen",
	"Spieleinstellungen: kein Netzwerk",
	"Keine Spiele in /data/PCSX2/games",
	"Cover werden geladen  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"PS5SX2: wird gestartet",
	"PS5SX2 (Testversion): wird gestartet",
	"PS5SX2: %d Cover wird geladen",
	"PS5SX2: %d Cover werden geladen",
	"Jetzt läuft: %s\n%s · viel Spaß!",
	"native Auflösung, %s",
	"Software-Renderer",
	"PS5SX2: Kein Spiel zum Starten. Lege deine .iso- oder .chd-Dateien in /data/PCSX2/games/ oder auf ein USB-Laufwerk und starte PS5SX2 dann neu.",
	"PS5SX2: Das Spiel ist nicht gestartet.\n%s",
	"PS5SX2: Das Menü konnte nicht geöffnet werden, zurück zum Spiel",
	"PS5SX2: Das Spiel wurde beendet",
	"Einstellungen",
	"Bewegen",
	"Ändern",
	"Zurücksetzen",
	"Zurück",
	"Dieses Spiel",
	"Alle Spiele",
	"PS5SX2: Kein PS2-BIOS gefunden. Kopiere deine BIOS-Datei (4 MB, z. B. SCPH-70012.bin) nach %s und starte das Spiel erneut.",
	"PS5SX2: Die Grafik des Spiels ist nicht gestartet. Starte es erneut; passiert es wieder, installiere PS5SX2 mit seinem Installer neu.",
	"Steuerung",
	"QR-Code",
	"Erfolge",
	"Alle",
	"Freigeschaltet",
	"Gesperrt",
	"Punkte",
	"Aktualisieren",
	"Zurück",
	"Weiter",
	"Erfolge werden geladen...",
	"Bilder werden geladen...",
	"Keine Erfolge für diesen Filter.",
	"Dieses Spiel hat keine veröffentlichten Erfolge.",
	"Wähle das laufende Spiel, um seine Erfolge zu sehen.",
	"Laufendes Spiel anzeigen",
	"Starte ein Spiel, um seine Erfolge zu sehen. Nutze vor dem Spielen den Reiter Erfolge in der Spieleliste.",
	"Die Erfolgsübersicht ist nicht verfügbar.",
	"Melde dich bei RetroAchievements an und starte ein Spiel.",
	"Verbindung zu RetroAchievements...",
	"Keine Erfolgsdaten für dieses Spiel verfügbar.",
	"Erfolge konnten nicht aktualisiert werden: ",
	"Wähle Dieses Spiel, um seine Erfolge zu sehen.",
	"Die vorherige Anfrage wird beendet. Drücke Dreieck, um dieses Spiel zu laden.",
	"Dreieck: aktualisieren",
	"Melde dich über die Spieleliste bei RetroAchievements an.",
	"Die Startdatei dieser Disc konnte nicht erkannt werden.",
	"Die sichere Verbindung konnte nicht hergestellt werden.",
	"Die Erfolgsübersicht konnte nicht erstellt werden.",
	"Verbindung fehlgeschlagen. Prüfe deine Verbindung oder melde dich erneut an.",
	"Keine Erfolge gefunden oder der Dienst ist nicht erreichbar.",
	"Die Erfolgsliste konnte nicht gelesen werden.",
	"Spiel- oder Kontodaten sind nicht verfügbar.",
	"Freigeschaltete Erfolge konnten nicht geladen werden. Drücke Dreieck zum Wiederholen.",
	"Die Haupterfolgsgruppe konnte nicht gelesen werden.",
	"Laden abgebrochen. Drücke Dreieck zum Wiederholen.",
	"Die Erfolgsübersicht konnte nicht gestartet werden.",
	"Freigeschaltet",
	"Gesperrt",
	"Melde dich mit deinem RetroAchievements-Konto an, um beim Spielen Erfolge freizuschalten.",
	"Benutzername",
	"Passwort",
	"Dein RetroAchievements-Benutzername",
	"Dein Passwort",
	"Anmelden",
	"Anmeldung läuft...",
	"Abmelden",
	"Zum Abmelden erneut drücken",
	"Angemeldet als %s",
	"Erfolge werden beim Spielen freigeschaltet. Dein Passwort wird nicht auf der Konsole gespeichert.",
	"Auswählen",
	"Bearbeiten",
	"Passwort anzeigen",
	"Passwort verbergen",
	"Löschen",
	"Fertig",
	"Leerzeichen",
	"HD-Texturen",
	"HD-Texturen bereit",
	"HD-Texturen fehlgeschlagen",
	"Herunterladen",
	"Abbrechen",
	"RetroAchievements: Serverfehler",
	"RetroAchievements ist nicht erreichbar. Du bleibst angemeldet: Die Erfolge sind beim nächsten Spiel wieder da.",
	"Ein Erfolg konnte vor dem Schließen nicht an RetroAchievements gesendet werden (keine Verbindung). Er fehlt in deinem Konto.",
	"Kein PS2-BIOS gefunden: Spiele können nicht starten. Kopiere deine BIOS-Datei (4 MB, z. B. SCPH-70012.bin) nach %s",
};

const char* const kItalian[kCount] = {
	"Gioca",
	"Sfoglia",
	"Salta",
	"Impostazioni di gioco: nessuna rete",
	"Nessun gioco in /data/PCSX2/games",
	"Download copertine  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"PS5SX2: avvio",
	"PS5SX2 (versione di prova): avvio",
	"PS5SX2: download di %d copertina",
	"PS5SX2: download di %d copertine",
	"In gioco: %s\n%s · buon divertimento!",
	"risoluzione nativa, %s",
	"rendering software",
	"PS5SX2: nessun gioco da avviare. Metti i tuoi file .iso o .chd in /data/PCSX2/games/ o su un'unità USB, poi riapri PS5SX2.",
	"PS5SX2: il gioco non si è avviato.\n%s",
	"PS5SX2: impossibile aprire il menu, si torna al gioco",
	"PS5SX2: il gioco si è fermato",
	"Impostazioni",
	"Sposta",
	"Cambia",
	"Ripristina",
	"Indietro",
	"Questo gioco",
	"Tutti i giochi",
	"PS5SX2: nessun BIOS PS2 trovato. Copia il tuo file BIOS (4 MB, ad es. SCPH-70012.bin) in %s e riavvia il gioco.",
	"PS5SX2: la grafica del gioco non si è avviata. Riavvialo; se succede di nuovo, reinstalla PS5SX2 con il suo programma di installazione.",
	"Comandi",
	"Codice QR",
	"Obiettivi",
	"Tutti",
	"Sbloccati",
	"Bloccati",
	"punti",
	"Aggiorna",
	"Precedente",
	"Successivo",
	"Caricamento obiettivi...",
	"Caricamento immagini...",
	"Nessun obiettivo per questo filtro.",
	"Questo gioco non ha obiettivi pubblicati.",
	"Seleziona il gioco in esecuzione per vedere i suoi obiettivi.",
	"Mostra gioco in esecuzione",
	"Avvia un gioco per vedere i suoi obiettivi in tempo reale. Prima di giocare, usa la scheda Obiettivi nella lista dei giochi.",
	"La lista degli obiettivi non è disponibile.",
	"Accedi a RetroAchievements e avvia un gioco.",
	"Connessione a RetroAchievements...",
	"Nessun dato sugli obiettivi disponibile per questo gioco.",
	"Impossibile aggiornare gli obiettivi: ",
	"Seleziona Questo gioco per vedere i suoi obiettivi.",
	"Completamento della richiesta precedente. Premi Triangolo per caricare questo gioco.",
	"Triangolo: aggiorna",
	"Accedi a RetroAchievements dalla lista dei giochi.",
	"Impossibile identificare il file di avvio di questo disco.",
	"Impossibile inizializzare la connessione sicura.",
	"Impossibile creare la lista degli obiettivi.",
	"Connessione non riuscita. Controlla la connessione o accedi di nuovo.",
	"Nessun gruppo di obiettivi trovato o servizio non raggiungibile.",
	"Impossibile leggere la lista degli obiettivi.",
	"I dati del gioco o dell'account non sono disponibili.",
	"Impossibile recuperare gli obiettivi sbloccati. Premi Triangolo per riprovare.",
	"Impossibile leggere il gruppo principale di obiettivi.",
	"Caricamento annullato. Premi Triangolo per riprovare.",
	"Impossibile avviare la lista degli obiettivi.",
	"Sbloccato",
	"Bloccato",
	"Accedi con il tuo account RetroAchievements per sbloccare obiettivi mentre giochi.",
	"Nome utente",
	"Password",
	"Il tuo nome utente RetroAchievements",
	"La tua password",
	"Accedi",
	"Accesso in corso...",
	"Esci",
	"Premi di nuovo per uscire",
	"Accesso eseguito come %s",
	"Gli obiettivi si sbloccano mentre giochi. La tua password non viene salvata sulla console.",
	"Seleziona",
	"Modifica",
	"Mostra password",
	"Nascondi password",
	"Cancella",
	"Fatto",
	"Spazio",
	"Texture HD",
	"Texture HD pronte",
	"Texture HD non riuscite",
	"Scarica",
	"Annulla",
	"RetroAchievements: errore del server",
	"Impossibile raggiungere RetroAchievements. Resti connesso: gli obiettivi torneranno con il prossimo gioco.",
	"Un obiettivo non è stato inviato a RetroAchievements prima della chiusura (nessuna connessione). Non comparirà sul tuo account.",
	"Nessun BIOS PS2 trovato: i giochi non possono avviarsi. Copia il tuo file BIOS (4 MB, es. SCPH-70012.bin) in %s",
};

const char* const kDutch[kCount] = {
	"Spelen",
	"Bladeren",
	"Springen",
	"Spelinstellingen: geen netwerk",
	"Geen games in /data/PCSX2/games",
	"Covers downloaden  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"PS5SX2: wordt gestart",
	"PS5SX2 (testversie): wordt gestart",
	"PS5SX2: %d cover downloaden",
	"PS5SX2: %d covers downloaden",
	"Nu speel je: %s\n%s · veel plezier!",
	"native resolutie, %s",
	"softwarerenderer",
	"PS5SX2: geen game om te starten. Zet je .iso- of .chd-bestanden in /data/PCSX2/games/ of op een USB-stick en start PS5SX2 opnieuw.",
	"PS5SX2: de game is niet gestart.\n%s",
	"PS5SX2: het menu kon niet worden geopend, terug naar de game",
	"PS5SX2: de game is gestopt",
	"Instellingen",
	"Verplaatsen",
	"Wijzigen",
	"Herstellen",
	"Terug",
	"Dit spel",
	"Alle spellen",
	"PS5SX2: geen PS2-BIOS gevonden. Kopieer je BIOS-bestand (4 MB, bijv. SCPH-70012.bin) naar %s en start de game opnieuw.",
	"PS5SX2: de graphics van de game zijn niet gestart. Start de game opnieuw; gebeurt het weer, installeer PS5SX2 dan opnieuw met het installatieprogramma.",
	"Besturing",
	"QR-code",
	"Prestaties",
	"Alle",
	"Ontgrendeld",
	"Vergrendeld",
	"punten",
	"Vernieuwen",
	"Vorige",
	"Volgende",
	"Prestaties laden...",
	"Afbeeldingen laden...",
	"Geen prestaties voor dit filter.",
	"Dit spel heeft geen gepubliceerde prestaties.",
	"Selecteer het actieve spel om de prestaties te bekijken.",
	"Actief spel tonen",
	"Start een spel om de prestaties te bekijken. Gebruik vooraf het tabblad Prestaties in de spellenlijst.",
	"Het prestatieoverzicht is niet beschikbaar.",
	"Meld je aan bij RetroAchievements en start een spel.",
	"Verbinden met RetroAchievements...",
	"Geen prestatiegegevens beschikbaar voor dit spel.",
	"Prestaties konden niet worden vernieuwd: ",
	"Selecteer Dit spel om de prestaties te bekijken.",
	"Vorige aanvraag afronden. Druk op Driehoek om dit spel te laden.",
	"Driehoek: vernieuwen",
	"Meld je aan bij RetroAchievements via de spellenlijst.",
	"Het opstartbestand van deze disc kon niet worden herkend.",
	"De beveiligde verbinding kon niet worden gestart.",
	"Het prestatieoverzicht kon niet worden gemaakt.",
	"Verbinding mislukt. Controleer je verbinding of meld je opnieuw aan.",
	"Geen prestaties gevonden of de dienst is niet bereikbaar.",
	"De prestatielijst kon niet worden gelezen.",
	"Spel- of accountgegevens zijn niet beschikbaar.",
	"Ontgrendelde prestaties konden niet worden opgehaald. Druk op Driehoek om opnieuw te proberen.",
	"De hoofdgroep van prestaties kon niet worden gelezen.",
	"Laden geannuleerd. Druk op Driehoek om opnieuw te proberen.",
	"Het prestatieoverzicht kon niet worden gestart.",
	"Ontgrendeld",
	"Vergrendeld",
	"Meld je aan met je RetroAchievements-account om prestaties te ontgrendelen terwijl je speelt.",
	"Gebruikersnaam",
	"Wachtwoord",
	"Je RetroAchievements-gebruikersnaam",
	"Je wachtwoord",
	"Aanmelden",
	"Aanmelden...",
	"Afmelden",
	"Druk nogmaals om af te melden",
	"Aangemeld als %s",
	"Prestaties worden ontgrendeld terwijl je speelt. Je wachtwoord wordt niet op de console bewaard.",
	"Selecteren",
	"Bewerken",
	"Wachtwoord tonen",
	"Wachtwoord verbergen",
	"Wissen",
	"Klaar",
	"Spatie",
	"HD-textures",
	"HD-textures klaar",
	"HD-textures mislukt",
	"Downloaden",
	"Annuleren",
	"RetroAchievements: serverfout",
	"RetroAchievements is niet bereikbaar. Je blijft aangemeld: de prestaties zijn er bij het volgende spel weer.",
	"Een prestatie kon voor het afsluiten niet naar RetroAchievements worden verstuurd (geen verbinding). Ze staat niet op je account.",
	"Geen PS2-BIOS gevonden: games kunnen niet starten. Kopieer je BIOS-bestand (4 MB, bijv. SCPH-70012.bin) naar %s",
};

const char* const kPortuguese[kCount] = {
	"Jogar",
	"Navegar",
	"Saltar",
	"Definições do jogo: sem rede",
	"Nenhum jogo em /data/PCSX2/games",
	"A transferir capas  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"PS5SX2: a iniciar",
	"PS5SX2 (versão de teste): a iniciar",
	"PS5SX2: a transferir %d capa",
	"PS5SX2: a transferir %d capas",
	"A jogar: %s\n%s · diverte-te!",
	"resolução nativa, %s",
	"renderização por software",
	"PS5SX2: nenhum jogo para iniciar. Coloca os teus ficheiros .iso ou .chd em /data/PCSX2/games/ ou numa pen USB e volta a abrir o PS5SX2.",
	"PS5SX2: o jogo não arrancou.\n%s",
	"PS5SX2: não foi possível abrir o menu, de volta ao jogo",
	"PS5SX2: o jogo parou",
	"Definições",
	"Mover",
	"Alterar",
	"Repor",
	"Voltar",
	"Este jogo",
	"Todos os jogos",
	"PS5SX2: nenhuma BIOS da PS2 encontrada. Copia o teu ficheiro de BIOS (4 MB, por ex. SCPH-70012.bin) para %s e volta a iniciar o jogo.",
	"PS5SX2: os gráficos do jogo não arrancaram. Volta a iniciá-lo; se acontecer de novo, reinstala o PS5SX2 com o instalador.",
	"Controlos",
	"Código QR",
	"Conquistas",
	"Todas",
	"Desbloqueadas",
	"Bloqueadas",
	"pontos",
	"Atualizar",
	"Anterior",
	"Seguinte",
	"A carregar conquistas...",
	"A carregar imagens...",
	"Não há conquistas neste filtro.",
	"Este jogo não tem conquistas publicadas.",
	"Seleciona o jogo em execução para veres as suas conquistas.",
	"Mostrar o jogo em execução",
	"Inicia um jogo para veres as suas conquistas em tempo real. Antes de jogar, usa o separador Conquistas da lista de jogos.",
	"A lista de conquistas não está disponível.",
	"Inicia sessão no RetroAchievements e começa um jogo.",
	"A ligar ao RetroAchievements...",
	"Não há dados de conquistas para este jogo.",
	"Não foi possível atualizar as conquistas: ",
	"Seleciona Este jogo para veres as suas conquistas.",
	"A terminar o pedido anterior. Prime Triângulo para carregar este jogo.",
	"Triângulo: atualizar",
	"Inicia sessão no RetroAchievements a partir da lista de jogos.",
	"Não foi possível identificar o executável de arranque deste disco.",
	"Não foi possível iniciar a ligação segura.",
	"Não foi possível criar a lista de conquistas.",
	"Não foi possível ligar. Verifica a tua ligação ou volta a iniciar sessão.",
	"Não foi encontrado nenhum conjunto de conquistas, ou o serviço está inacessível.",
	"Não foi possível ler a lista de conquistas.",
	"Os dados do jogo ou da conta não estão disponíveis.",
	"Não foi possível obter as conquistas desbloqueadas. Prime Triângulo para tentar de novo.",
	"Não foi possível ler o conjunto principal de conquistas.",
	"Carregamento cancelado. Prime Triângulo para tentar de novo.",
	"Não foi possível iniciar a lista de conquistas.",
	"Desbloqueada",
	"Bloqueada",
	"Inicia sessão com a tua conta RetroAchievements para desbloquear conquistas enquanto jogas.",
	"Nome de utilizador",
	"Palavra-passe",
	"O teu nome de utilizador RetroAchievements",
	"A tua palavra-passe",
	"Iniciar sessão",
	"A iniciar sessão...",
	"Terminar sessão",
	"Prime novamente para terminar sessão",
	"Sessão iniciada como %s",
	"As conquistas desbloqueiam-se enquanto jogas. A tua palavra-passe não fica guardada na consola.",
	"Selecionar",
	"Editar",
	"Mostrar palavra-passe",
	"Ocultar palavra-passe",
	"Apagar",
	"Concluir",
	"Espaço",
	"Texturas HD",
	"Texturas HD prontas",
	"Falha nas texturas HD",
	"Transferir",
	"Cancelar",
	"RetroAchievements: erro do servidor",
	"Não foi possível contactar o RetroAchievements. Continuas com a sessão iniciada: as conquistas voltam no próximo jogo.",
	"Não foi possível enviar uma conquista ao RetroAchievements antes de fechar (sem ligação). Não vai aparecer na tua conta.",
	"Nenhuma BIOS da PS2 encontrada: os jogos não podem arrancar. Copie o seu ficheiro de BIOS (4 MB, p. ex. SCPH-70012.bin) para %s",
};

const char* const kPortugueseBrazil[kCount] = {
	"Jogar",
	"Navegar",
	"Pular",
	"Configurações do jogo: sem rede",
	"Nenhum jogo em /data/PCSX2/games",
	"Baixando capas  %d / %d",
	"%s GB",
	"%s MB",
	",",
	"PS5SX2: iniciando",
	"PS5SX2 (versão de teste): iniciando",
	"PS5SX2: baixando %d capa",
	"PS5SX2: baixando %d capas",
	"Jogando: %s\n%s · divirta-se!",
	"resolução nativa, %s",
	"renderização por software",
	"PS5SX2: nenhum jogo para iniciar. Coloque seus arquivos .iso ou .chd em /data/PCSX2/games/ ou em um pendrive e abra o PS5SX2 de novo.",
	"PS5SX2: o jogo não iniciou.\n%s",
	"PS5SX2: não foi possível abrir o menu, voltando ao jogo",
	"PS5SX2: o jogo parou",
	"Configurações",
	"Mover",
	"Alterar",
	"Redefinir",
	"Voltar",
	"Este jogo",
	"Todos os jogos",
	"PS5SX2: nenhuma BIOS de PS2 encontrada. Copie seu arquivo de BIOS (4 MB, ex.: SCPH-70012.bin) para %s e inicie o jogo de novo.",
	"PS5SX2: os gráficos do jogo não iniciaram. Inicie de novo; se acontecer outra vez, reinstale o PS5SX2 com o instalador.",
	"Controles",
	"QR code",
	"Conquistas",
	"Todas",
	"Desbloqueadas",
	"Bloqueadas",
	"pontos",
	"Atualizar",
	"Anterior",
	"Próxima",
	"Carregando conquistas...",
	"Carregando imagens...",
	"Nenhuma conquista neste filtro.",
	"Este jogo não tem conquistas publicadas.",
	"Selecione o jogo em execução para ver suas conquistas.",
	"Mostrar jogo em execução",
	"Inicie um jogo para ver suas conquistas em tempo real. Antes de jogar, use a aba Conquistas na lista de jogos.",
	"A consulta de conquistas está indisponível.",
	"Entre no RetroAchievements e inicie um jogo.",
	"Conectando ao RetroAchievements...",
	"Não há dados de conquistas disponíveis para este jogo.",
	"Não foi possível atualizar as conquistas: ",
	"Selecione Este jogo para ver suas conquistas.",
	"Finalizando a solicitação anterior. Pressione Triângulo para carregar este jogo.",
	"Triângulo: atualizar",
	"Entre no RetroAchievements pela lista de jogos.",
	"Não foi possível identificar o executável de inicialização deste disco.",
	"Não foi possível iniciar a conexão segura.",
	"Não foi possível criar a consulta de conquistas.",
	"Não foi possível conectar. Verifique sua conexão ou entre novamente.",
	"Nenhum conjunto de conquistas encontrado ou o serviço está inacessível.",
	"Não foi possível ler a lista de conquistas.",
	"Os dados do jogo ou da conta estão indisponíveis.",
	"Não foi possível consultar as conquistas desbloqueadas. Pressione Triângulo para tentar novamente.",
	"Não foi possível ler o conjunto principal de conquistas.",
	"Carregamento cancelado. Pressione Triângulo para tentar novamente.",
	"Não foi possível iniciar a consulta de conquistas.",
	"Desbloqueada",
	"Bloqueada",
	"Entre com sua conta RetroAchievements para desbloquear conquistas enquanto joga.",
	"Nome de usuário",
	"Senha",
	"Seu nome de usuário do RetroAchievements",
	"Sua senha",
	"Entrar",
	"Entrando...",
	"Sair",
	"Pressione de novo para sair",
	"Conectado como %s",
	"As conquistas são desbloqueadas enquanto você joga. Sua senha não fica salva no console.",
	"Selecionar",
	"Editar",
	"Mostrar senha",
	"Ocultar senha",
	"Apagar",
	"Concluir",
	"Espaço",
	"Texturas HD",
	"Texturas HD prontas",
	"Falha nas texturas HD",
	"Baixar",
	"Cancelar",
	"RetroAchievements: erro do servidor",
	"Não foi possível acessar o RetroAchievements. Você continua conectado: as conquistas voltam no próximo jogo.",
	"Não foi possível enviar uma conquista ao RetroAchievements antes de fechar (sem conexão). Ela não vai aparecer na sua conta.",
	"Nenhuma BIOS do PS2 encontrada: os jogos não podem iniciar. Copie seu arquivo de BIOS (4 MB, ex.: SCPH-70012.bin) para %s",
};

// The region names fe_games.cpp takes from a file name's first group (kRegions there), in its order.
constexpr int kRegionCount = 19;
const char* const kRegionsEnglish[kRegionCount] = {"USA", "Europe", "Japan", "Korea", "Asia", "World", "Australia",
	"France", "Germany", "Italy", "Spain", "UK", "Canada", "Brazil", "Russia", "China", "Taiwan", "Netherlands", "Sweden"};
const char* const kRegionsSpanish[kRegionCount] = {"EE. UU.", "Europa", "Japón", "Corea", "Asia", "Mundo", "Australia",
	"Francia", "Alemania", "Italia", "España", "Reino Unido", "Canadá", "Brasil", "Rusia", "China", "Taiwán",
	"Países Bajos", "Suecia"};
const char* const kRegionsFrench[kRegionCount] = {"États-Unis", "Europe", "Japon", "Corée", "Asie", "Monde",
	"Australie", "France", "Allemagne", "Italie", "Espagne", "Royaume-Uni", "Canada", "Brésil", "Russie", "Chine",
	"Taïwan", "Pays-Bas", "Suède"};
const char* const kRegionsGerman[kRegionCount] = {"USA", "Europa", "Japan", "Korea", "Asien", "Welt", "Australien",
	"Frankreich", "Deutschland", "Italien", "Spanien", "Großbritannien", "Kanada", "Brasilien", "Russland", "China",
	"Taiwan", "Niederlande", "Schweden"};
const char* const kRegionsItalian[kRegionCount] = {"USA", "Europa", "Giappone", "Corea", "Asia", "Mondo", "Australia",
	"Francia", "Germania", "Italia", "Spagna", "Regno Unito", "Canada", "Brasile", "Russia", "Cina", "Taiwan",
	"Paesi Bassi", "Svezia"};
const char* const kRegionsDutch[kRegionCount] = {"VS", "Europa", "Japan", "Korea", "Azië", "Wereld", "Australië",
	"Frankrijk", "Duitsland", "Italië", "Spanje", "VK", "Canada", "Brazilië", "Rusland", "China", "Taiwan",
	"Nederland", "Zweden"};
const char* const kRegionsPortuguese[kRegionCount] = {"EUA", "Europa", "Japão", "Coreia", "Ásia", "Mundo",
	"Austrália", "França", "Alemanha", "Itália", "Espanha", "Reino Unido", "Canadá", "Brasil", "Rússia", "China",
	"Taiwan", "Países Baixos", "Suécia"};
const char* const kRegionsPortugueseBrazil[kRegionCount] = {"EUA", "Europa", "Japão", "Coreia", "Ásia", "Mundo",
	"Austrália", "França", "Alemanha", "Itália", "Espanha", "Reino Unido", "Canadá", "Brasil", "Rússia", "China",
	"Taiwan", "Holanda", "Suécia"};

struct Language
{
	const char* code;
	const char* const* text;
	const char* const* regions;
};

const Language kLanguages[] = {
	{"en", kEnglish, kRegionsEnglish},
	{"fr", kFrench, kRegionsFrench},
	{"es", kSpanish, kRegionsSpanish},
	{"es-419", kSpanishLatAm, kRegionsSpanish},
	{"de", kGerman, kRegionsGerman},
	{"it", kItalian, kRegionsItalian},
	{"nl", kDutch, kRegionsDutch},
	{"pt", kPortuguese, kRegionsPortuguese},
	{"pt-BR", kPortugueseBrazil, kRegionsPortugueseBrazil},
};

const Language* s_lang = &kLanguages[0];
std::vector<std::string> s_text_over(kCount);
std::vector<std::string> s_region_over(kRegionCount);

// The PS5's language ids (as the PS4's: 0 Japanese, 1 English (US), 2 French, 3 Spanish, 4 German, 5 Italian,
// 6 Dutch, 7 Portuguese, 17 Portuguese (Brazil), 18 English (UK), 20 Spanish (Latin America), 22 French
// (Canada); the others have no table here).
const Language* LanguageFor(int ps5)
{
	switch (ps5)
	{
		case 2:
		case 22:
			return &kLanguages[1];
		case 3:
			return &kLanguages[2];
		case 20:
			return &kLanguages[3];
		case 4:
			return &kLanguages[4];
		case 5:
			return &kLanguages[5];
		case 6:
			return &kLanguages[6];
		case 7:
			return &kLanguages[7];
		case 17:
			return &kLanguages[8];
		default:
			return &kLanguages[0];
	}
}

std::string Trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
		a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
		b--;
	return s.substr(a, b - a);
}

// "\n" in a value is a line break.
std::string Unescape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++)
	{
		if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n')
		{
			out += '\n';
			i++;
		}
		else
			out += s[i];
	}
	return out;
}

// The conversions of a printf format, in order ("%d / %d" -> "dd"); an unfinished one gives "?".
std::string Conversions(const char* f)
{
	std::string out;
	for (const char* p = f; *p; p++)
	{
		if (*p != '%')
			continue;
		p++;
		if (*p == '%')
			continue;
		while (*p && std::strchr("-+ #0123456789.lhzjt", *p))
			p++;
		if (!*p)
		{
			out += '?';
			break;
		}
		out += *p;
	}
	return out;
}

void LoadOverrides(const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return;
	int used = 0, refused = 0;
	char line[1024];
	while (std::fgets(line, sizeof(line), f))
	{
		std::string l = Trim(line);
		if (l.size() >= 3 && static_cast<unsigned char>(l[0]) == 0xEF && static_cast<unsigned char>(l[1]) == 0xBB &&
			static_cast<unsigned char>(l[2]) == 0xBF)
			l = Trim(l.substr(3)); // a byte-order mark from Notepad
		if (l.empty() || l[0] == '#' || l[0] == ';')
			continue;
		const size_t eq = l.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = Trim(l.substr(0, eq));
		const std::string value = Unescape(Trim(l.substr(eq + 1)));
		bool known = false;
		for (int i = 0; i < kCount; i++)
			if (key == kKeys[i])
			{
				known = true;
				if (value.empty() || !SameFormat(kEnglish[i], value.c_str()))
				{
					std::printf("[i18n] %s: %s refused (it has to keep the English text's %%d and %%s, in order)\n", path.c_str(),
						key.c_str());
					refused++;
				}
				else
				{
					s_text_over[i] = value;
					used++;
				}
			}
		if (key.compare(0, 7, "region.") == 0)
			for (int i = 0; i < kRegionCount; i++)
				if (key.compare(7, std::string::npos, kRegionsEnglish[i]) == 0 && !value.empty() && value.find('%') == std::string::npos)
				{
					known = true;
					s_region_over[i] = value;
					used++;
				}
		if (!known)
		{
			std::printf("[i18n] %s: unknown key %s\n", path.c_str(), key.c_str());
			refused++;
		}
	}
	std::fclose(f);
	std::printf("[i18n] %s: %d entries used, %d refused\n", path.c_str(), used, refused);
}
} // namespace

void SetLanguage(int ps5_language, const std::string& lang_dir)
{
	s_lang = LanguageFor(ps5_language);
	for (std::string& s : s_text_over)
		s.clear();
	for (std::string& s : s_region_over)
		s.clear();
	std::printf("[i18n] PS5 language %d: text in %s\n", ps5_language, s_lang->code);
	if (!lang_dir.empty())
		LoadOverrides(lang_dir + "/" + s_lang->code + ".txt");
	std::fflush(stdout);
}

const char* Tr(Str id)
{
	const int i = static_cast<int>(id);
	if (i < 0 || i >= kCount)
		return "";
	if (!s_text_over[static_cast<size_t>(i)].empty())
		return s_text_over[static_cast<size_t>(i)].c_str();
	const char* t = s_lang->text[i];
	return t ? t : kEnglish[i];
}

const char* LanguageCode()
{
	return s_lang->code;
}

const char* Key(Str id)
{
	const int i = static_cast<int>(id);
	return i >= 0 && i < kCount ? kKeys[i] : "";
}

std::string Region(const std::string& region)
{
	std::string out;
	size_t pos = 0;
	while (pos <= region.size())
	{
		size_t comma = region.find(", ", pos);
		if (comma == std::string::npos)
			comma = region.size();
		const std::string token = region.substr(pos, comma - pos);
		std::string shown = token;
		for (int i = 0; i < kRegionCount; i++)
			if (token == kRegionsEnglish[i])
				shown = !s_region_over[static_cast<size_t>(i)].empty() ? s_region_over[static_cast<size_t>(i)] : s_lang->regions[i];
		if (!out.empty())
			out += ", ";
		out += shown;
		pos = comma + 2;
	}
	return out;
}

std::string Size(uint64_t bytes)
{
	char num[32];
	const double gb = static_cast<double>(bytes) / 1e9;
	const bool big = gb >= 1.0;
	if (big)
		std::snprintf(num, sizeof(num), "%.1f", gb);
	else
		std::snprintf(num, sizeof(num), "%.0f", static_cast<double>(bytes) / 1e6);
	std::string n = num;
	const std::string dec = Tr(Str::Decimal);
	const size_t dot = n.find('.');
	if (dot != std::string::npos)
		n.replace(dot, 1, dec);
	char out[64];
	std::snprintf(out, sizeof(out), Tr(big ? Str::SizeGB : Str::SizeMB), n.c_str());
	return out;
}

int Ps2LanguageFor(int ps5_language)
{
	switch (ps5_language)
	{
		case 0:
			return 0; // Japanese
		case 1:
		case 18:
			return 1; // English (US, UK)
		case 2:
		case 22:
			return 2; // French (France, Canada)
		case 3:
		case 20:
			return 3; // Spanish (Spain, Latin America)
		case 4:
			return 4; // German
		case 5:
			return 5; // Italian
		case 6:
			return 6; // Dutch
		case 7:
		case 17:
			return 7; // Portuguese (Portugal, Brazil)
		default:
			return 1; // no such language on the PS2: English
	}
}

const char* Ps2LanguageName(int ps2_language)
{
	static const char* const names[] = {"Japanese", "English", "French", "Spanish", "German", "Italian", "Dutch", "Portuguese"};
	return ps2_language >= 0 && ps2_language < 8 ? names[ps2_language] : "?";
}

bool SameFormat(const char* a, const char* b)
{
	return Conversions(a) == Conversions(b);
}
} // namespace fe
