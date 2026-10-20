    if (common->IsMultiplayer()) {
#if defined(__vita__)
        // Keep Vita multiplayer on the same 30 Hz map logic rate as the
        // campaign. The Vita port is tuned for a 30 Hz game tic in maps, and
        // the PC-style 60/120Hz multiplayer override makes the game feel
        // noticeably slowed down.
        extern idCVar vita_gameHz;
        const int vitaHz = (vita_gameHz.GetInteger() >= 60) ? 60 : 30;
        com_engineHz_denominator = 100LL * vitaHz;
        com_engineHz_latched = (float)vitaHz;
        common->Printf("vita: multiplayer game logic at %d Hz in this map (vita_gameHz)\n", vitaHz);
#else
        // In multiplayer, make sure the player is either 60Hz or 120Hz
        // to avoid potential issues.
        const float mpEngineHz = (com_engineHz.GetFloat() < 90.0f) ? 60.0f : 120.0f;
        com_engineHz_denominator = 100LL * mpEngineHz;
        com_engineHz_latched = mpEngineHz;
#endif
    } else {
        // allow com_engineHz to be changed between map loads
#if defined(__vita__)
        extern idCVar vita_gameHz;
        const int vitaHz = (vita_gameHz.GetInteger() >= 60) ? 60 : 30;
        com_engineHz_denominator = 100LL * vitaHz;
        com_engineHz_latched = (float)vitaHz;
        common->Printf("vita: game logic at %d Hz in this map (vita_gameHz)\n", vitaHz);
#else
        com_engineHz_denominator = 100LL * com_engineHz.GetFloat();
        com_engineHz_latched = com_engineHz.GetFloat();
#endif
    }