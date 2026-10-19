/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"

/*
================================================================================
The port's own text, in every language the game has.

Everything the port added to the screen - menu rows, the controller layout,
the game browser, profiles, the online dialogs, the on-screen keyboard's
titles - is a "#str_vita_..." string id, added to the language dictionary
right after the game's own .lang files load (Vita_AddPortStrings, from
idCommonLocal::InitLanguageDict), in the language the game runs in. SWF
labels take the ids as they are; code calls idLocalization::GetString.
This file is UTF-8.
================================================================================
*/

struct vitaString_t {
    const char* key;
    const char* en;
    const char* fr;
    const char* it;
    const char* de;
    const char* es;
    const char* ja;
};

static const vitaString_t vitaStrings[] = {
    // ---- controller layout (sys/vita/vita_ui.cpp) ----
    { "#str_vita_layout_title", "CONTROLLER LAYOUT", "DISPOSITION DES COMMANDES", "CONFIGURAZIONE COMANDI", "TASTENBELEGUNG", "ESQUEMA DE CONTROLES", "コントローラー配置" },
    { "#str_vita_back", "BACK", "RETOUR", "INDIETRO", "ZURÜCK", "VOLVER", "戻る" },
    { "#str_vita_fire_melee", "Primary Fire / Melee", "Tir principal / Mêlée", "Fuoco primario / Corpo a corpo", "Primärfeuer / Nahkampf", "Disparo principal / Cuerpo a cuerpo", "通常攻撃 / 近接攻撃" },
    { "#str_vita_flashlight", "Flashlight", "Lampe torche", "Torcia", "Taschenlampe", "Linterna", "フラッシュライト" },
    { "#str_vita_reload", "Reload", "Recharger", "Ricarica", "Nachladen", "Recargar", "リロード" },
    { "#str_vita_next_weapon", "Next Weapon", "Arme suivante", "Arma successiva", "Nächste Waffe", "Arma siguiente", "次の武器" },
    { "#str_vita_prev_weapon", "Previous Weapon", "Arme précédente", "Arma precedente", "Vorherige Waffe", "Arma anterior", "前の武器" },
    { "#str_vita_quick1", "Quick-slot 1: Grenades", "Raccourci 1 : Grenades", "Slot rapido 1: Granate", "Schnellwahl 1: Granaten", "Ranura rápida 1: Granadas", "クイックスロット1: グレネード" },
    { "#str_vita_quick2", "Quick-slot 2: BFG 9000", "Raccourci 2 : BFG 9000", "Slot rapido 2: BFG 9000", "Schnellwahl 2: BFG 9000", "Ranura rápida 2: BFG 9000", "クイックスロット2: BFG 9000" },
    { "#str_vita_fists", "Fists / Chainsaw", "Poings / Tronçonneuse", "Pugni / Motosega", "Fäuste / Kettensäge", "Puños / Motosierra", "素手 / チェーンソー" },
    { "#str_vita_soulcube", "Soul Cube / Artifact", "Cube d'âme / Artefact", "Cubo delle anime / Artefatto", "Seelenwürfel / Artefakt", "Cubo de almas / Artefacto", "ソウルキューブ / アーティファクト" },
    { "#str_vita_jump", "Jump", "Sauter", "Salta", "Springen", "Saltar", "ジャンプ" },
    { "#str_vita_use", "Use / Interact", "Utiliser / Interagir", "Usa / Interagisci", "Benutzen / Interagieren", "Usar / Interactuar", "使う / 調べる" },
    { "#str_vita_pda", "PDA / Logs", "PDA / Journaux", "PDA / Registri", "PDA / Protokolle", "PDA / Registros", "PDA / ログ" },
    { "#str_vita_center_view", "Center View", "Centrer la vue", "Centra visuale", "Ansicht zentrieren", "Centrar vista", "視点リセット" },
    { "#str_vita_zoom", "Zoom", "Zoom", "Zoom", "Zoom", "Zoom", "ズーム" },
    { "#str_vita_move", "Move", "Se déplacer", "Muoviti", "Bewegen", "Moverse", "移動" },
    { "#str_vita_camera", "Camera / Aim", "Caméra / Visée", "Visuale / Mira", "Kamera / Zielen", "Cámara / Apuntar", "カメラ / 照準" },
    { "#str_vita_quickturn", "180° Quick Turn", "Demi-tour rapide 180°", "Giravolta rapida 180°", "180°-Schnellwende", "Giro rápido de 180°", "180度クイックターン" },
    { "#str_vita_sprint_toggle", "Sprint (Toggle)", "Sprint (bascule)", "Scatto (attiva/disattiva)", "Sprinten (umschalten)", "Correr (alternar)", "ダッシュ (切替)" },
    { "#str_vita_sprint_hold", "Sprint (Hold)", "Sprint (maintenir)", "Scatto (tieni premuto)", "Sprinten (halten)", "Correr (mantener)", "ダッシュ (長押し)" },
    { "#str_vita_crouch_toggle", "Crouch (Toggle)", "S'accroupir (bascule)", "Accovacciati (attiva/disattiva)", "Ducken (umschalten)", "Agacharse (alternar)", "しゃがむ (切替)" },
    { "#str_vita_crouch_hold", "Crouch (Hold)", "S'accroupir (maintenir)", "Accovacciati (tieni premuto)", "Ducken (halten)", "Agacharse (mantener)", "しゃがむ (長押し)" },
    { "#str_vita_pause_menu", "Pause Menu", "Menu pause", "Menu di pausa", "Pausenmenü", "Menú de pausa", "ポーズメニュー" },
    // DOOM / DOOM II page
    { "#str_vita_c_fire", "Fire", "Tirer", "Fuoco", "Feuer", "Disparar", "攻撃" },
    { "#str_vita_c_menu_nav", "Menu Navigation", "Navigation dans les menus", "Navigazione menu", "Menünavigation", "Navegar por los menús", "メニュー操作" },
    { "#str_vita_c_turn", "Turn", "Tourner", "Gira", "Drehen", "Girar", "回転" },
    { "#str_vita_c_use_zoomout", "Use / Open: Map Zoom Out", "Utiliser / Ouvrir : dézoomer la carte", "Usa / Apri: riduci mappa", "Benutzen / Öffnen: Karte verkleinern", "Usar / Abrir: alejar mapa", "使う / 開く: マップ縮小" },
    { "#str_vita_c_back_zoomin", "Menu Back: Map Zoom In", "Retour menu : zoomer la carte", "Indietro menu: ingrandisci mappa", "Menü zurück: Karte vergrößern", "Atrás en menú: acercar mapa", "メニュー戻る: マップ拡大" },
    { "#str_vita_c_automap", "Automap", "Carte automatique", "Automappa", "Automap", "Automapa", "オートマップ" },
    { "#str_vita_c_use_confirm", "Use / Open: Menu Confirm", "Utiliser / Ouvrir : valider", "Usa / Apri: conferma", "Benutzen / Öffnen: Bestätigen", "Usar / Abrir: confirmar", "使う / 開く: 決定" },
    { "#str_vita_c_main_menu", "Main Menu", "Menu principal", "Menu principale", "Hauptmenü", "Menú principal", "メインメニュー" },
    { "#str_vita_c_run_toggle", "Run (Toggle)", "Courir (bascule)", "Corri (attiva/disattiva)", "Rennen (umschalten)", "Correr (alternar)", "走る (切替)" },
    { "#str_vita_c_run_hold", "Run (Hold)", "Courir (maintenir)", "Corri (tieni premuto)", "Rennen (halten)", "Correr (mantener)", "走る (長押し)" },

    // ---- settings ----
    { "#str_vita_rear_left", "Rear Touch Left Deadzone", "Zone morte arrière gauche", "Zona morta posteriore sinistra", "Toter Bereich hinten links", "Zona muerta trasera izquierda", "背面タッチ左デッドゾーン" },
    { "#str_vita_rear_left_desc", "Ignore touches near the left half's edge", "Ignorer les contacts près du bord gauche", "Ignora i tocchi vicino al bordo sinistro", "Berührungen am linken Rand ignorieren", "Ignorar toques cerca del borde izquierdo", "左半分の端付近のタッチを無視" },
    { "#str_vita_rear_right", "Rear Touch Right Deadzone", "Zone morte arrière droite", "Zona morta posteriore destra", "Toter Bereich hinten rechts", "Zona muerta trasera derecha", "背面タッチ右デッドゾーン" },
    { "#str_vita_rear_right_desc", "Ignore touches near the right half's edge", "Ignorer les contacts près du bord droit", "Ignora i tocchi vicino al bordo destro", "Berührungen am rechten Rand ignorieren", "Ignorar toques cerca del borde derecho", "右半分の端付近のタッチを無視" },
    { "#str_vita_language", "Language", "Langue", "Lingua", "Sprache", "Idioma", "言語" },
    { "#str_vita_language_desc", "Changes the language of the text and voices. Applies the next time the game starts.", "Change la langue des textes et des voix. Prend effet au prochain démarrage du jeu.",
      "Cambia la lingua di testi e voci. Ha effetto al prossimo avvio del gioco.", "Ändert die Sprache von Texten und Stimmen. Wird beim nächsten Spielstart wirksam.",
      "Cambia el idioma de los textos y las voces. Se aplica la próxima vez que inicies el juego.", "テキストと音声の言語を変更します。次回の起動時に反映されます。" },
    { "#str_vita_after_restart", "(after restart)", "(après redémarrage)", "(dopo il riavvio)", "(nach Neustart)", "(tras reiniciar)", "(再起動後)" },
    { "#str_vita_classic_screen", "DOOM / DOOM II Screen", "Écran DOOM / DOOM II", "Schermo DOOM / DOOM II", "DOOM / DOOM II-Bildschirm", "Pantalla de DOOM / DOOM II", "DOOM / DOOM II 画面" },
    { "#str_vita_screen_original", "Original", "Original", "Originale", "Original", "Original", "オリジナル" },
    { "#str_vita_screen_full", "Full screen", "Plein écran", "Schermo intero", "Vollbild", "Pantalla completa", "フルスクリーン" },

    // ---- game browser ----
    { "#str_vita_add_remove_friend", "Add / Remove Friend", "Ajouter / retirer un ami", "Aggiungi / rimuovi amico", "Freund hinzufügen / entfernen", "Añadir / quitar amigo", "フレンド追加 / 削除" },
    { "#str_vita_add_friend_addr", "Add Friend by Address", "Ajouter un ami par adresse", "Aggiungi amico tramite indirizzo", "Freund per Adresse hinzufügen", "Añadir amigo por dirección", "アドレスでフレンド追加" },
    { "#str_vita_ip_hint", "IP or IP:port", "IP ou IP:port", "IP o IP:porta", "IP oder IP:Port", "IP o IP:puerto", "IP または IP:ポート" },
    { "#str_vita_friend_suffix", "(friend)", "(ami)", "(amico)", "(Freund)", "(amigo)", "(フレンド)" },
    { "#str_vita_no_address", "No address", "Aucune adresse", "Nessun indirizzo", "Keine Adresse", "Sin dirección", "アドレスなし" },
    { "#str_vita_not_hosting", "Not hosting", "N'héberge pas", "Non ospita", "Hostet nicht", "No hospeda", "ホストしていません" },
    { "#str_vita_online", "Online", "En ligne", "Online", "Online", "En línea", "オンライン" },

    // ---- profiles, bans, online ----
    { "#str_vita_profile_title", "PLAYER PROFILE", "PROFIL DU JOUEUR", "PROFILO GIOCATORE", "SPIELERPROFIL", "PERFIL DEL JUGADOR", "プレイヤープロフィール" },
    { "#str_vita_player_id", "Player ID", "ID du joueur", "ID giocatore", "Spieler-ID", "ID de jugador", "プレイヤーID" },
    { "#str_vita_hw_id", "HW ID", "ID matériel", "ID hardware", "Hardware-ID", "ID de hardware", "ハードウェアID" },
    { "#str_vita_friends", "Friends", "Amis", "Amici", "Freunde", "Amigos", "フレンド" },
    { "#str_vita_players_online", "Players online", "Joueurs en ligne", "Giocatori online", "Spieler online", "Jugadores en línea", "オンラインのプレイヤー" },
    { "#str_vita_your_friend", "Your friend", "Votre ami", "Tuo amico", "Dein Freund", "Tu amigo", "フレンドです" },
    { "#str_vita_not_friend", "Not on your friends list", "Absent de votre liste d'amis", "Non è nella tua lista amici", "Nicht in deiner Freundesliste", "No está en tu lista de amigos", "フレンドリストにいません" },
    { "#str_vita_unknown", "unknown", "inconnu", "sconosciuto", "unbekannt", "desconocido", "不明" },
    { "#str_vita_owner_badge", "OWNER - administrator of online play", "PROPRIÉTAIRE - administrateur du jeu en ligne", "PROPRIETARIO - amministratore del gioco online", "EIGENTÜMER - Administrator des Online-Spiels", "PROPIETARIO - administrador del juego en línea", "オーナー - オンラインプレイ管理者" },
    { "#str_vita_owner_tag", "[OWNER]", "[PROPRIÉTAIRE]", "[PROPRIETARIO]", "[EIGENTÜMER]", "[PROPIETARIO]", "[オーナー]" },
    { "#str_vita_global_ban", "BAN FROM ONLINE PLAY", "BANNIR DU JEU EN LIGNE", "BANDISCI DAL GIOCO ONLINE", "VOM ONLINE-SPIEL VERBANNEN", "VETAR DEL JUEGO EN LÍNEA", "オンラインプレイから追放" },
    { "#str_vita_global_unban", "LIFT ONLINE BAN", "LEVER LE BANNISSEMENT EN LIGNE", "REVOCA IL BANDO ONLINE", "ONLINE-BANN AUFHEBEN", "LEVANTAR VETO EN LÍNEA", "オンライン追放を解除" },
    { "#str_vita_banned_from_yours", "(banned from your games)", "(banni de vos parties)", "(bandito dalle tue partite)", "(aus deinen Spielen verbannt)", "(vetado de tus partidas)", "(あなたのゲームから追放済み)" },
    { "#str_vita_ban", "BAN FROM MY GAMES", "BANNIR DE MES PARTIES", "BANDISCI DALLE MIE PARTITE", "AUS MEINEN SPIELEN VERBANNEN", "VETAR DE MIS PARTIDAS", "自分のゲームから追放" },
    { "#str_vita_unban", "UNBAN", "LEVER LE BANNISSEMENT", "REVOCA IL BANDO", "VERBANNUNG AUFHEBEN", "LEVANTAR EL VETO", "追放を解除" },
    { "#str_vita_invite_from", "%s invites you to join a game.", "%s vous invite à rejoindre une partie.", "%s ti invita a unirti a una partita.", "%s lädt dich in ein Spiel ein.", "%s te invita a unirte a una partida.", "%sがゲームに招待しています。" },
    { "#str_vita_invites_sent", "Invitation sent to %d friend(s).", "Invitation envoyée à %d ami(s).", "Invito inviato a %d amici.", "Einladung an %d Freund(e) gesendet.", "Invitación enviada a %d amigo(s).", "%d人のフレンドに招待を送信しました。" },
    { "#str_vita_no_friends_to_invite", "No friend to invite yet.", "Aucun ami à inviter pour l'instant.", "Nessun amico da invitare per ora.", "Noch keine Freunde zum Einladen.", "Aún no hay amigos a los que invitar.", "招待できるフレンドがいません。" },
    { "#str_vita_join", "JOIN", "REJOINDRE", "UNISCITI", "BEITRETEN", "UNIRSE", "参加する" },
    { "#str_vita_decline", "DECLINE", "REFUSER", "RIFIUTA", "ABLEHNEN", "RECHAZAR", "断る" },
    { "#str_vita_banned", "BANNED", "BANNI", "BANDITO", "GESPERRT", "VETADO", "追放" },
    { "#str_vita_banned_notice", "This PS Vita is banned from online play. Appeal on the Ensoclock Discord server.",
      "Cette PS Vita est bannie du jeu en ligne. Faites appel sur le serveur Discord Ensoclock.",
      "Questa PS Vita è bandita dal gioco online. Fai ricorso sul server Discord di Ensoclock.",
      "Diese PS Vita ist vom Online-Spiel gesperrt. Einspruch auf dem Discord-Server von Ensoclock.",
      "Esta PS Vita está vetada del juego en línea. Apela en el servidor de Discord de Ensoclock.",
      "このPS Vitaはオンラインプレイから追放されています。EnsoclockのDiscordサーバーで異議を申し立ててください。" },
    { "#str_vita_disclaimer",
      "ONLINE PLAY IS DIRECT PEER-TO-PEER. Players you meet, and the public DHT used to find them, see your internet address (your router's public IP). Use a router or VPN that hides it if that matters to you. Go online?",
      "LE JEU EN LIGNE SE FAIT EN PAIR-À-PAIR DIRECT. Les joueurs rencontrés, et le DHT public qui sert à les trouver, voient votre adresse Internet (l'IP publique de votre routeur). Utilisez un routeur ou un VPN qui la masque si cela compte pour vous. Se connecter ?",
      "IL GIOCO ONLINE È PEER-TO-PEER DIRETTO. I giocatori che incontri, e la DHT pubblica usata per trovarli, vedono il tuo indirizzo Internet (l'IP pubblico del router). Usa un router o una VPN che lo nasconda se per te è importante. Andare online?",
      "ONLINE-SPIEL LÄUFT DIREKT PEER-TO-PEER. Mitspieler und das öffentliche DHT, über das sie gefunden werden, sehen deine Internetadresse (die öffentliche IP deines Routers). Nutze einen Router oder ein VPN, das sie verbirgt, wenn dir das wichtig ist. Online gehen?",
      "EL JUEGO EN LÍNEA ES PEER-TO-PEER DIRECTO. Los jugadores que encuentres, y la DHT pública que se usa para encontrarlos, ven tu dirección de Internet (la IP pública de tu router). Usa un router o una VPN que la oculte si te importa. ¿Conectarse?",
      "オンラインプレイは直接のピアツーピアです。出会ったプレイヤーと、検索に使う公開DHTには、あなたのインターネットアドレス(ルーターのグローバルIP)が見えます。気になる場合は、それを隠せるルーターやVPNを使ってください。オンラインにしますか?" },
    { "#str_vita_username_title", "Choose your username (only once)", "Choisissez votre nom d'utilisateur (une seule fois)", "Scegli il tuo nome utente (una sola volta)", "Wähle deinen Benutzernamen (nur einmal)", "Elige tu nombre de usuario (solo una vez)", "ユーザー名を決めてください (一度だけ)" },
    { "#str_vita_friend_addr_title", "Friend's address (IP or IP:port)", "Adresse de l'ami (IP ou IP:port)", "Indirizzo dell'amico (IP o IP:porta)", "Adresse des Freundes (IP oder IP:Port)", "Dirección del amigo (IP o IP:puerto)", "フレンドのアドレス (IP または IP:ポート)" },
    { "#str_vita_console_title", "Console command", "Commande console", "Comando console", "Konsolenbefehl", "Comando de consola", "コンソールコマンド" },
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

/*
================================================================================
Fixes to the game's own text (2026-10-17 audit of the six .lang files:
tools/vita - scratchpad langaudit.py). NULL keeps the game's text in that
language.
  - the save notice said not to turn off "your computer";
  - Steam wording in two online dialogs;
  - a Lost Mission audio log's title and info exist only in English (every
    language showed English), two control names are missing in Spanish and
    three key names in Japanese;
  - a Japanese objective carried mis-encoded characters ("窶骭" - a dash
    decoded as Shift-JIS) and a prefix no other language has.
================================================================================
*/
static const vitaString_t vitaGameStringFixes[] = {
    { "#str_dlg_pc_saving", "Saving content. Please don't turn off your PlayStation Vita.", "Sauvegarde en cours. Ne pas éteindre la PlayStation Vita.",
        "Salvataggio in corso. Non spegnere la PlayStation Vita.", "Inhalt wird gespeichert. Bitte schalte deine PlayStation Vita nicht aus.",
        "Guardando contenido. No apagues la PlayStation Vita.", "コンテンツのセーブ中です。 PlayStation Vitaの電源を切らないでください。" },
    { "#str_dlg_pc_online_join_error_two_profiles_one_box", "Unable to join match, this PlayStation Vita is already in this lobby.",
        "Vous ne pouvez pas rejoindre la partie, cette PlayStation Vita est déjà dans ce lobby.",
        "Impossibile unirsi alla partita. Questa PlayStation Vita è già nella sala d'attesa.",
        "Du kannst dem Spiel nicht beitreten, da diese PlayStation Vita bereits in dieser Lobby ist.",
        "Ha sido imposible unirse a la partida. Esta PlayStation Vita ya está en esta sala.",
        "このPlayStation Vitaはすでにこのロビーにいるため、マッチに参加できません。" },
    { "#str_le_exis_tooley_audio_2_audio_name", "Emergency Situation in Operations", "Situation d'urgence aux opérations",
        "Situazione d'emergenza alle Operazioni", "Notfall in der Betriebsabteilung", "Situación de emergencia en Operaciones", "オペレーションでの緊急事態" },
    { "#str_le_exis_tooley_audio_2_audio_info", "Made by: Bernice Tooley\nDate: 11-15-2145\nOpen for Amendment: No\nSet for Archive: Yes",
        "Auteur : Bernice Tooley\nDate : 15-11-2145\nModification possible : non\nPrêt pour l'archivage : oui",
        "Autore: Bernice Tooley\nData: 15-11-2145\nModificabile: no\nDa archiviare: sì",
        "Erstellt von: Bernice Tooley\nDatum: 15.11.2145\nÄnderungen möglich: Nein\nZur Archivierung: Ja",
        "Hecho por: Bernice Tooley\nFecha: 15-11-2145\nAbierto a correcciones: no\nEstablecer para archivo: sí",
        "Made by: バニース トゥーリー\nDate: 2145-11-15\n改訂の可否: 否\n保管予定: あり" },
    { "#str_swf_action_impulse11", NULL, NULL, NULL, NULL, "Cuerpo a cuerpo/Agarrador", NULL },
    { "#str_swf_action_impulse12", NULL, NULL, NULL, NULL, "Cubo de almas/Artefacto", NULL },
    { "#str_bind_RALT", NULL, NULL, NULL, NULL, NULL, "右Alt" },
    { "#str_bind_RCTRL", NULL, NULL, NULL, NULL, NULL, "右Ctrl" },
    { "#str_bind_RSHIFT", NULL, NULL, NULL, NULL, NULL, "右Shift" },
    { "#str_00200089", NULL, NULL, NULL, NULL, NULL, "生存者を探して救助する" },
    // characters the Japanese font (DFPHeiseiGothicW7, 1516 glyphs) does not
    // have, found by the full verification's glyph check: the same words with
    // ones it has (強豪 -> 強敵, 賞金 -> 報酬, full-width digits, 怒り / 煙 / 巻き in kana)
    { "#str_03870", NULL, NULL, NULL, NULL, NULL, "USM Earthでプライベートシャトルが爆発、4名が死亡 --IPN NEWS-- 天王星スキャンダルで、スクレッグス上院議員への告訴取り下げ --IPN NEWS-- 猛威を振るうボルジアウイルス、新たに3つの宇宙ステーションで感染者が --IPN NEWS-- 流星塵が火星の極地域を襲撃 --IPN NEWS-- 月面基地の放射性物質漏れは当初の報告よりも深刻 --IPN NEWS-- 当局がウランの違法取引で2人の鉱員を逮捕 --IPN NEWS-- 第149回クエイクコンゲームスで、アンディ ストラットンが並み居る強敵を抑えて金メダルと報酬1000万クレジットを獲得 --IPN NEWS-- " },
    { "#str_00200072", NULL, NULL, NULL, NULL, NULL, "侵入から6時間" },
    { "#str_00200143", NULL, NULL, NULL, NULL, NULL, "第2クーラント倉庫" },
    { "#str_00200222", NULL, NULL, NULL, NULL, NULL, "御いかりの大いなる日がすでにきたのだ。だれがその前に立つことができようか?" },
    { "#str_00200223", NULL, NULL, NULL, NULL, NULL, "巨大な地の裂け目を見下ろしていると、遺跡から火とけむりが吹き起こった・・・" },
    { NULL, NULL, NULL, NULL, NULL, NULL, NULL }
};

// texts that should read as another one of the game's (in every language)
static const char* const vitaGameStringAliases[][2] = {
    // Steam permissions: there is no account to lack them
    { "#str_dlg_pc_incorrect_online_permissions", "#str_dlg_pc_online_signin_request" },
    { NULL, NULL }
};

void Vita_AddPortStringsTo(idLangDict& dict, int column, bool log);

void Vita_AddPortStrings(const char* lang)
{
    int column = 0; // english
    if (idStr::Icmp(lang, ID_LANG_FRENCH) == 0) {
        column = 1;
    } else if (idStr::Icmp(lang, ID_LANG_ITALIAN) == 0) {
        column = 2;
    } else if (idStr::Icmp(lang, ID_LANG_GERMAN) == 0) {
        column = 3;
    } else if (idStr::Icmp(lang, ID_LANG_SPANISH) == 0) {
        column = 4;
    } else if (idStr::Icmp(lang, ID_LANG_JAPANESE) == 0) {
        column = 5;
    }
    Vita_AddPortStringsTo(idLocalization::GetDictionary(), column, true);
}

// the port's strings, the game's own fixed and aliased, into a dictionary
// (the game's, or the verification's copy of a language - VitaVerify.cpp)
void Vita_AddPortStringsTo(idLangDict& dict, int column, bool log)
{
    int n = 0;
    for (int i = 0; vitaStrings[i].key != NULL; i++) {
        const vitaString_t& s = vitaStrings[i];
        const char* text = (&s.en)[column];
        dict.AddKeyVal(s.key, text != NULL ? text : s.en);
        n++;
    }
    int fixes = 0;
    for (int i = 0; vitaGameStringFixes[i].key != NULL; i++) {
        const vitaString_t& s = vitaGameStringFixes[i];
        const char* text = (&s.en)[column];
        if (text != NULL) {
            // the .lang files hold \n as two characters, turned into a line
            // break when parsed: do the same here
            idStr t = text;
            t.Replace("\\n", "\n");
            dict.AddKeyVal(s.key, t.c_str());
            fixes++;
        }
    }
    for (int i = 0; vitaGameStringAliases[i][0] != NULL; i++) {
        idStr to = dict.GetString(vitaGameStringAliases[i][1]);
        dict.AddKeyVal(vitaGameStringAliases[i][0], to.c_str());
        fixes++;
    }
    if (log) {
        common->Printf("vita: %d port strings added (language column %d), %d of the game's own fixed\n", n, column, fixes);
    }
}

// the column of a language in the tables above
int Vita_PortStringColumn(const char* lang)
{
    static const char* langs[] = { ID_LANG_ENGLISH, ID_LANG_FRENCH, ID_LANG_ITALIAN, ID_LANG_GERMAN, ID_LANG_SPANISH, ID_LANG_JAPANESE };
    for (int i = 0; i < 6; i++) {
        if (idStr::Icmp(lang, langs[i]) == 0) {
            return i;
        }
    }
    return 0;
}

// the English text of a port string (for fonts without the language's glyphs)
const char* Vita_PortStringEnglish(const char* key)
{
    for (int i = 0; vitaStrings[i].key != NULL; i++) {
        if (idStr::Icmp(vitaStrings[i].key, key) == 0) {
            return vitaStrings[i].en;
        }
    }
    return key;
}
