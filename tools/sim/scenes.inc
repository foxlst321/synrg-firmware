// Which screens to capture. Each shot() writes shots/<name>.ppm.
void runScenes() {
  drawBootScreen(); shot("boot");
  currentScreen = SCREEN_LOCK; drawLockScreen(clockNow()); shot("lock");
  contacts[0].unread = 1; strcpy(msgBanner, "New message from Hound Dog");
  strcpy(weatherTemp, "54"); strcpy(weatherDesc, "Partly cloudy");
  drawLockScreen(clockNow()); shot("lock_notifs");
  contacts[1].unread = 3; drawLockScreen(DateTime(2026, 11, 16, 18, 5, 0)); shot("lock_evening");
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("home_unread");
  contacts[0].unread = 0; contacts[1].unread = 0; msgBanner[0] = 0;
  for (int w = 0; w < WIDGET_COUNT; w++) {
    homeWidget = w; drawHomeScreen(clockNow());
    char nm[24]; snprintf(nm, sizeof(nm), "home_widget%d", w); shot(nm);
  }
  homeWidget = WIDGET_CLOCK;
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("home");
  currentScreen = SCREEN_TODO; drawTodoScreen(); shot("todo");
  currentScreen = SCREEN_HOMEWORK; drawHomeworkScreen(); shot("homework");
  currentScreen = SCREEN_WEATHER; drawWeatherScreen(); shot("weather");
  { int codes[] = {0, 2, 3, 45, 63, 73, 95};
    for (int i = 0; i < 7; i++) {
      weatherCode = codes[i]; strcpy(weatherTemp, "54"); strcpy(weatherDesc, weatherCodeToDesc(codes[i]));
      weatherHigh = 61; weatherLow = 43; drawWeatherScreen();
      char nm[24]; snprintf(nm, sizeof(nm), "weather_%d", codes[i]); shot(nm);
    }
    weatherCode = -1; weatherHigh = weatherLow = -999; strcpy(weatherTemp, "--"); strcpy(weatherDesc, "Not fetched yet"); }
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
  tap(175, 265);                 // Settings tile
  tap(120, 60);                  // Appearance
  tap(120, 118);                 // Animations toggle (partial redraw + ring release)
  shot("tap_appearance");
  tap(20, 10); tap(20, 10);      // back, back
  tap(66, 265);                  // Messages tile
  tap(120, 85);                  // first contact
  tap(80, 290);                  // Write
  tap(60, 140); tap(84, 140);    // type two letters
  shot("tap_typing");
  tap(20, 10); tap(20, 10); tap(20, 10);
  tap(66, 130);                  // To-Do tile
  tap(120, 295);                 // Add Task
  tap(60, 95); tap(84, 95); tap(200, 228);   // type 2, delete 1
  shot("tap_keyboard");

  // secret theme: 7 taps on the firmware line in About
  currentScreen = SCREEN_MORE_ABOUT; drawMoreAboutScreen();
  for (int i = 0; i < 7; i++) tap(80, 54);
  shot("jensen_about");
  currentScreen = SCREEN_THEMES; drawThemesScreen(); shot("jensen_themes");
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("jensen_home");
  currentScreen = SCREEN_LOCK; drawLockScreen(clockNow()); shot("jensen_lock");
  drawBootScreen(); shot("jensen_boot");
  openChat(0); shot("jensen_chat");
  settingsCategory = -1; currentScreen = SCREEN_SETTINGS; drawSettingsScreen(); shot("jensen_settings");

  // ---- News: run sample RSS through the real parser ----
  currentTheme = THEME_OLED; applyTheme(currentTheme);
  { const char* feeds[3] = {
      "<rss><channel><title>NPR Top</title><link>https://www.npr.org/</link><description>top</description><item><title>Senate passes spending bill after late-night session, avoiding a government shutdown</title><description><![CDATA[The vote came just hours before funding was set to lapse. The bill now heads to the House, where leaders say they expect to take it up early next week.<img src='x.png' />]]></description><link>https://www.npr.org/2026/11/16/nx-s1-5609712/senate-passes-spending-bill</link><guid>z</guid></item>"
      "<item><title><![CDATA[Storm brings heavy snow &amp; ice to the Midwest]]></title></item>"
      "<item><title>What we know about the bridge closure</title></item></channel></rss>",
      "<rss><channel><title>Tech</title><item>\n  <title>Chipmakers race to build faster AI processors as demand keeps climbing worldwide this year</title></item>"
      "<item><title>Why your phone\xE2\x80\x99s battery drains faster in the cold</title></item>"
      "<item><title>A new &#8220;smart&#8221; glasses rival arrives</title></item></channel></rss>",
      "<rss><channel><item><title>Lions win in overtime thriller</title></item><item><title>World Cup draw: what to watch for</title></item></channel></rss>" };
    int cats[3] = {0, 1, 5};
    for (int f = 0; f < 3; f++) {
      newsCount[cats[f]] = 0; newsParserReset();
      for (const char* p = feeds[f]; *p; p++) newsParserFeed(*p, cats[f]);
    }
    newsMask = 0x23; buildNewsMix(); strcpy(newsStatus, "Updated 10:42 AM"); }
  openNews(); shot("news");
  tap(120, 41); shot("news_tech_off");       // toggle Tech chip off
  tap(120, 41);
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("home_ticker0");
  for (int i = 0; i < 60; i++) { simMillis += 45; updateNewsTicker(); }
  shot("home_ticker1");
  for (int i = 0; i < 120; i++) { simMillis += 45; updateNewsTicker(); }
  shot("home_ticker2");
  newsMask = 0; buildNewsMix(); drawHomeScreen(clockNow()); shot("home_ticker_empty");
  newsMask = 0x23; buildNewsMix();
  tap(120, 295); shot("news_from_ticker");   // tapping the ticker opens News
  currentTheme = 0; applyTheme(0); currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("home_ticker_nook");

  // Jensen theme switches on the hidden Nvidia topic (Google News style titles)
  { const char* feed = "<rss><channel><title>\"Nvidia\" - Google News</title><item><title>Nvidia unveils next-generation AI chips at keynote - Reuters</title></item>"
      "<item><title>Jensen Huang says demand for AI computing is &#39;incredible&#39; - CNBC</title></item></channel></rss>";
    newsCount[NEWS_CAT_NVIDIA] = 0; newsParserReset();
    for (const char* p = feed; *p; p++) newsParserFeed(*p, NEWS_CAT_NVIDIA); }
  currentTheme = THEME_SECRET; applyTheme(currentTheme);
  openNews(); shot("news_jensen");
  currentScreen = SCREEN_HOME; drawHomeScreen(clockNow());
  for (int i = 0; i < 90; i++) { simMillis += 45; updateNewsTicker(); }
  shot("home_ticker_jensen");
  currentTheme = THEME_OLED; applyTheme(currentTheme); openNews(); shot("news_after_jensen");

  // ---- article: summary, then a sample text.npr.org page through the real parser ----
  openNews(); tap(120, 100); shot("article_summary");
  { char u[80]; nprTextUrl(newsLinks[0][0], u, sizeof(u)); printf("text url: %s\n", u); }
  { const char* page = "<html><body><header><p>NPR</p><ul><li><a href='/'>Home</a></li></ul></header><article><div class=\"story-head\"><h1 class=\"story-title\">Senate passes spending bill</h1><p>By Jane Reporter</p><p>Monday, November 16, 2026 &bull; 8:02 AM EST</p></div><div class=\"paragraphs-container\">"
      "<p>WASHINGTON &mdash; The Senate passed a stopgap spending bill late Sunday night, sending the measure to the House with only hours to spare before federal funding was due to run out.</p>"
      "<p>The vote was 68 to 30, with a group of lawmakers from both parties <a href=\"/x\">joining together</a> after days of negotiation over how long the measure should last and what it should include.</p>"
      "<p>&ldquo;Nobody got everything they wanted,&rdquo; one senator said after the vote. &ldquo;That is usually how you know it is a real compromise.&rdquo;</p>"
      "<p>House leaders said they expect to bring the bill to the floor early this week. If it passes there, it would keep agencies running into the new year while talks continue on full-year funding.</p>"
      "<p>Federal workers had been preparing for the possibility of a shutdown, with agencies sending out notices about who would be asked to keep working without pay.</p>"
      "<p>Analysts say the short-term deal avoids an immediate crisis but sets up another deadline in a few months, when many of the same disagreements are likely to return.</p>"
      "<p>The White House said the president would sign the measure as soon as it reaches his desk.</p>"
      "</div></article><footer><p>Topics</p></footer></body></html>";
    articleParserReset();
    for (const char* p = page; *p; p++) articleParserFeed(*p);
    paginateArticle(); articlePage = 0; articleFull = true; drawArticleScreen(); shot("article_page1");
    printf("article chars %d pages %d\n", articleLen, articlePages);
    tap(200, 298); shot("article_page2"); }
  articleFull = false; strcpy(articleStatus, "Couldn't load it (-1)"); drawArticleScreen(); shot("article_error");
  currentTheme = THEME_SECRET; applyTheme(currentTheme); openArticle(NEWS_CAT_NVIDIA, 0); shot("article_nvidia");
  currentTheme = THEME_OLED; applyTheme(currentTheme);

  // v3.7: ticker switched off keeps the bar; ShareNote rename
  newsTickerOn = false; currentScreen = SCREEN_HOME; drawHomeScreen(clockNow()); shot("home_ticker_disabled");
  tap(120, 295); shot("news_ticker_off");
  newsTickerOn = true;
  openMessages(); shot("sharenote");
}
