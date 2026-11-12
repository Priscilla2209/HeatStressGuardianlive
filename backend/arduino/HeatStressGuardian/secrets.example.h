// =====================================================
//  secrets.example.h  -  template. Copy this file, rename the copy to secrets.h,
//  and fill in your private logins for Heat Stress Guardian
//
//  The real secrets.h stays on your own laptop only.
//  It is listed in .gitignore so it never goes to GitHub.
// =====================================================

#ifndef SECRETS_H
#define SECRETS_H

// -------- WIFI (2.4 GHz hotspot or router) --------

#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// -------- FIREBASE --------

#define API_KEY       "YOUR_FIREBASE_API_KEY"
#define USER_EMAIL    "YOUR_FIREBASE_LOGIN_EMAIL"
#define USER_PASSWORD "YOUR_FIREBASE_LOGIN_PASSWORD"

#define DATABASE_URL  "https://YOUR-PROJECT-default-rtdb.firebaseio.com"

#endif
