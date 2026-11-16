// Which screens to capture. Each shot() writes shots/<name>.ppm.
void runScenes() {
  currentScreen = SCREEN_LOCK; drawLockScreen(clockNow()); shot("lock");
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("home");
  currentScreen = SCREEN_TODO; drawTodoScreen(); shot("todo");
  currentScreen = SCREEN_HOMEWORK; drawHomeworkScreen(); shot("homework");
  currentScreen = SCREEN_WEATHER; drawWeatherScreen(); shot("weather");
  settingsCategory = -1; currentScreen = SCREEN_SETTINGS; drawSettingsScreen(); shot("settings");
  settingsCategory = 0; drawSettingsScreen(); shot("settings_appearance");
  currentScreen = SCREEN_THEMES; drawThemesScreen(); shot("themes");
  currentScreen = SCREEN_MORE_ABOUT; drawMoreAboutScreen(); shot("about");
  currentScreen = SCREEN_WIFI; drawWifiScreen(); shot("wifi");
  currentScreen = SCREEN_SET_TIME; drawSetTimeScreen(); shot("settime");
  currentScreen = SCREEN_SET_DATE; drawSetDateScreen(); shot("setdate");
  currentScreen = SCREEN_OTA; drawOtaScreen(); shot("ota");
  kbContext = KB_CONTEXT_ADD_TODO; strcpy(kbBuffer, "Buy milk"); kbBufferLen = 8;
  currentScreen = SCREEN_KEYBOARD; drawKeyboardScreen(); shot("keyboard");
  currentScreen = SCREEN_NOTES; drawNotesScreen(); shot("notes");
  openMessages(); shot("messages");
  openChat(0); shot("chat");
  chatTyping = true; strcpy(kbBuffer, "on my way"); kbBufferLen = 9; drawChatScreen(); shot("chat_typing");
  chatTyping = false; kbBuffer[0] = 0; kbBufferLen = 0;
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow());
  showBanner("Hound Dog", "192.168.1.77", "where are you"); shot("banner");
  bannerActive = false;
  strcpy(reqName, "Hound Dog"); strcpy(reqIp, "192.168.1.77"); showConnectPopup(); shot("connect_popup");
  currentScreen = SCREEN_WELCOME; drawWelcomeScreen(); shot("welcome");
  snakeInitGame(); currentScreen = SCREEN_SNAKE; drawSnakeScreen(); shot("snake");

  // press feedback ring on a Settings row
  settingsCategory = -1; currentScreen = SCREEN_SETTINGS; drawSettingsScreen();
  flashPressedButton(120, 110); shot("press_ring");
  // dark theme spot checks
  currentTheme = 1; applyTheme(1);
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("dark_home");
  openChat(0); shot("dark_chat");
  kbContext = KB_CONTEXT_ADD_TODO; strcpy(kbBuffer, "Buy milk"); kbBufferLen = 8;
  currentScreen = SCREEN_KEYBOARD; drawKeyboardScreen(); shot("dark_keyboard");
  currentScreen = SCREEN_TODO; drawTodoScreen(); shot("dark_todo");
  currentTheme = 0; applyTheme(0);

  // tap-through smoke test: lock -> home -> settings -> appearance -> toggle -> back
  currentScreen = SCREEN_LOCK; drawLockScreen(clockNow());
  tap(120, 200);                 // unlock
  tap(175, 250);                 // Settings tile
  tap(120, 60);                  // Appearance
  tap(120, 118);                 // Animations toggle (partial redraw + ring release)
  shot("tap_appearance");
  tap(20, 10); tap(20, 10);      // back, back
  tap(150, 12);                  // envelope -> Messages
  tap(120, 85);                  // first contact
  tap(80, 290);                  // Write
  tap(60, 140); tap(84, 140);    // type two letters
  shot("tap_typing");
  tap(20, 10); tap(20, 10); tap(20, 10);
  tap(175, 80);                  // To-Do tile
  tap(120, 295);                 // Add Task
  tap(60, 95); tap(84, 95); tap(200, 228);   // type 2, delete 1
  shot("tap_keyboard");
}
